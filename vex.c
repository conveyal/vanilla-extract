/* vex.c : vanilla-extract main */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <google/protobuf-c/protobuf-c.h> // contains varint functions
#include "intpack.h"
#include "pbf.h"
#include "tags.h"
#include "idtracker.h"

#include "lmdb/libraries/liblmdb/lmdb.h"

// 14 bits -> 1.7km at 45 degrees
// 13 bits -> 3.4km at 45 degrees
// at 45 degrees cos(pi/4)~=0.7
// TODO maybe shift one more bit off of y to make bins more square
#define GRID_BITS 14
/* The width and height of the grid root is 2^bits. */
#define GRID_DIM (1 << GRID_BITS)

/* Way reference block size is based on the typical number of ways per grid cell. */
#define WAY_BLOCK_SIZE 32

/* Assume one-fifth as many blocks as cells in the grid. Observed number is ~15000000 blocks. */
#define MAX_WAY_BLOCKS (GRID_DIM * GRID_DIM / 5)

/*
  Define the sequence in which elements are read and written, while allowing element types as
  function parameters and array indexes.
*/
#define NODE 0
#define WAY  1
#define RELATION 2

/* The location where we will save all files. This can be set using a command line parameter. */
static const char *database_path;

/* Compact geographic position. Latitude and longitude mapped to the signed 32-bit int range. */
typedef struct {
    int32_t x;
    int32_t y;
} coord_t;

/* Convert double-precision floating point latitude and longitude to internal representation. */
static void to_coord (/*OUT*/ coord_t *coord, double lat, double lon) {
    coord->x = (lon * INT32_MAX) / 180;
    coord->y = (lat * INT32_MAX) / 90;
} // TODO this is a candidate for return by value

/* Converts the y field of a coord to a floating point latitude. */
static double get_lat (coord_t *coord) {
    return ((double) coord->y) * 90 / INT32_MAX;
}

/* Converts the x field of a coord to a floating point longitude. */
static double get_lon (coord_t *coord) {
    return ((double) coord->x) * 180 / INT32_MAX;
}

/* 
  A block of way references. Chained together to record which ways begin in each grid cell. 
  Way references can still be stored in signed 32 bit integers since there are not as many of 
  them as there are nodes. If the last reference in a block is negative, it indicates how many
  slots are unused at the end of the block. New empty way blocks for a particular grid cell are 
  inserted at the head of the list, so even when the head block is not completely full it may
  point to a next block.
*/
typedef struct {
    int32_t refs[WAY_BLOCK_SIZE];
    uint32_t next; // the index of the next way block in the chain, or zero if there is no next way block.
} WayBlock;

/*
  A single OSM node. An array of 2^64 these serves as a map from node ids to nodes.
  OSM assigns node IDs sequentially, so you only need about the first 2^32 entries as of 2014.
  Note that when nodes are deleted their IDs are not reused, so there are holes in
  this range, but sparse file support in the filesystem should take care of that.
  "Deleted node ids must not be reused, unless a former node is now undeleted."
*/
typedef struct {
    coord_t coord; // compact internal representation of latitude and longitude
    uint32_t tags; // byte offset into the packed tags array where this node's tag list begins
} Node;

/*
  A single OSM way. Like nodes, way IDs are assigned sequentially, so a zero-indexed array of these
  serves as a map from way IDs to ways.
*/
typedef struct {
    uint32_t node_ref_offset; // the index of the first node in this way's node list
    uint32_t tags; // byte offset into the packed tags array where this node's tag list begins
} Way;

/*
  A single OSM relation. Like nodes, relation IDs are assigned sequentially, so a zero-indexed array 
  of these serves as a map from relation IDs to relations. OSM is just over 2^31 entities now, so
  even if every node was in a relation we could still index them all relation members with a uint32.
*/
typedef struct {
    uint32_t member_offset; // the index of the first member in this relation's member list
    uint32_t tags; // byte offset into the packed tags array where this relation's tag list begins
    uint32_t next; // the index of the next relation in this grid cell
} Relation;

/* Indexes for the first block of nodes and the first relation in each grid cell. */
typedef struct {
    uint32_t head_way_block;
    uint32_t head_relation;
} GridCell;

/*
  The spatial index grid. A node's grid bin is determined by right-shifting its coordinates.
  Initially this was a multi-level grid, but it turns out to work fine as a single level.
  Rather than being directly composed of way reference blocks, there is a level of indirection
  because the grid is mostly empty due to ocean and wilderness. 
  TODO eliminate coastlines etc.
  TODO struct is no longer necessary because this is not a compound type.
*/
typedef struct {
    GridCell cells[GRID_DIM][GRID_DIM]; // contains indexes to way_blocks and relations
} Grid;

/* Print human readable representation based on multiples of 1024 into a static buffer. */
static char human_buffer[128];
char *human (size_t bytes) {
    /* Convert to a double, so division can yield results with decimal places. */
    double s = bytes;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf ", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf ki", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf Mi", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf Gi", s);
        return human_buffer;
    }
    s /= 1024;
    sprintf (human_buffer, "%.1lf Ti", s);
    return human_buffer;
}

