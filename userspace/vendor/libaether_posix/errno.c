#include <errno.h>

/* Single global errno — safe for single-threaded ported apps.
 * When/if real pthreads land, this becomes __thread int errno. */
int errno = 0;
