/* util.c utility functions */

#include <stdlib.h>
#include <stdio.h>

static char human_buffer[128];

char *human (size_t bytes) {
    /* Convert to a double, so division can yield results with decimal places. */
    double s = bytes;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf ", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf ki", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf Mi", s);
        return human_buffer;
    }
    s /= 1024;
    if (s < 1024) {
        sprintf (human_buffer, "%.1lf Gi", s);
        return human_buffer;
    }
    s /= 1024;
    sprintf (human_buffer, "%.1lf Ti", s);
    return human_buffer;
}

void die (char *s) {
    fprintf(stderr, "%s\n", s);
    exit(EXIT_FAILURE);
}
