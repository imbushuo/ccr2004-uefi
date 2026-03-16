/** @file
  UEFI platform services for Alpine HAL.

  Maps Alpine HAL platform services (MMIO, barriers, memory ops, etc.)
  to UEFI/EDK2 library calls.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __PLAT_SERVICES_H__
#define __PLAT_SERVICES_H__

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

/*
 * Register I/O — map to EDK2 MmioRead/MmioWrite
 */
static inline uint8_t al_reg_read8(uint8_t *offset)
{
  return MmioRead8((UINTN)offset);
}

static inline uint16_t al_reg_read16(uint16_t *offset)
{
  return MmioRead16((UINTN)offset);
}

static inline uint32_t al_reg_read32(uint32_t *offset)
{
  return MmioRead32((UINTN)offset);
}

static inline uint64_t al_reg_read64(uint64_t *offset)
{
  return MmioRead64((UINTN)offset);
}

static inline uint32_t al_reg_read32_relaxed(uint32_t *offset)
{
  return MmioRead32((UINTN)offset);
}

static inline void al_reg_write8(uint8_t *offset, uint8_t val)
{
  MmioWrite8((UINTN)offset, val);
}

static inline void al_reg_write16(uint16_t *offset, uint16_t val)
{
  MmioWrite16((UINTN)offset, val);
}

static inline void al_reg_write32(uint32_t *offset, uint32_t val)
{
  MmioWrite32((UINTN)offset, val);
}

static inline void al_reg_write32_relaxed(uint32_t *offset, uint32_t val)
{
  MmioWrite32((UINTN)offset, val);
}

static inline void al_reg_write64(uint64_t *offset, uint64_t val)
{
  MmioWrite64((UINTN)offset, val);
}

/*
 * Logging — map to EDK2 DEBUG macros
 */
#define al_print(...)   DEBUG((DEBUG_INFO, __VA_ARGS__))
#define al_err(...)     DEBUG((DEBUG_ERROR, __VA_ARGS__))
#define al_warn(...)    DEBUG((DEBUG_WARN, __VA_ARGS__))
#define al_info(...)    DEBUG((DEBUG_INFO, __VA_ARGS__))
#define al_dbg(...)     DEBUG((DEBUG_VERBOSE, __VA_ARGS__))

/*
 * sprintf — stub (unused by ethernet drivers, but referenced by some HAL headers)
 */
#define al_sprintf(buf, ...)  (0)

/*
 * Assertions — map to EDK2 ASSERT (no exit())
 */
#define al_assert(COND) \
  do { \
    if (!(COND)) { \
      DEBUG((DEBUG_ERROR, "%a:%d:%a: Assertion failed! (%a)\n", \
             __FILE__, __LINE__, __func__, #COND)); \
      ASSERT(FALSE); \
    } \
  } while (AL_FALSE)

#define al_assert_msg(COND, ...) \
  do { \
    if (!(COND)) { \
      DEBUG((DEBUG_ERROR, "%a:%d:%a: Assertion failed! (%a)\n", \
             __FILE__, __LINE__, __func__, #COND)); \
      DEBUG((DEBUG_ERROR, __VA_ARGS__)); \
      ASSERT(FALSE); \
    } \
  } while (AL_FALSE)

/*
 * Memory barriers
 */
static inline void al_data_memory_barrier(void)
{
  MemoryFence();
}

static inline void al_local_data_memory_barrier(void)
{
  MemoryFence();
}

static inline void al_smp_data_memory_barrier(void)
{
  MemoryFence();
}

static inline void al_smp_write_data_memory_barrier(void)
{
  MemoryFence();
}

/*
 * Delays
 */
static inline void al_udelay(unsigned long loops)
{
  MicroSecondDelay(loops);
}

static inline void al_msleep(unsigned long loops)
{
  MicroSecondDelay(loops * 1000);
}

/*
 * Endianness — AArch64 UEFI is always LE, so LE conversions are identity
 */
#define swap16_to_le(x)    (x)
#define swap32_to_le(x)    (x)
#define swap64_to_le(x)    (x)
#define swap16_from_le(x)  (x)
#define swap32_from_le(x)  (x)
#define swap64_from_le(x)  (x)
#define swap16_to_be(x)    SwapBytes16(x)
#define swap32_to_be(x)    SwapBytes32(x)
#define swap64_to_be(x)    SwapBytes64(x)
#define swap16_from_be(x)  SwapBytes16(x)
#define swap32_from_be(x)  SwapBytes32(x)
#define swap64_from_be(x)  SwapBytes64(x)

/*
 * Memory operations
 * Note: al_memset arg order is (p, val, cnt) matching C memset,
 * while SetMem is (p, cnt, val) — swap cnt and val.
 */
#define al_memset(p, val, cnt)   SetMem((p), (cnt), (UINT8)(val))
#define al_memcpy(p1, p2, cnt)   CopyMem((p1), (p2), (cnt))
#define al_memcmp(p1, p2, cnt)   ((int)CompareMem((p1), (p2), (cnt)))

/*
 * String operations
 */
#define al_strcmp(s1, s2)  ((int)AsciiStrCmp((s1), (s2)))

/*
 * CPU ID (single core from UEFI perspective)
 */
#define al_get_cpu_id()      0
#define al_get_cluster_id()  0

#endif /* __PLAT_SERVICES_H__ */