void die (char *s) {
    fprintf(stderr, "%s\n", s);
    exit(EXIT_FAILURE);
}

/* Make a filename under the database directory, performing some checks. */
static char path_buf[512];
static char *make_db_path (const char *name, uint32_t subfile) {
    if (strlen(name) >= sizeof(path_buf) - strlen(database_path) - 12) {
        die("Name too long.");
    }
    size_t path_length = strlen(database_path);
    if (path_length == 0) {
        die("Database path must be non-empty.");
    }
    if (database_path[path_length - 1] == '/') {
        path_length -= 1;
    }
    if (subfile == 0) {
        sprintf (path_buf, "%.*s/%s", (int) path_length, database_path, name);
    } else {
        sprintf (path_buf, "%.*s/%s.%03d", (int) path_length, database_path, name, subfile);
    }
    return path_buf;
}

/*
  Map a file in the database directory into memory, letting the OS handle paging.
  Note that we cannot reliably re-map a file to the same memory address, so the files should not
  contain pointers. Instead we store array indexes, which can have the advantage of being 32-bits
  wide. We map one file per OSM object type.

  Mmap will happily map a zero-length file to a nonzero-length block of memory, but a bus error
  will occur when you try to write to the memory.

  It is tricky to expand the mapped region on demand you'd need to trap the bus error.
  Instead we reserve enough address space for the maximum size we ever expect the file to reach.
  Linux provides the mremap() system call for expanding or shrinking the size of a given mapping.
  msync() flushes the changes in memory to disk.

  The ext3 and ext4 filesystems understand "holes" via the sparse files mechanism:
  http://en.wikipedia.org/wiki/Sparse_file#Sparse_files_in_Unix
  Creating 100GB of empty file by calling truncate() does not increase the disk usage.
  The files appear to have their full size using 'ls', but 'du' reveals that no blocks are in use.
*/
void *map_file (const char *name, uint32_t subfile, size_t size) {
    make_db_path (name, subfile);
    int fd;
    fprintf(stderr, "Mapping file '%s' of size %sB.\n", path_buf, human(size));
    // including O_TRUNC causes much slower write (swaps pages in?)
    fd = open(path_buf, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        die("Could not memory map file.");
    }
    // Resize file
    if (ftruncate (fd, size - 1)) {
        die("Error resizing file.");
    }
    return base;
}

/* Open a buffered FILE in the current working directory for writing, performing some checks. */
FILE *open_output_file(const char *name, uint8_t subfile) {
    fprintf(stderr, "Opening file '%s' for binary writing.\n", name);
    FILE *file = fopen(name, "wb"); // Creates if file does not exist.
    if (file == NULL) die("Could not open file for output.");
    return file;
}

/* Arrays of memory-mapped structs. This is where we store the bulk of our data. */
Grid      *grid;
Node      *nodes;
Way       *ways;
WayBlock  *way_blocks;
Relation  *relations;
RelMember *rel_members;
int64_t   *node_refs;        // A negative node_ref marks the end of a list of refs.
uint32_t  n_rel_members = 1; // The number of relation members currently used. start at 1 since zero marks the end of lists.
uint32_t  n_node_refs = 0;   // The number of node refs currently used.
// FIXME n_node_refs will eventually overflow. The fact that it's unsigned gives us a little slack.
// FIXME the n_vars were not initialized before?

/*
  The number of way reference blocks currently allocated.
  Sparse files appear to be full of zeros until you write to them. Therefore we skip way block zero
  so we can use the zero index to mean "no way block".
*/
uint32_t way_block_count = 1;
static uint32_t new_way_block() {
    if (way_block_count % 100000 == 0)
        fprintf(stderr, "%dk way blocks in use out of %dk.\n", way_block_count/1000, MAX_WAY_BLOCKS/1000);
    if (way_block_count >= MAX_WAY_BLOCKS)
        die("More way reference blocks are used than expected.");
    // A negative value in the last ref entry gives the number of free slots in this block.
    way_blocks[way_block_count].refs[WAY_BLOCK_SIZE-1] = -WAY_BLOCK_SIZE;
    // Also set the next block index to 0 to indicate no next block
    way_blocks[way_block_count].next = 0; 
    // fprintf(stderr, "created way block %d\n", way_block_count);
    return way_block_count++;
}

/* Get the x or y bin for the given x or y coordinate. */
static uint32_t bin (int32_t xy) {
    return ((uint32_t)(xy)) >> (32 - GRID_BITS); // unsigned: logical shift
}

/* Get the address of the grid cell for the given internal coord. */
static GridCell *get_grid_cell_for_coord (coord_t coord) {
    return &(grid->cells[bin(coord.x)][bin(coord.y)]);
}

/* Return the GridCell containing the first member of the given relation. */
static GridCell *get_grid_cell_for_relation (Relation *r) {
    RelMember first_member = rel_members[r->member_offset];
    if (first_member.id < 0) {
        // The relation has only one member. This is invalid so don't index it.
        return NULL;
    }
    if (first_member.element_type == NODE) {
        return get_grid_cell_for_coord (nodes[first_member.id].coord);
    } else if (first_member.element_type == WAY) {
        Way way = ways[first_member.id];
        Node first_node = nodes[way.node_ref_offset];
        return get_grid_cell_for_coord (first_node.coord);
    } else { 
        // (first_member.element_type == RELATION) {
        // TODO recurse... but the referenced relation may not be loaded.
        // so return null and catch this condition in the caller.
        return NULL;
    }
}

