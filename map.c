#include "map.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

struct map {
    int size;
    // TODO track fill rate
    // TODO make this an array of element pointers rather than an array of elements.
    struct element *elements;
};

struct element {
    KEY_T key;
    VAL_T val;
    struct element *next;
};

static void free_list (struct element *e) {
    while (e != NULL) {
        struct element *next_e = e->next;
        free (e);
        e = next_e;
    }
}

/**** BUG HERE IN ORIGINAL RRRR code lists are assumed to be NULL. ****/

/* Initialize an IntSet for the first time, or after freeing its element lists. */
static void Map_init (Map *map) {
    for (int i = 0; i < map->size; ++i) {
        map->elements[i].key = KEY_NONE;
        map->elements[i].val = VAL_NONE;
        map->elements[i].next = NULL;
    }
}

/* 
  Free the lists in an already-initialized IntSet. This renders it invalid, 
  re-initialize it afterward. 
*/
void Map_free_lists (Map *map) {
    for (int i = 0; i < map->size; ++i) {
        if (map->elements[i].next != NULL) free_list (map->elements[i].next);
    }
}

Map *Map_new (int size) {
    Map *map = malloc (sizeof (Map));
    map->size = size;
    map->elements = malloc (sizeof (struct element) * size);
    Map_init (map);
    return map;
}

void Map_destroy (Map **map) {
    Map_free_lists (*map);
    free ((*map)->elements);
    free (*map);
    *map = NULL;
}

void Map_print (Map *map) {
    for (int i = 0; i < map->size; ++i) {
        printf ("[%02d] ", i);
        struct element *e = &(map->elements[i]);
        while (e != NULL) {
            if (e->key == KEY_NONE) printf ("NONE ");
            else printf ("(%ld %d) ", e->key, e->val);
            e = e->next;
        }
        printf ("\n");
    }
}

// Hash function from Robert Jenkins
// via Thomas Wang at https://gist.github.com/badboy/6267743
/*
static inline uint32_t hash_code_jenkins (uint32_t a) { 
    a = (a+0x7ed55d16) + (a<<12);
    a = (a^0xc761c23c) ^ (a>>19);
    a = (a+0x165667b1) + (a<<5);
    a = (a+0xd3a2646c) ^ (a<<9);
    a = (a+0xfd7046c5) + (a<<3);
    a = (a^0xb55a4f09) ^ (a>>16);
    return a;
}
*/

// FNV1a hash function
// http://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
#define FNV32_OFFSET_BASIS 0x811C9DC5
#define FNV32_PRIME 0x01000193
static inline uint32_t hash_code (uint32_t a) { 
    uint32_t hash = FNV32_OFFSET_BASIS;
    const unsigned char* c = (const unsigned char*) &a;
    // consume four bytes
    hash ^= *(c++);
    hash *= FNV32_PRIME;
    hash ^= *(c++);
    hash *= FNV32_PRIME;
    hash ^= *(c++);
    hash *= FNV32_PRIME;
    hash ^= *(c++);
    hash *= FNV32_PRIME;
    return hash;
}

bool Map_contains_key (Map *map, KEY_T key) {
    uint32_t hash = hash_code(key) % map->size;
    struct element *e = &(map->elements[hash]);
    while (e != NULL) {
        if (e->key == key) return true;
        e = e->next;
    }
    return false;
}

VAL_T Map_get (Map *map, KEY_T key) {
    uint32_t hash = hash_code(key) % map->size;
    struct element *e = &(map->elements[hash]);
    while (e != NULL) {
        if (e->key == key) return e->val;
        e = e->next;
    }
    return VAL_NONE;
}

void Map_put (Map *map, KEY_T key, VAL_T val) {
    uint32_t hash = hash_code(key) % map->size;
    struct element *e = &(map->elements[hash]);
    if (e->key != KEY_NONE) {
        while (true) {
            if (e->key == key) {
                // key already in set, overwrite val
                e->val = val;
                return;
            }
            if (e->next == NULL) break;
            e = e->next;
        }
        e->next = malloc (sizeof (struct element));
        e = e->next;
    }
    e->key = key;
    e->val = val;
    e->next = NULL;
}

