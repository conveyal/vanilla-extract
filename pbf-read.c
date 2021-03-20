/* pbf-read.c */
#include "pbf.h"
#include "fileformat.pb-c.h"
#include "osmformat.pb-c.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include "zlib.h"
#include "slab.h"
#include "util.h"

// sudo apt-get install protobuf-c-compiler libprotobuf-c0-dev zlib1g-dev
// then compile the protobuf with:
// protoc-c --c_out . ./osmformat.proto
// protoc-c --c_out . ./fileformat.proto

// The osmpbf-dev debian package (https://github.com/scrosby/OSM-binary) is for C++ but provides
// the protobuf definition files.

/* Private PBF reading state variables. */
static uint32_t curr_block;   // Current block number being read in input PBF
static void *curr_block_pos;  // Pointer to first byte in the current block
static int curr_phase;        // The element type at the current position in the file
static void *curr_pos;        // Pointer to the next byte to be read in the input PBF

static uint32_t mark_block;   // Block to which we can rewind to commence slow_seek
static void *mark_block_pos;  // Pointer to the first byte in the marked block
static int mark_phase;        // Element type of the first primitive group in the marked block

/* The memory-mapped input file and its size. */
static void *map;
static size_t map_size;

/* Memory-map the input file before reading it. */
static void pbf_map(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        die("Could not find input file.");
    }
    struct stat st;
    if (stat(filename, &st) == -1) {
        die("Could not stat input file.");
    }
    map = mmap((void*)0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    map_size = st.st_size;
    if (map == (void*)(-1)) {
        die("Could not map input file.");
    }
}

/* Release the memory map of the input file when finished reading it. */
static void pbf_unmap() {
    // TODO check success like when mapping
    if (munmap(map, map_size)) {
        die("Failed to unmap input file.");
    }
    map = NULL;
}

// OSMPBF spec says: "The uncompressed length of a Blob *should* be less than 16 MiB and *must* be less than 32 MiB."
#define MAX_BLOB_SIZE_UNCOMPRESSED 32 * 1024 * 1024
// Reuse this single large buffer for every block.
static unsigned char zbuf[MAX_BLOB_SIZE_UNCOMPRESSED];

/* Return the size of the inflated data. */
// ZLIB has utility (un)compress functions that work on buffers. Maybe use those instead of hand-rolling.
static int zinflate(ProtobufCBinaryData *in, unsigned char *out) {
    /* Initialize inflate state. */
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    int ret = inflateInit(&strm);
    if (ret != Z_OK) {
        die("zlib init failed.");
    }
    /* Our input ProtobufCBinaryData is {size_t len; uint8_t *data}. */
    strm.avail_in = in->len;
    strm.next_in = in->data;
    strm.avail_out = MAX_BLOB_SIZE_UNCOMPRESSED;
    strm.next_out = out;
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_STREAM_END) {
        die("zlib inflate failed to reach end of stream.");
    }
    ret = inflateEnd(&strm);
    if (ret != Z_OK) {
        die("zlib inflateEnd failed.");
    }
    return MAX_BLOB_SIZE_UNCOMPRESSED - strm.avail_out;
}

/* Given an already unpacked primitive group, determine what element type it contains and check some invariants. */
static int detect_element_type (OSMPBF__PrimitiveGroup *group) {
    int n_element_types = 0;
    int element_type = -1;
    if (group->dense || group->n_nodes > 0) {
        n_element_types += 1;
        element_type = NODE;
    }
    if (group->n_ways > 0) {
        n_element_types += 1;
        element_type = WAY;
    }
    if (group->n_relations > 0) {
        n_element_types += 1;
        element_type = RELATION;
    }
    if (n_element_types != 1) {
        die("Each primitive group should contain exactly one element type (nodes, ways, or relations).");
    }
    return element_type;
}

// Make callbacks available deep in call stack. Access/pass by value not reference to encourage optimizations.
static PbfReadCallbacks callbacks;

static bool no_callback_for_phase () {
    return (curr_phase == NODE && !(callbacks.node))
           || (curr_phase == WAY && !(callbacks.way))
           || (curr_phase == RELATION && !(callbacks.relation));
}

