/*
 * Bare-metal platform types for Alpine HAL in SpiLoader context.
 */
#ifndef AL_HAL_PLAT_TYPES_H_
#define AL_HAL_PLAT_TYPES_H_

#include <stdint.h>
#include <stddef.h>

typedef int al_bool;
#define AL_TRUE   1
#define AL_FALSE  0

#define al_bool  al_bool

#define __iomem

/* Map EINVAL/ETIME/EIO/EBUSY to simple negative values */
#ifndef EINVAL
#define EINVAL  22
#endif
#ifndef ETIME
#define ETIME   62
#endif
#ifndef EIO
#define EIO      5
#endif
#ifndef EBUSY
#define EBUSY   16
#endif

#endif
