/** @file
  FlashApp — writes images to SPI NOR flash.

  Usage:
    FlashApp [-slot 0|1] <uefi.elf>       (ELF mode: validate AArch64 ELF)
    FlashApp -raw <hex_offset> <file.bin>  (Raw mode: write file at given offset)

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

#define FLASH_SLOT0_OFFSET    0x00800000U
#define FLASH_SLOT1_OFFSET    0x00C00000U
#define FLASH_SLOT_SIZE       0x00400000U
#define FLASH_SECTOR_SIZE     0x1000U
#define FLASH_SIZE            (16U * 1024U * 1024U)
#define FLASH_MAX_IMAGE_SIZE  (FLASH_SLOT_SIZE - 0x10000U)

#define ELF_MAGIC             0x464C457FU
#define ELF_CLASS64           2
#define ELF_DATA_LSB          1
#define ELF_MACHINE_AARCH64   183

#pragma pack(1)
typedef struct {
  UINT8   e_ident[16];
  UINT16  e_type;
  UINT16  e_machine;
  UINT32  e_version;
  UINT64  e_entry;
} ELF64_EHDR_SHORT;
#pragma pack()

extern EFI_GUID  gEfiSpiNorFlashProtocolGuid;

/* ---- Helpers ---- */

STATIC
VOID
PrintUsage (
  VOID
  )
{
  Print (L"Usage:\n");
  Print (L"  FlashApp [-slot 0|1] <uefi.elf>       Write AArch64 ELF to firmware slot\n");
  Print (L"  FlashApp -raw <hex_offset> <file.bin>  Write raw binary at flash offset\n");
  Print (L"\n");
  Print (L"  Slot 0 (default): flash 0x%x    Slot 1: flash 0x%x\n",
         FLASH_SLOT0_OFFSET, FLASH_SLOT1_OFFSET);
}