static bool no_more_callbacks () {
    if (curr_phase == NODE &&
        callbacks.node == NULL && callbacks.way == NULL && callbacks.relation == NULL) {
        fprintf (stderr, "Skipping the rest of the PBF file, no callbacks were defined.\n");
        return true;
    }
    if (curr_phase == WAY && callbacks.way == NULL && callbacks.relation == NULL) {
        fprintf (stderr, "Skipping the rest of the PBF file, only a way callback was defined.\n");
        return true;
    }
    if (curr_phase == RELATION && callbacks.relation == NULL) {
        fprintf (stderr, "Skipping the rest of the PBF file, no relation callback is defined.\n");
        return true;
    }
    return false;
}

/* Enforce (node, way, relation) ordering, and return true when we pass from one phase to the next. */
// rename to detect_phase_transition?
static bool enforce_ordering (OSMPBF__PrimitiveGroup *group) {
    int element_type = detect_element_type(group);
    if (element_type < curr_phase) {
        die ("PBF blocks did not follow the order nodes, ways, relations.");
    }
    // Move into a new phase if we detected an element type that should come after the current phase.
    if (element_type > curr_phase) {
        curr_phase = element_type;
        fprintf(stderr, "Entered phase %d\n", curr_phase);
        return true;
    } else {
        return false;
    }
}

// If true, skip over many blocks at a time to seek to other element types.
// We only need to support the transition reading -> fast_forward -> slow_seek, not returning to fast_forward.
// With only three sections to the file and the ability bail out when no callbacks are defined for the rest of the file,
// any fast-forwarded sections are always contiguous.
static bool fast_forward;
static bool slow_seek;

/* Rewind to last known block of previous type and process at normal speed. */
static void pbf_rewind () {
    if (fast_forward) {
        fprintf(stderr, "Rewinding to block number %dk.\n", mark_block/1000);
        fast_forward = false;
        slow_seek = true;
        curr_block = mark_block;
        curr_pos = curr_block_pos = mark_block_pos;
        curr_phase = mark_phase;
    } else {
        // Valid rewind markers are set when we enter fast forward mode, avoid infinite loops or dangling pointers.
        die("Rewind occurred when not in fast forward mode.");
    }
}

/* Tags are stored in a string table at the PrimitiveBlock level. */
// TODO multiple return codes for CONTINUE, REWIND, DONE?
#define MAX_TAGS 256
static bool handle_primitive_block (OSMPBF__PrimitiveBlock *block) {
    ProtobufCBinaryData *string_table = block->stringtable->s;
    int32_t granularity = block->has_granularity ? block->granularity : 100;
    int64_t lat_offset = block->has_lat_offset ? block->lat_offset : 0;
    int64_t lon_offset = block->has_lon_offset ? block->lon_offset : 0;
    // fprintf(stderr, "pblock with granularity %d and offsets %d, %d\n", granularity, lat_offset, lon_offset);
    // All blocks appear to contain only one group.
    if (block->n_primitivegroup != 1) {
        printf("Unusual number of primitive groups: %zu\n", block->n_primitivegroup);
    }
    for (int g = 0; g < block->n_primitivegroup; ++g) {
        OSMPBF__PrimitiveGroup *group = block->primitivegroup[g];
        if (enforce_ordering (group)) {
            // Transition has occurred from one element type to another.
            if (fast_forward) {
                pbf_rewind();
                return false; // Do not end processing, continue iteration over blocks.
            }
            if (no_more_callbacks()) {
                return true; // End file processing early, no callbacks are defined for the rest of the file.
            }
        }
        if (no_callback_for_phase()) {
            if (!slow_seek) {
                if (!fast_forward) {
                    fprintf(stderr, "Fast forwarding...\n");
                    fast_forward = true;
                }
                mark_block = curr_block;
                mark_block_pos = curr_block_pos;
                mark_phase = curr_phase;
            }
            return false; // Do not process this block, but do not end processing, continue iteration over blocks.
        }
        // Apply callback handlers to OSM entities present in this primitive group.
        // fprintf(stderr, "pgroup with %d nodes, %d dense nodes, %d ways, %d relations\n",  group->n_nodes,
        //     group->dense ? group->dense->n_id : 0, group->n_ways, group->n_relations);
        if (callbacks.way) {
            for (int w = 0; w < group->n_ways; ++w) {
                OSMPBF__Way *way = group->ways[w];
                (*(callbacks.way))(way, string_table);
            }
        }
        if (callbacks.node) {
            for (int n = 0; n < group->n_nodes; ++n) {
                OSMPBF__Node *node = group->nodes[n];
                node->lat = lat_offset + (node->lat * granularity);
                node->lon = lon_offset + (node->lon * granularity);
                (*(callbacks.node))(node, string_table);
            }
            if (group->dense) {
                OSMPBF__DenseNodes *dense = group->dense;
                OSMPBF__Node node; // struct reused to carry the data from each dense node
                uint32_t keys[MAX_TAGS]; // keys and vals reused for string table references
                uint32_t vals[MAX_TAGS];
                node.keys = keys;
                node.vals = vals;
                node.n_keys = 0;
                node.n_vals = 0;
                int kv0 = 0; // index into source keys_values array (concatenated, 0-len separated)
                int64_t id  = 0;
                // lat and lon are passed into node callback function in nanodegrees.
                // offsets are also in nanodegrees.
                int64_t lat = lat_offset;
                int64_t lon = lon_offset;
                for (int n = 0; n < dense->n_id; ++n) {
                    // Coordinates and IDs are delta coded
                    id  += dense->id[n];
                    lat += dense->lat[n] * granularity;
                    lon += dense->lon[n] * granularity;
                    node.id  = id;
                    node.lat = lat;
                    node.lon = lon;
                    // Copy tag string indexes over from concatenated alternating array
                    int kv1 = 0; // index into target keys and values array
                    // some blocks have no tags at all, check that the array pointer is not null
                    if (dense->keys_vals != NULL) {
                        // key-val list for each node is terminated with a zero-length string
                        while (string_table[dense->keys_vals[kv0]].len > 0) {
                            if (kv1 < MAX_TAGS) { // target buffers are reused and fixed-length
                                keys[kv1] = dense->keys_vals[kv0++];
                                vals[kv1] = dense->keys_vals[kv0++];
                                kv1++;
                            } else {
                                kv0 += 2; // skip both key and value
                                fprintf (stderr, "Skipping tags after number %d.\n", MAX_TAGS);
                            }
                        }
                    }
                    node.n_keys = kv1;
                    node.n_vals = kv1;
                    kv0++; // skip zero length string indicating end of k-v pairs for this node
                    // TODO should that be validated?
                    (*(callbacks.node))(&node, string_table);
                }
            }
        }
        if (callbacks.relation) {
            for (int r = 0; r < group->n_relations; ++r) {
                OSMPBF__Relation *relation = group->relations[r];
                (*(callbacks.relation))(relation, string_table);
            }
        }
    }
    return false; // signal not to break iteration, loading should continue
}

