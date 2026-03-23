/** @file
  ZlibLib — zlib for UEFI (Z_SOLO freestanding mode).

  Usage:
    #include <Library/ZlibLib.h>

    z_stream strm = {0};
    strm.zalloc = zcalloc;
    strm.zfree  = zcfree;
    strm.opaque = Z_NULL;
    inflateInit (&strm);
    ...
    inflateEnd (&strm);

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef ZLIB_LIB_H_
#define ZLIB_LIB_H_

#ifndef Z_SOLO
#define Z_SOLO
#endif

#include "zlib.h"

/*
 * UEFI pool-backed allocator/free matching zlib's alloc_func/free_func.
 * Pass these as strm.zalloc / strm.zfree.
 */
voidpf zcalloc (voidpf opaque, unsigned items, unsigned size);
void   zcfree  (voidpf opaque, voidpf ptr);

#endif
