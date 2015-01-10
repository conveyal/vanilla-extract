/* pbf.c */
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

// sudo apt-get install protobuf-c-compiler libprotobuf-c0-dev zlib1g-dev
// then compile the protobuf with:
// protoc-c --c_out . ./osmformat.proto
// protoc-c --c_out . ./fileformat.proto

// The osmpbf-dev debian package (https://github.com/scrosby/OSM-binary) is for C++ but provides
// the protobuf definition files.

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

/* Sequential states used to enforce ordering of elements in a PBF and bail out early if possible. */
#define PHASE_NODE 0
#define PHASE_WAY 1
#define PHASE_RELATION 2
static int phase;

static void *map;
static size_t map_size;

static void pbf_map(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1)
        die("could not find input file");
    struct stat st;
    if (stat(filename, &st) == -1)
        die("could not stat input file");
    map = mmap((void*)0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    map_size = st.st_size;
    if (map == (void*)(-1))
        die("could not map input file");
}

static void pbf_unmap() {
    munmap(map, map_size);
}

// "The uncompressed length of a Blob *should* be less than 16 MiB (16*1024*1024 bytes)
// and *must* be less than 32 MiB."
#define MAX_BLOB_SIZE_UNCOMPRESSED 32 * 1024 * 1024
static unsigned char zbuf[MAX_BLOB_SIZE_UNCOMPRESSED];

// ZLIB has utility (un)compress functions that work on buffers.
static int zinflate(ProtobufCBinaryData *in, unsigned char *out) {
    int ret;

    /* initialize inflate state */
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);
    if (ret != Z_OK)
        die("zlib init failed");

    /* ProtobufCBinaryData is {size_t len; uint8_t *data} */
    strm.avail_in = in->len;
    strm.next_in = in->data;
    strm.avail_out = MAX_BLOB_SIZE_UNCOMPRESSED;
    strm.next_out = out;

    ret = inflate(&strm, Z_NO_FLUSH);
    // check ret
    (void)inflateEnd(&strm);
    return MAX_BLOB_SIZE_UNCOMPRESSED - strm.avail_out;

}

/* 
  Enforce (node, way, relation) ordering, and bail out early when possible. 
  Returns true if loading should terminate due to incorrect ordering or just to save time.
*/
static bool enforce_ordering (OSMPBF__PrimitiveGroup *group, PbfReadCallbacks *callbacks) {
    int n_element_types = 0;
    int element_type = -1;
    if (group->dense || group->n_nodes > 0) {
        n_element_types += 1;
        element_type = PHASE_NODE;
    }
    if (group->n_ways > 0) {
        n_element_types += 1;
        element_type = PHASE_WAY;
    }
    if (group->n_relations > 0) {
        n_element_types += 1;
        element_type = PHASE_RELATION;
    }
    if (n_element_types > 1) {
        fprintf (stderr, "ERROR: Block should contain only one element type (nodes, ways, or relations).\n");
        return true;
    }
    if (element_type < phase) {
        fprintf (stderr, "ERROR: PBF blocks did not follow the order nodes, ways, relations.\n");
        return true;
    }
    if (element_type > phase) {
        phase = element_type;
        if (phase == PHASE_NODE && 
            callbacks->node == NULL && callbacks->way == NULL && callbacks->relation == NULL) {
            fprintf (stderr, "Skipping the rest of the PBF file, no callbacks were defined.\n");
            return true;
        } 
        if (phase == PHASE_WAY && callbacks->way == NULL && callbacks->relation == NULL) {
            fprintf (stderr, "Skipping the rest of the PBF file, only a way callback was defined.\n");
            return true;
        } 
        if (phase == PHASE_RELATION && callbacks->relation == NULL) {
            fprintf (stderr, "Skipping the end of the PBF file, no relation callback is defined.\n");
            return true;
        } 
    }
    /* Phase stayed the same or advanced without triggering an early exit. */
    return false;
}

