/* SPDX-License-Identifier: 0BSD */
/*
 * UEFI configuration for XZ Embedded.
 * Replaces the Linux kernel headers with UEFI equivalents.
 *
 * xz.h includes <stdint.h> and <stddef.h> which provide the standard C
 * types (uint8_t, size_t, etc.).  We must NOT redefine them here.
 * We only provide stdbool.h, memory helpers, and allocator wrappers.
 */

#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

#include <stdbool.h>

#include "xz.h"

/* Enable both SINGLE and PREALLOC modes.
 * PREALLOC is needed because MikroTik's boot/kernel contains XZ streams
 * with truncated container metadata (valid LZMA2 data but corrupt/missing
 * footer).  PREALLOC mode preserves partial output on error, unlike SINGLE
 * which resets out_pos to 0. */
#define XZ_DEC_SINGLE
#define XZ_DEC_PREALLOC

/* ---- UEFI headers for memory services ---- */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

/* ---- memory helpers (via BaseMemoryLib) ---- */
#define memcpy(dst, src, n)   CopyMem((dst), (src), (n))
#define memmove(dst, src, n)  CopyMem((dst), (src), (n))
#define memset(s, c, n)       SetMem((s), (n), (uint8_t)(c))
#define memcmp(a, b, n)       ((int)CompareMem((a), (b), (n)))

#define memeq(a, b, size)  (CompareMem(a, b, size) == 0)
#define memzero(buf, size) ZeroMem(buf, size)

/* ---- memory allocator (via UEFI pool) ---- */
#define kmalloc(size, flags)  AllocatePool(size)
#define kfree(ptr)            do { if (ptr) FreePool(ptr); } while (0)
#define kmalloc_obj(x)        AllocatePool(sizeof(x))

/* ---- misc kernel-ism replacements ---- */
#define min_t(type, a, b)  ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define min(a, b)          ((a) < (b) ? (a) : (b))

/* __always_inline */
#ifndef __always_inline
#  define __always_inline  __attribute__((always_inline)) inline
#endif

/*
 * vmalloc/vfree: only used in XZ_DYNALLOC paths which are compiled out when
 * XZ_DEC_DYNALLOC is not defined, but the C compiler still parses the code.
 * Provide stubs so compilation doesn't fail.
 */
#define vmalloc(size)  AllocatePool(size)
#define vfree(ptr)     do { if (ptr) FreePool(ptr); } while (0)

static inline uint32_t get_le32(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;
	return (uint32_t)b[0]
	     | ((uint32_t)b[1] << 8)
	     | ((uint32_t)b[2] << 16)
	     | ((uint32_t)b[3] << 24);
}

#if __GNUC__ >= 7
#  define fallthrough  __attribute__((fallthrough))
#else
#  define fallthrough  ((void)0)
#endif

#endif /* XZ_CONFIG_H */
