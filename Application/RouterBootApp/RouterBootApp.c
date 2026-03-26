/** @file
  RouterBootApp — chainloads the MikroTik RouterBoot bootloader.

  Loads the RouterBoot binary from the firmware volume, retrieves the
  DTB from the system configuration table, exits boot services, disables
  MMU/caches/interrupts, relocates RouterBoot to 0x00110000, and jumps
  to it with the DTB address in x0.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/Fdt.h>
#include <Pi/PiFirmwareVolume.h>
#include <Pi/PiFirmwareFile.h>
#include <Protocol/FirmwareVolume2.h>

//
// RouterBoot binary GUID — matches the FILE FREEFORM in the FDF
//
#define ROUTERBOOT_BIN_GUID \
  { 0x7d3a1b2e, 0xc5f8, 0x4a69, { 0xb0, 0x12, 0x4e, 0x8c, 0x6d, 0x1f, 0x3a, 0x95 } }

STATIC EFI_GUID  mRouterBootBinGuid = ROUTERBOOT_BIN_GUID;

//
// Assembly trampoline: copies binary to 0x00110000, flushes caches, jumps
//
extern VOID RouterBootTrampoline (
  IN UINT64  DtbAddress,
  IN UINT64  SourceAddress,
  IN UINT64  ImageSize
  );

/**
  Find the DTB in the EFI system configuration table.
**/
STATIC
VOID *
FindDtb (
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
  Load RouterBoot binary from the firmware volume.
**/
STATIC
EFI_STATUS
LoadRouterBootFromFv (
  OUT VOID   **ImageData,
  OUT UINTN  *ImageSize
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *Handles;
  UINTN                          HandleCount;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
  VOID                           *Buffer;
  UINTN                          BufferSize;
  EFI_FV_FILETYPE                FileType;
  UINT32                         AuthStatus;
  EFI_FV_FILE_ATTRIBUTES         Attrs;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) continue;

    Buffer = NULL;
    BufferSize = 0;
    FileType = EFI_FV_FILETYPE_FREEFORM;

    Status = Fv->ReadFile (
                   Fv,
                   &mRouterBootBinGuid,
                   &Buffer,
                   &BufferSize,
                   &FileType,
                   &Attrs,
                   &AuthStatus
                   );
    if (!EFI_ERROR (Status) && Buffer != NULL && BufferSize > 0) {
      //
      // ReadFile with Buffer=NULL allocates and returns the raw file content.
      // For FREEFORM files this includes section headers. We need to find the
      // RAW section within.
      //
      Status = Fv->ReadSection (
                     Fv,
                     &mRouterBootBinGuid,
                     EFI_SECTION_RAW,
                     0,
                     &Buffer,
                     &BufferSize,
                     &AuthStatus
                     );
      if (!EFI_ERROR (Status)) {
        *ImageData = Buffer;
        *ImageSize = BufferSize;
        FreePool (Handles);
        return EFI_SUCCESS;
      }
    }
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

/**
  Entry point.
**/
EFI_STATUS
EFIAPI
RouterBootAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *RouterBootBin;
  UINTN       RouterBootSize;
  VOID        *Dtb;
  VOID        *DtbCopy;
  UINTN       DtbSize;
  UINTN       MemoryMapSize;
  EFI_MEMORY_DESCRIPTOR  *MemoryMap;
  UINTN       MapKey;
  UINTN       DescriptorSize;
  UINT32      DescriptorVersion;

  Print (L"RouterBootApp: loading RouterBoot binary...\n");

  //
  // Load RouterBoot from firmware volume
  //
  Status = LoadRouterBootFromFv (&RouterBootBin, &RouterBootSize);
  if (EFI_ERROR (Status)) {
    Print (L"Error: RouterBoot binary not found in FV: %r\n", Status);
    return Status;
  }

  Print (L"RouterBootApp: binary loaded (%u bytes)\n", RouterBootSize);

  //
  // Find and copy DTB to EfiLoaderData so it survives ExitBootServices
  //
  Dtb = FindDtb ();
  if (Dtb == NULL) {
    Print (L"Error: DTB not found in configuration table\n");
    FreePool (RouterBootBin);
    return EFI_NOT_FOUND;
  }

  {
    //
    // FdtTotalSize requires FdtLib — use a simple read instead
    //
    UINT32  TotalSize;
    UINT8   *DtbBytes = (UINT8 *)Dtb;

    //
    // DTB header: bytes 4-7 = totalsize (big-endian)
    //
    TotalSize = ((UINT32)DtbBytes[4] << 24) | ((UINT32)DtbBytes[5] << 16) |
                ((UINT32)DtbBytes[6] << 8)  | (UINT32)DtbBytes[7];
    DtbSize = TotalSize;
  }

  //
  // Allocate DTB copy in EfiLoaderData pages (survives ExitBootServices)
  //
  {
    EFI_PHYSICAL_ADDRESS  DtbPages;
    UINTN                 PageCount;

    PageCount = EFI_SIZE_TO_PAGES (DtbSize);
    Status = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, PageCount, &DtbPages);
    if (EFI_ERROR (Status)) {
      Print (L"Error: Failed to allocate DTB pages: %r\n", Status);
      FreePool (RouterBootBin);
      return Status;
    }

    DtbCopy = (VOID *)(UINTN)DtbPages;
    CopyMem (DtbCopy, Dtb, DtbSize);
  }

  Print (L"RouterBootApp: DTB at 0x%lx (%u bytes)\n", (UINT64)(UINTN)DtbCopy, DtbSize);

  //
  // Also copy RouterBoot binary to EfiLoaderData pages
  //
  {
    EFI_PHYSICAL_ADDRESS  RbPages;
    UINTN                 PageCount;
    VOID                  *RbCopy;

    PageCount = EFI_SIZE_TO_PAGES (RouterBootSize);
    Status = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, PageCount, &RbPages);
    if (EFI_ERROR (Status)) {
      Print (L"Error: Failed to allocate RouterBoot pages: %r\n", Status);
      FreePool (RouterBootBin);
      return Status;
    }

    RbCopy = (VOID *)(UINTN)RbPages;
    CopyMem (RbCopy, RouterBootBin, RouterBootSize);
    FreePool (RouterBootBin);
    RouterBootBin = RbCopy;
  }

  Print (L"RouterBootApp: exiting boot services...\n");

  //
  // Exit boot services
  //
  MemoryMapSize = 0;
  MemoryMap = NULL;
  Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    MemoryMapSize += 2 * DescriptorSize;
    MemoryMap = AllocatePool (MemoryMapSize);
    if (MemoryMap == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->ExitBootServices (ImageHandle, MapKey);
  if (EFI_ERROR (Status)) {
    //
    // Retry: memory map may have changed
    //
    MemoryMapSize = 0;
    Status = gBS->GetMemoryMap (&MemoryMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
    MemoryMapSize += 2 * DescriptorSize;
    MemoryMap = AllocatePool (MemoryMapSize);
    Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    Status = gBS->ExitBootServices (ImageHandle, MapKey);
  }

  //
  // Past this point: no UEFI services available.
  // The assembly trampoline handles MMU disable, cache flush,
  // interrupt masking, relocation, and jump.
  //
  RouterBootTrampoline (
    (UINT64)(UINTN)DtbCopy,
    (UINT64)(UINTN)RouterBootBin,
    (UINT64)RouterBootSize
    );

  // Never reached
  CpuDeadLoop ();
  return EFI_SUCCESS;
}
