/** @file
  MikroTik YAFFS2 filesystem driver — Read, Write, Position, Flush.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"
#include <Library/TimeBaseLib.h>

/**
  Build an EFI_FILE_INFO structure for the given object.

  @param[in]  Obj         The filesystem object.
  @param[out] Buffer      Output buffer (may be NULL for size query).
  @param[in,out] BufSize  On input, buffer size. On output, required size.

  @retval EFI_SUCCESS          Info written to Buffer.
  @retval EFI_BUFFER_TOO_SMALL BufSize updated with required size.
**/
STATIC
EFI_STATUS
BuildFileInfo (
  IN     YAFFS2_OBJECT  *Obj,
  OUT    VOID           *Buffer,
  IN OUT UINTN          *BufSize
  )
{
  EFI_FILE_INFO  *Info;
  UINTN          NameLen;
  UINTN          InfoSize;
  UINTN          i;

  //
  // Calculate name length (ASCII -> CHAR16)
  //
  NameLen = AsciiStrLen (Obj->Name);
  InfoSize = SIZE_OF_EFI_FILE_INFO + (NameLen + 1) * sizeof (CHAR16);

  if (*BufSize < InfoSize) {
    *BufSize = InfoSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  Info = (EFI_FILE_INFO *)Buffer;
  ZeroMem (Info, InfoSize);

  Info->Size         = InfoSize;
  Info->FileSize     = Obj->FileSize;
  Info->PhysicalSize = Obj->FileSize;
  Info->Attribute    = EFI_FILE_READ_ONLY;

  if (Obj->Type == YAFFS2_TYPE_DIR) {
    Info->Attribute |= EFI_FILE_DIRECTORY;
  }

  //
  // Convert unix mtime to EFI_TIME
  //
  if (Obj->MTime != 0) {
    EpochToEfiTime ((UINTN)Obj->MTime, &Info->ModificationTime);
    CopyMem (&Info->CreateTime, &Info->ModificationTime, sizeof (EFI_TIME));
    CopyMem (&Info->LastAccessTime, &Info->ModificationTime, sizeof (EFI_TIME));
  }

  //
  // Convert name from ASCII to UCS-2
  //
  for (i = 0; i < NameLen; i++) {
    Info->FileName[i] = (CHAR16)Obj->Name[i];
  }
  Info->FileName[NameLen] = L'\0';

  *BufSize = InfoSize;
  return EFI_SUCCESS;
}

/**
  Read from a file or enumerate a directory.

  For files: reads up to *BufferSize bytes starting at Position.
  For directories: returns one EFI_FILE_INFO per call; returns
  *BufferSize=0 when enumeration is complete.
**/
EFI_STATUS
EFIAPI
Yaffs2Read (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  OUT    VOID               *Buffer
  )
{
  YAFFS2_IFILE   *IFile;
  YAFFS2_VOLUME  *Volume;
  YAFFS2_OBJECT  *OFile;
  YAFFS2_OBJECT  *Child;
  EFI_STATUS     Status;
  UINTN          ReadLen;
  UINTN          BytesCopied;
  UINT32         ChunkIdx;
  UINT32         Offset;
  UINT32         CopyLen;
  UINT32         ValidBytes;

  if ((This == NULL) || (BufferSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  IFile  = YAFFS2_IFILE_FROM_HANDLE (This);
  Volume = IFile->Volume;
  OFile  = IFile->OFile;

  if (OFile->Type == YAFFS2_TYPE_DIR) {
    //
    // Directory enumeration
    //
    if (IFile->DirCursor == &OFile->ChildHead) {
      //
      // End of directory
      //
      *BufferSize = 0;
      return EFI_SUCCESS;
    }

    Child = BASE_CR (IFile->DirCursor, YAFFS2_OBJECT, SiblingLink);

    //
    // Follow hardlinks for display
    //
    if (Child->Type == YAFFS2_TYPE_HARDLINK) {
      YAFFS2_OBJECT  *Target;
      if ((Child->EquivId > 0) &&
          (Child->EquivId <= Volume->MaxObjId) &&
          (Volume->Objects[Child->EquivId] != NULL))
      {
        Target = Volume->Objects[Child->EquivId];
        //
        // Build info using hardlink's name but target's attributes.
        // We build a temporary object on stack for this purpose.
        //
        YAFFS2_OBJECT  TempObj;
        CopyMem (&TempObj, Target, sizeof (YAFFS2_OBJECT));
        CopyMem (TempObj.Name, Child->Name, sizeof (TempObj.Name));
        Status = BuildFileInfo (&TempObj, Buffer, BufferSize);
      } else {
        Status = BuildFileInfo (Child, Buffer, BufferSize);
      }
    } else {
      Status = BuildFileInfo (Child, Buffer, BufferSize);
    }

    if (!EFI_ERROR (Status)) {
      //
      // Advance cursor
      //
      IFile->DirCursor = IFile->DirCursor->ForwardLink;
      IFile->Position++;
    }

    return Status;
  }

  //
  // File read
  //
  if (Buffer == NULL && *BufferSize > 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (IFile->Position >= OFile->FileSize) {
    *BufferSize = 0;
    return EFI_SUCCESS;
  }

  ReadLen = *BufferSize;
  if (IFile->Position + ReadLen > OFile->FileSize) {
    ReadLen = (UINTN)(OFile->FileSize - IFile->Position);
  }

  BytesCopied = 0;
  while (BytesCopied < ReadLen) {
    //
    // Determine which chunk and offset within it
    //
    ChunkIdx = (UINT32)((IFile->Position + BytesCopied) / YAFFS2_DATA_PER_PAGE);
    Offset   = (UINT32)((IFile->Position + BytesCopied) % YAFFS2_DATA_PER_PAGE);
    CopyLen  = YAFFS2_DATA_PER_PAGE - Offset;
    if (CopyLen > ReadLen - BytesCopied) {
      CopyLen = (UINT32)(ReadLen - BytesCopied);
    }

    if ((ChunkIdx < OFile->ChunkCount) &&
        (OFile->Chunks[ChunkIdx].PageIndex != YAFFS2_CHUNK_UNSET))
    {
      Status = Volume->Nand->ReadPage (
                               Volume->Nand,
                               OFile->Chunks[ChunkIdx].PageIndex,
                               Volume->PageBuffer
                               );
      if (EFI_ERROR (Status)) {
        if (BytesCopied > 0) {
          break;  // Return what we have
        }
        return EFI_DEVICE_ERROR;
      }

      //
      // Only copy up to valid bytes in the chunk
      //
      ValidBytes = OFile->Chunks[ChunkIdx].NBytes;
      if (ValidBytes > YAFFS2_DATA_PER_PAGE) {
        ValidBytes = YAFFS2_DATA_PER_PAGE;
      }

      if (Offset < ValidBytes) {
        UINT32  ActualCopy;
        ActualCopy = ValidBytes - Offset;
        if (ActualCopy > CopyLen) {
          ActualCopy = CopyLen;
        }
        CopyMem ((UINT8 *)Buffer + BytesCopied, Volume->PageBuffer + Offset, ActualCopy);
        if (ActualCopy < CopyLen) {
          ZeroMem ((UINT8 *)Buffer + BytesCopied + ActualCopy, CopyLen - ActualCopy);
        }
      } else {
        ZeroMem ((UINT8 *)Buffer + BytesCopied, CopyLen);
      }
    } else {
      //
      // Missing chunk — fill with zeros
      //
      ZeroMem ((UINT8 *)Buffer + BytesCopied, CopyLen);
    }

    BytesCopied += CopyLen;
  }

  IFile->Position += BytesCopied;
  *BufferSize = BytesCopied;
  return EFI_SUCCESS;
}

/**
  Write is not supported on a read-only filesystem.
**/
EFI_STATUS
EFIAPI
Yaffs2Write (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  IN     VOID               *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

/**
  Get the current file position.
**/
EFI_STATUS
EFIAPI
Yaffs2GetPosition (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT UINT64             *Position
  )
{
  YAFFS2_IFILE  *IFile;

  if ((This == NULL) || (Position == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  IFile = YAFFS2_IFILE_FROM_HANDLE (This);

  if (IFile->OFile->Type == YAFFS2_TYPE_DIR) {
    return EFI_UNSUPPORTED;
  }

  *Position = IFile->Position;
  return EFI_SUCCESS;
}

/**
  Set the file position.

  For files: any value; 0xFFFFFFFFFFFFFFFF means end-of-file.
  For directories: only 0 is accepted (resets enumeration).
**/
EFI_STATUS
EFIAPI
Yaffs2SetPosition (
  IN  EFI_FILE_PROTOCOL  *This,
  IN  UINT64             Position
  )
{
  YAFFS2_IFILE  *IFile;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  IFile = YAFFS2_IFILE_FROM_HANDLE (This);

  if (IFile->OFile->Type == YAFFS2_TYPE_DIR) {
    if (Position != 0) {
      return EFI_UNSUPPORTED;
    }
    //
    // Reset directory enumeration
    //
    IFile->DirCursor = IFile->OFile->ChildHead.ForwardLink;
    IFile->Position  = 0;
    return EFI_SUCCESS;
  }

  //
  // File positioning
  //
  if (Position == MAX_UINT64) {
    IFile->Position = IFile->OFile->FileSize;
  } else {
    IFile->Position = Position;
  }

  return EFI_SUCCESS;
}

/**
  Flush is a no-op on a read-only filesystem.
**/
EFI_STATUS
EFIAPI
Yaffs2Flush (
  IN  EFI_FILE_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}
