/** @file
  Kernel extraction — ELF64 parsing, XZ stream finding, decompression,
  CPIO initrd detection.

  The boot/kernel from the NPK is a self-decompressing ELF64 wrapper:
    - Single PT_LOAD segment: small decompressor stub
    - After PT_LOAD: XZ-compressed EFI stub kernel (decompresses to MZ image)
    - Deeper in the file: XZ-compressed CPIO initrd

  MikroTik's XZ streams have valid LZMA2 data but truncated/corrupt
  container metadata (missing or bad footer/CRC). We use XZ_PREALLOC mode
  which preserves partial output on error, then accept the result if the
  LZMA2 payload decompressed successfully.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"
#include "XzDec/xz.h"

CONST UINT8 gXzMagic[XZ_MAGIC_SIZE] = { 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00 };

//
// Dictionary size for XZ_PREALLOC mode (4 MB — covers the 2 MB dict
// used by MikroTik's XZ streams with headroom).
//
#define XZ_DICT_MAX  (4U * 1024U * 1024U)

/**
  Validate ELF64 AArch64 and return the file offset just past PT_LOAD.
**/
STATIC
EFI_STATUS
ParseElf64 (
  IN  CONST UINT8  *Data,
  IN  UINTN        Size,
  OUT UINTN        *PayloadStart
  )
{
  CONST ELF64_EHDR  *Ehdr;
  CONST ELF64_PHDR  *Phdr;
  UINTN             i;
  UINTN             PtLoadEnd;

  if (Size < sizeof (ELF64_EHDR)) {
    return EFI_INVALID_PARAMETER;
  }

  Ehdr = (CONST ELF64_EHDR *)Data;

  if (*(CONST UINT32 *)Ehdr->e_ident != ELF_MAGIC) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Not an ELF file\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (Ehdr->e_ident[4] != ELF_CLASS64 || Ehdr->e_ident[5] != ELF_DATA_LSB) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Not ELF64 little-endian\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (Ehdr->e_machine != ELF_MACHINE_AARCH64) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Not AArch64 (machine=%d)\n", Ehdr->e_machine));
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] ELF64 AArch64, %d program headers, entry 0x%lx\n",
          Ehdr->e_phnum, Ehdr->e_entry));

  //
  // Find PT_LOAD end — payload data starts after it
  //
  PtLoadEnd = 0;
  for (i = 0; i < Ehdr->e_phnum; i++) {
    UINTN  PhOff;

    PhOff = (UINTN)Ehdr->e_phoff + i * Ehdr->e_phentsize;
    if (PhOff + sizeof (ELF64_PHDR) > Size) {
      break;
    }

    Phdr = (CONST ELF64_PHDR *)(Data + PhOff);
    if (Phdr->p_type == ELF_PT_LOAD) {
      UINTN  SegEnd;

      SegEnd = (UINTN)Phdr->p_offset + (UINTN)Phdr->p_filesz;
      if (SegEnd > PtLoadEnd) {
        PtLoadEnd = SegEnd;
      }

      DEBUG ((DEBUG_WARN, "[RouterOS] PT_LOAD: offset 0x%x, filesz 0x%x, end 0x%x\n",
              (UINTN)Phdr->p_offset, (UINTN)Phdr->p_filesz, SegEnd));
    }
  }

  *PayloadStart = PtLoadEnd;
  return EFI_SUCCESS;
}

/**
  Try to decompress an XZ stream using PREALLOC mode (tolerant of
  truncated XZ containers — preserves output even on CRC/footer errors).

  @param[in]  Data        Start of compressed data (at XZ magic).
  @param[in]  MaxSize     Maximum bytes available.
  @param[out] OutBuffer   Allocated output buffer (caller must FreePool).
  @param[out] OutSize     Actual decompressed size.

  @retval EFI_SUCCESS     Decompression produced output.
  @retval other           No useful output produced.
**/
STATIC
EFI_STATUS
TryXzDecompress (
  IN  CONST UINT8  *Data,
  IN  UINTN        MaxSize,
  OUT UINT8        **OutBuffer,
  OUT UINTN        *OutSize
  )
{
  struct xz_dec  *Xz;
  struct xz_buf  Buf;
  enum xz_ret    Ret;
  UINT8          *Output;

  Output = AllocatePool (XZ_OUTPUT_MAX_SIZE);
  if (Output == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  xz_crc32_init ();

  //
  // Use PREALLOC mode: on XZ container errors (bad CRC, truncated footer),
  // the already-decompressed LZMA2 output is preserved in Buf.out.
  //
  Xz = xz_dec_init (XZ_PREALLOC, XZ_DICT_MAX);
  if (Xz == NULL) {
    FreePool (Output);
    return EFI_OUT_OF_RESOURCES;
  }

  Buf.in       = Data;
  Buf.in_pos   = 0;
  Buf.in_size  = MaxSize;
  Buf.out      = Output;
  Buf.out_pos  = 0;
  Buf.out_size = XZ_OUTPUT_MAX_SIZE;

  //
  // Run decompression — may need multiple calls in multi-call mode
  //
  do {
    Ret = xz_dec_run (Xz, &Buf);
  } while (Ret == XZ_OK);

  xz_dec_end (Xz);

  if (Ret == XZ_STREAM_END) {
    //
    // Clean completion
    //
    *OutBuffer = Output;
    *OutSize   = Buf.out_pos;
    return EFI_SUCCESS;
  }

  //
  // Error — but PREALLOC mode preserves partial output.
  // Accept it if we got meaningful data (the LZMA2 payload was valid,
  // only the XZ container wrapper was truncated/corrupt).
  //
  if (Buf.out_pos > 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] XZ container error (ret=%d) but got %u bytes of output\n",
            (int)Ret, Buf.out_pos));
    *OutBuffer = Output;
    *OutSize   = Buf.out_pos;
    return EFI_SUCCESS;
  }

  FreePool (Output);
  return EFI_COMPROMISED_DATA;
}

