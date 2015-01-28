/* idtracker.h */

#ifndef IDTRACKER_H_INCLUDED
#define IDTRACKER_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>

void IDTracker_reset ();

bool IDTracker_set (uint64_t id);

bool IDTracker_get (uint64_t id);

#endif // IDTRACKER_H_INCLUDED
