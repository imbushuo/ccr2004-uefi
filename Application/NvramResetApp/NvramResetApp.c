/** @file
  NvramResetApp — erases the SPI flash NVRAM region, restoring defaults.

  The next boot will start with a fresh (empty) variable store.
  Can be launched from the UEFI Shell or from the Boot Manager menu.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SpiNorFlash.h>

#define NVRAM_SPI_OFFSET     0x00F80000U
#define NVRAM_SPI_SIZE       0x00080000U  // 512 KB
#define NVRAM_SECTOR_SIZE    0x00001000U  // 4 KB

EFI_STATUS
EFIAPI
NvramResetAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                  Status;
  EFI_SPI_NOR_FLASH_PROTOCOL  *SpiNor;
  UINT32                      SectorsToErase;
  UINT32                      Idx;
  UINT32                      LastPct;
  EFI_INPUT_KEY               Key;

  Print (L"NVRAM Reset Utility\n\n");
  Print (L"This will erase the NVRAM storage at SPI flash offset 0x%x (%u KB).\n",
         NVRAM_SPI_OFFSET, NVRAM_SPI_SIZE / 1024);
  Print (L"All saved UEFI variables will be lost. Continue? [y/N] ");

  //
  // Wait for keypress
  //
  Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &(UINTN){0});
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  Print (L"%c\n", Key.UnicodeChar);

  if (Key.UnicodeChar != L'y' && Key.UnicodeChar != L'Y') {
    Print (L"Aborted.\n");
    return EFI_ABORTED;
  }

  //
  // Locate SPI flash
  //
  Status = gBS->LocateProtocol (&gEfiSpiNorFlashProtocolGuid, NULL, (VOID **)&SpiNor);
  if (EFI_ERROR (Status)) {
    Print (L"Error: SPI flash not found: %r\n", Status);
    return Status;
  }

  //
  // Erase sector by sector with progress
  //
  SectorsToErase = NVRAM_SPI_SIZE / NVRAM_SECTOR_SIZE;
  LastPct = 0;

  Print (L"Erasing NVRAM (%u sectors)...\n", SectorsToErase);

  for (Idx = 0; Idx < SectorsToErase; Idx++) {
    Status = SpiNor->Erase (SpiNor, NVRAM_SPI_OFFSET + Idx * NVRAM_SECTOR_SIZE, 1);
    if (EFI_ERROR (Status)) {
      Print (L"\nError: Erase failed at sector %u: %r\n", Idx, Status);
      return Status;
    }

    UINT32 Pct = (Idx + 1) * 100 / SectorsToErase;
    if (Pct != LastPct) {
      Print (L"\rErasing... %3u%%", Pct);
      LastPct = Pct;
    }
  }

  Print (L"\rErasing... done    \n");
  Print (L"\nNVRAM erased. Please reboot for changes to take effect.\n");

  return EFI_SUCCESS;
}