STATIC
EFI_STATUS
ReadFile (
  IN  CHAR16  *FileName,
  OUT UINT8   **Data,
  OUT UINTN   *Size
  )
{
  EFI_STATUS       Status;
  SHELL_FILE_HANDLE Handle;
  EFI_FILE_INFO    *Info;
  UINTN            ReadSize;

  Status = ShellOpenFileByName (FileName, &Handle, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) return Status;

  Info = ShellGetFileInfo (Handle);
  if (Info == NULL) { ShellCloseFile (&Handle); return EFI_DEVICE_ERROR; }

  *Size = (UINTN)Info->FileSize;
  FreePool (Info);

  *Data = AllocatePool (*Size);
  if (*Data == NULL) { ShellCloseFile (&Handle); return EFI_OUT_OF_RESOURCES; }

  ReadSize = *Size;
  Status = ShellReadFile (Handle, &ReadSize, *Data);
  ShellCloseFile (&Handle);

  if (EFI_ERROR (Status) || ReadSize != *Size) {
    FreePool (*Data);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
UINTN
HexStrToUintn (
  IN CHAR16  *Str
  )
{
  UINTN  Val = 0;

  if (Str[0] == L'0' && (Str[1] == L'x' || Str[1] == L'X')) {
    Str += 2;
  }

  while (*Str) {
    CHAR16 C = *Str++;
    Val <<= 4;
    if (C >= L'0' && C <= L'9')      Val |= C - L'0';
    else if (C >= L'a' && C <= L'f') Val |= C - L'a' + 10;
    else if (C >= L'A' && C <= L'F') Val |= C - L'A' + 10;
    else break;
  }

  return Val;
}

/**
  Erase, program, and verify data to flash with progress reporting.
**/
STATIC
EFI_STATUS
FlashWriteVerify (
  IN EFI_SPI_NOR_FLASH_PROTOCOL  *Flash,
  IN UINT32                      FlashOffset,
  IN UINT8                       *Data,
  IN UINT32                      DataSize
  )
{
  EFI_STATUS  Status;
  UINT32      EraseSize;
  UINT32      SectorsToErase;
  UINT8       *VerifyBuf;
  UINT32      Offset;
  UINT32      LastPct;

  EraseSize = (DataSize + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
  SectorsToErase = EraseSize / FLASH_SECTOR_SIZE;

  /* Erase */
  Print (L"Erasing %u KB at flash offset 0x%x...\n", EraseSize / 1024, FlashOffset);
  LastPct = 0;
  for (UINT32 Idx = 0; Idx < SectorsToErase; Idx++) {
    Status = Flash->Erase (Flash, FlashOffset + Idx * FLASH_SECTOR_SIZE, 1);
    if (EFI_ERROR (Status)) {
      Print (L"\nError: Erase failed at sector %u: %r\n", Idx, Status);
      return Status;
    }
    UINT32 Pct = (Idx + 1) * 100 / SectorsToErase;
    if (Pct != LastPct) { Print (L"\rErasing... %3u%%", Pct); LastPct = Pct; }
  }
  Print (L"\rErasing... done    \n");

  /* Program + verify */
  Print (L"Programming %u bytes...\n", DataSize);
  VerifyBuf = AllocatePool (FLASH_SECTOR_SIZE);
  if (VerifyBuf == NULL) return EFI_OUT_OF_RESOURCES;

  Offset = 0;
  LastPct = 0;
  while (Offset < DataSize) {
    UINT32 Chunk = DataSize - Offset;
    if (Chunk > FLASH_SECTOR_SIZE) Chunk = FLASH_SECTOR_SIZE;

    Status = Flash->WriteData (Flash, FlashOffset + Offset, Chunk, Data + Offset);
    if (EFI_ERROR (Status)) {
      Print (L"\nError: Write failed at offset 0x%x: %r\n", Offset, Status);
      FreePool (VerifyBuf);
      return Status;
    }

    Status = Flash->ReadData (Flash, FlashOffset + Offset, Chunk, VerifyBuf);
    if (EFI_ERROR (Status)) {
      Print (L"\nError: Read-back failed at offset 0x%x: %r\n", Offset, Status);
      FreePool (VerifyBuf);
      return EFI_DEVICE_ERROR;
    }

    if (CompareMem (Data + Offset, VerifyBuf, Chunk) != 0) {
      for (UINT32 i = 0; i < Chunk; i++) {
        if (Data[Offset + i] != VerifyBuf[i]) {
          Print (L"\nError: Mismatch at offset 0x%x: wrote 0x%02x, read 0x%02x\n",
                 Offset + i, Data[Offset + i], VerifyBuf[i]);
          break;
        }
      }
      FreePool (VerifyBuf);
      return EFI_DEVICE_ERROR;
    }

    Offset += Chunk;
    UINT32 Pct = Offset * 100 / DataSize;
    if (Pct != LastPct) { Print (L"\rProgramming... %3u%%", Pct); LastPct = Pct; }
  }

  FreePool (VerifyBuf);
  Print (L"\rProgramming... done    \n");
  return EFI_SUCCESS;
}

/* ---- Entry point ---- */

EFI_STATUS
EFIAPI
FlashAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_SHELL_PARAMETERS_PROTOCOL  *ShellParams;
  EFI_SPI_NOR_FLASH_PROTOCOL     *Flash;
  UINT8                          *FileData;
  UINTN                          FileSize;
  UINT32                         FlashOffset;

  Status = gBS->HandleProtocol (
                  ImageHandle, &gEfiShellParametersProtocolGuid, (VOID **)&ShellParams);
  if (EFI_ERROR (Status) || ShellParams->Argc < 2) {
    PrintUsage ();
    return EFI_INVALID_PARAMETER;
  }

  /* Locate flash protocol */
  Status = gBS->LocateProtocol (&gEfiSpiNorFlashProtocolGuid, NULL, (VOID **)&Flash);
  if (EFI_ERROR (Status)) {
    Print (L"Error: SPI NOR flash protocol not found: %r\n", Status);
    return Status;
  }

  /* ---- Raw mode: FlashApp -raw <hex_offset> <file.bin> ---- */
  if (StrCmp (ShellParams->Argv[1], L"-raw") == 0) {
    if (ShellParams->Argc < 4) {
      Print (L"Usage: FlashApp -raw <hex_offset> <file.bin>\n");
      return EFI_INVALID_PARAMETER;
    }

    FlashOffset = (UINT32)HexStrToUintn (ShellParams->Argv[2]);

    Print (L"Raw mode: flash offset 0x%x\n", FlashOffset);
    Print (L"Reading %s...\n", ShellParams->Argv[3]);

    Status = ReadFile (ShellParams->Argv[3], &FileData, &FileSize);
    if (EFI_ERROR (Status)) {
      Print (L"Error: Failed to read file: %r\n", Status);
      return Status;
    }

    Print (L"File size: %u bytes\n", FileSize);

    if (FlashOffset + (UINT32)FileSize > FLASH_SIZE) {
      Print (L"Error: Write would exceed flash size (0x%x)\n", FLASH_SIZE);
      FreePool (FileData);
      return EFI_INVALID_PARAMETER;
    }

    if ((FlashOffset & (FLASH_SECTOR_SIZE - 1)) != 0) {
      Print (L"Error: Offset must be 4KB-sector aligned\n");
      FreePool (FileData);
      return EFI_INVALID_PARAMETER;
    }

    Status = FlashWriteVerify (Flash, FlashOffset, FileData, (UINT32)FileSize);
    FreePool (FileData);

    if (!EFI_ERROR (Status)) {
      Print (L"Success: %u bytes written to flash at 0x%x\n", FileSize, FlashOffset);
    }

    return Status;
  }

  /* ---- ELF mode: FlashApp [-slot 0|1] <uefi.elf> ---- */
  {
    UINTN   Slot = 0;
    UINTN   ArgIdx = 1;
    CHAR16  *FileName;

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

    Print (L"ELF mode: slot %u @ flash offset 0x%x\n", Slot, FlashOffset);
    Print (L"Reading %s...\n", FileName);

    Status = ReadFile (FileName, &FileData, &FileSize);
    if (EFI_ERROR (Status)) {
      Print (L"Error: Failed to read file: %r\n", Status);
      return Status;
    }

    Print (L"File size: %u bytes\n", FileSize);

    /* Validate ELF */
    if (FileSize < sizeof (ELF64_EHDR_SHORT)) {
      Print (L"Error: File too small for ELF header\n");
      FreePool (FileData);
      return EFI_INVALID_PARAMETER;
    }

    {
      ELF64_EHDR_SHORT *Ehdr = (ELF64_EHDR_SHORT *)FileData;

      if (*(UINT32 *)Ehdr->e_ident != ELF_MAGIC) {
        Print (L"Error: Not an ELF file\n");
        FreePool (FileData);
        return EFI_INVALID_PARAMETER;
      }
      if (Ehdr->e_ident[4] != ELF_CLASS64 || Ehdr->e_ident[5] != ELF_DATA_LSB) {
        Print (L"Error: Not a 64-bit little-endian ELF\n");
        FreePool (FileData);
        return EFI_INVALID_PARAMETER;
      }
      if (Ehdr->e_machine != ELF_MACHINE_AARCH64) {
        Print (L"Error: Not AArch64 (machine=%u)\n", Ehdr->e_machine);
        FreePool (FileData);
        return EFI_INVALID_PARAMETER;
      }
      if (FileSize >= FLASH_MAX_IMAGE_SIZE) {
        Print (L"Error: Too large (%u bytes, max %u)\n", FileSize, FLASH_MAX_IMAGE_SIZE);
        FreePool (FileData);
        return EFI_INVALID_PARAMETER;
      }

      Print (L"ELF validated: AArch64, entry 0x%lx\n", Ehdr->e_entry);
    }

    Status = FlashWriteVerify (Flash, FlashOffset, FileData, (UINT32)FileSize);
    FreePool (FileData);

    if (!EFI_ERROR (Status)) {
      Print (L"Success: %u bytes written to flash at 0x%x\n", FileSize, FlashOffset);
    }

    return Status;
  }
}