// Once we've already unpacked a blob message, we may need to zlib-expand its contents before further processing.
// Return true when we are done reading because no more callbacks apply to the rest of the file.
// We now know the starting position of the next block, so only need to decode this block when callbacks apply.
// If in fast-forward mode, skip decompression and decoding of most blocks.
static bool process_pbf_blob (OSMPBF__BlobHeader *blobh, OSMPBF__Blob *blob) {
    if (fast_forward && curr_block % 1000 != 0) {
        return false;
    }
    uint8_t* bdata;
    size_t bsize;
    /* Check if the blob is raw or compressed. */
    // Unfortunately the information about which OSM element types are present in a blob of type OSMData
    // is hidden inside the compressed data. To seek forward, rather than decompressing the whole thing
    // we can probe only the header.
    if (blob->has_zlib_data) {
        bdata = zbuf;
        bsize = blob->raw_size;
        int inflated_size = zinflate(&(blob->zlib_data), zbuf);
        if (inflated_size != bsize) {
            die("Inflated blob size does not match expected size.");
        }
    } else if (blob->has_raw) {
        fprintf(stderr, "Uncompressed blob, this is unusual.\n");
        bdata = blob->raw.data;
        bsize = blob->raw.len;
    } else {
        die("Neither compressed nor raw data was present in this blob.");
    }
    bool done_reading = false;
    if (curr_block == 0) {
        /* Get header block from first blob. */
        if (strcmp(blobh->type, "OSMHeader") != 0) {
            die("Expected the first blob to contain a header, but it did not.");
        }
        // Note that the header block is allocated from the slab, so it will not survive across iterations.
        // We can't use it after this point, just validate it and trash it.
        // If we need to save it we can switch allocator to default malloc with NULL.
        OSMPBF__HeaderBlock *header = osmpbf__header_block__unpack(&slabAllocator, bsize, bdata);
        if (header == NULL) {
            die("Failed to read OSM header message from header blob.");
        }
        // TODO enforce PBF features and granularity found in header
        osmpbf__header_block__free_unpacked(header, &slabAllocator);
    } else {
        /* Get an OSM primitive block from all subsequent blobs. */
        if (strcmp(blobh->type, "OSMData") != 0) {
            die("Unrecognized blob type.");
        }
        OSMPBF__PrimitiveBlock *block = osmpbf__primitive_block__unpack(&slabAllocator, bsize, bdata);
        if (block == NULL) {
            die("Error unpacking primitive block.");
        }
        // Note: Good place to return the primitive block from this function for reuse without nesting function calls.
        done_reading = handle_primitive_block(block);
        osmpbf__primitive_block__free_unpacked(block, &slabAllocator);
    }
    return done_reading;
}

