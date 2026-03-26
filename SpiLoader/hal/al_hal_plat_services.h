/*
 * Bare-metal platform services for Alpine HAL in SpiLoader context.
 * No UEFI, no libc — just volatile MMIO and inline barriers.
 */
#ifndef AL_HAL_PLAT_SERVICES_H_
#define AL_HAL_PLAT_SERVICES_H_

#include "al_hal_plat_types.h"

/* ---- Register access ---- */

static inline uint8_t al_reg_read8(uint8_t *offset)
{
	return *(volatile uint8_t *)offset;
}

static inline uint16_t al_reg_read16(uint16_t *offset)
{
	return *(volatile uint16_t *)offset;
}

static inline uint32_t al_reg_read32(uint32_t *offset)
{
	return *(volatile uint32_t *)offset;
}

static inline uint64_t al_reg_read64(uint64_t *offset)
{
	return *(volatile uint64_t *)offset;
}

static inline uint32_t al_reg_read32_relaxed(uint32_t *offset)
{
	return *(volatile uint32_t *)offset;
}

static inline void al_reg_write8(uint8_t *offset, uint8_t val)
{
	*(volatile uint8_t *)offset = val;
}

static inline void al_reg_write16(uint16_t *offset, uint16_t val)
{
	*(volatile uint16_t *)offset = val;
}

static inline void al_reg_write32(uint32_t *offset, uint32_t val)
{
	__asm__ volatile("dsb st" ::: "memory");
	*(volatile uint32_t *)offset = val;
}

static inline void al_reg_write32_relaxed(uint32_t *offset, uint32_t val)
{
	*(volatile uint32_t *)offset = val;
}

static inline void al_reg_write64(uint64_t *offset, uint64_t val)
{
	__asm__ volatile("dsb st" ::: "memory");
	*(volatile uint64_t *)offset = val;
}

/* al_reg_write32_masked is defined in al_hal_reg_utils.h */

/* ---- Logging (silent in SpiLoader) ---- */

#define al_err(...)     do {} while (0)
#define al_warn(...)    do {} while (0)
#define al_info(...)    do {} while (0)
#define al_dbg(...)     do {} while (0)
#define al_print(...)   do {} while (0)

/* ---- Assertions ---- */

#define al_assert(COND)              do { (void)(COND); } while (0)
#define al_assert_msg(COND, ...)     do { (void)(COND); } while (0)

/* ---- Memory barriers ---- */

static inline void al_data_memory_barrier(void)
{
	__asm__ volatile("dmb sy" ::: "memory");
}

static inline void al_local_data_memory_barrier(void)
{
	__asm__ volatile("dmb ish" ::: "memory");
}

static inline void al_smp_data_memory_barrier(void)
{
	__asm__ volatile("dmb ish" ::: "memory");
}

/* ---- Delay ---- */

static inline void al_udelay(unsigned long us)
{
	/* Rough busy-wait: ~2 GHz CPU, ~4 cycles per loop iteration */
	volatile unsigned long count = us * 500;
	while (count--)
		;
}

static inline void al_msleep(unsigned long ms)
{
	al_udelay(ms * 1000);
}

/* ---- Byte swap (AArch64 is little-endian) ---- */

static inline uint32_t swap_uint32(uint32_t val)
{
	return ((val >> 24) & 0xFF) |
	       ((val >> 8)  & 0xFF00) |
	       ((val << 8)  & 0xFF0000) |
	       ((val << 24) & 0xFF000000);
}

#define swap32_to_le(x)    (x)
#define swap32_from_le(x)  (x)
#define swap32_to_be(x)    swap_uint32(x)
#define swap32_from_be(x)  swap_uint32(x)

/* ---- Misc ---- */

/* AL_BIT / AL_BIT_64 defined in al_hal_reg_utils.h */

#endif
