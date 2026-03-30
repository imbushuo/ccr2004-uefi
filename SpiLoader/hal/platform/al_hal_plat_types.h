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

typedef uint64_t al_phys_addr_t;

#define __iomem

#ifndef inline
#define inline __inline__
#endif

/* Alpine V2 device identification for conditional compilation */
#define AL_DEV_ID_ALPINE_V1  0
#define AL_DEV_ID_ALPINE_V2  1
#define AL_DEV_ID_ALPINE_V3  2
#define AL_DEV_ID_ALPINE_V4  3
#define AL_DEV_ID            AL_DEV_ID_ALPINE_V2

#ifndef EPERM
#define EPERM    1
#endif
#ifndef ENOMEM
#define ENOMEM  12
#endif
#ifndef ENOSYS
#define ENOSYS  38
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

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
