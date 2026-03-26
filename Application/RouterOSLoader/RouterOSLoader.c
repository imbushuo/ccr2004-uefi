/** @file
  RouterOS Kernel Loader — entry point, filesystem scan, orchestration.

  Reads /var/pdb/system/image from NAND (YAFFS2), parses the NPK package
  to extract boot/kernel, decompresses the XZ-compressed EFI stub kernel,
  and boots it via the Linux EFI stub protocol.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"
#include "npk_parser.h"
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/ShellParameters.h>

//
// Path to the RouterOS system image within the YAFFS2 filesystem
//
STATIC CONST CHAR16 mImagePath[] = L"\\var\\pdb\\system\\image";

/**
  Read a file completely into an allocated buffer.
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
  EFI_STATUS      Status;
  UINT8           *NpkData;
  UINTN           NpkSize;
  npk_package_t   *Package;
  npk_status_t    NpkStatus;
  void            *KernelElf;
  size_t          KernelElfSize;
  UINT8           *StubBuffer;
  UINTN           StubSize;
  UINT8           *InitrdData;
  UINTN           InitrdSize;

  DEBUG ((DEBUG_WARN, "[RouterOS] RouterOS Kernel Loader starting\n"));

  //
  // Check for -RouterBoot parameter: write magic and warm reset
  //
  {
    EFI_SHELL_PARAMETERS_PROTOCOL  *ShellParams;

    Status = gBS->HandleProtocol (
                    ImageHandle,
                    &gEfiShellParametersProtocolGuid,
                    (VOID **)&ShellParams
                    );
    if (!EFI_ERROR (Status)) {
      UINTN  Idx;

      for (Idx = 1; Idx < ShellParams->Argc; Idx++) {
        if (StrCmp (ShellParams->Argv[Idx], L"-RouterBoot") == 0) {
          volatile UINT32  *Magic = (volatile UINT32 *)(UINTN)0x20102000;

          DEBUG ((DEBUG_WARN, "[RouterOS] -RouterBoot: writing magic 0xFDFFB001 to 0x20102000\n"));
          *Magic = 0xFDFFB001U;

          DEBUG ((DEBUG_WARN, "[RouterOS] Warm resetting...\n"));
          gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

          /* Should not return */
          CpuDeadLoop ();
        }
      }
    }
  }

  //
  // Step 1: Find and read the NPK system image
  //
  Status = FindAndReadNpk (&NpkData, &NpkSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Failed to read NPK: %r\n", Status));
    return Status;
  }

  //
  // Step 2: Parse NPK and extract boot/kernel
  //
  NpkStatus = npk_parse_buffer (NpkData, NpkSize, &Package);
  if (NpkStatus != NPK_STATUS_OK) {
    DEBUG ((DEBUG_WARN, "[RouterOS] NPK parse failed: %a\n",
            npk_status_string (NpkStatus)));
    FreePool (NpkData);
    return EFI_COMPROMISED_DATA;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] NPK parsed: %u entries\n", Package->entry_count));

  NpkStatus = npk_extract_file (Package, "boot/kernel", &KernelElf, &KernelElfSize);
  npk_package_free (Package);
  FreePool (NpkData);

  if (NpkStatus != NPK_STATUS_OK) {
    DEBUG ((DEBUG_WARN, "[RouterOS] boot/kernel extraction failed: %a\n",
            npk_status_string (NpkStatus)));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] boot/kernel extracted: %u bytes\n", KernelElfSize));

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

  npk_file_buffer_free (KernelElf);

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
