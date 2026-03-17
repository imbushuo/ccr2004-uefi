/** @file
  MikroTik YAFFS2 filesystem driver — entry point and driver binding.

  UEFI_DRIVER that binds to handles with MIKROTIK_NAND_FLASH_PROTOCOL,
  detects YAFFS2 filesystem, scans NAND, and installs SimpleFileSystem.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"

//
// Driver Binding Protocol instance
//
STATIC EFI_DRIVER_BINDING_PROTOCOL  mMtYaffs2DriverBinding;

/**
  Test whether this driver supports ControllerHandle.
**/
STATIC
EFI_STATUS
EFIAPI
MtYaffs2Supported (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS                       Status;
  MIKROTIK_NAND_FLASH_PROTOCOL     *Nand;

  //
  // Just check if NandFlash protocol is present and can be claimed.
  // Actual YAFFS2 detection happens in Start() during the scan —
  // avoids reading hundreds of NAND pages twice.
  //
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gMikroTikNandFlashProtocolGuid,
                  (VOID **)&Nand,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->CloseProtocol (
         ControllerHandle,
         &gMikroTikNandFlashProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return EFI_SUCCESS;
}

/**
  Start managing ControllerHandle — scan NAND and install SimpleFileSystem.
**/
STATIC
EFI_STATUS
EFIAPI
MtYaffs2Start (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS                       Status;
  MIKROTIK_NAND_FLASH_PROTOCOL     *Nand;
  YAFFS2_VOLUME                    *Volume;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gMikroTikNandFlashProtocolGuid,
                  (VOID **)&Nand,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Yaffs2AllocateVolume (ControllerHandle, Nand, &Volume);
  if (EFI_ERROR (Status)) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gMikroTikNandFlashProtocolGuid,
           This->DriverBindingHandle,
           ControllerHandle
           );
  }

  return Status;
}

/**
  Stop managing ControllerHandle — uninstall SimpleFileSystem and free resources.
**/
STATIC
EFI_STATUS
EFIAPI
MtYaffs2Stop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS                          Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     *Sfs;
  YAFFS2_VOLUME                       *Volume;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Sfs,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Volume = YAFFS2_VOLUME_FROM_SFS (Sfs);
  Yaffs2FreeVolume (Volume);

  gBS->CloseProtocol (
         ControllerHandle,
         &gMikroTikNandFlashProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return EFI_SUCCESS;
}

/**
  Driver entry point — install driver binding protocol.
**/
EFI_STATUS
EFIAPI
MtYaffs2EntryPoint (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  mMtYaffs2DriverBinding.Supported           = MtYaffs2Supported;
  mMtYaffs2DriverBinding.Start               = MtYaffs2Start;
  mMtYaffs2DriverBinding.Stop                = MtYaffs2Stop;
  mMtYaffs2DriverBinding.Version             = 0x10;
  mMtYaffs2DriverBinding.ImageHandle         = ImageHandle;
  mMtYaffs2DriverBinding.DriverBindingHandle = ImageHandle;

  return gBS->InstallMultipleProtocolInterfaces (
                &ImageHandle,
                &gEfiDriverBindingProtocolGuid,
                &mMtYaffs2DriverBinding,
                NULL
                );
}
