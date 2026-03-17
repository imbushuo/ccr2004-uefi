/** @file
  DumpBlockApp — Read a block device and dump to a file.

  Usage: DumpBlockApp <HandleHex> <StartLBA> <Count> <OutputFile>

  Reads <Count> blocks starting at <StartLBA> from the BlockIo device
  identified by <HandleHex> and writes the raw data to <OutputFile>
  on any mounted filesystem.

  Example:
    FormatBlockApp ramdisk 256
    map -r
    DumpBlockApp <nand_handle> 0 65536 fs0:\nand.bin

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Shell.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

/* Read this many blocks per I/O to balance speed vs memory */
#define BLOCKS_PER_IO  64

/* Progress bar every N blocks */
#define PROGRESS_INTERVAL  256

STATIC
EFI_STATUS
OpenOutputFile (
  IN  CHAR16              *FilePath,
  OUT EFI_FILE_PROTOCOL   **File
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *FsHandles;
  UINTN                            FsCount;
  UINTN                            i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;

  /*
   * Strategy: try to open the file via each SimpleFileSystem handle.
   * The shell sets the working directory, but we don't have ShellLib,
   * so we try all FS handles until one accepts the path.
   * Paths like "fs0:\foo.bin" work because the shell maps them,
   * but from our perspective we need to strip the fsN: prefix
   * and find the right volume.
   */

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &FsCount,
                  &FsHandles
                  );
  if (EFI_ERROR (Status) || FsCount == 0) {
    Print (L"Error: No filesystem volumes found\n");
    return EFI_NOT_FOUND;
  }

  /* Strip "fsN:" prefix if present to get the bare filename */
  {
    CHAR16  *Colon = NULL;
    UINTN   j;

    for (j = 0; FilePath[j] != L'\0'; j++) {
      if (FilePath[j] == L':') {
        Colon = &FilePath[j];
        break;
      }
      /* If we hit a path separator before colon, no prefix */
      if (FilePath[j] == L'\\' || FilePath[j] == L'/') {
        break;
      }
    }

    if (Colon != NULL) {
      /* Try to match "fsN" prefix to a specific volume */
      CHAR16  Prefix[16];
      UINTN   PrefixLen = (UINTN)(Colon - FilePath);

      if (PrefixLen < sizeof (Prefix) / sizeof (CHAR16)) {
        CopyMem (Prefix, FilePath, PrefixLen * sizeof (CHAR16));
        Prefix[PrefixLen] = L'\0';

        /* Advance past "fsN:\" */
        FilePath = Colon + 1;
        if (*FilePath == L'\\' || *FilePath == L'/') {
          FilePath++;
        }
      }
    }
  }

  /* Try each filesystem volume */
  for (i = 0; i < FsCount; i++) {
    Status = gBS->HandleProtocol (FsHandles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Root->Open (
                     Root,
                     File,
                     FilePath,
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                     0
                     );
    Root->Close (Root);

    if (!EFI_ERROR (Status)) {
      FreePool (FsHandles);
      return EFI_SUCCESS;
    }
  }

  FreePool (FsHandles);
  Print (L"Error: Could not create file '%s' on any volume\n", FilePath);
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
DumpBlockAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_SHELL_PARAMETERS_PROTOCOL *ShellParams;
  UINTN                         Argc;
  CHAR16                        **Argv;
  UINTN                         HandleValue;
  EFI_HANDLE                    SrcHandle;
  EFI_BLOCK_IO_PROTOCOL         *BlockIo;
  EFI_LBA                       StartLba;
  UINTN                         BlockCount;
  UINT32                        BlockSize;
  EFI_FILE_PROTOCOL             *OutFile;
  UINT8                         *Buf;
  UINTN                         BufBlocks;
  UINTN                         Remaining;
  EFI_LBA                       CurLba;
  UINT64                        TotalBytes;
  UINT64                        WrittenBytes;

  /* Parse arguments */
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Error: Must be run from UEFI Shell\n");
    return Status;
  }

  Argc = ShellParams->Argc;
  Argv = ShellParams->Argv;

  if (Argc != 5) {
    Print (L"Usage: DumpBlockApp <Device> <StartLBA> <Count> <OutputFile>\n");
    Print (L"\n");
    Print (L"  Reads <Count> blocks from BlockIo device and writes to file.\n");
    Print (L"\n");
    Print (L"  Device:     Shell mapping (blk0:, fs0:) or handle in hex\n");
    Print (L"  StartLBA:   First block to read (decimal)\n");
    Print (L"  Count:      Number of blocks to read (decimal)\n");
    Print (L"  OutputFile: Output file path (e.g. fs0:\\dump.bin)\n");
    Print (L"\n");
    Print (L"  Examples:\n");
    Print (L"    DumpBlockApp blk0: 0 65536 fs0:\\nand.bin\n");
    Print (L"    DumpBlockApp 5A 0 65536 fs0:\\nand.bin\n");
    return EFI_INVALID_PARAMETER;
  }

  /* Resolve device: try shell mapping name first, then hex handle */
  SrcHandle = NULL;
  {
    EFI_SHELL_PROTOCOL              *Shell;
    CONST EFI_DEVICE_PATH_PROTOCOL  *ConstDevPath;

    Status = gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell);
    if (!EFI_ERROR (Status)) {
      ConstDevPath = Shell->GetDevicePathFromMap (Argv[1]);
      if (ConstDevPath != NULL) {
        EFI_DEVICE_PATH_PROTOCOL  *DevPathCopy;
        EFI_DEVICE_PATH_PROTOCOL  *Walker;
        UINTN                     DpSize;

        DpSize = GetDevicePathSize (ConstDevPath);
        DevPathCopy = AllocateCopyPool (DpSize, ConstDevPath);
        if (DevPathCopy != NULL) {
          Walker = DevPathCopy;
          Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Walker, &SrcHandle);
          FreePool (DevPathCopy);
          if (EFI_ERROR (Status)) {
            SrcHandle = NULL;
          }
        }
      }
    }
  }

  if (SrcHandle == NULL) {
    /* Fall back to hex handle */
    HandleValue = 0;
    Status = StrHexToUintnS (Argv[1], NULL, &HandleValue);
    if (EFI_ERROR (Status) || HandleValue == 0) {
      Print (L"Error: '%s' is not a valid mapping or hex handle\n", Argv[1]);
      return EFI_INVALID_PARAMETER;
    }
    SrcHandle = (EFI_HANDLE)(UINTN)HandleValue;
  }

  /* Parse start LBA */
  Status = StrDecimalToUintnS (Argv[2], NULL, (UINTN *)&StartLba);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Invalid StartLBA '%s'\n", Argv[2]);
    return EFI_INVALID_PARAMETER;
  }

  /* Parse count */
  Status = StrDecimalToUintnS (Argv[3], NULL, &BlockCount);
  if (EFI_ERROR (Status) || BlockCount == 0) {
    Print (L"Error: Invalid Count '%s'\n", Argv[3]);
    return EFI_INVALID_PARAMETER;
  }

  /* Open source BlockIo */
  Status = gBS->OpenProtocol (
                  SrcHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Error: Device '%s' has no BlockIo: %r\n", Argv[1], Status);
    return Status;
  }

  BlockSize = BlockIo->Media->BlockSize;
  TotalBytes = (UINT64)BlockCount * BlockSize;

  /* Validate range */
  if (StartLba + BlockCount > BlockIo->Media->LastBlock + 1) {
    Print (L"Error: Range exceeds device (last block = %lu)\n", BlockIo->Media->LastBlock);
    return EFI_INVALID_PARAMETER;
  }

  Print (L"Source: %s  block_size=%u  start=%lu  count=%lu  total=%lu MB\n",
         Argv[1], BlockSize, StartLba, (UINT64)BlockCount,
         TotalBytes / (1024 * 1024));

  /* Open output file */
  Status = OpenOutputFile (Argv[4], &OutFile);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"Output: %s\n", Argv[4]);

  /* Allocate transfer buffer */
  BufBlocks = BLOCKS_PER_IO;
  if (BufBlocks > BlockCount) {
    BufBlocks = BlockCount;
  }

  Buf = AllocatePool (BufBlocks * BlockSize);
  if (Buf == NULL) {
    OutFile->Close (OutFile);
    return EFI_OUT_OF_RESOURCES;
  }

  /* Read and write loop */
  Remaining = BlockCount;
  CurLba = StartLba;
  WrittenBytes = 0;

  Print (L"Dumping...\n");

  while (Remaining > 0) {
    UINTN  Chunk = (Remaining > BufBlocks) ? BufBlocks : Remaining;
    UINTN  ReadSize = Chunk * BlockSize;
    UINTN  WriteSize;

    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, CurLba, ReadSize, Buf);
    if (EFI_ERROR (Status)) {
      Print (L"\nError: Read failed at LBA %lu: %r\n", CurLba, Status);
      goto Done;
    }

    WriteSize = ReadSize;
    Status = OutFile->Write (OutFile, &WriteSize, Buf);
    if (EFI_ERROR (Status)) {
      Print (L"\nError: Write failed at offset 0x%lx: %r\n", WrittenBytes, Status);
      goto Done;
    }

    CurLba += Chunk;
    Remaining -= Chunk;
    WrittenBytes += WriteSize;

    /* Progress */
    if (((BlockCount - Remaining) % PROGRESS_INTERVAL) == 0 || Remaining == 0) {
      UINT32  Pct = (UINT32)((BlockCount - Remaining) * 100 / BlockCount);
      Print (L"\r  %3u%% (%lu / %lu blocks, %lu KB written)",
             Pct, (UINT64)(BlockCount - Remaining), (UINT64)BlockCount,
             WrittenBytes / 1024);
    }
  }

  Print (L"\n");
  Status = EFI_SUCCESS;

Done:
  OutFile->Flush (OutFile);
  OutFile->Close (OutFile);
  FreePool (Buf);

  if (!EFI_ERROR (Status)) {
    Print (L"Done. %lu bytes written to %s\n", WrittenBytes, Argv[4]);
  }

  return Status;
}