/*
  Given a Node struct, return the index of the way reference block at the head of the Node's grid
  cell, creating a new way reference block if the grid cell is currently empty.
*/
static uint32_t get_grid_way_block (Node *node) {
    GridCell *cell = get_grid_cell_for_coord (node->coord);
    if (cell->head_way_block == 0) {
        cell->head_way_block = new_way_block();
    }
    return cell->head_way_block;
}

/* TODO make this insert a new block at the list head instead of just setting the grid cell contents. */
static void set_grid_way_block (Node *node, uint32_t way_block_index) {
    GridCell *cell = get_grid_cell_for_coord (node->coord);
    cell->head_way_block = way_block_index;
}

/* A memory block holding tags for a sub-range of the OSM ID space. */
typedef struct {
    uint8_t *data;
    size_t pos;
} TagSubfile;

// MAX_SUBFILES must be larger than MAX_WAY_ID divided by the number of IDs per partition, 15 at present.
#define MAX_SUBFILES 20
static TagSubfile tag_subfiles[MAX_SUBFILES] = {[0 ... MAX_SUBFILES - 1] = {.data=NULL, .pos=0}};

/*
  To allow 32-bit byte offsets for tags, we associate blocks of entity ID space with tag storage partitions.
  Most tags are on ways. There are about 10 times as many nodes as ways, and 100 times less relations than ways, 
  so we divide node IDs and multiply relation IDs to roughly normalize them to the range of way IDs.
  This partitioning scheme should also be applied to other tables, such as node references (partition by way ID)
  and way references (partition by flattened grid cell index). All should be scaled to the same range of MAX_WAY_ID.
*/
static uint32_t subfile_index_for_id (int64_t osmid, int entity_type) {
    if (entity_type == NODE) osmid /= 16;
    else if (entity_type == RELATION) osmid *= 64;
    // Bit-shifting by 25 bits splits the way id space into sub-ranges of about 33 million IDs.
    // The 2.5% of nodes that have tags have an average of 3.2 tags each. 
    // So we expect around 2.7 million way tags in this ID range (2^15 * 0.025 * 3.2).
    // Way density is fixed at 1/16 of nodes, and ways have an average of 2.3 tags.
    // So we expect about 4.8 million way tags in this ID range ((2^15 / 16) * 2.3).
    // This gives us an average of 572 bytes of addressable storage per tag
    // (2^32 / ((2^25 * 0.025 * 3.2) + ((2^25 / 16) * 2.3))).
    // Shifting by 26 bits halves this to an average of 286 bytes per tag, making less files.
    uint32_t subfile = osmid >> 26; 
    return subfile;
}

/* Get the subfile in which the tags for the given OSM entity should be stored. */
static TagSubfile *tag_subfile_for_id (int64_t osmid, int entity_type) {
    uint32_t subfile = subfile_index_for_id (osmid, entity_type);
    if (subfile >= MAX_SUBFILES) die ("Need more subfiles than expected.");
    TagSubfile *ts = &(tag_subfiles[subfile]);
    if (ts->data == NULL) {
        /* Lazy-map a subfile the first time it is needed. */
        ts->data = map_file("tags", subfile, UINT32_MAX); // all files are 4GB sparse maps
        /* 
          Store a tag list terminator byte at the beginning of each file. This empty list will 
          be shared by all entities that do not have any tags, which all have tag offset zero.
        */
        ts->data[0] = INT8_MAX; 
        ts->pos = 1;
    }
    return ts;
}

/*
  Grab a pointer to tag subfile data directly. Convenience method to avoid manually dereferencing.
  This does not seek to the element within the tag file, it returns the beginning adress.
  TODO perform the seek here as well?
*/
static uint8_t *tag_data_for_id (int64_t osmid, int entity_type) {
    return tag_subfile_for_id(osmid, entity_type)->data;
}

/* Write a ProtobufCBinaryData out to a TagSubfile, updating the subfile position accordingly. */
static void ts_write(ProtobufCBinaryData *bd, TagSubfile *ts) {
    uint8_t *dst = ts->data + ts->pos;
    uint8_t *src = bd->data;
    for (int i = 0; i < bd->len; i++) *(dst++) = *(src++);
    ts->pos += bd->len;
}

/* Write a ProtobufCBinaryData out to buffer, prefixed with its length as an unsigned 64 bit varint. */
static size_t bd_write (uint8_t *buf, ProtobufCBinaryData *bd) {
    uint8_t *dst = buf + uint64_pack(bd->len, buf);
    uint8_t *src = bd->data;
    for (int i = 0; i < bd->len; i++) *(dst++) = *(src++);
    return dst - buf;
}

/* Write a single char out to a TagSubfile, updating the subfile position accordingly. */
static void ts_putc(char c, TagSubfile *ts) {
    ts->data[(ts->pos)++] = c;
}

