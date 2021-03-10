/* 
   Slab or "arena" allocation. 
   Allocate one large chunk, and perform small sub-allocations by simply bumping a pointer.
   All sub-allocations are freed at once by simply moving the pointer back to the beginning of the slab.
   This works well in a loop where the number of allocations is bounded and they all go out of scope at once.
   Anecdotally gives a ~10% speedup on Protobuf decoding.
*/

#include <stdlib.h>
#include "pbf.h"

// Allocate 8MB once and slice it up.
#define SLAB_SIZE (8 * 1024 * 1024)

static void *slab;
static void *next;
static void *limit;

void slab_init () {
    slab = malloc(SLAB_SIZE);
	limit = slab + SLAB_SIZE;
    next = slab;
}

void slab_reset () {
    // Bulk free of all allocations.
    next = slab;
}   
 
void slab_done () {
	// Clean up be freeing the slab itself.
    free(slab);
}

static void *slab_alloc (void *allocator_data, size_t size) {
	if (next >= limit) {
		return NULL;
	}
    void *ret = next;
    next += size;
    return ret;
}

static void slab_free (void *allocator_data, void *pointer) {
    // Do nothing. All allocations will be freed at once.
}

ProtobufCAllocator slabAllocator = {
    .alloc = &slab_alloc, 
    .free = &slab_free, 
    .allocator_data = NULL
};

