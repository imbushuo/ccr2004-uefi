/** @file
  RouterOS Kernel Loader — entry point, filesystem scan, orchestration.

  Reads /var/pdb/system/image from NAND (YAFFS2), parses the NPK package
  to extract boot/kernel, decompresses the XZ-compressed EFI stub kernel,
  and boots it via the Linux EFI stub protocol.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"

//
// Path to the RouterOS system image within the YAFFS2 filesystem
//
STATIC CONST CHAR16 mImagePath[] = L"\\var\\pdb\\system\\image";

/**
  Read a file completely into an allocated buffer.

  @param[in]  Root      Open root directory handle.
  @param[in]  Path      File path to open.
  @param[out] FileData  Allocated buffer with file contents. Caller must FreePool.
  @param[out] FileSize  Size of the file in bytes.

  @retval EFI_SUCCESS   File read successfully.
**/
STATIC
EFI_STATUS
ReadFileToBuffer (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT UINT8              **FileData,
  OUT UINTN              *FileSize
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINT8              *Buffer;
  UINTN              ReadSize;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

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

  //
  // Allocate and read
  //
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
  Scan all SimpleFileSystem handles to find and read the NPK image file.

  @param[out] NpkData  Allocated buffer with NPK contents. Caller must FreePool.
  @param[out] NpkSize  Size of the NPK data.

  @retval EFI_SUCCESS  NPK file found and read.
**/
STATIC
EFI_STATUS
FindAndReadNpk (
  OUT UINT8  **NpkData,
  OUT UINTN  *NpkSize
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] No filesystem handles found: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] Scanning %u filesystem handles...\n", HandleCount));

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = ReadFileToBuffer (Root, mImagePath, NpkData, NpkSize);
    Root->Close (Root);

    if (!EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[RouterOS] Found NPK: %s (%u bytes)\n",
              mImagePath, *NpkSize));
      FreePool (Handles);
      return EFI_SUCCESS;
    }
  }

  FreePool (Handles);
  DEBUG ((DEBUG_WARN, "[RouterOS] NPK file not found on any filesystem\n"));
  return EFI_NOT_FOUND;
}

/**
  Entry point for the RouterOS Kernel Loader.
**/
EFI_STATUS
EFIAPI
RouterOSLoaderEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS   Status;
  UINT8        *NpkData;
  UINTN        NpkSize;
  CONST UINT8  *KernelElf;
  UINTN        KernelElfSize;
  UINT8        *StubBuffer;
  UINTN        StubSize;
  UINT8        *InitrdData;
  UINTN        InitrdSize;

  DEBUG ((DEBUG_WARN, "[RouterOS] RouterOS Kernel Loader starting\n"));

  //
  // Step 1: Find and read the NPK system image
  //
  Status = FindAndReadNpk (&NpkData, &NpkSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Failed to read NPK: %r\n", Status));
    return Status;
  }

  //
  // Step 2: Parse NPK to find boot/kernel
  //
  Status = NpkFindBootKernel (NpkData, NpkSize, &KernelElf, &KernelElfSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Failed to find boot/kernel in NPK: %r\n", Status));
    FreePool (NpkData);
    return Status;
  }

  //
  // Step 3: Extract EFI stub kernel + initrd from boot/kernel ELF
  //
  Status = ExtractKernelAndInitrd (
             KernelElf,
             KernelElfSize,
             &StubBuffer,
             &StubSize,
             &InitrdData,
             &InitrdSize
             );

  //
  // Free NPK data — kernel ELF pointer was into this buffer
  //
  FreePool (NpkData);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Failed to extract kernel: %r\n", Status));
    return Status;
  }

  //
  // Step 4: Boot the kernel
  //
  Status = BootLinuxKernel (
             ImageHandle,
             StubBuffer,
             StubSize,
             InitrdData,
             InitrdSize
             );

  //
  // If we get here, boot failed
  //
  if (InitrdData != NULL) {
    FreePool (InitrdData);
  }
  FreePool (StubBuffer);
  return Status;
}
