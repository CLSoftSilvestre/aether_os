#ifndef AETHER_PIPE_H
#define AETHER_PIPE_H

#include "aether/types.h"

#define MAX_PIPES    16
#define PIPE_BUF     4096

/* pipe_alloc — allocate a new pipe; returns index (0..MAX_PIPES-1) or -1 */
int  pipe_alloc(void);

/* pipe_close_read / pipe_close_write — decrement ref count on each end */
void pipe_close_read(int idx);
void pipe_close_write(int idx);

/* pipe_read — read up to len bytes; blocks if empty (returns 0 if write end closed) */
long pipe_read(int idx, char *buf, long len);

/* pipe_write — write len bytes; returns bytes written or -1 if read end closed */
long pipe_write(int idx, const char *buf, long len);

/* pipe_init — zero-initialise all pipe slots; call once from kernel_main */
void pipe_init(void);

#endif /* AETHER_PIPE_H */
