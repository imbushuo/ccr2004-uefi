/** @file
  TftpPutApp — Upload a local file to a TFTP server.

  Usage: TftpPutApp <ServerIP> <RemoteFile> <LocalFile>

  Reads <LocalFile> from a mounted filesystem and uploads it to
  <ServerIP> as <RemoteFile> using TFTP WRQ (write request).

  Example:
    TftpPutApp 192.168.1.10 nand.bin fs0:\nand.bin

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ManagedNetwork.h>
#include <Protocol/Mtftp4.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

STATIC
EFI_STATUS
ReadLocalFile (
  IN  CHAR16  *FilePath,
  OUT VOID    **Buffer,
  OUT UINTN   *FileSize
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *FsHandles;
  UINTN                            FsCount;
  UINTN                            i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  CHAR16                           *BarePath;
  CHAR16                           *Colon;
  UINTN                            j;

  *Buffer   = NULL;
  *FileSize = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &FsCount,
                  &FsHandles
                  );
  if (EFI_ERROR (Status) || FsCount == 0) {
    return EFI_NOT_FOUND;
  }

  /* Strip "fsN:" prefix if present */
  BarePath = FilePath;
  Colon = NULL;
  for (j = 0; FilePath[j] != L'\0'; j++) {
    if (FilePath[j] == L':') {
      Colon = &FilePath[j];
      break;
    }
    if (FilePath[j] == L'\\' || FilePath[j] == L'/') {
      break;
    }
  }
  if (Colon != NULL) {
    BarePath = Colon + 1;
    if (*BarePath == L'\\' || *BarePath == L'/') {
      BarePath++;
    }
  }

  File = NULL;
  for (i = 0; i < FsCount; i++) {
    Status = gBS->HandleProtocol (FsHandles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) continue;

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) continue;

    Status = Root->Open (Root, &File, BarePath, EFI_FILE_MODE_READ, 0);
    Root->Close (Root);
    if (!EFI_ERROR (Status)) break;
    File = NULL;
  }

  FreePool (FsHandles);

  if (File == NULL) {
    Print (L"Error: Could not open '%s'\n", FilePath);
    return EFI_NOT_FOUND;
  }

  /* Get file size */
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

  *FileSize = (UINTN)Info->FileSize;
  FreePool (Info);

  if (*FileSize == 0) {
    File->Close (File);
    Print (L"Error: File is empty\n");
    return EFI_INVALID_PARAMETER;
  }

  /* Read entire file */
  *Buffer = AllocatePool (*FileSize);
  if (*Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  {
    UINTN  ReadSize = *FileSize;
    Status = File->Read (File, &ReadSize, *Buffer);
    if (EFI_ERROR (Status) || ReadSize != *FileSize) {
      FreePool (*Buffer);
      *Buffer = NULL;
      File->Close (File);
      return EFI_DEVICE_ERROR;
    }
  }

  File->Close (File);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
TftpPutAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_SHELL_PARAMETERS_PROTOCOL *ShellParams;
  UINTN                         Argc;
  CHAR16                        **Argv;
  EFI_IPv4_ADDRESS              ServerIp;
  CHAR8                         *AsciiRemotePath;
  UINTN                         RemotePathLen;
  VOID                          *FileData;
  UINTN                         FileSize;
  EFI_HANDLE                    *Handles;
  UINTN                         HandleCount;
  UINTN                         i;
  EFI_HANDLE                    ControllerHandle;
  EFI_HANDLE                    Mtftp4Child;
  EFI_MTFTP4_PROTOCOL           *Mtftp4;
  EFI_MTFTP4_CONFIG_DATA        ConfigData;
  EFI_MTFTP4_TOKEN              Token;

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

  if (Argc != 4) {
    Print (L"Usage: TftpPutApp <ServerIP> <RemoteFile> <LocalFile>\n");
    Print (L"\n");
    Print (L"  Uploads a local file to a TFTP server.\n");
    Print (L"\n");
    Print (L"  Example: TftpPutApp 192.168.1.10 nand.bin fs0:\\nand.bin\n");
    return EFI_INVALID_PARAMETER;
  }

  /* Parse server IP */
  Status = NetLibStrToIp4 (Argv[1], &ServerIp);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Invalid IP address '%s'\n", Argv[1]);
    return EFI_INVALID_PARAMETER;
  }

  /* Convert remote path to ASCII */
  RemotePathLen = StrLen (Argv[2]) + 1;
  AsciiRemotePath = AllocatePool (RemotePathLen);
  if (AsciiRemotePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  UnicodeStrToAsciiStrS (Argv[2], AsciiRemotePath, RemotePathLen);

  /* Read local file */
  Print (L"Reading %s...\n", Argv[3]);
  Status = ReadLocalFile (Argv[3], &FileData, &FileSize);
  if (EFI_ERROR (Status)) {
    FreePool (AsciiRemotePath);
    return Status;
  }

  Print (L"File size: %lu bytes (%lu KB)\n", (UINT64)FileSize, (UINT64)FileSize / 1024);

  /* Find a network interface with MTFTP4 service binding */
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiManagedNetworkServiceBindingProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status) || HandleCount == 0) {
    Print (L"Error: No network interfaces found\n");
    FreePool (FileData);
    FreePool (AsciiRemotePath);
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;

  for (i = 0; i < HandleCount; i++) {
    ControllerHandle = Handles[i];
    Mtftp4Child = NULL;

    /* Create MTFTP4 child */
    Status = NetLibCreateServiceChild (
               ControllerHandle,
               ImageHandle,
               &gEfiMtftp4ServiceBindingProtocolGuid,
               &Mtftp4Child
               );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = gBS->OpenProtocol (
                    Mtftp4Child,
                    &gEfiMtftp4ProtocolGuid,
                    (VOID **)&Mtftp4,
                    ImageHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      NetLibDestroyServiceChild (
        ControllerHandle, ImageHandle,
        &gEfiMtftp4ServiceBindingProtocolGuid, Mtftp4Child
        );
      continue;
    }

    /* Configure */
    ZeroMem (&ConfigData, sizeof (ConfigData));
    ConfigData.UseDefaultSetting  = TRUE;
    ConfigData.InitialServerPort  = 69;
    ConfigData.TryCount           = 6;
    ConfigData.TimeoutValue       = 4;
    CopyMem (&ConfigData.ServerIp, &ServerIp, sizeof (EFI_IPv4_ADDRESS));

    Status = Mtftp4->Configure (Mtftp4, &ConfigData);
    if (EFI_ERROR (Status)) {
      Print (L"  NIC %u: Configure failed: %r\n", i, Status);
      NetLibDestroyServiceChild (
        ControllerHandle, ImageHandle,
        &gEfiMtftp4ServiceBindingProtocolGuid, Mtftp4Child
        );
      continue;
    }

    /* Upload */
    Print (L"Uploading to %s:%a (%lu bytes)...\n", Argv[1], AsciiRemotePath, (UINT64)FileSize);

    ZeroMem (&Token, sizeof (Token));
    Token.Filename    = (UINT8 *)AsciiRemotePath;
    Token.BufferSize  = FileSize;
    Token.Buffer      = FileData;
    Token.OverrideData = NULL;
    Token.ModeStr     = NULL;
    Token.OptionCount = 0;
    Token.OptionList  = NULL;
    Token.Context     = NULL;
    Token.CheckPacket = NULL;
    Token.TimeoutCallback = NULL;
    Token.PacketNeeded = NULL;
    Token.Event       = NULL;  /* synchronous */

    Status = Mtftp4->WriteFile (Mtftp4, &Token);

    /* Clean up this child */
    Mtftp4->Configure (Mtftp4, NULL);
    NetLibDestroyServiceChild (
      ControllerHandle, ImageHandle,
      &gEfiMtftp4ServiceBindingProtocolGuid, Mtftp4Child
      );

    if (!EFI_ERROR (Status)) {
      Print (L"Upload complete.\n");
      break;
    }

    /* Check the token status too */
    if (!EFI_ERROR (Status) && EFI_ERROR (Token.Status)) {
      Status = Token.Status;
    }

    Print (L"  NIC %u: Upload failed: %r\n", i, Status);
  }

  FreePool (Handles);
  FreePool (FileData);
  FreePool (AsciiRemotePath);

  if (EFI_ERROR (Status)) {
    Print (L"Error: TFTP upload failed: %r\n", Status);
  }

  return Status;
}
