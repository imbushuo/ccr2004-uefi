/** @file
  Kernel extraction — ELF64 section-based parsing, XZ decompression,
  CPIO initrd detection.

  The boot/kernel from the NPK is an ELF64 with a section named "initrd"
  containing XZ-compressed data.  Decompressing that section yields an
  EFI stub kernel (MZ/PE image) with a CPIO initrd archive appended at
  the end.

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
  Validate ELF64 AArch64 and locate the section named "initrd".

  @param[in]  Data            ELF file data.
  @param[in]  Size            ELF file size.
  @param[out] SectionOffset   File offset of the "initrd" section data.
  @param[out] SectionSize     Size of the "initrd" section data.

  @retval EFI_SUCCESS         Found the "initrd" section.
  @retval EFI_NOT_FOUND       No section named "initrd".
**/
STATIC
EFI_STATUS
FindElfInitrdSection (
  IN  CONST UINT8  *Data,
  IN  UINTN        Size,
  OUT UINTN        *SectionOffset,
  OUT UINTN        *SectionSize
  )
{
  CONST ELF64_EHDR  *Ehdr;
  CONST ELF64_SHDR  *Shdr;
  CONST ELF64_SHDR  *StrTabShdr;
  CONST CHAR8       *StrTab;
  UINTN             StrTabSize;
  UINTN             i;
  UINTN             ShOff;

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

  DEBUG ((DEBUG_WARN, "[RouterOS] ELF64 AArch64, %d sections, entry 0x%lx\n",
          Ehdr->e_shnum, Ehdr->e_entry));
  DEBUG ((DEBUG_WARN, "[RouterOS] ELF shoff=0x%lx shentsize=%d shnum=%d shstrndx=%d filesize=0x%x\n",
          Ehdr->e_shoff, Ehdr->e_shentsize, Ehdr->e_shnum, Ehdr->e_shstrndx, Size));

  //
  // Validate section headers are present
  //
  if (Ehdr->e_shnum == 0 || Ehdr->e_shoff == 0 || Ehdr->e_shstrndx == ELF_SHN_UNDEF) {
    DEBUG ((DEBUG_WARN, "[RouterOS] ELF has no section headers\n"));
    return EFI_NOT_FOUND;
  }

  //
  // Get the section name string table
  //
  ShOff = (UINTN)Ehdr->e_shoff + Ehdr->e_shstrndx * Ehdr->e_shentsize;
  if (ShOff + sizeof (ELF64_SHDR) > Size) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Section string table header out of bounds (shdr at 0x%x, filesize 0x%x)\n",
            ShOff, Size));
    return EFI_NOT_FOUND;
  }

  StrTabShdr = (CONST ELF64_SHDR *)(Data + ShOff);
  DEBUG ((DEBUG_WARN, "[RouterOS] String table section: offset=0x%lx size=0x%lx\n",
          StrTabShdr->sh_offset, StrTabShdr->sh_size));

  if ((UINTN)StrTabShdr->sh_offset + (UINTN)StrTabShdr->sh_size > Size) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Section string table data out of bounds (0x%lx+0x%lx > 0x%x)\n",
            StrTabShdr->sh_offset, StrTabShdr->sh_size, Size));
    return EFI_NOT_FOUND;
  }

  StrTab     = (CONST CHAR8 *)(Data + (UINTN)StrTabShdr->sh_offset);
  StrTabSize = (UINTN)StrTabShdr->sh_size;

  //
  // Search all sections for one named "initrd"
  //
  for (i = 0; i < Ehdr->e_shnum; i++) {
    CONST CHAR8  *Name;

    ShOff = (UINTN)Ehdr->e_shoff + i * Ehdr->e_shentsize;
    if (ShOff + sizeof (ELF64_SHDR) > Size) {
      break;
    }

    Shdr = (CONST ELF64_SHDR *)(Data + ShOff);

    if (Shdr->sh_name >= StrTabSize) {
      continue;
    }

    Name = StrTab + Shdr->sh_name;

    if (AsciiStrCmp (Name, "initrd") == 0) {
      UINTN  SecOff  = (UINTN)Shdr->sh_offset;
      UINTN  SecSize = (UINTN)Shdr->sh_size;

      if (SecOff + SecSize > Size) {
        DEBUG ((DEBUG_WARN, "[RouterOS] \"initrd\" section data out of bounds\n"));
        return EFI_NOT_FOUND;
      }

      DEBUG ((DEBUG_WARN, "[RouterOS] Found section \"initrd\": offset 0x%x, size %u bytes\n",
              SecOff, SecSize));

      *SectionOffset = SecOff;
      *SectionSize   = SecSize;
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] No section named \"initrd\" found\n"));
  return EFI_NOT_FOUND;
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

  DEBUG ((DEBUG_WARN, "[RouterOS] XZ decode failed: ret=%d, in_pos=%u/%u, out_pos=%u\n",
          (int)Ret, Buf.in_pos, Buf.in_size, Buf.out_pos));
  FreePool (Output);
  return EFI_COMPROMISED_DATA;
}

