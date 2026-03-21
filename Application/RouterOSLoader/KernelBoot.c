/** @file
  Kernel boot — LoadImage, LoadFile2 initrd protocol, StartImage.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"

//
// Hardcoded command line for RouterOS
//
STATIC CONST CHAR16 mCmdLine[] =
  L"console=ttyS0,115200 benand_no_swecc=2 bootimage=1 yaffs.inband_tags=1 "
  L"parts=1 arm64=Y board=UEFI_CCR2004-1G-2XS-PCIe ver=2026.317.308 "
  L"bver=2026.317.308 hw_opt=00100001 boot=1 mlc=12 loglevel=7 "
  L"earlycon=uart8250,mmio32,0xfd883000,115200n8 earlyprintk verbose";

//
// LoadFile2 initrd protocol state
//
STATIC UINT8  *mInitrdBuffer;
STATIC UINTN  mInitrdSize;

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VenMediaNode;
  EFI_DEVICE_PATH_PROTOCOL    EndNode;
} INITRD_VENDOR_DEVPATH;
#pragma pack()

STATIC CONST INITRD_VENDOR_DEVPATH mInitrdDevicePath = {
  {
    {
      MEDIA_DEVICE_PATH,
      MEDIA_VENDOR_DP,
      { sizeof (VENDOR_DEVICE_PATH) }
    },
    LINUX_EFI_INITRD_MEDIA_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL) }
  }
};

STATIC
EFI_STATUS
EFIAPI
InitrdLoadFile2 (
  IN      EFI_LOAD_FILE2_PROTOCOL   *This,
  IN      EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN      BOOLEAN                   BootPolicy,
  IN  OUT UINTN                     *BufferSize,
  OUT     VOID                      *Buffer     OPTIONAL
  )
{
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }

  if (BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Buffer == NULL || *BufferSize < mInitrdSize) {
    *BufferSize = mInitrdSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, mInitrdBuffer, mInitrdSize);
  *BufferSize = mInitrdSize;
  return EFI_SUCCESS;
}

STATIC EFI_LOAD_FILE2_PROTOCOL mLoadFile2Protocol = {
  InitrdLoadFile2
};

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
  EFI_HANDLE                 InitrdHandle;
  EFI_HANDLE                 KernelHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  UINTN                      ExitDataSize;
  CHAR16                     *ExitData;

  InitrdHandle = NULL;

  //
  // Install LoadFile2 protocol for initrd if we have one
  //
  if (InitrdData != NULL && InitrdSize > 0) {
    mInitrdBuffer = InitrdData;
    mInitrdSize   = InitrdSize;

    DEBUG ((DEBUG_WARN, "[RouterOS] Installing initrd LoadFile2 protocol (%u bytes)\n",
            InitrdSize));

    Status = gBS->InstallMultipleProtocolInterfaces (
                    &InitrdHandle,
                    &gEfiDevicePathProtocolGuid,
                    &mInitrdDevicePath,
                    &gEfiLoadFile2ProtocolGuid,
                    &mLoadFile2Protocol,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[RouterOS] Failed to install initrd protocol: %r\n", Status));
      return Status;
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
    goto Cleanup;
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
    goto Cleanup;
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
    DEBUG ((DEBUG_WARN, "[RouterOS] Exit data: %s\n", ExitData));
    FreePool (ExitData);
  }

Cleanup:
  if (InitrdHandle != NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           InitrdHandle,
           &gEfiDevicePathProtocolGuid,
           &mInitrdDevicePath,
           &gEfiLoadFile2ProtocolGuid,
           &mLoadFile2Protocol,
           NULL
           );
  }

  return Status;
}
