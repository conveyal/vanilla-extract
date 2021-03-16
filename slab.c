/* 
   Slab or "arena" allocation.
   Allocate one large chunk of memory, then perform small sub-allocations by simply bumping a pointer.
   All sub-allocations are freed at once by simply moving the pointer back to the beginning of the slab.
   Besides avoiding the bookkeeping of many small heap allocations, this may improve locality of reference.
   This works well in a loop where the number of allocations is bounded and they all go out of scope at once.
   Anecdotally gives a ~10% speedup on Protobuf decoding.
*/

#include <stdlib.h>
#include <stdio.h>
#include "pbf.h"

static void *slab;   // The starting address of the slab
static void *next;   // The address of the next unused byte
static void *limit;  // The address of the first byte outside the slab (for range checking)

//////// Externally visible functions ////////

void slab_init (size_t slab_size) {
    if (slab != NULL) {
        fprintf(stderr, "Slab already initialized (this is a bug).\n");
        exit(EXIT_FAILURE);
    }
    slab = malloc(slab_size);
	limit = slab + slab_size;
    next = slab;
}

// Bulk free of all allocations.
void slab_reset () {
    next = slab;
}   
 
// Clean up by freeing the slab itself.
void slab_done () {
    free(slab);
}

//////// Functions not visible outside this compilation unit. ////////
// Exposed as function pointers in the ProtobufCAllocator struct below.

// For allocation, return the next free byte, bumping the next free byte pointer in preparation for the next allocation.
static void *slab_alloc (void *allocator_data, size_t size) {
	if (next >= limit) {
		return NULL;
	}
    void *ret = next;
    next += size;
    return ret;
}

// For deallocation, do nothing. All allocations will be freed at once at the end of a loop or method.
static void slab_free (void *allocator_data, void *pointer) { }

// ProtobufCAllocator declared extern in the header, to be supplied to standard functions when decoding protobuf.
ProtobufCAllocator slabAllocator = {
    .alloc = &slab_alloc, 
    .free = &slab_free, 
    .allocator_data = NULL
};