// TODO check true upper limit of slab size. A whole primitive block with many nodes is unpacked into this slab.
#define SLAB_SIZE (8 * 1024 * 1024)

// Only use fast-forward behavior on files larger than this.
#define FFWD_MIN_BYTES (100 * 1024 * 1024)

/* Externally visible function to read PBF with arbitrary entity handler functions. */
void pbf_read (const char *filename, PbfReadCallbacks pbfReadCallbacks) {
    callbacks = pbfReadCallbacks;
    pbf_map(filename);
    slab_init(SLAB_SIZE);
    // Initialize iteration over file blocks starting at the beginning of the mapped file.
    {
        curr_pos = map;
        curr_block = mark_block = 0;
        curr_phase = -1;
        curr_block_pos = mark_block_pos = NULL;
        fast_forward = false;
        slow_seek = false;
    }
    for (;;) {
        if (curr_block % 1000 == 0) {
            fprintf(stderr, "Reading PBF blob %dk (position %ldMB)\n", curr_block/1000, (curr_pos - map)/1024/1024);
        }
        // Retain start position of current block for use in other functions (marking fast-forward start position).
        curr_block_pos = curr_pos;
        /* Read blob header, prefixed with 4 bytes containing its message length in network (big-endian) order. */
        uint32_t msg_length = ntohl(*((uint32_t *)curr_pos));
        curr_pos += sizeof(uint32_t);
        OSMPBF__BlobHeader *blobh = osmpbf__blob_header__unpack(&slabAllocator, msg_length, curr_pos);
        curr_pos += msg_length;
        if (blobh == NULL) {
            die("Error unpacking blob header.");
        }
        /* Read blob data itself, without decompressing. */
        OSMPBF__Blob *blob = osmpbf__blob__unpack(&slabAllocator, blobh->datasize, curr_pos);
        if (blobh == NULL) {
            die("Error unpacking blob data.");
        }
        curr_pos += blobh->datasize;
        bool done_reading = process_pbf_blob(blobh, blob);
        /* Deallocate block and its header, and reset slab allocator for the next iteration. */
        // With the slab allocator these are probably not necessary, but calling them in case it uses malloc anywhere.
        osmpbf__blob_header__free_unpacked(blobh, &slabAllocator);
        osmpbf__blob__free_unpacked(blob, &slabAllocator);
        slab_reset();
        curr_block++;
        if (done_reading) break;
        if (curr_pos >= (map + map_size)) {
            fprintf(stderr, "Reached end of input PBF file.\n");
            if (fast_forward) {
                // We skipped completely over the section of the input file where callbacks apply, so rewind.
                // Note it's important to do this _after_ the block number increment, so it's overwritten.
                pbf_rewind();
            } else break;
        }
    }
    slab_done();
    pbf_unmap();
}

//// EXAMPLE USAGE ////

/* Example way callback that just counts node references. */
static long noderefs = 0;
static void handle_way(OSMPBF__Way *way, ProtobufCBinaryData *string_table) {
    noderefs += way->n_refs;
}

/* Example node callback that just counts nodes. */
static long nodecount = 0;
static void handle_node(OSMPBF__Node *node, ProtobufCBinaryData *string_table) {
    ++nodecount;
}

/* Example main function that calls this PBF reader. */
int test_main (int argc, const char * argv[]) {
    if (argc < 2) die("usage: pbf input.pbf");
    const char *filename = argv[1];
    PbfReadCallbacks callbacks = {
        .way  = &handle_way,
        .node = &handle_node,
        .relation = NULL
    };
    pbf_read(filename, callbacks);
    fprintf(stderr, "total node references %ld\n", noderefs);
    fprintf(stderr, "total nodes %ld\n", nodecount);
    return EXIT_SUCCESS;
}