/*
  Given parallel tag key and value arrays of length n containing string table indexes,
  write compacted lists of key=value pairs to a buffer that does not require the string table.
  Returns the number of bytes written.
*/
static uint32_t write_tags (uint32_t *keys, uint32_t *vals, int n, ProtobufCBinaryData *string_table, uint8_t *buf, size_t buflen) {
    // Pointer to next output byte in the output buffer.
    uint8_t *b = buf;
    // First store the number of tags that will follow, even when the number is zero.
    b += uint64_pack (n, b);
    for (int t = 0; t < n; t++) {
        ProtobufCBinaryData key = string_table[keys[t]];
        ProtobufCBinaryData val = string_table[vals[t]];
        int8_t code;
        // Skip unneeded keys that are very common and voluminous
        if (memcmp("created_by",  key.data, key.len) == 0 ||
            memcmp("import_uuid", key.data, key.len) == 0 ||
            memcmp("attribution", key.data, key.len) == 0 ||
            memcmp("source",      key.data, 6) == 0 ||
            memcmp("tiger:",      key.data, 6) == 0) {
            code = 0; // TODO completely skip these tags (count them before output). Just record a zero for now.
        } else {
            code = encode_tag(key, val);
        }
        // Always write out the code, to encode a key-value pair, a key with freetext value, or completely freetext.
        *(b++) = code;
        if (code == 0) {
            // Code 0 means key and value are written out in full as freetext.
            // Saving only tags with 'known' keys (nonzero codes) cuts file sizes in half.
            // Some are reduced by over 4x, which seem to contain a lot of bot tags.
            b += bd_write(b, &key);
            b += bd_write(b, &val);
        } else if (code < 0) {
            // Negative code provides key lookup, but value is written as zero-terminated free text.
            b += bd_write(b, &val);
        }
        if (b > buf + buflen) {
            die("Tag buffer overflow.");
        }
    }
    return b - buf;
}

/* Count the number of nodes and ways loaded, just for progress reporting. */
static long nodes_loaded = 0;
static long ways_loaded = 0;
static long rels_loaded = 0;

// LMDB state
static MDB_env *env;
static MDB_dbi dbi_nodes, dbi_ways, dbi_relations;
static MDB_val key, data;
static MDB_txn *txn;
// static MDB_cursor *cursor;

// Handle LMDB error codes
static inline void err_trap (char* name, int err) {
  if (err) {
    printf("%s ERROR: %s\n", name, mdb_strerror(err));
    exit(err);
  } else if (name != NULL){
    printf("%s SUCCESS\n", name);
  }
}

// Keep reusing a single struct. The end with tags is variable length. 
// Store only up to the end of the used portion of the tags buffer.
#define NODE_BUF_LEN (1024 * 1024) 
static struct {
    coord_t coord; 
    uint8_t tags[NODE_BUF_LEN];
} tempNode;

/* Node callback handed to the general-purpose PBF loading code. */
static void handle_node (OSMPBF__Node *node, ProtobufCBinaryData *string_table) {
    // lat and lon are in nanodegrees
    double lat = node->lat * 0.000000001;
    double lon = node->lon * 0.000000001;
            
    to_coord(&(tempNode.coord), lat, lon);
    size_t nTagBytes = write_tags(node->keys, node->vals, node->n_keys, string_table, tempNode.tags, NODE_BUF_LEN);

    key.mv_size = sizeof(uint64_t);
    key.mv_data = &(node->id);
    data.mv_size = sizeof(coord_t) + nTagBytes;
    data.mv_data = &tempNode;
    // printf("key: %llu \n", node->id);
    // TODO check result code, ensure ascending IDs before using MDB_APPEND
    err_trap(NULL, mdb_put(txn, dbi_nodes, &key, &data, MDB_APPEND));

    nodes_loaded++;
    if (nodes_loaded % 1000000 == 0) {
        fprintf(stderr, "Loaded %ldM nodes.\n", nodes_loaded / 1000000);
    }
    //printf ("---\nlon=%.5f lat=%.5f\nx=%d y=%d\n", lon, lat, nodes[node->id].x, nodes[node->id].y);
}

// Buffer bytes representing one way out to database.
static uint8_t tempWay[NODE_BUF_LEN];

/*
  Way callback handed to the general-purpose PBF loading code.
  All nodes must come before any ways in the input for this to work.
*/
static void handle_way (OSMPBF__Way *way, ProtobufCBinaryData *string_table) {
    // Pointer to next output byte in the output buffer.
    uint8_t *b = tempWay;
    // Check n_refs is sane (>1)
    if (way->n_refs < 2) {
        fprintf(stderr, "Way %lld has less than two nodes, skipping.\n", way->id);
        return;
    }        
    // First store the number of nodes that will follow.
    b += uint64_pack (way->n_refs, b);
    // Then copy over delta coded varint node refs.
    for (int r = 0; r < way->n_refs; r++, n_node_refs++) {
        // node refs are delta coded and we're going to keep them that way. 
        // note we could just copy the raw packed PBF data.
        b += sint64_pack (way->refs[r], b);
    }
    b += write_tags(way->keys, way->vals, way->n_keys, string_table, b, NODE_BUF_LEN);

    // TODO Handle spatial indexing, possibly still using grid or with duplicate keys in LMDB.

    //// LMDB
    key.mv_size = sizeof(uint64_t);
    key.mv_data = &(way->id);
    data.mv_size = b - tempWay;
    data.mv_data = &tempWay;
    // TODO check result code, check ascending IDs before using MDB_APPEND
    err_trap(NULL, mdb_put(txn, dbi_ways, &key, &data, MDB_APPEND));
    ////
    
    ways_loaded++;
    if (ways_loaded % 1000000 == 0) {
        fprintf(stderr, "Loaded %ldM ways.\n", ways_loaded / 1000000);
    }
}

