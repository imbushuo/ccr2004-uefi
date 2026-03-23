/** @file
  Kernel boot — LoadImage, DTB initrd patching, StartImage.

  MikroTik's kernel EFI stub does not use the LoadFile2 initrd protocol.
  Instead, it reads linux,initrd-start / linux,initrd-end from the DTB
  /chosen node.  We patch the installed DTB configuration table with
  the initrd memory address before starting the kernel.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"
#include <Library/FdtLib.h>
#include <Guid/Fdt.h>

//
// Hardcoded command line for RouterOS
//
STATIC CONST CHAR16 mCmdLine[] =
  L"console=ttyS0,115200 benand_no_swecc=2 bootimage=1 yaffs.inband_tags=1 "
  L"parts=1 arm64=Y board=UEFI_CCR2004-1G-2XS-PCIe ver=2026.317.308 "
  L"bver=2026.317.308 hw_opt=00100001 boot=1 mlc=12 loglevel=7 "
  L"earlycon=uart8250,mmio32,0xfd883000,115200n8 earlyprintk verbose";

/**
  Find the installed DTB in the EFI system configuration table.
**/
STATIC
VOID *
FindInstalledDtb (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gFdtTableGuid)) {
      return gST->ConfigurationTable[Index].VendorTable;
    }
  }

  return NULL;
}

/**
  Patch the DTB /chosen node with initrd memory location and reinstall
  the configuration table so the kernel EFI stub picks it up.

  @param[in] InitrdStart  Physical address of initrd data.
  @param[in] InitrdEnd    Physical address just past initrd data.
**/
STATIC
EFI_STATUS
PatchDtbInitrd (
  IN UINT64  InitrdStart,
  IN UINT64  InitrdEnd
  )
{
  VOID        *OrigDtb;
  VOID        *NewDtb;
  UINTN       DtbSize;
  UINTN       NewSize;
  INT32       Chosen;
  INT32       Err;
  EFI_STATUS  Status;

  OrigDtb = FindInstalledDtb ();
  if (OrigDtb == NULL) {
    DEBUG ((DEBUG_WARN, "[RouterOS] No DTB found in configuration table\n"));
    return EFI_NOT_FOUND;
  }

  Err = FdtCheckHeader (OrigDtb);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Invalid DTB header (err=%d)\n", Err));
    return EFI_COMPROMISED_DATA;
  }

  DtbSize = FdtTotalSize (OrigDtb);
  NewSize = DtbSize + 256;  // extra space for new properties

  NewDtb = AllocatePool (NewSize);
  if (NewDtb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Err = FdtOpenInto (OrigDtb, NewDtb, (INT32)NewSize);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] FdtOpenInto failed (err=%d)\n", Err));
    FreePool (NewDtb);
    return EFI_DEVICE_ERROR;
  }

  //
  // Find or create /chosen
  //
  Chosen = FdtPathOffset (NewDtb, "/chosen");
  if (Chosen < 0) {
    Chosen = FdtAddSubnode (NewDtb, 0, "chosen");
    if (Chosen < 0) {
      DEBUG ((DEBUG_WARN, "[RouterOS] Cannot create /chosen (err=%d)\n", Chosen));
      FreePool (NewDtb);
      return EFI_DEVICE_ERROR;
    }
  }

  //
  // Set initrd location properties
  //
  Err = FdtSetPropU64 (NewDtb, Chosen, "linux,initrd-start", InitrdStart);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Cannot set initrd-start (err=%d)\n", Err));
    FreePool (NewDtb);
    return EFI_DEVICE_ERROR;
  }

  Err = FdtSetPropU64 (NewDtb, Chosen, "linux,initrd-end", InitrdEnd);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Cannot set initrd-end (err=%d)\n", Err));
    FreePool (NewDtb);
    return EFI_DEVICE_ERROR;
  }

  FdtPack (NewDtb);

  DEBUG ((DEBUG_WARN, "[RouterOS] DTB patched: initrd @ 0x%lx-0x%lx\n",
          InitrdStart, InitrdEnd));

  //
  // Reinstall as configuration table (replaces existing DTB entry)
  //
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, NewDtb);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] InstallConfigurationTable failed: %r\n", Status));
    FreePool (NewDtb);
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Boot the Linux kernel via EFI stub.
**/
EFI_STATUS
BootLinuxKernel (
  IN EFI_HANDLE  ImageHandle,
  IN UINT8       *KernelBuffer,
  IN UINTN       KernelSize,
  IN UINT8       *InitrdData,
  IN UINTN       InitrdSize
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 KernelHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  UINTN                      ExitDataSize;
  CHAR16                     *ExitData;

  //
  // Copy initrd into EfiLoaderData memory so it survives ExitBootServices.
  // The original buffer from AllocatePool is EfiBootServicesData which gets
  // reclaimed when the kernel calls ExitBootServices.
  //
  if (InitrdData != NULL && InitrdSize > 0) {
    EFI_PHYSICAL_ADDRESS  InitrdPages;
    UINTN                 PageCount;
    UINT8                 *InitrdCopy;

    PageCount = EFI_SIZE_TO_PAGES (InitrdSize);
    Status = gBS->AllocatePages (
                    AllocateAnyPages,
                    EfiLoaderData,
                    PageCount,
                    &InitrdPages
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[RouterOS] Failed to allocate initrd pages: %r\n", Status));
      return Status;
    }

    InitrdCopy = (UINT8 *)(UINTN)InitrdPages;
    CopyMem (InitrdCopy, InitrdData, InitrdSize);

    DEBUG ((DEBUG_WARN, "[RouterOS] Initrd copied to EfiLoaderData at 0x%lx (%u bytes)\n",
            InitrdPages, InitrdSize));

    Status = PatchDtbInitrd (
               InitrdPages,
               InitrdPages + InitrdSize
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[RouterOS] DTB initrd patch failed: %r (continuing without)\n",
              Status));
    }
  }

  //
  // Load the kernel image from memory
  //
  DEBUG ((DEBUG_WARN, "[RouterOS] Loading kernel image (%u bytes)...\n", KernelSize));

  KernelHandle = NULL;
  Status = gBS->LoadImage (
                  TRUE,
                  ImageHandle,
                  NULL,
                  KernelBuffer,
                  KernelSize,
                  &KernelHandle
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] LoadImage failed: %r\n", Status));
    return Status;
  }

  //
  // Set the command line via LoadedImage protocol
  //
  Status = gBS->HandleProtocol (
                  KernelHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] HandleProtocol(LoadedImage) failed: %r\n", Status));
    return Status;
  }

  LoadedImage->LoadOptions     = (VOID *)mCmdLine;
  LoadedImage->LoadOptionsSize = sizeof (mCmdLine);

  DEBUG ((DEBUG_WARN, "[RouterOS] Command line: \"%s\"\n", mCmdLine));
  DEBUG ((DEBUG_WARN, "[RouterOS] Starting kernel...\n"));

  //
  // Start the kernel — does not return on success
  //
  ExitDataSize = 0;
  ExitData     = NULL;
  Status = gBS->StartImage (KernelHandle, &ExitDataSize, &ExitData);

  DEBUG ((DEBUG_WARN, "[RouterOS] StartImage returned: %r\n", Status));
  if (ExitData != NULL) {
    FreePool (ExitData);
  }

  return Status;
}
