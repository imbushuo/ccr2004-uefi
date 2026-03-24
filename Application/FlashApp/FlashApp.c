/** @file
  FlashApp — writes a UEFI ELF image to SPI NOR flash.

  Usage: FlashApp <filename.elf>

  Validates the file is an AArch64 ELF under 6 MB, then erases and
  programs it to SPI flash starting at offset 0x00800000 (8 MB mark).

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SpiNorFlash.h>
#include <Guid/FileInfo.h>

//
// SPI flash target parameters
//
#define FLASH_TARGET_OFFSET   0x00800000U   // 8 MB mark
#define FLASH_SECTOR_SIZE     0x1000U       // 4 KB erase sector
#define FLASH_MAX_IMAGE_SIZE  (6U * 1024U * 1024U)  // 6 MB limit

//
// ELF64 header constants
//
#define ELF_MAGIC           0x464C457FU     // "\x7FELF"
#define ELF_CLASS64         2
#define ELF_DATA_LSB        1
#define ELF_MACHINE_AARCH64 183

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
#pragma pack()

//
// Protocol GUID used by our SpiNorFlashDxe
//
extern EFI_GUID  gEdk2JedecSfdpSpiDxeDriverGuid;

/**
  Read a file from the shell's filesystem into an allocated buffer.
**/
STATIC
EFI_STATUS
ReadFileFromShell (
  IN  EFI_HANDLE   ImageHandle,
  IN  CHAR16       *FileName,
  OUT UINT8        **FileData,
  OUT UINTN        *FileSize
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  UINT8                            *Buffer;
  UINTN                            ReadSize;

  //
  // Get the filesystem from the loaded image's device
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status)) {
    //
    // Try all filesystem handles
    //
    EFI_HANDLE  *Handles;
    UINTN       Count;
    UINTN       i;

    Status = gBS->LocateHandleBuffer (
                    ByProtocol,
                    &gEfiSimpleFileSystemProtocolGuid,
                    NULL,
                    &Count,
                    &Handles
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    for (i = 0; i < Count; i++) {
      Status = gBS->HandleProtocol (Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (EFI_ERROR (Status)) continue;

      Status = Fs->OpenVolume (Fs, &Root);
      if (EFI_ERROR (Status)) continue;

      Status = Root->Open (Root, &File, FileName, EFI_FILE_MODE_READ, 0);
      Root->Close (Root);
      if (!EFI_ERROR (Status)) {
        goto FileOpened;
      }
    }

    FreePool (Handles);
    return EFI_NOT_FOUND;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Root->Open (Root, &File, FileName, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

FileOpened:
  //
  // Get file size
  //
  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_DEVICE_ERROR;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    File->Close (File);
    return Status;
  }

  ReadSize = (UINTN)Info->FileSize;
  FreePool (Info);

  Buffer = AllocatePool (ReadSize);
  if (Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &ReadSize, Buffer);
  File->Close (File);

  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    return Status;
  }

  *FileData = Buffer;
  *FileSize = ReadSize;
  return EFI_SUCCESS;
}

/**
  Entry point.
**/
EFI_STATUS
EFIAPI
FlashAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_SHELL_PARAMETERS_PROTOCOL    *ShellParams;
  CHAR16                           *FileName;
  UINT8                            *FileData;
  UINTN                            FileSize;
  ELF64_EHDR                       *Ehdr;
  EFI_SPI_NOR_FLASH_PROTOCOL       *Flash;
  UINT32                           SectorsToErase;
  UINT32                           EraseSize;

  //
  // Get command line arguments
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams
                  );
  if (EFI_ERROR (Status) || ShellParams->Argc < 2) {
    Print (L"Usage: FlashApp <uefi.elf>\n");
    Print (L"  Writes an AArch64 ELF image to SPI flash at offset 0x%x\n",
           FLASH_TARGET_OFFSET);
    return EFI_INVALID_PARAMETER;
  }

  FileName = ShellParams->Argv[1];

  //
  // Read the file
  //
  Print (L"Reading %s...\n", FileName);
  Status = ReadFileFromShell (ImageHandle, FileName, &FileData, &FileSize);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to read file: %r\n", Status);
    return Status;
  }

  Print (L"File size: %u bytes\n", FileSize);

  //
  // Validate ELF header
  //
  if (FileSize < sizeof (ELF64_EHDR)) {
    Print (L"Error: File too small for ELF header\n");
    FreePool (FileData);
    return EFI_INVALID_PARAMETER;
  }

  Ehdr = (ELF64_EHDR *)FileData;

  if (*(UINT32 *)Ehdr->e_ident != ELF_MAGIC) {
    Print (L"Error: Not an ELF file (bad magic)\n");
    FreePool (FileData);
    return EFI_INVALID_PARAMETER;
  }

  if (Ehdr->e_ident[4] != ELF_CLASS64 || Ehdr->e_ident[5] != ELF_DATA_LSB) {
    Print (L"Error: Not a 64-bit little-endian ELF\n");
    FreePool (FileData);
    return EFI_INVALID_PARAMETER;
  }

  if (Ehdr->e_machine != ELF_MACHINE_AARCH64) {
    Print (L"Error: Not an AArch64 ELF (machine=%u)\n", Ehdr->e_machine);
    FreePool (FileData);
    return EFI_INVALID_PARAMETER;
  }

  if (FileSize >= FLASH_MAX_IMAGE_SIZE) {
    Print (L"Error: File too large (%u bytes, max %u)\n",
           FileSize, FLASH_MAX_IMAGE_SIZE);
    FreePool (FileData);
    return EFI_INVALID_PARAMETER;
  }

  Print (L"ELF validated: AArch64, entry 0x%lx\n", Ehdr->e_entry);

  //
  // Locate SPI NOR flash protocol
  //
  Status = gBS->LocateProtocol (
                  &gEdk2JedecSfdpSpiDxeDriverGuid,
                  NULL,
                  (VOID **)&Flash
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Error: SPI NOR flash protocol not found: %r\n", Status);
    FreePool (FileData);
    return Status;
  }

  //
  // Erase the target region (4KB sectors, one at a time for progress)
  //
  EraseSize = (UINT32)((FileSize + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1));
  SectorsToErase = EraseSize / FLASH_SECTOR_SIZE;

  Print (L"Erasing %u KB at flash offset 0x%x...\n", EraseSize / 1024, FLASH_TARGET_OFFSET);

  {
    UINT32  Idx;
    UINT32  LastPct = 0;

    for (Idx = 0; Idx < SectorsToErase; Idx++) {
      Status = Flash->Erase (Flash, FLASH_TARGET_OFFSET + Idx * FLASH_SECTOR_SIZE, 1);
      if (EFI_ERROR (Status)) {
        Print (L"\nError: Erase failed at sector %u: %r\n", Idx, Status);
        FreePool (FileData);
        return Status;
      }

      UINT32 Pct = (Idx + 1) * 100 / SectorsToErase;
      if (Pct != LastPct) {
        Print (L"\rErasing... %3u%%", Pct);
        LastPct = Pct;
      }
    }

    Print (L"\rErasing... done    \n");
  }

  //
  // Program in 4KB chunks for progress reporting
  //
  Print (L"Programming %u bytes...\n", FileSize);

  {
    UINT32  Offset = 0;
    UINT32  LastPct = 0;

    while (Offset < (UINT32)FileSize) {
      UINT32  Chunk = (UINT32)FileSize - Offset;
      if (Chunk > FLASH_SECTOR_SIZE) {
        Chunk = FLASH_SECTOR_SIZE;
      }

      Status = Flash->WriteData (Flash, FLASH_TARGET_OFFSET + Offset, Chunk, FileData + Offset);
      if (EFI_ERROR (Status)) {
        Print (L"\nError: Write failed at offset 0x%x: %r\n", Offset, Status);
        FreePool (FileData);
        return Status;
      }

      Offset += Chunk;

      UINT32 Pct = Offset * 100 / (UINT32)FileSize;
      if (Pct != LastPct) {
        Print (L"\rProgramming... %3u%%", Pct);
        LastPct = Pct;
      }
    }

    Print (L"\rProgramming... done    \n");
  }

  //
  // Verify in 4KB chunks for progress reporting
  //
  {
    UINT8   *VerifyBuf;
    UINT32  Offset = 0;
    UINT32  LastPct = 0;

    Print (L"Verifying...\n");

    VerifyBuf = AllocatePool (FLASH_SECTOR_SIZE);
    if (VerifyBuf == NULL) {
      Print (L"Warning: Could not allocate verify buffer, skipping verification\n");
    } else {
      while (Offset < (UINT32)FileSize) {
        UINT32  Chunk = (UINT32)FileSize - Offset;
        if (Chunk > FLASH_SECTOR_SIZE) {
          Chunk = FLASH_SECTOR_SIZE;
        }

        Status = Flash->ReadData (Flash, FLASH_TARGET_OFFSET + Offset, Chunk, VerifyBuf);
        if (EFI_ERROR (Status)) {
          Print (L"\nError: Read-back failed at offset 0x%x: %r\n", Offset, Status);
          FreePool (VerifyBuf);
          FreePool (FileData);
          return EFI_DEVICE_ERROR;
        }

        if (CompareMem (FileData + Offset, VerifyBuf, Chunk) != 0) {
          Print (L"\nError: Verification FAILED at offset 0x%x!\n", Offset);
          FreePool (VerifyBuf);
          FreePool (FileData);
          return EFI_DEVICE_ERROR;
        }

        Offset += Chunk;

        UINT32 Pct = Offset * 100 / (UINT32)FileSize;
        if (Pct != LastPct) {
          Print (L"\rVerifying... %3u%%", Pct);
          LastPct = Pct;
        }
      }

      Print (L"\rVerifying... done    \n");
      FreePool (VerifyBuf);
    }
  }

  FreePool (FileData);
  Print (L"Success: %u bytes written to flash at 0x%x\n",
         FileSize, FLASH_TARGET_OFFSET);

  return EFI_SUCCESS;
}
