/** @file
  Kernel extraction — standard ELF64 parsing, XZ decompression,
  CPIO initrd detection.

  The boot/kernel from the NPK is an ELF64 with an "initrd" section
  containing XZ-compressed data.  Decompressing yields an EFI stub
  kernel (MZ/PE image).  A CPIO initrd archive may be appended after
  the kernel in the decompressed output.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"
#include "XzDec/xz.h"

CONST UINT8 gXzMagic[XZ_MAGIC_SIZE] = { 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00 };

#define XZ_DICT_MAX  (4U * 1024U * 1024U)

/**
  Standard ELF64 section lookup — find section by name.

  @param[in]  Data          ELF file data.
  @param[in]  Size          ELF file size.
  @param[in]  Name          Section name to find.
  @param[out] SecOffset     File offset of section data.
  @param[out] SecSize       Size of section data.

  @retval EFI_SUCCESS       Found.
  @retval EFI_NOT_FOUND     Section not present.
**/
STATIC
EFI_STATUS
FindElfSection (
  IN  CONST UINT8  *Data,
  IN  UINTN        Size,
  IN  CONST CHAR8  *Name,
  OUT UINTN        *SecOffset,
  OUT UINTN        *SecSize
  )
{
  CONST ELF64_EHDR  *Ehdr;
  CONST ELF64_SHDR  *StrTabShdr;
  CONST CHAR8       *StrTab;
  UINTN             StrTabOff;
  UINTN             i;

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

  if (Ehdr->e_shnum == 0 || Ehdr->e_shoff == 0 ||
      Ehdr->e_shstrndx >= Ehdr->e_shnum) {
    DEBUG ((DEBUG_WARN, "[RouterOS] ELF has no usable section headers\n"));
    return EFI_NOT_FOUND;
  }

  //
  // Locate .shstrtab
  //
  StrTabOff = (UINTN)Ehdr->e_shoff + Ehdr->e_shstrndx * Ehdr->e_shentsize;
  if (StrTabOff + sizeof (ELF64_SHDR) > Size) {
    return EFI_NOT_FOUND;
  }

  StrTabShdr = (CONST ELF64_SHDR *)(Data + StrTabOff);
  if ((UINTN)StrTabShdr->sh_offset + (UINTN)StrTabShdr->sh_size > Size) {
    DEBUG ((DEBUG_WARN, "[RouterOS] .shstrtab out of bounds\n"));
    return EFI_NOT_FOUND;
  }

  StrTab = (CONST CHAR8 *)(Data + (UINTN)StrTabShdr->sh_offset);

  //
  // Iterate sections
  //
  for (i = 0; i < Ehdr->e_shnum; i++) {
    UINTN              ShOff;
    CONST ELF64_SHDR  *Shdr;
    CONST CHAR8       *SecName;

    ShOff = (UINTN)Ehdr->e_shoff + i * Ehdr->e_shentsize;
    if (ShOff + sizeof (ELF64_SHDR) > Size) {
      break;
    }

    Shdr = (CONST ELF64_SHDR *)(Data + ShOff);

    if (Shdr->sh_name >= (UINTN)StrTabShdr->sh_size) {
      continue;
    }

    SecName = StrTab + Shdr->sh_name;

    if (AsciiStrCmp (SecName, Name) == 0) {
      if ((UINTN)Shdr->sh_offset + (UINTN)Shdr->sh_size > Size) {
        DEBUG ((DEBUG_WARN, "[RouterOS] Section \"%a\" data out of bounds\n", Name));
        return EFI_NOT_FOUND;
      }

      *SecOffset = (UINTN)Shdr->sh_offset;
      *SecSize   = (UINTN)Shdr->sh_size;
      DEBUG ((DEBUG_WARN, "[RouterOS] Section \"%a\": offset 0x%x, size %u\n",
              Name, *SecOffset, *SecSize));
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Section \"%a\" not found\n", Name));
  return EFI_NOT_FOUND;
}