#define MAX_REL_MEMBERS 100000000

/*
  Relation callback handed to the general-purpose PBF loading code.
  All nodes and ways must come before relations in the input file for this to work.
  Copies one OSMPBF__Relation into a VEx Relation and inserts it in the grid spatial index.
*/
static void handle_relation (OSMPBF__Relation* relation, ProtobufCBinaryData *string_table) {
    if (relation->n_memids == 0) return; // logic below expects at least one member reference
    Relation *r = &(relations[relation->id]); // the Vex struct into which we are copying the PBF relation
    r->member_offset = n_rel_members;
    RelMember *rm = &(rel_members[n_rel_members]);
    /* Check to avoid writing past the end of the relation members file. */
    if (n_rel_members + relation->n_memids >= MAX_REL_MEMBERS) {
        die ("There are more relation members in the OSM data than expected.");
    }
    /* Copy all the relation members from PBF into the VEx array. */
    int64_t last_id = 0;
    for (int m = 0; m < relation->n_memids; m++, n_rel_members++, rm++) {
        rm->role = encode_role(string_table[relation->roles_sid[m]]);
        /* OSMPBF NODE, WAY, RELATION constants use the same ints as ours. */
        rm->element_type = relation->types[m];
        int64_t id = relation->memids[m] + last_id; // delta-decode
        last_id = id;
        rm->id = (uint32_t)id; // currently, 2^31 < max osmid < 2^32
    }
    (rm - 1)->id *= -1; // Negate the last relation member id to signal the end of the list
    /* Save tags to compacted tag array, and record the index where this relation's tag list begins. */
    TagSubfile *ts = tag_subfile_for_id (relation->id, RELATION);
    // TODO REPLACE RELATION TAG WRITING
    // r->tags = write_tags (relation->keys, relation->vals, relation->n_keys, string_table, ts);
    /* Insert this relation at the head of a linked list in its containing spatial index grid cell.
       The GridCell's head field is initially set to zero since it is in a new mmapped file. */
    GridCell *grid_cell = get_grid_cell_for_relation (r);
    r->next = 0; // zero means no next relation in this grid cell (we start real relations at index 1).
    if (grid_cell != NULL) {
        r->next = grid_cell->head_relation;
        grid_cell->head_relation = relation->id;
    }
    rels_loaded++;
    if (rels_loaded % 100000 == 0)
        fprintf(stderr, "loaded %ldk relations\n", rels_loaded / 1000);
}

/*
  Show the percentage of grid cells containing any objects.
  Used to give empirical hints on setting the grid cell size.
  With 8 bit (256x256) grid, planet.pbf gives 36.87% full
  With 14 bit grid: 248351486 empty 20083970 used, 7.48% full
*/
static void fillFactor () {
    int used = 0;
    for (int i = 0; i < GRID_DIM; ++i) {
        for (int j = 0; j < GRID_DIM; ++j) {
            if (grid->cells[i][j].head_way_block != 0) used++;
        }
    }
    fprintf(stderr, "index grid: %d used, %.2f%% full\n",
        used, ((double)used) / (GRID_DIM * GRID_DIM) * 100);
}

/* Print out a message explaining command line parameters to the user, then exit. */
static void usage () {
    fprintf(stderr, "usage:\nvex database_dir <input.osm.pbf>\n");
    fprintf(stderr, "vex database_dir min_lon,min_lat,max_lon,max_lat <output.osm.pbf>\n");
    fprintf(stderr, "The output file name can also end in .vex or be - for stdout.\n");
    exit(EXIT_SUCCESS);
}

/* Range checking. */
static void check_lat_range(double lat) {
    if (lat < -90 || lat > 90)
        die ("Latitude out of range.");
}

/* Range checking. */
static void check_lon_range(double lon) {
    if (lon < -180 || lon > 180)
        die ("Longitude out of range.");
}

/* 
  Functions beginning with print_ output OSM in a simple structured text format.
  They are not static because they never need to be fast and they are only called when debugging.
  External visibility will keep the compiler from complaining when they are unused (hack).
*/
void print_tags (uint8_t *tag_data) {
    char *t = (char*)tag_data;
    KeyVal kv;
    while (*t != INT8_MAX) {
        t += decode_tag(t, &kv);
        fprintf(stderr, "%s=%s ", kv.key, kv.val);
    }
}

