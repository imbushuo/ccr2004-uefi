/** @file
  MikroTik YAFFS2 filesystem driver — volume lifecycle and OpenVolume.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"
#include <Library/TimerLib.h>

/**
  Allocate and initialize a YAFFS2 volume, scan the NAND, and install
  the SimpleFileSystem protocol on the given handle.

  @param[in]  Handle     Controller handle (with NandFlash protocol).
  @param[in]  Nand       NAND flash protocol instance.
  @param[out] VolumeOut  On success, the allocated volume.

  @retval EFI_SUCCESS  Volume ready, SFS protocol installed.
**/
EFI_STATUS
Yaffs2AllocateVolume (
  IN  EFI_HANDLE                     Handle,
  IN  MIKROTIK_NAND_FLASH_PROTOCOL   *Nand,
  OUT YAFFS2_VOLUME                  **VolumeOut
  )
{
  EFI_STATUS     Status;
  YAFFS2_VOLUME  *Volume;

  Volume = AllocateZeroPool (sizeof (YAFFS2_VOLUME));
  if (Volume == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Volume->Signature = YAFFS2_VOLUME_SIGNATURE;
  Volume->Handle    = Handle;
  Volume->Nand      = Nand;

  Volume->PageBuffer = AllocatePool (Nand->PageSize);
  if (Volume->PageBuffer == NULL) {
    FreePool (Volume);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Scan NAND and build filesystem tree
  //
  {
    UINT64  StartTick;
    UINT64  EndTick;
    UINT64  Freq;
    UINT64  ElapsedMs;

    Freq      = GetPerformanceCounterProperties (NULL, NULL);
    StartTick = GetPerformanceCounter ();

    Status = Yaffs2ScanNand (Volume);

    EndTick   = GetPerformanceCounter ();
    ElapsedMs = ((EndTick - StartTick) * 1000) / Freq;
    DEBUG ((DEBUG_WARN, "[MtYaffs2] NAND scan took %lu ms\n", ElapsedMs));
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[MtYaffs2] NAND scan failed: %r\n", Status));
    if (Volume->Objects != NULL) {
      FreePool (Volume->Objects);
    }
    FreePool (Volume->PageBuffer);
    FreePool (Volume);
    return Status;
  }

  //
  // Set up SimpleFileSystem protocol
  //
  Volume->VolumeInterface.Revision   = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  Volume->VolumeInterface.OpenVolume = Yaffs2OpenVolume;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Volume->Handle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Volume->VolumeInterface,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[MtYaffs2] Failed to install SFS: %r\n", Status));
    //
    // Free all objects
    //
    Yaffs2FreeVolume (Volume);
    return Status;
  }

  *VolumeOut = Volume;
  DEBUG ((DEBUG_WARN, "[MtYaffs2] Volume ready on handle %p\n", Handle));
  return EFI_SUCCESS;
}

/**
  Free all volume resources. Uninstalls SFS protocol if still installed.
**/
VOID
Yaffs2FreeVolume (
  IN  YAFFS2_VOLUME  *Volume
  )
{
  UINT32         i;
  YAFFS2_OBJECT  *Obj;

  if (Volume == NULL) {
    return;
  }

  //
  // Uninstall SFS protocol (ignore error if not installed)
  //
  gBS->UninstallMultipleProtocolInterfaces (
         Volume->Handle,
         &gEfiSimpleFileSystemProtocolGuid,
         &Volume->VolumeInterface,
         NULL
         );

  //
  // Free all objects
  //
  if (Volume->Objects != NULL) {
    for (i = 0; i <= Volume->MaxObjId; i++) {
      Obj = Volume->Objects[i];
      if (Obj == NULL) {
        continue;
      }
      if (Obj->Chunks != NULL) {
        FreePool (Obj->Chunks);
      }
      FreePool (Obj);
    }
    FreePool (Volume->Objects);
  }

  if (Volume->PageBuffer != NULL) {
    FreePool (Volume->PageBuffer);
  }

  FreePool (Volume);
}

/**
  Open the root directory of the YAFFS2 volume.

  @param[in]  This  SimpleFileSystem protocol instance.
  @param[out] Root  Returned file protocol for the root directory.

  @retval EFI_SUCCESS  Root directory opened.
**/
EFI_STATUS
EFIAPI
Yaffs2OpenVolume (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL                **Root
  )
{
  YAFFS2_VOLUME  *Volume;
  YAFFS2_IFILE   *IFile;

  Volume = YAFFS2_VOLUME_FROM_SFS (This);

  if (Volume->Root == NULL) {
    return EFI_VOLUME_CORRUPTED;
  }

  IFile = Yaffs2AllocateIFile (Volume, Volume->Root);
  if (IFile == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *Root = &IFile->Handle;
  return EFI_SUCCESS;
}
