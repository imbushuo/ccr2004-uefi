/** @file
  RouterOS Kernel Loader - shared types, constants, prototypes.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef ROUTEROS_LOADER_H_
#define ROUTEROS_LOADER_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

//
// XZ magic bytes
//
#define XZ_MAGIC_SIZE  6
extern CONST UINT8 gXzMagic[XZ_MAGIC_SIZE];

//
// Max decompression output size (32 MB)
//
#define XZ_OUTPUT_MAX_SIZE  (32 * 1024 * 1024)

//
// CPIO newc magic
//
#define CPIO_MAGIC       "070701"
#define CPIO_MAGIC_SIZE  6

//
// ELF64 constants
//
#define ELF_MAGIC             0x464C457FU  // "\x7FELF"
#define ELF_CLASS64           2
#define ELF_DATA_LSB          1
#define ELF_MACHINE_AARCH64   183
#define ELF_PT_LOAD           1
#define ELF_SHT_PROGBITS      1
#define ELF_SHT_STRTAB        3

#pragma pack(1)
typedef struct {
  UINT8   e_ident[16];
  UINT16  e_type;
  UINT16  e_machine;
  UINT32  e_version;
  UINT64  e_entry;
  UINT64  e_phoff;
  UINT64  e_shoff;
  UINT32  e_flags;
  UINT16  e_ehsize;
  UINT16  e_phentsize;
  UINT16  e_phnum;
  UINT16  e_shentsize;
  UINT16  e_shnum;
  UINT16  e_shstrndx;
} ELF64_EHDR;

typedef struct {
  UINT32  p_type;
  UINT32  p_flags;
  UINT64  p_offset;
  UINT64  p_vaddr;
  UINT64  p_paddr;
  UINT64  p_filesz;
  UINT64  p_memsz;
  UINT64  p_align;
} ELF64_PHDR;

typedef struct {
  UINT32  sh_name;
  UINT32  sh_type;
  UINT64  sh_flags;
  UINT64  sh_addr;
  UINT64  sh_offset;
  UINT64  sh_size;
  UINT32  sh_link;
  UINT32  sh_info;
  UINT64  sh_addralign;
  UINT64  sh_entsize;
} ELF64_SHDR;
#pragma pack()

//
// KernelExtract.c
//

/**
  Extract the EFI stub kernel and optional initrd from the boot/kernel ELF64.

  @param[in]  KernelElf       Pointer to boot/kernel ELF data.
  @param[in]  KernelElfSize   Size of boot/kernel ELF data.
  @param[out] StubBuffer      Allocated buffer holding decompressed EFI stub kernel.
  @param[out] StubSize        Size of the decompressed EFI stub kernel.
  @param[out] InitrdData      Pointer into StubBuffer for CPIO initrd, or NULL.
  @param[out] InitrdSize      Size of CPIO initrd, or 0.

  @retval EFI_SUCCESS         Kernel extracted and has MZ header.
  @retval EFI_NOT_FOUND       No XZ stream produced an MZ-prefixed image.
**/
EFI_STATUS
ExtractKernelAndInitrd (
  IN  CONST UINT8  *KernelElf,
  IN  UINTN        KernelElfSize,
  OUT UINT8        **StubBuffer,
  OUT UINTN        *StubSize,
  OUT UINT8        **InitrdData,
  OUT UINTN        *InitrdSize
  );

//
// KernelBoot.c
//

/**
  Boot the Linux kernel via EFI stub protocol.
**/
EFI_STATUS
BootLinuxKernel (
  IN EFI_HANDLE  ImageHandle,
  IN UINT8       *KernelBuffer,
  IN UINTN       KernelSize,
  IN UINT8       *InitrdData,
  IN UINTN       InitrdSize
  );

#endif // ROUTEROS_LOADER_H_
