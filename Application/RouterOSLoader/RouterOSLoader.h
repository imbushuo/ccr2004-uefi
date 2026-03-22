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
#include <Protocol/LoadFile2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Guid/LinuxEfiInitrdMedia.h>

//
// NPK item header: 2 byte type + 4 byte size (all little-endian)
//
#define NPK_HEADER_SIZE  6

//
// NPK item type for file payload
//
#define NPK_TYPE_FILE_PAYLOAD  4

//
// File entry header size within a type-4 NPK item:
//   24 bytes metadata + 4 bytes data_size + 2 bytes name_length = 30 bytes
//
#define NPK_FILE_ENTRY_HEADER_SIZE  30

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
// CPIO trailer marker
//
#define CPIO_TRAILER       "TRAILER!!!"
#define CPIO_TRAILER_SIZE  10

//
// ELF64 constants
//
#define ELF_MAGIC       0x464C457FU  // "\x7FELF"
#define ELF_CLASS64     2
#define ELF_DATA_LSB    1
#define ELF_MACHINE_AARCH64  183
#define ELF_PT_LOAD     1
#define ELF_SHN_UNDEF   0

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
// NpkParser.c
//

/**
  Parse NPK data to find the "boot/kernel" file entry.

  @param[in]  NpkData       Pointer to NPK file data in memory.
  @param[in]  NpkSize       Size of NPK data in bytes.
  @param[out] KernelData    On success, pointer to the kernel file data within NpkData.
  @param[out] KernelSize    On success, size of the kernel file data.

  @retval EFI_SUCCESS       boot/kernel found.
  @retval EFI_NOT_FOUND     No boot/kernel entry in the NPK.
**/
EFI_STATUS
NpkFindBootKernel (
  IN  CONST UINT8  *NpkData,
  IN  UINTN        NpkSize,
  OUT CONST UINT8  **KernelData,
  OUT UINTN        *KernelSize
  );

//
// KernelExtract.c
//

/**
  Extract the EFI stub kernel and optional initrd (CPIO) from the
  boot/kernel ELF64 blob by finding and decompressing XZ streams.

  @param[in]  KernelElf       Pointer to boot/kernel ELF data.
  @param[in]  KernelElfSize   Size of boot/kernel ELF data.
  @param[out] StubBuffer      Allocated buffer holding decompressed EFI stub kernel.
                               Caller must FreePool.
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

  @param[in] ImageHandle     This application's image handle.
  @param[in] KernelBuffer    Decompressed EFI stub kernel image.
  @param[in] KernelSize      Size of kernel image.
  @param[in] InitrdData      CPIO initrd data, or NULL.
  @param[in] InitrdSize      Size of initrd data, or 0.

  @retval EFI_SUCCESS        Should not return on success.
  @retval other              Error from LoadImage/StartImage.
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