/**
  Decompress a single XZ stream (SINGLE mode).

  @param[in]  Data         XZ stream data.
  @param[in]  MaxSize      Maximum input bytes available.
  @param[out] OutBuffer    Allocated output buffer.
  @param[out] OutSize      Decompressed size.
  @param[out] InConsumed   Optional: bytes consumed from input.
**/
STATIC
EFI_STATUS
TryXzDecompress (
  IN  CONST UINT8  *Data,
  IN  UINTN        MaxSize,
  OUT UINT8        **OutBuffer,
  OUT UINTN        *OutSize,
  OUT UINTN        *InConsumed  OPTIONAL
  )
{
  struct xz_dec  *Xz;
  struct xz_buf  Buf;
  enum xz_ret    Ret;
  UINT8          *Output;

  Output = AllocatePool (XZ_OUTPUT_MAX_SIZE);
  if (Output == NULL) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Failed to allocate %u byte output buffer\n",
            XZ_OUTPUT_MAX_SIZE));
    return EFI_OUT_OF_RESOURCES;
  }

  xz_crc32_init ();
  Xz = xz_dec_init (XZ_SINGLE, 0);
  if (Xz == NULL) {
    DEBUG ((DEBUG_WARN, "[RouterOS] xz_dec_init failed\n"));
    FreePool (Output);
    return EFI_OUT_OF_RESOURCES;
  }

  Buf.in       = Data;
  Buf.in_pos   = 0;
  Buf.in_size  = MaxSize;
  Buf.out      = Output;
  Buf.out_pos  = 0;
  Buf.out_size = XZ_OUTPUT_MAX_SIZE;

  DEBUG ((DEBUG_WARN, "[RouterOS] XZ input: %u bytes, output buffer: %u bytes\n",
          MaxSize, XZ_OUTPUT_MAX_SIZE));

  Ret = xz_dec_run (Xz, &Buf);

  DEBUG ((DEBUG_WARN, "[RouterOS] XZ result: ret=%d, in_pos=%u/%u, out_pos=%u\n",
          (int)Ret, Buf.in_pos, Buf.in_size, Buf.out_pos));

  xz_dec_end (Xz);

  if (InConsumed != NULL) {
    *InConsumed = Buf.in_pos;
  }

  if (Ret == XZ_STREAM_END) {
    *OutBuffer = Output;
    *OutSize   = Buf.out_pos;
    return EFI_SUCCESS;
  }

  if (Buf.out_pos > 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] XZ container error (ret=%d) but got %u bytes\n",
            (int)Ret, Buf.out_pos));
    *OutBuffer = Output;
    *OutSize   = Buf.out_pos;
    return EFI_SUCCESS;
  }

  FreePool (Output);
  return EFI_COMPROMISED_DATA;
}

/**
  Get the actual PE file size by scanning section headers for the
  maximum (PointerToRawData + SizeOfRawData).  This is the on-disk
  file size, NOT SizeOfImage (which includes virtual BSS).

  @param[in]  Data      Buffer starting with MZ header.
  @param[in]  DataSize  Buffer size.
  @param[out] FileSize  Actual PE file extent in bytes.

  @retval TRUE  Parsed successfully.
  @retval FALSE Not a valid PE/COFF image.
**/
STATIC
BOOLEAN
GetPeFileSize (
  IN  CONST UINT8  *Data,
  IN  UINTN        DataSize,
  OUT UINTN        *FileSize
  )
{
  UINT32  PeOffset;
  UINT16  NumSections;
  UINT16  OptHdrSize;
  UINTN   SecStart;
  UINTN   MaxEnd;
  UINT16  i;

  if (DataSize < 0x40 || Data[0] != 'M' || Data[1] != 'Z') {
    return FALSE;
  }

  PeOffset = Data[0x3C] | ((UINT32)Data[0x3D] << 8) |
             ((UINT32)Data[0x3E] << 16) | ((UINT32)Data[0x3F] << 24);

  if (PeOffset + 24 > DataSize) {
    return FALSE;
  }

  if (Data[PeOffset] != 'P' || Data[PeOffset + 1] != 'E' ||
      Data[PeOffset + 2] != 0 || Data[PeOffset + 3] != 0) {
    return FALSE;
  }

  //
  // COFF header: NumSections at +6, SizeOfOptionalHeader at +20
  //
  NumSections = Data[PeOffset + 6] | ((UINT16)Data[PeOffset + 7] << 8);
  OptHdrSize  = Data[PeOffset + 20] | ((UINT16)Data[PeOffset + 21] << 8);

  //
  // Section headers start after PE sig (4) + COFF header (20) + optional header
  //
  SecStart = PeOffset + 24 + OptHdrSize;
  if (SecStart + (UINTN)NumSections * 40 > DataSize) {
    return FALSE;
  }

  //
  // Find the maximum file extent across all sections
  //
  MaxEnd = 0;
  for (i = 0; i < NumSections; i++) {
    UINTN  So;
    UINT32 RawSize;
    UINT32 RawPtr;
    UINTN  End;

    So     = SecStart + (UINTN)i * 40;
    RawSize = Data[So + 16] | ((UINT32)Data[So + 17] << 8) |
              ((UINT32)Data[So + 18] << 16) | ((UINT32)Data[So + 19] << 24);
    RawPtr  = Data[So + 20] | ((UINT32)Data[So + 21] << 8) |
              ((UINT32)Data[So + 22] << 16) | ((UINT32)Data[So + 23] << 24);
    End = (UINTN)RawPtr + RawSize;
    if (End > MaxEnd) {
      MaxEnd = End;
    }
  }

  *FileSize = MaxEnd;
  return TRUE;
}