void print_node (uint64_t node_id) {
    Node node = nodes[node_id];
    fprintf (stderr, "  node %llu (%.6f, %.6f) ", node_id, get_lat(&node.coord), get_lon(&node.coord));
    uint8_t *tag_data = tag_data_for_id (node_id, NODE);
    fprintf (stderr, "(offset %d)", node.tags);
    print_tags (tag_data + node.tags);
    fprintf (stderr, "\n");
}

void print_way (int64_t way_id) {
    fprintf (stderr, "way %llu ", way_id);
    uint8_t *tag_data = tag_data_for_id (way_id, WAY);
    print_tags (tag_data + ways[way_id].tags);
    fprintf (stderr, "\n");
}

/* 
    Functions prefixed with vexbin_write_ output OSM in a much simpler binary format.
    This is comparable in size or smaller than PBF if you zlib it in blocks, but much simpler.
    Q: why does PBF use string tables since a similar result is achieved by zipping the blocks?
    The variables declared here are used to hold shared state for all the write functions.
    TODO move to another module, or namespace these variables with a prefix or a struct.
*/

int32_t last_x, last_y;
int64_t last_node_id, last_way_id;
FILE *ofile;

/* Begin writing to a VEx format OSM file. */
void vexbin_write_init (FILE *output_file) {
    last_x = 0;
    last_y = 0;
    last_node_id = 0;
    last_way_id = 0;
    ofile = output_file;
}

/* Write a positive integer to the output file using Protobuf variable width conventions. */
static void vexbin_write_length (size_t length) {
    // max length of a 64 bit varint is 10 bytes
    uint8_t varint_buf[10]; 
    size_t size = uint64_pack (length, varint_buf);
    fwrite (&varint_buf, size, 1, ofile);
}

/* Write a signed integer to the output file using Protobuf variable width conventions. */
static void vexbin_write_signed (int64_t length) {
    // max length of a 64 bit varint is 10 bytes
    uint8_t varint_buf[10]; 
    size_t size = sint64_pack (length, varint_buf);
    fwrite (&varint_buf, size, 1, ofile);
}

/* 
  Write a byte buffer to the output file, where the buffer address and length are provided separately.
  The raw bytes are prefixed with a variable-width integer giving their length. This format should 
  be the same size as the zero-terminated representation for for all strings up to 128 characters.
*/
static void vexbin_write_buf (char *bytes, size_t length) {
    vexbin_write_length (length);
    fwrite (bytes, length, 1, ofile);
}

/* Write a zero-terminated string using our byte buffer format. */
static void vexbin_write_string (char *string) {
    size_t len = strlen (string);
    vexbin_write_buf (string, len);
}

/* 
  Decode a list of tags from VEx internal format and write them out as length-prefixed strings.
  The length of this list is output first as a variable-width integer.
  The subsequent data compression pass should tokenize any frequently occurring tags.
*/
static void vexbin_write_tags (uint8_t *tag_data) {
    KeyVal kv; // stores the output of the tag decoder function
    char *t0 = (char*) tag_data;
    char *t = t0;
    int ntags = 0;
    /* First count the number of tags and write out that number. */
    while (*t != INT8_MAX) {
        t += decode_tag (t, &kv);
        ntags += 1;
    }
    vexbin_write_length (ntags);
    /* Then reset to the beginning of the list and actually write out the tags. */
    t = t0;
    while (*t != INT8_MAX) {
        t += decode_tag (t, &kv);        
        vexbin_write_string (kv.key);
        vexbin_write_string (kv.val);
    }
}

static void vexbin_write_node (int64_t node_id) {
    Node node = nodes[node_id];
    int64_t id_delta = node_id - last_node_id;
    // TODO convert to fixed-point lat,lon as in PBF?
    int32_t x_delta = node.coord.x - last_x;
    int32_t y_delta = node.coord.y - last_y;
    vexbin_write_signed (id_delta);
    vexbin_write_signed (x_delta);
    vexbin_write_signed (y_delta);
    uint8_t *tag_data = tag_data_for_id (node_id, NODE);
    vexbin_write_tags (tag_data + node.tags); // TODO does this work if tag list is empty?
    /* Retain values to allow delta-coding on next node to be written. */
    last_node_id = node_id;
    last_x = node.coord.x;
    last_y = node.coord.y;
}

static void vexbin_write_way (int64_t way_id) {
    Way way = ways[way_id];
    int64_t id_delta = way_id - last_way_id;
    vexbin_write_signed (id_delta);
    /* Count the number of node refs in this way and write out the count before the list. */
    int n_refs = 0;
    for (int64_t *node_ref_p = node_refs + way.node_ref_offset; true; node_ref_p++) {
        n_refs++;
        if (*node_ref_p < 0) break;
    }
    vexbin_write_length (n_refs);
    int64_t *node_refs_for_way = node_refs + way.node_ref_offset;
    for (int r = 0; r < n_refs; r++) {
        int64_t node_ref = node_refs_for_way[r];
        if (node_ref < 0) node_ref = -node_ref;
        // Delta code way references (even across ways) 
        int64_t ref_delta = node_ref - last_node_id;
        last_node_id = node_ref; 
        vexbin_write_signed (ref_delta);
    }
    uint8_t *tag_data = tag_data_for_id (way_id, WAY);
    vexbin_write_tags (tag_data + way.tags);
    /* Retain value to allow delta-coding on next way to be written. */
    last_way_id = way_id;
}