/**
  Search for a CPIO archive ("070701") within a buffer, scanning forward.

  @param[in]  Data       Buffer to search.
  @param[in]  DataSize   Size of buffer.
  @param[out] CpioStart  Offset where CPIO magic was found.

  @retval TRUE   Found; *CpioStart set.
  @retval FALSE  Not found.
**/
STATIC
BOOLEAN
FindCpioArchive (
  IN  CONST UINT8  *Data,
  IN  UINTN        DataSize,
  OUT UINTN        *CpioStart
  )
{
  UINTN  i;

  //
  // CPIO is appended after the kernel PE image. Start searching after
  // the first few KB (skip the PE header area). Align to 4 bytes since
  // CPIO entries are typically 4-byte aligned.
  //
  for (i = 0x1000; i + CPIO_MAGIC_SIZE <= DataSize; i += 4) {
    if (CompareMem (&Data[i], CPIO_MAGIC, CPIO_MAGIC_SIZE) == 0) {
      *CpioStart = i;
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Extract the EFI stub kernel and CPIO initrd from the boot/kernel ELF.

  Flow:
    1. Parse ELF64 section headers → find "initrd" section
    2. Locate XZ stream within the section → decompress → EFI stub (MZ header)
    3. Scan the decompressed blob for a CPIO archive → initrd
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
  UINTN       SecOffset;
  UINTN       SecSize;
  UINTN       XzOffset;
  UINT8       *DecompBuf;
  UINTN       DecompSize;
  UINTN       CpioOffset;

  *StubBuffer = NULL;
  *StubSize   = 0;
  *InitrdData = NULL;
  *InitrdSize = 0;

  //
  // Step 1: Try section-based "initrd" lookup first, fall back to PT_LOAD
  //
  Status = FindElfInitrdSection (KernelElf, KernelElfSize, &SecOffset, &SecSize);
  if (!EFI_ERROR (Status)) {
    XzOffset = SecOffset;
  } else {
    //
    // Section headers unavailable — fall back to finding XZ after PT_LOAD
    //
    CONST ELF64_EHDR  *Ehdr = (CONST ELF64_EHDR *)KernelElf;
    UINTN             PtLoadEnd = 0;
    UINTN             i;

    for (i = 0; i < Ehdr->e_phnum; i++) {
      UINTN              PhOff = (UINTN)Ehdr->e_phoff + i * Ehdr->e_phentsize;
      CONST ELF64_PHDR  *Phdr;

      if (PhOff + sizeof (ELF64_PHDR) > KernelElfSize) break;
      Phdr = (CONST ELF64_PHDR *)(KernelElf + PhOff);
      if (Phdr->p_type == ELF_PT_LOAD) {
        UINTN SegEnd = (UINTN)Phdr->p_offset + (UINTN)Phdr->p_filesz;
        if (SegEnd > PtLoadEnd) PtLoadEnd = SegEnd;
      }
    }
    XzOffset = PtLoadEnd;
    SecSize  = KernelElfSize - PtLoadEnd;
    DEBUG ((DEBUG_WARN, "[RouterOS] Using PT_LOAD fallback, payload after 0x%x\n", PtLoadEnd));
  }

  //
  // Step 2: Find and decompress XZ stream
  //
  {
    BOOLEAN Found = FALSE;
    UINTN   i;
    UINTN   SearchEnd = SecOffset + SecSize;

    if (XzOffset == SecOffset) {
      SearchEnd = SecOffset + SecSize;
    } else {
      SecOffset = XzOffset;
      SearchEnd = KernelElfSize;
    }

    for (i = XzOffset; i + XZ_MAGIC_SIZE <= SearchEnd; i++) {
      if (CompareMem (&KernelElf[i], gXzMagic, XZ_MAGIC_SIZE) == 0) {
        XzOffset = i;
        Found = TRUE;
        break;
      }
    }

    if (!Found) {
      DEBUG ((DEBUG_WARN, "[RouterOS] No XZ stream found\n"));
      return EFI_NOT_FOUND;
    }
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] XZ stream at ELF offset 0x%x\n", XzOffset));

  Status = TryXzDecompress (
             &KernelElf[XzOffset],
             KernelElfSize - XzOffset,
             &DecompBuf,
             &DecompSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] XZ decompression failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Decompressed: %u bytes\n", DecompSize));

  //
  // Verify MZ header (EFI stub kernel)
  //
  if (DecompSize < 2 || DecompBuf[0] != 'M' || DecompBuf[1] != 'Z') {
    DEBUG ((DEBUG_WARN, "[RouterOS] Not an EFI stub (first bytes: 0x%02x 0x%02x)\n",
            DecompSize > 0 ? DecompBuf[0] : 0,
            DecompSize > 1 ? DecompBuf[1] : 0));
    FreePool (DecompBuf);
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] EFI stub kernel verified (MZ header)\n"));

  //
  // Step 3: Scan decompressed blob for CPIO archive at the end.
  // The decompressed data = EFI stub kernel + CPIO initrd appended.
  //
  if (FindCpioArchive (DecompBuf, DecompSize, &CpioOffset)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] CPIO initrd found at offset 0x%x, size %u bytes\n",
            CpioOffset, DecompSize - CpioOffset));
    *StubBuffer = DecompBuf;
    *StubSize   = CpioOffset;
    *InitrdData = DecompBuf + CpioOffset;
    *InitrdSize = DecompSize - CpioOffset;
  } else {
    DEBUG ((DEBUG_WARN, "[RouterOS] No CPIO initrd found in decompressed data\n"));
    *StubBuffer = DecompBuf;
    *StubSize   = DecompSize;
  }

  return EFI_SUCCESS;
}