/**
  Find the first XZ magic at or after StartOffset.
**/
STATIC
BOOLEAN
FindXzStream (
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

  The "initrd" ELF section contains two concatenated XZ streams:
    Stream 1 -> EFI stub kernel (MZ/PE image)
    Stream 2 -> CPIO initrd archive

  xz-embedded in SINGLE mode decompresses one stream per call and
  reports how much input was consumed (in_pos). We use in_pos to
  find stream 2 after stream 1 completes.
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
  UINTN       SearchStart;
  UINTN       SearchEnd;
  UINTN       XzOffset;
  UINT8       *KernelBuf;
  UINTN       KernelDecompSize;
  UINTN       KernelFileSize;
  UINTN       Stream1Consumed;
  UINTN       Stream2Start;
  UINT8       *InitrdBuf;
  UINTN       InitrdDecompSize;

  *StubBuffer = NULL;
  *StubSize   = 0;
  *InitrdData = NULL;
  *InitrdSize = 0;

  //
  // Find "initrd" section via standard ELF section headers
  //
  Status = FindElfSection (KernelElf, KernelElfSize, "initrd",
                           &SearchStart, &SearchEnd);
  if (!EFI_ERROR (Status)) {
    SearchEnd = SearchStart + SearchEnd;
  } else {
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

    SearchStart = PtLoadEnd;
    SearchEnd   = KernelElfSize;
    DEBUG ((DEBUG_WARN, "[RouterOS] No initrd section, scanning after PT_LOAD at 0x%x\n",
            PtLoadEnd));
  }

  //
  // Locate first XZ stream
  //
  if (!FindXzStream (KernelElf, SearchEnd, SearchStart, &XzOffset)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] No XZ stream found\n"));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] XZ stream 1 at ELF offset 0x%x\n", XzOffset));

  //
  // Decompress stream 1 -> EFI stub kernel
  //
  Status = TryXzDecompress (
             &KernelElf[XzOffset],
             SearchEnd - XzOffset,
             &KernelBuf,
             &KernelDecompSize,
             &Stream1Consumed
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Kernel XZ decompression failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Kernel decompressed: %u bytes (consumed %u input bytes)\n",
          KernelDecompSize, Stream1Consumed));

  if (KernelDecompSize < 2 || KernelBuf[0] != 'M' || KernelBuf[1] != 'Z') {
    DEBUG ((DEBUG_WARN, "[RouterOS] Not an EFI stub (0x%02x 0x%02x)\n",
            KernelDecompSize > 0 ? KernelBuf[0] : 0,
            KernelDecompSize > 1 ? KernelBuf[1] : 0));
    FreePool (KernelBuf);
    return EFI_NOT_FOUND;
  }

  //
  // Get actual PE file size from section headers
  //
  if (GetPeFileSize (KernelBuf, KernelDecompSize, &KernelFileSize)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] PE file size: %u bytes (0x%x)\n",
            KernelFileSize, KernelFileSize));
  } else {
    KernelFileSize = KernelDecompSize;
    DEBUG ((DEBUG_WARN, "[RouterOS] Could not parse PE, using decompressed size\n"));
  }

  *StubBuffer = KernelBuf;
  *StubSize   = KernelFileSize;

  //
  // Decompress stream 2 -> initrd (skip padding between streams)
  //
  Stream2Start = XzOffset + Stream1Consumed;

  //
  // Skip zero padding between XZ streams
  //
  while (Stream2Start < SearchEnd && KernelElf[Stream2Start] == 0) {
    Stream2Start++;
  }

  if (FindXzStream (KernelElf, SearchEnd, Stream2Start, &Stream2Start)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] XZ stream 2 at ELF offset 0x%x\n", Stream2Start));

    Status = TryXzDecompress (
               &KernelElf[Stream2Start],
               SearchEnd - Stream2Start,
               &InitrdBuf,
               &InitrdDecompSize,
               NULL
               );
    if (!EFI_ERROR (Status) && InitrdDecompSize > 0) {
      DEBUG ((DEBUG_WARN, "[RouterOS] Initrd decompressed: %u bytes\n", InitrdDecompSize));

      if (InitrdDecompSize >= CPIO_MAGIC_SIZE &&
          CompareMem (InitrdBuf, CPIO_MAGIC, CPIO_MAGIC_SIZE) == 0) {
        DEBUG ((DEBUG_WARN, "[RouterOS] CPIO initrd verified\n"));
        *InitrdData = InitrdBuf;
        *InitrdSize = InitrdDecompSize;
      } else {
        DEBUG ((DEBUG_WARN, "[RouterOS] Stream 2 is not CPIO, skipping\n"));
        FreePool (InitrdBuf);
      }
    } else {
      DEBUG ((DEBUG_WARN, "[RouterOS] Initrd decompression failed, proceeding without\n"));
    }
  } else {
    DEBUG ((DEBUG_WARN, "[RouterOS] No second XZ stream, proceeding without initrd\n"));
  }

  return EFI_SUCCESS;
}