#define ACTION_NONE 0
#define ACTION_LOAD 1
#define ACTION_EXTRACT 2

int main (int argc, const char * argv[]) {

    /* Decide whether we are loading or extracting based on the number of command line parameters. */
    int action = ACTION_NONE;
    if (argc == 3) {
        action = ACTION_LOAD;
    } else if (argc == 4) {
        action = ACTION_EXTRACT;
    } else {
        usage();
    }
    
    /* When creating an on-disk database, create the directory and complain loudly if it already exists.
    We don't want to accidentally destroy two hours of PBF loading, and we don't want to re-open an 
    existing database for writing (that's not supported yet and causes undefined behavior). */
    database_path = argv[1];
    if (ACTION_LOAD == action) {
        int err = mkdir(database_path, 0777);
        if (err == -1) {
            die("Could not create database. Directory already exists or insufficient permissions?");
        }
    }

    /* Open or create the lock file, which will prevent database writes from happening during reads.
    Use BSD-style locks which are associated with the file, not the process. */
    int lock_fd = open("/tmp/vex.lock", O_CREAT, S_IRWXU);
    if (lock_fd == -1) {
        die ("Error opening or creating lock file.");
    }

    /* Memory-map files or create shared memory objects for each OSM element type, 
    and for references between them. */
    // NOTE mapping all these large files caused significant pauses in the LMDB loading.
    // Not mapping them gave something like a 20% speedup.
    
    // grid        = map_file("grid",        0, sizeof(Grid));
    // ways        = map_file("ways",        0, sizeof(Way)       * MAX_WAY_ID);
    // nodes       = map_file("nodes",       0, sizeof(Node)      * MAX_NODE_ID);
    // node_refs   = map_file("node_refs",   0, sizeof(int64_t)   * MAX_NODE_REFS);
    // way_blocks  = map_file("way_blocks",  0, sizeof(WayBlock)  * MAX_WAY_BLOCKS);
    // relations   = map_file("relations",   0, sizeof(Relation)  * MAX_REL_ID);
    // rel_members = map_file("rel_members", 0, sizeof(RelMember) * MAX_REL_MEMBERS);

    if (ACTION_LOAD == action) {

        /* LOAD INTO DATABASE */
        const char *filename = argv[2];
        PbfReadCallbacks callbacks = {
            //.node = &handle_node,
            .node = NULL,
            .way  = &handle_way,
            // .relation = &handle_relation
            // .way  = NULL,
            .relation = NULL
        };
        /* Request an exclusive write lock, blocking while reads complete. */
        fprintf(stderr, "Acquiring exclusive write lock on database.\n");
        flock(lock_fd, LOCK_EX);
        
        ////////// LMDB
        err_trap("Env create    ", mdb_env_create(&env));
        err_trap("Env maxsize   ", mdb_env_set_mapsize(env, 1024L * 1024 * 1024 * 20)); // 20GB maximum map size
        err_trap("Env maxdbs    ", mdb_env_set_maxdbs(env, 5));
        err_trap("Env open      ", mdb_env_open(env, "./testdb", MDB_WRITEMAP, 0664));
        err_trap("Txn begin     ", mdb_txn_begin(env, NULL, 0, &txn));
        err_trap("Dbi open nodes", mdb_dbi_open(txn, "nodes", MDB_INTEGERKEY | MDB_CREATE, &dbi_nodes));
        err_trap("Dbi open ways ", mdb_dbi_open(txn, "ways", MDB_INTEGERKEY | MDB_CREATE, &dbi_ways));
        err_trap("Dbi open rels ", mdb_dbi_open(txn, "relations", MDB_INTEGERKEY | MDB_CREATE, &dbi_relations));
        //////////

        // Callbacks could also be static variables, why not.
        pbf_read (filename, callbacks);
        
        ////////// LMDB
        // Should we commit more often? Any downside to one huge commit? Can we disable transactions?
        fprintf(stderr, "Committing transaction...\n");
        err_trap("Txn commit", mdb_txn_commit(txn));
        mdb_env_close(env);
        //////////
        
        // Disable grid summary since grid is not mapped/allocated
        // fillFactor();
        /* Release exclusive write lock, allowing reads to begin. */
        flock(lock_fd, LOCK_UN);
        fprintf(stderr, "loaded %ld nodes, %ld ways, and %ld relations total.\n", 
                nodes_loaded, ways_loaded, rels_loaded);
        return EXIT_SUCCESS;
        
    } else if (ACTION_EXTRACT == action) {
    
        /* EXTRACT FROM DATABASE */
        double min_lon = strtod(strtok((char *) argv[2], ","), NULL);
        double min_lat = strtod(strtok(NULL, ","), NULL);
        double max_lon = strtod(strtok(NULL, ","), NULL);
        double max_lat = strtod(strtok(NULL, ","), NULL);
        fprintf(stderr, "min = (%.5lf, %.5lf) max = (%.5lf, %.5lf)\n", min_lon, min_lat, max_lon, max_lat);
        check_lat_range(min_lat);
        check_lat_range(max_lat);
        check_lon_range(min_lon);
        check_lon_range(max_lon);
        if (min_lat >= max_lat) die ("min lat must be less than max lat.");
        if (min_lon >= max_lon) die ("min lon must be less than max lon.");
        coord_t cmin, cmax;
        to_coord(&cmin, min_lat, min_lon);
        to_coord(&cmax, max_lat, max_lon);
        uint32_t min_xbin = bin(cmin.x);
        uint32_t max_xbin = bin(cmax.x);
        uint32_t min_ybin = bin(cmin.y);
        uint32_t max_ybin = bin(cmax.y);
        bool vexformat = false;

        /* Request a shared read lock, blocking while any writes to complete. */
        fprintf(stderr, "Acquiring shared read lock on database.\n");
        flock(lock_fd, LOCK_SH);

        /* Get the output stream, interpreting the dash character as stdout. */
        FILE *output_file;
        if (strcmp(argv[3], "-") == 0) {
            output_file = stdout;
        } else {
            output_file = open_output_file (argv[3], 0);
            char *dot = strrchr (argv[3],'.');
            /* Use a custom binary format when the file extension is .vex */
            if (dot != NULL && strcmp (dot,".vex") == 0) {
                vexformat = true;
                fprintf (stderr, "Output will be in VEX binary format.\n");
            }
        }

        /* Initialize writing state for the chosen format. */
        if (vexformat) {
            vexbin_write_init (output_file);
        } else {
            pbf_write_begin (output_file);
        }

        /* Initialize the ID tracker so we can avoid outputting nodes more than once. */
        IDTracker_reset (); // TODO also track ways so we can store ways in more than one tile
        
        /* Make three passes, first outputting all nodes, then all ways, then all relations. */
        for (int stage = NODE; stage <= RELATION; stage++) {
            for (uint32_t x = min_xbin; x <= max_xbin; x++) {
                for (uint32_t y = min_ybin; y <= max_ybin; y++) {
                    if (stage == RELATION) {
                        uint32_t relation_id = grid->cells[x][y].head_relation;
                        while (relation_id > 0) {
                            Relation rel = relations[relation_id];
                            if (vexformat) {
                                // TODO Output relations in VEX format
                            } else {
                                uint8_t *tags = tag_data_for_id (relation_id, RELATION);
                                pbf_write_relation (relation_id, 
                                    &(rel_members[rel.member_offset]), &(tags[rel.tags])
                                );
                            }
                            /* Within a tile, relations are linked into a list. */
                            relation_id = rel.next; 
                        }
                        /* All the remaining code in the y loop body relates only to WAY and NODE stages. */
                        continue; 
                    }
                    /* The following code handles NODE and WAY stages if RELATION clause was not entered.
                    Iterate over all ways in this block, then repeat for any chained blocks.
                    If there are no ways in this grid cell, the head way block index will be zero. */
                    // TODO factor this and the above relation output code out into functions
                    uint32_t way_block_index = grid->cells[x][y].head_way_block;
                    for (WayBlock *way_block = NULL; way_block_index > 0; way_block_index = way_block->next) {
                        way_block = &(way_blocks[way_block_index]);
                        for (int w = 0; w < WAY_BLOCK_SIZE; w++) {
                            int64_t way_id = way_block->refs[w];
                            /* Empty slots in the way block will be either negative or zero. */
                            if (way_id <= 0) break;
                            Way way = ways[way_id];
                            if (stage == WAY) {
                                if (vexformat) {
                                    vexbin_write_way (way_id);
                                } else {
                                    uint8_t *tags = tag_data_for_id(way_id, WAY);
                                    pbf_write_way(way_id, &(node_refs[way.node_ref_offset]), &(tags[way.tags]));
                                }
                            } else if (stage == NODE) {
                                /* Output all nodes in this way. */
                                uint32_t nr = way.node_ref_offset;
                                for (bool more = true; more; nr++) {
                                    int64_t node_id = node_refs[nr];
                                    if (node_id < 0) {
                                        node_id = -node_id;
                                        more = false;
                                    }
                                    // print_node (node_id); // DEBUG
                                    /* Mark this node, and skip outputting it if already seen. */
                                    if (IDTracker_set (node_id)) continue;
                                    if (vexformat) {
                                        vexbin_write_node (node_id);
                                    } else {
                                        Node node = nodes[node_id];
                                        uint8_t *tags = tag_data_for_id(node_id, NODE);
                                        pbf_write_node(node_id, get_lat(&(node.coord)),
                                            get_lon(&(node.coord)), &(tags[node.tags]));
                                    }
                                }
                            }
                        }
                    }
                }
            }
            /* Write out any buffered nodes or ways before beginning the next PBF writing stage. */
            if (!vexformat) pbf_write_flush();
        }
        fclose(output_file);
        /* Release the shared lock, allowing writes to begin. */
        flock(lock_fd, LOCK_UN); 
    }

}

// TODO simple network protocol for fetching PBF.
