/*
 * Minimal C stdlib/string shim for UEFI — provides malloc/calloc/realloc/free
 * and string primitives using EDK2 pool allocator and BaseMemoryLib.
 *
 * Allocations carry a hidden size_t header so realloc() works without
 * requiring the caller to track sizes.
 */

#ifndef UEFI_LIBC_H
#define UEFI_LIBC_H

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

static inline void *malloc(UINTN size)
{
	UINTN *p = AllocatePool(size + sizeof(UINTN));
	if (!p)
		return NULL;
	*p = size;
	return p + 1;
}

static inline void *calloc(UINTN n, UINTN size)
{
	UINTN total = n * size;
	UINTN *p = AllocateZeroPool(total + sizeof(UINTN));
	if (!p)
		return NULL;
	*p = total;
	return p + 1;
}

static inline void free(void *ptr)
{
	if (ptr) {
		UINTN *p = (UINTN *)ptr - 1;
		FreePool(p);
	}
}

static inline void *realloc(void *ptr, UINTN new_size)
{
	UINTN *new_p;
	UINTN old_size;
	UINTN copy_size;

	if (!ptr)
		return malloc(new_size);

	old_size = *((UINTN *)ptr - 1);
	new_p = AllocatePool(new_size + sizeof(UINTN));
	if (!new_p)
		return NULL;

	*new_p = new_size;
	copy_size = old_size < new_size ? old_size : new_size;
	CopyMem(new_p + 1, ptr, copy_size);
	free(ptr);
	return new_p + 1;
}

static inline void *memset(void *s, int c, UINTN n)
{
	SetMem(s, n, (UINT8)c);
	return s;
}

static inline void *memcpy(void *dst, const void *src, UINTN n)
{
	CopyMem(dst, src, n);
	return dst;
}

static inline int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

#endif /* UEFI_LIBC_H */
