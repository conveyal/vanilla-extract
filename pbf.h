#ifndef PBF_H_INCLUDED
#define PBF_H_INCLUDED

#include "fileformat.pb-c.h"
#include "osmformat.pb-c.h"
#include <stdio.h> // for FILE

/* This bundles together callback functions for reading the three main OSM element types. */
typedef struct {
    void (*way)      (OSMPBF__Way*,      ProtobufCBinaryData *string_table);
    void (*node)     (OSMPBF__Node*,     ProtobufCBinaryData *string_table);
    void (*relation) (OSMPBF__Relation*, ProtobufCBinaryData *string_table);
} PbfReadCallbacks;

/* This bundles together callback functions for writing the three main OSM element types. (incomplete) */
typedef struct {
    void (*way)      ();
    void (*node)     ();
    void (*relation) ();
} OsmWriteCallbacks;

/* READ */
void pbf_read(const char *filename, PbfReadCallbacks *callbacks);

/* WRITE */
void pbf_write_begin(FILE *out);
void pbf_write_way(uint64_t way_id, int64_t *refs, uint8_t *coded_tags);
void pbf_write_node(uint64_t node_id, double lat, double lon, uint8_t *coded_tags);
void pbf_write_relation(uint64_t relation_id, int64_t *refs, uint8_t *coded_tags);
void pbf_write_flush();

#endif /* PBF_H_INCLUDED */
