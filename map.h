/* map.h : a hashtable. only does dynamic allocation on collisions. */

#include <stdbool.h>
#include <stdint.h>

#define KEY_T uint64_t
#define VAL_T uint32_t
#define KEY_NONE UINT64_MAX
#define VAL_NONE UINT32_MAX

// TODO allow user-specified per-Map default return value

typedef struct map Map;

void Map_clear (Map *map);

Map *Map_new (int size);

void Map_destroy (Map **mam);

void Map_put (Map *map, KEY_T key, VAL_T val);

bool Map_contains_key (Map *map, KEY_T key);

VAL_T Map_get (Map *map, KEY_T key);

void Map_print (Map *map);
