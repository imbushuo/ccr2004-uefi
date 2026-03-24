/** @file
  Device Tree loader with MAC address patching.

  Replaces the upstream DtPlatformDxe for CCR2004.  Loads the DTB blob,
  queries the BoardInfo protocol for the board MAC address, patches
  the root-level "mac-address" property in the DTB, then installs the
  FDT configuration table.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DtPlatformDtbLoaderLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Guid/Fdt.h>
#include <Protocol/BoardInfo.h>

/**
  Patch the root "mac-address" property with the real board MAC.

  @param[in,out] Dtb        Writable DTB blob (must have been opened
                              with FdtOpenInto with extra space).
  @param[in]     MacAddr    6-byte MAC address.
**/
STATIC
VOID
PatchMacAddress (
  IN OUT VOID              *Dtb,
  IN     EFI_MAC_ADDRESS   *MacAddr
  )
{
  INT32  RootNode;
  INT32  Err;

  RootNode = 0;  // root is always node offset 0

  Err = FdtSetProp (Dtb, RootNode, "mac-address", MacAddr->Addr, 6);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "DtPatchDxe: failed to set mac-address (err=%d)\n", Err));
  } else {
    DEBUG ((DEBUG_INFO, "DtPatchDxe: mac-address set to %02x:%02x:%02x:%02x:%02x:%02x\n",
            MacAddr->Addr[0], MacAddr->Addr[1], MacAddr->Addr[2],
            MacAddr->Addr[3], MacAddr->Addr[4], MacAddr->Addr[5]));
  }
}

/**
  Entry point.
**/
EFI_STATUS
EFIAPI
DtPatchDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                      Status;
  VOID                            *Dtb;
  UINTN                           DtbSize;
  VOID                            *PatchedDtb;
  UINTN                           PatchedSize;
  MIKROTIK_BOARD_INFO_PROTOCOL    *BoardInfo;
  EFI_MAC_ADDRESS                 MacAddr;

  //
  // Load the platform DTB blob (from FV FREEFORM file)
  //
  Dtb = NULL;
  Status = DtPlatformLoadDtb (&Dtb, &DtbSize);
  if (EFI_ERROR (Status) || Dtb == NULL) {
    DEBUG ((DEBUG_ERROR, "DtPatchDxe: failed to load DTB: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DtPatchDxe: DTB loaded (%u bytes)\n", DtbSize));

  //
  // Open into a larger buffer so we can modify properties
  //
  PatchedSize = DtbSize + 256;
  PatchedDtb = AllocatePool (PatchedSize);
  if (PatchedDtb == NULL) {
    FreePool (Dtb);
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (Dtb, PatchedDtb, (INT32)PatchedSize) != 0) {
    DEBUG ((DEBUG_ERROR, "DtPatchDxe: FdtOpenInto failed\n"));
    FreePool (PatchedDtb);
    FreePool (Dtb);
    return EFI_DEVICE_ERROR;
  }

  FreePool (Dtb);

  //
  // Query BoardInfo for the MAC address and patch the DTB
  //
  Status = gBS->LocateProtocol (
                  &gMikroTikBoardInfoProtocolGuid,
                  NULL,
                  (VOID **)&BoardInfo
                  );
  if (!EFI_ERROR (Status)) {
    Status = BoardInfo->GetMacAddress (BoardInfo, &MacAddr);
    if (!EFI_ERROR (Status)) {
      PatchMacAddress (PatchedDtb, &MacAddr);
    } else {
      DEBUG ((DEBUG_WARN, "DtPatchDxe: GetMacAddress failed: %r\n", Status));
    }
  } else {
    DEBUG ((DEBUG_WARN, "DtPatchDxe: BoardInfo protocol not found: %r\n", Status));
  }

  FdtPack (PatchedDtb);

  //
  // Install as FDT configuration table
  //
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, PatchedDtb);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DtPatchDxe: InstallConfigurationTable failed: %r\n", Status));
    FreePool (PatchedDtb);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DtPatchDxe: FDT configuration table installed\n"));
  return EFI_SUCCESS;
}
