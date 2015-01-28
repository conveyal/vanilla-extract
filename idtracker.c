/* idtracker.c */
#include "idtracker.h"

/*
  A bitset intended for tracking usage of OSM IDs, which are 64 bit integers.
  However most of that ID range is unused. If we just reserve one bit per ID that may be flagged,
  we get something like 512 MB of space needed. To keep this simple we just allocate all that at
  once and let the OS take care of paging. 
  Note that the VM page size is typically 4 kBytes, so this is not as sparse as we'd ideally like.
  But with one or more levels of indirection to dynamically allocated bins, the pointers to the 
  bins are each 64 bits so the bins would need to be relatively large to be effective. It would
  only really be advantageous if the bins were looked up in a dynamically resized hashtable rather
  than a flat array, so it gets complicated quickly.
  The approach used here is much more simple and much less prone to error.
*/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_ID (1L << 32)
#define BIN_BITS 6
#define BIN_MASK (64 - 1)
#define N_BINS (MAX_ID >> BIN_BITS)

static uint64_t bins[N_BINS];

void IDTracker_reset () {
    memset (bins, 0, sizeof(bins));
}

bool IDTracker_set (uint64_t id) {
    int bin_index = id >> BIN_BITS;
    int bit_index = id & BIN_MASK;
    if (bin_index >= N_BINS) exit (-12);
    uint64_t bit_flag = 1L << bit_index;
    bool already_set = bins[bin_index] & bit_flag;
    bins[bin_index] |= bit_flag;
    return already_set;
}

bool IDTracker_get (uint64_t id) {
    int bin_index = id >> BIN_BITS;
    int bit_index = id & BIN_MASK;
    if (bin_index >= N_BINS) exit (-12);
    uint64_t bit_flag = 1L << bit_index;
    return bins[bin_index] & bit_flag;
}

int main_test () {

    IDTracker_reset ();
    for (int i = 0; i < 10000; i += 3) {
        IDTracker_set (i);
    }
    
    for (int i = 0; i < 10000; i++) {
        bool set = IDTracker_get (i);
        printf ("%d %s \n", i, set ? "SET" : "NO");
    }
    return 0;
    
}