/**
  Find the next XZ magic at or after StartOffset in Data.

  @retval TRUE   Found; *Offset set to the position.
  @retval FALSE  Not found.
**/
STATIC
BOOLEAN
FindNextXzStream (
  IN  CONST UINT8  *Data,
  IN  UINTN        DataSize,
  IN  UINTN        StartOffset,
  OUT UINTN        *Offset
  )
{
  UINTN  i;

  for (i = StartOffset; i + XZ_MAGIC_SIZE <= DataSize; i++) {
    if (CompareMem (&Data[i], gXzMagic, XZ_MAGIC_SIZE) == 0) {
      *Offset = i;
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Extract the EFI stub kernel and CPIO initrd from the boot/kernel ELF.

  Flow:
    1. Parse ELF64 → find PT_LOAD end
    2. Find first XZ stream after PT_LOAD → decompress → EFI stub (MZ header)
    3. Find next XZ stream after the first → decompress → initrd (CPIO "070701")
**/
EFI_STATUS
ExtractKernelAndInitrd (
  IN  CONST UINT8  *KernelElf,
  IN  UINTN        KernelElfSize,
  OUT UINT8        **StubBuffer,
  OUT UINTN        *StubSize,
  OUT UINT8        **InitrdData,
  OUT UINTN        *InitrdSize
  )
{
  EFI_STATUS  Status;
  UINTN       PayloadStart;
  UINTN       XzOffset;
  UINT8       *DecompBuf;
  UINTN       DecompSize;

  *StubBuffer = NULL;
  *StubSize   = 0;
  *InitrdData = NULL;
  *InitrdSize = 0;

  //
  // Step 1: Parse ELF64 and find where payload data starts (after PT_LOAD)
  //
  Status = ParseElf64 (KernelElf, KernelElfSize, &PayloadStart);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Step 2: Find and decompress the first XZ stream after PT_LOAD → EFI stub
  //
  XzOffset = PayloadStart;
  if (!FindNextXzStream (KernelElf, KernelElfSize, XzOffset, &XzOffset)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] No XZ stream found after PT_LOAD (offset 0x%x)\n",
            PayloadStart));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Kernel XZ stream at ELF offset 0x%x\n", XzOffset));

  Status = TryXzDecompress (
             &KernelElf[XzOffset],
             KernelElfSize - XzOffset,
             &DecompBuf,
             &DecompSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Kernel XZ decompression failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Kernel decompressed: %u bytes\n", DecompSize));

  //
  // Verify MZ header
  //
  if (DecompSize < 2 || DecompBuf[0] != 'M' || DecompBuf[1] != 'Z') {
    DEBUG ((DEBUG_WARN, "[RouterOS] Not an EFI stub (first bytes: 0x%02x 0x%02x) — bail\n",
            DecompSize > 0 ? DecompBuf[0] : 0,
            DecompSize > 1 ? DecompBuf[1] : 0));
    FreePool (DecompBuf);
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] EFI stub verified (MZ header)\n"));
  *StubBuffer = DecompBuf;
  *StubSize   = DecompSize;

  //
  // Step 3: Find and decompress the next XZ stream → CPIO initrd
  //
  XzOffset += 1;  // advance past the kernel stream's magic
  if (!FindNextXzStream (KernelElf, KernelElfSize, XzOffset, &XzOffset)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] No initrd XZ stream found, proceeding without initrd\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Initrd XZ stream at ELF offset 0x%x\n", XzOffset));

  Status = TryXzDecompress (
             &KernelElf[XzOffset],
             KernelElfSize - XzOffset,
             &DecompBuf,
             &DecompSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Initrd XZ decompression failed: %r, proceeding without\n",
            Status));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Initrd decompressed: %u bytes\n", DecompSize));

  //
  // Verify CPIO magic
  //
  if (DecompSize >= CPIO_MAGIC_SIZE &&
      CompareMem (DecompBuf, CPIO_MAGIC, CPIO_MAGIC_SIZE) == 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] CPIO initrd verified (%u bytes)\n", DecompSize));
    *InitrdData = DecompBuf;
    *InitrdSize = DecompSize;
  } else {
    DEBUG ((DEBUG_WARN, "[RouterOS] Decompressed data is not CPIO, skipping\n"));
    FreePool (DecompBuf);
  }

  return EFI_SUCCESS;
}
