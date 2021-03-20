/* vex.h project-wide definitions */
#ifndef VEX_H_INCLUDED
#define VEX_H_INCLUDED

/*
  Define the sequence in which elements are read and written,
  while allowing element types as function parameters and array indexes.
*/
#define NODE 0
#define WAY 1
#define RELATION 2

/* Compact geographic position. Latitude and longitude mapped to the signed 32-bit int range. */
typedef struct {
    int32_t x;
    int32_t y;
} coord_t;

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

#endif /* VEX_H_INCLUDED */
