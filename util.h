/* util.h utility functions */

#ifndef UTIL_H_INCLUDED
#define UTIL_H_INCLUDED

/* Print human readable representation based on multiples of 1024 into a static buffer, returning buffer address. */
char *human (size_t bytes);

/* Terminate the process after printing an error message. Crude error handling. */
void die (char *s);

#endif /* UTIL_H_INCLUDED */