/* Tags are stored in a string table at the PrimitiveBlock level. */
#define MAX_TAGS 256
static bool handle_primitive_block(OSMPBF__PrimitiveBlock *block, PbfReadCallbacks *callbacks) {
    ProtobufCBinaryData *string_table = block->stringtable->s;
    int32_t granularity = block->has_granularity ? block->granularity : 100;
    int64_t lat_offset = block->has_lat_offset ? block->lat_offset : 0;
    int64_t lon_offset = block->has_lon_offset ? block->lon_offset : 0;
    // fprintf(stderr, "pblock with granularity %d and offsets %d, %d\n", granularity, lat_offset, lon_offset);
    // It seems like a block often contains only one group.
    for (int g = 0; g < block->n_primitivegroup; ++g) {
        OSMPBF__PrimitiveGroup *group = block->primitivegroup[g];
        if (enforce_ordering (group, callbacks)) {
            return true; // signal early exit due to improper ordering or callbacks were exhausted
        }
        // fprintf(stderr, "pgroup with %d nodes, %d dense nodes, %d ways, %d relations\n",  group->n_nodes,
        //     group->dense ? group->dense->n_id : 0, group->n_ways, group->n_relations);
        if (callbacks->way) {
            for (int w = 0; w < group->n_ways; ++w) {
                OSMPBF__Way *way = group->ways[w];
                (*(callbacks->way))(way, string_table);
            }
        }
        if (callbacks->node) {
            for (int n = 0; n < group->n_nodes; ++n) {
                OSMPBF__Node *node = group->nodes[n];
                node->lat = lat_offset + (node->lat * granularity);
                node->lon = lon_offset + (node->lon * granularity);
                (*(callbacks->node))(node, string_table);
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
                                fprintf (stderr, "skipping tags after number %d.\n", MAX_TAGS);
                            }
                        }
                    }
                    node.n_keys = kv1;
                    node.n_vals = kv1;
                    kv0++; // skip zero length string indicating end of k-v pairs for this node
                    (*(callbacks->node))(&node, string_table);
                }
            }
        }
        if (callbacks->relation) {
            for (int r = 0; r < group->n_relations; ++r) {
                OSMPBF__Relation *relation = group->relations[r];
                (*(callbacks->relation))(relation, string_table);
            }
        }
    }
    return false; // signal not to break iteration, loading should continue
}

// TODO break out open, read, and close into separate functions
// TODO store PBF file offsets to each entity type when they are found, allowing repeated calls
// TODO pbf_read_nodes, pbf_read_ways, pbf_read_relations convenience functions

/* Externally visible function. */
void pbf_read (const char *filename, PbfReadCallbacks *callbacks) {
    pbf_map(filename);
    OSMPBF__HeaderBlock *header = NULL;
    int blobcount = 0;
    phase = PHASE_NODE;
    bool break_iteration = false;
    for (void *buf = map; buf < map + map_size; ++blobcount) {
        if (blobcount % 1000 == 0) {
            fprintf(stderr, "Loading PBF blob %dk (position %ldMB)\n", blobcount/1000, (buf - map) / 1024 / 1024);
        }
        /* read blob header */
        OSMPBF__BlobHeader *blobh;
        // header prefixed with 4-byte contain network (big-endian) order message length
        int32_t msg_length = ntohl(*((int*)buf)); // TODO shouldn't this be an exact-width type cast?
        buf += sizeof(int32_t);
        blobh = osmpbf__blob_header__unpack(NULL, msg_length, buf);
        buf += msg_length;
        if (blobh == NULL)
            die("error unpacking blob header");

        /* read blob data */
        OSMPBF__Blob *blob;
        blob = osmpbf__blob__unpack(NULL, blobh->datasize, buf);
        buf += blobh->datasize;
        if (blobh == NULL)
            die("error unpacking blob data");

        /* check if the blob is raw or compressed */
        uint8_t* bdata;
        size_t bsize;
        if (blob->has_zlib_data) {
            bdata = zbuf;
            bsize = blob->raw_size;
            int inflated_size = zinflate(&(blob->zlib_data), zbuf);
            if (inflated_size != bsize)
                die("inflated blob size does not match expected size");
        } else if (blob->has_raw) {
            fprintf(stderr, "uncompressed\n");
            bdata = blob->raw.data;
            bsize = blob->raw.len;
        } else
            die("neither compressed nor raw data present in blob");

        /* get header block from first blob */
        if (header == NULL) {
            if (strcmp(blobh->type, "OSMHeader") != 0)
                die("expected first blob to be a header");
            header = osmpbf__header_block__unpack(NULL, bsize, bdata);
            if (header == NULL)
                die("failed to read OSM header message from header blob");
            goto free_blob;
        }

        /* get an OSM primitive block from subsequent blobs */
        if (strcmp(blobh->type, "OSMData") != 0) {
            fprintf(stderr, "skipping unrecognized blob type\n");
            goto free_blob;
        }
        
        OSMPBF__PrimitiveBlock *block;
        block = osmpbf__primitive_block__unpack(NULL, bsize, bdata);
        if (block == NULL)
            die("error unpacking primitive block");
        break_iteration = handle_primitive_block(block, callbacks);
        osmpbf__primitive_block__free_unpacked(block, NULL);

        /* post-iteration cleanup */
        free_blob:
        osmpbf__blob_header__free_unpacked(blobh, NULL);
        osmpbf__blob__free_unpacked(blob, NULL);
        if (break_iteration) break;
    }
    osmpbf__header_block__free_unpacked(header, NULL);
    pbf_unmap();
}

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
    pbf_read(filename, &callbacks);
    fprintf(stderr, "total node references %ld\n", noderefs);
    fprintf(stderr, "total nodes %ld\n", nodecount);
    return EXIT_SUCCESS;
}
