#ifndef PL031_H
#define PL031_H

#include "aether/types.h"

/* Returns seconds since epoch as stored in the PL031 RTCDR register. */
u32 pl031_read(void);

#endif /* PL031_H */
