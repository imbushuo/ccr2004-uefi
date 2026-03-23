/** @file
  UEFI port layer for zlib — provides zcalloc/zcfree using UEFI pool memory.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/MemoryAllocationLib.h>

typedef void *voidpf;

voidpf
zcalloc (
  voidpf        opaque,
  unsigned int  items,
  unsigned int  size
  )
{
  (void)opaque;
  return AllocatePool ((UINTN)items * size);
}

void
zcfree (
  voidpf  opaque,
  voidpf  ptr
  )
{
  (void)opaque;
  if (ptr != NULL) {
    FreePool (ptr);
  }
}
