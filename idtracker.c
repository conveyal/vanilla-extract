/* idtracker.c */
#include "idtracker.h"
#include "CRoaring/include/roaring/roaring.h"

/*
  Track 64-bit IDs in a bitset.
  OSM IDs are 64 bit integers, but most of that ID range is unused. If we use a dense bitset,
  we need something like 1GB of space. On filtered loads this bitset may be very sparse,
  causing us to spray reads and writes accross a lot of 4k pages. This quickly turns into a
  complex problem, but fortunately it's a well studied one with many existing implementations.

  Here we use the efficient compressed Roaring Bitmap implementation. It natively handles
  only 32 bit integers, and wider integers must be handled using multiple 32-bit Roaring bitsets.
*/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// The maximum node ID is almost 2^33 as of March 2021, requiring 2 bins.
// Shifting 32 bits off of 34 will result in 4 bins, rather than the currently necessary 2 bins.
#define MAX_ID (1L << 34)
#define BIN_BITS 32
#define BIN_MASK ((1L << BIN_BITS) - 1)
#define N_BINS (MAX_ID >> BIN_BITS)

static roaring_bitmap_t *(bins[N_BINS]);

void IDTracker_init () {
    for (int b = 0; b < N_BINS; b++) {
        if (bins[b]) exit(EXIT_FAILURE);
        bins[b] = roaring_bitmap_create();
    }
}

void IDTracker_free () {
    for (int b = 0; b < N_BINS; b++) {
        roaring_bitmap_free(bins[b]);
        bins[b] = NULL;
    }
}

void IDTracker_set (uint64_t id) {
    int bin_index = id >> BIN_BITS;
    int bit_index = id & BIN_MASK;
    if (bin_index >= N_BINS) exit (EXIT_FAILURE);
    roaring_bitmap_add(bins[bin_index], bit_index);
}

bool IDTracker_get (uint64_t id) {
    int bin_index = id >> BIN_BITS;
    int bit_index = id & BIN_MASK;
    if (bin_index >= N_BINS) exit (EXIT_FAILURE);
    return roaring_bitmap_contains(bins[bin_index], bit_index);
}

int main_test () {

    IDTracker_init ();
    for (int i = 0; i < 10000; i += 3) {
        IDTracker_set (i);
    }
    for (int i = 0; i < 10000; i++) {
        bool set = IDTracker_get (i);
        printf ("%d %s \n", i, set ? "SET" : "NO");
    }
    IDTracker_free ();
    return EXIT_SUCCESS;
    
}
