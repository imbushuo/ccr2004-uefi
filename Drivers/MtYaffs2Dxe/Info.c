/** @file
  MikroTik YAFFS2 filesystem driver — GetInfo / SetInfo.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"
#include <Library/TimeBaseLib.h>

#define YAFFS2_VOLUME_LABEL  L"MikroTik NAND"

/**
  Return information about a file or the volume.

  Supports:
    - gEfiFileInfoGuid — file metadata
    - gEfiFileSystemInfoGuid — volume metadata
    - gEfiFileSystemVolumeLabelInfoIdGuid — volume label only
**/
EFI_STATUS
EFIAPI
Yaffs2GetInfo (
  IN     EFI_FILE_PROTOCOL  *This,
  IN     EFI_GUID           *InformationType,
  IN OUT UINTN              *BufferSize,
     OUT VOID               *Buffer
  )
{
  YAFFS2_IFILE                        *IFile;
  YAFFS2_VOLUME                       *Volume;
  YAFFS2_OBJECT                       *OFile;
  EFI_FILE_INFO                       *FileInfo;
  EFI_FILE_SYSTEM_INFO                *FsInfo;
  EFI_FILE_SYSTEM_VOLUME_LABEL        *VolLabel;
  UINTN                               NameLen;
  UINTN                               Needed;
  UINTN                               i;
  UINTN                               LabelLen;

  if ((This == NULL) || (InformationType == NULL) || (BufferSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  IFile  = YAFFS2_IFILE_FROM_HANDLE (This);
  Volume = IFile->Volume;
  OFile  = IFile->OFile;

  // -----------------------------------------------------------------
  // EFI_FILE_INFO — file or directory metadata
  // -----------------------------------------------------------------
  if (CompareGuid (InformationType, &gEfiFileInfoGuid)) {
    //
    // For root directory, use empty name (UEFI convention)
    //
    if (OFile == Volume->Root) {
      NameLen = 0;
    } else {
      NameLen = AsciiStrLen (OFile->Name);
    }

    Needed = SIZE_OF_EFI_FILE_INFO + (NameLen + 1) * sizeof (CHAR16);
    if (*BufferSize < Needed) {
      *BufferSize = Needed;
      return EFI_BUFFER_TOO_SMALL;
    }

    FileInfo = (EFI_FILE_INFO *)Buffer;
    ZeroMem (FileInfo, Needed);

    FileInfo->Size         = Needed;
    FileInfo->FileSize     = OFile->FileSize;
    FileInfo->PhysicalSize = OFile->FileSize;
    FileInfo->Attribute    = EFI_FILE_READ_ONLY;

    if (OFile->Type == YAFFS2_TYPE_DIR) {
      FileInfo->Attribute |= EFI_FILE_DIRECTORY;
    }

    if (OFile->MTime != 0) {
      EpochToEfiTime ((UINTN)OFile->MTime, &FileInfo->ModificationTime);
      CopyMem (&FileInfo->CreateTime, &FileInfo->ModificationTime, sizeof (EFI_TIME));
      CopyMem (&FileInfo->LastAccessTime, &FileInfo->ModificationTime, sizeof (EFI_TIME));
    }

    for (i = 0; i < NameLen; i++) {
      FileInfo->FileName[i] = (CHAR16)OFile->Name[i];
    }
    FileInfo->FileName[NameLen] = L'\0';

    *BufferSize = Needed;
    return EFI_SUCCESS;
  }

  // -----------------------------------------------------------------
  // EFI_FILE_SYSTEM_INFO — volume metadata
  // -----------------------------------------------------------------
  if (CompareGuid (InformationType, &gEfiFileSystemInfoGuid)) {
    LabelLen = StrLen (YAFFS2_VOLUME_LABEL);
    Needed   = SIZE_OF_EFI_FILE_SYSTEM_INFO + (LabelLen + 1) * sizeof (CHAR16);

    if (*BufferSize < Needed) {
      *BufferSize = Needed;
      return EFI_BUFFER_TOO_SMALL;
    }

    FsInfo = (EFI_FILE_SYSTEM_INFO *)Buffer;
    ZeroMem (FsInfo, Needed);

    FsInfo->Size        = Needed;
    FsInfo->ReadOnly    = TRUE;
    FsInfo->VolumeSize  = (UINT64)Volume->Nand->NumBlocks *
                          (UINT64)Volume->Nand->PagesPerBlock *
                          (UINT64)YAFFS2_DATA_PER_PAGE;
    FsInfo->FreeSpace   = 0;
    FsInfo->BlockSize   = YAFFS2_DATA_PER_PAGE;

    StrCpyS (FsInfo->VolumeLabel, LabelLen + 1, YAFFS2_VOLUME_LABEL);

    *BufferSize = Needed;
    return EFI_SUCCESS;
  }

  // -----------------------------------------------------------------
  // EFI_FILE_SYSTEM_VOLUME_LABEL — volume label only
  // -----------------------------------------------------------------
  if (CompareGuid (InformationType, &gEfiFileSystemVolumeLabelInfoIdGuid)) {
    LabelLen = StrLen (YAFFS2_VOLUME_LABEL);
    Needed   = sizeof (EFI_FILE_SYSTEM_VOLUME_LABEL) +
               (LabelLen + 1) * sizeof (CHAR16);

    if (*BufferSize < Needed) {
      *BufferSize = Needed;
      return EFI_BUFFER_TOO_SMALL;
    }

    VolLabel = (EFI_FILE_SYSTEM_VOLUME_LABEL *)Buffer;
    StrCpyS (VolLabel->VolumeLabel, LabelLen + 1, YAFFS2_VOLUME_LABEL);

    *BufferSize = Needed;
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

/**
  SetInfo is not supported on a read-only filesystem.
**/
EFI_STATUS
EFIAPI
Yaffs2SetInfo (
  IN  EFI_FILE_PROTOCOL  *This,
  IN  EFI_GUID           *InformationType,
  IN  UINTN              BufferSize,
  IN  VOID               *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}
