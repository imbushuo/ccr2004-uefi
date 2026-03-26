/** @file
  FlashApp — writes a UEFI ELF image to SPI NOR flash.

  Usage: FlashApp [-slot 0|1] <filename.elf>

  Slot 0 (default): flash offset 0x00800000 (primary firmware)
  Slot 1:           flash offset 0x00C00000 (backup firmware)

  Validates the file is an AArch64 ELF under 3 MB, then erases and
  programs it to the selected slot.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/ShellLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SpiNorFlash.h>
#include <Guid/FileInfo.h>

//
// SPI flash dual-firmware layout:
//   Slot 0 (primary): 0x00800000 - 0x00BFFFFF (4 MB)
//   Slot 1 (backup):  0x00C00000 - 0x00FFFFFF (4 MB)
//
#define FLASH_SLOT0_OFFSET    0x00800000U
#define FLASH_SLOT1_OFFSET    0x00C00000U
#define FLASH_SLOT_SIZE       0x00400000U   // 4 MB per slot
#define FLASH_SECTOR_SIZE     0x1000U       // 4 KB erase sector
#define FLASH_MAX_IMAGE_SIZE  (FLASH_SLOT_SIZE - 0x10000U)  // ~3.9 MB limit

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
extern EFI_GUID  gEfiSpiNorFlashProtocolGuid;

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
  SHELL_FILE_HANDLE                FileHandle;
  EFI_FILE_INFO                    *Info;
  UINTN                            FileSize;
  UINT8                            *FileData;
  UINTN                            ReadSize;
  ELF64_EHDR                       *Ehdr;
  EFI_SPI_NOR_FLASH_PROTOCOL       *Flash;
  UINT32                           EraseSize;
  UINT32                           SectorsToErase;
  UINT32                           FlashOffset;
  UINTN                            Slot;
  UINTN                            ArgIdx;

  //
  // Get command line arguments
  //
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams
                  );
  if (EFI_ERROR (Status) || ShellParams->Argc < 2) {
    Print (L"Usage: FlashApp [-slot 0|1] <uefi.elf>\n");
    Print (L"  Slot 0 (default): primary firmware  @ flash 0x%x\n", FLASH_SLOT0_OFFSET);
    Print (L"  Slot 1:           backup firmware   @ flash 0x%x\n", FLASH_SLOT1_OFFSET);
    return EFI_INVALID_PARAMETER;
  }

  //
  // Parse optional -slot argument
  //
  Slot    = 0;
  ArgIdx  = 1;

  if (ShellParams->Argc >= 4 && StrCmp (ShellParams->Argv[1], L"-slot") == 0) {
    if (StrCmp (ShellParams->Argv[2], L"1") == 0) {
      Slot = 1;
    } else if (StrCmp (ShellParams->Argv[2], L"0") != 0) {
      Print (L"Error: Invalid slot (must be 0 or 1)\n");
      return EFI_INVALID_PARAMETER;
    }
    ArgIdx = 3;
  }

  FlashOffset = (Slot == 0) ? FLASH_SLOT0_OFFSET : FLASH_SLOT1_OFFSET;
  FileName = ShellParams->Argv[ArgIdx];

  Print (L"Target: slot %u @ flash offset 0x%x\n", Slot, FlashOffset);

  //
  // Open the file via ShellLib (handles fsX: prefixes)
  //
  Print (L"Reading %s...\n", FileName);

  Status = ShellOpenFileByName (FileName, &FileHandle, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Cannot open file: %r\n", Status);
    return Status;
  }

  //
  // Get file size
  //
  Info = ShellGetFileInfo (FileHandle);
  if (Info == NULL) {
    Print (L"Error: Cannot get file info\n");
    ShellCloseFile (&FileHandle);
    return EFI_DEVICE_ERROR;
  }

  FileSize = (UINTN)Info->FileSize;
  FreePool (Info);

  Print (L"File size: %u bytes\n", FileSize);

  //
  // Validate ELF header (read into buffer)
  //
  FileData = AllocatePool (FileSize);
  if (FileData == NULL) {
    Print (L"Error: Out of memory\n");
    ShellCloseFile (&FileHandle);
    return EFI_OUT_OF_RESOURCES;
  }

  ReadSize = FileSize;
  Status = ShellReadFile (FileHandle, &ReadSize, FileData);
  ShellCloseFile (&FileHandle);

  if (EFI_ERROR (Status) || ReadSize != FileSize) {
    Print (L"Error: Failed to read file: %r\n", Status);
    FreePool (FileData);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

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
                  &gEfiSpiNorFlashProtocolGuid,
                  NULL,
                  (VOID **)&Flash
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Error: SPI NOR flash protocol not found: %r\n", Status);
    FreePool (FileData);
    return Status;
  }

  //
  // Clear flash write protection (SR block protect bits)
  //
  {
    UINT8  Sr;

    Status = Flash->ReadStatus (Flash, 1, &Sr);
    if (!EFI_ERROR (Status)) {
      Print (L"Flash status register: 0x%02x\n", Sr);
      if (Sr & 0x7C) {
        //
        // BP0-BP4 bits (bits 2-6) are set — clear them to unprotect all sectors.
        // Write 0x00 to status register to clear all block protection.
        //
        Print (L"Clearing write protection (BP bits)...\n");
        Sr = 0x00;
        Status = Flash->WriteStatus (Flash, 1, &Sr);
        if (EFI_ERROR (Status)) {
          Print (L"Error: Failed to clear write protection: %r\n", Status);
          FreePool (FileData);
          return Status;
        }

        //
        // Verify protection cleared
        //
        Status = Flash->ReadStatus (Flash, 1, &Sr);
        if (!EFI_ERROR (Status)) {
          Print (L"Flash status register after clear: 0x%02x\n", Sr);
        }
      }
    }
  }

  //
  // Erase the target region (4KB sectors, one at a time for progress)
  //
  EraseSize = (UINT32)((FileSize + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1));
  SectorsToErase = EraseSize / FLASH_SECTOR_SIZE;

  Print (L"Erasing %u KB at flash offset 0x%x...\n", EraseSize / 1024, FlashOffset);

  {
    UINT32  Idx;
    UINT32  LastPct = 0;

    for (Idx = 0; Idx < SectorsToErase; Idx++) {
      Status = Flash->Erase (Flash, FlashOffset + Idx * FLASH_SECTOR_SIZE, 1);
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
  // Program + verify in 4KB chunks (verify each chunk immediately)
  //
  Print (L"Programming %u bytes...\n", FileSize);

  {
    UINT8   *VerifyBuf;
    UINT32  Offset = 0;
    UINT32  LastPct = 0;

    VerifyBuf = AllocatePool (FLASH_SECTOR_SIZE);
    if (VerifyBuf == NULL) {
      Print (L"Error: Out of memory for verify buffer\n");
      FreePool (FileData);
      return EFI_OUT_OF_RESOURCES;
    }

    while (Offset < (UINT32)FileSize) {
      UINT32  Chunk = (UINT32)FileSize - Offset;
      if (Chunk > FLASH_SECTOR_SIZE) {
        Chunk = FLASH_SECTOR_SIZE;
      }

      Status = Flash->WriteData (Flash, FlashOffset + Offset, Chunk, FileData + Offset);
      if (EFI_ERROR (Status)) {
        Print (L"\nError: Write failed at offset 0x%x: %r\n", Offset, Status);
        FreePool (VerifyBuf);
        FreePool (FileData);
        return Status;
      }

      //
      // Verify this chunk immediately after writing
      //
      Status = Flash->ReadData (Flash, FlashOffset + Offset, Chunk, VerifyBuf);
      if (EFI_ERROR (Status)) {
        Print (L"\nError: Read-back failed at offset 0x%x: %r\n", Offset, Status);
        FreePool (VerifyBuf);
        FreePool (FileData);
        return EFI_DEVICE_ERROR;
      }

      if (CompareMem (FileData + Offset, VerifyBuf, Chunk) != 0) {
        UINT32  i;

        for (i = 0; i < Chunk; i++) {
          if (FileData[Offset + i] != VerifyBuf[i]) {
            Print (L"\nError: Mismatch at offset 0x%x: wrote 0x%02x, read 0x%02x\n",
                   Offset + i, FileData[Offset + i], VerifyBuf[i]);
            break;
          }
        }

        FreePool (VerifyBuf);
        FreePool (FileData);
        return EFI_DEVICE_ERROR;
      }

      Offset += Chunk;

      UINT32 Pct = Offset * 100 / (UINT32)FileSize;
      if (Pct != LastPct) {
        Print (L"\rProgramming... %3u%%", Pct);
        LastPct = Pct;
      }
    }

    FreePool (VerifyBuf);
    Print (L"\rProgramming... done    \n");
  }

  FreePool (FileData);
  Print (L"Success: %u bytes written to flash at 0x%x\n",
         FileSize, FlashOffset);

  return EFI_SUCCESS;
}
