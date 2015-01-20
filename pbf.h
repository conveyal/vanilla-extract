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

/* 
  A single member of a relation. We lose some information so that these can be fixed-width. 
  Specifically, only the 255 most common roles are supported.
  FIXME this is the only internal OSM storage type that is in the read/write header, which breaks 
  library encapsulation. Perhaps the write code should not be kept separate from vex itself, and 
  all vex internal storage arrays should be in extern declarations in the header, which would 
  simplify pbf write function call signatures. 
*/
typedef struct {
    uint8_t role; // 1-255 are the 255 most common roles, 0 for all others
    uint8_t element_type; // NODE, WAY, or RELATION
    int64_t id; // the id of the node or way being referenced, last ID in the list is negative
} RelMember;

/* PUBLIC READ FUNCTIONS */
void pbf_read(const char *filename, PbfReadCallbacks *callbacks);

/* PUBLIC WRITE FUNCTIONS */
void pbf_write_begin(FILE *out);
void pbf_write_way(int64_t way_id, int64_t *refs, uint8_t *coded_tags);
void pbf_write_node(int64_t node_id, double lat, double lon, uint8_t *coded_tags);
void pbf_write_relation(int64_t relation_id, RelMember *members, uint8_t *coded_tags);
void pbf_write_flush();

#endif /* PBF_H_INCLUDED */
