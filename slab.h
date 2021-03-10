/* slab.h */

#include "pbf.h"

void slab_init ();

void slab_reset ();
 
void slab_done ();

extern ProtobufCAllocator slabAllocator;
