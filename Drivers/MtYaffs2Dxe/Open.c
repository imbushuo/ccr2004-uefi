/** @file
  MikroTik YAFFS2 filesystem driver — Open, Close, Delete.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"

/**
  Allocate an open-file instance (IFILE) for the given object.
**/
YAFFS2_IFILE *
Yaffs2AllocateIFile (
  IN  YAFFS2_VOLUME   *Volume,
  IN  YAFFS2_OBJECT   *OFile
  )
{
  YAFFS2_IFILE  *IFile;

  IFile = AllocateZeroPool (sizeof (YAFFS2_IFILE));
  if (IFile == NULL) {
    return NULL;
  }

  IFile->Signature          = YAFFS2_IFILE_SIGNATURE;
  IFile->Handle.Revision    = EFI_FILE_PROTOCOL_REVISION;
  IFile->Handle.Open        = Yaffs2Open;
  IFile->Handle.Close       = Yaffs2Close;
  IFile->Handle.Delete      = Yaffs2Delete;
  IFile->Handle.Read        = Yaffs2Read;
  IFile->Handle.Write       = Yaffs2Write;
  IFile->Handle.GetPosition = Yaffs2GetPosition;
  IFile->Handle.SetPosition = Yaffs2SetPosition;
  IFile->Handle.GetInfo     = Yaffs2GetInfo;
  IFile->Handle.SetInfo     = Yaffs2SetInfo;
  IFile->Handle.Flush       = Yaffs2Flush;
  IFile->Position           = 0;
  IFile->OFile              = OFile;
  IFile->Volume             = Volume;

  if (OFile->Type == YAFFS2_TYPE_DIR) {
    IFile->DirCursor = OFile->ChildHead.ForwardLink;
  } else {
    IFile->DirCursor = NULL;
  }

  return IFile;
}

/**
  Resolve a YAFFS2 object, following hardlinks to the target.
**/
STATIC
YAFFS2_OBJECT *
ResolveHardlinks (
  IN  YAFFS2_VOLUME   *Volume,
  IN  YAFFS2_OBJECT   *Obj
  )
{
  UINT32  Depth;

  for (Depth = 0; Depth < 16; Depth++) {
    if (Obj->Type != YAFFS2_TYPE_HARDLINK) {
      return Obj;
    }
    if ((Obj->EquivId == 0) ||
        (Obj->EquivId > Volume->MaxObjId) ||
        (Volume->Objects[Obj->EquivId] == NULL))
    {
      return NULL;
    }
    Obj = Volume->Objects[Obj->EquivId];
  }

  return NULL;  // Too many hardlink redirections
}

/**
  Open a file or directory relative to this file handle.

  @retval EFI_SUCCESS         File opened.
  @retval EFI_NOT_FOUND       Path component not found.
  @retval EFI_WRITE_PROTECTED Write/create modes rejected.
**/
EFI_STATUS
EFIAPI
Yaffs2Open (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL  **NewHandle,
  IN  CHAR16             *FileName,
  IN  UINT64             OpenMode,
  IN  UINT64             Attributes
  )
{
  YAFFS2_IFILE   *IFile;
  YAFFS2_VOLUME  *Volume;
  YAFFS2_OBJECT  *Current;
  YAFFS2_OBJECT  *Child;
  YAFFS2_IFILE   *NewIFile;
  CHAR8          Path8[512];
  UINTN          Len;
  UINTN          i;
  CHAR8          *p;
  CHAR8          *End;
  UINTN          CompLen;
  LIST_ENTRY     *Entry;
  BOOLEAN        Found;

  if ((This == NULL) || (NewHandle == NULL) || (FileName == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Reject write/create modes — filesystem is read-only
  //
  if ((OpenMode & EFI_FILE_MODE_WRITE) != 0) {
    return EFI_WRITE_PROTECTED;
  }
  if ((OpenMode & EFI_FILE_MODE_CREATE) != 0) {
    return EFI_WRITE_PROTECTED;
  }

  IFile  = YAFFS2_IFILE_FROM_HANDLE (This);
  Volume = IFile->Volume;

  //
  // Start path resolution from current object.
  // If current is a file, start from its parent directory.
  //
  Current = IFile->OFile;
  if (Current->Type != YAFFS2_TYPE_DIR) {
    Current = Current->Parent;
    if (Current == NULL) {
      Current = Volume->Root;
    }
  }

  //
  // Convert CHAR16 path to ASCII
  //
  Len = StrLen (FileName);
  if (Len >= sizeof (Path8)) {
    Len = sizeof (Path8) - 1;
  }
  for (i = 0; i <= Len; i++) {
    Path8[i] = (CHAR8)FileName[i];
  }

  //
  // Walk path components
  //
  p = Path8;

  //
  // Leading backslash = absolute path from root
  //
  if (*p == '\\') {
    Current = Volume->Root;
    p++;
  }

  while (*p != '\0') {
    //
    // Extract next component (delimited by '\\')
    //
    End = p;
    while ((*End != '\0') && (*End != '\\')) {
      End++;
    }
    CompLen = (UINTN)(End - p);

    if (CompLen == 0) {
      //
      // Skip empty components (e.g., "\\\\")
      //
      p = End + 1;
      continue;
    }

    if ((CompLen == 1) && (p[0] == '.')) {
      //
      // "." — stay in current directory
      //
    } else if ((CompLen == 2) && (p[0] == '.') && (p[1] == '.')) {
      //
      // ".." — go to parent
      //
      if (Current->Parent != NULL) {
        Current = Current->Parent;
      }
    } else {
      //
      // Search children by name
      //
      if (Current->Type != YAFFS2_TYPE_DIR) {
        return EFI_NOT_FOUND;
      }

      Found = FALSE;
      for (Entry = Current->ChildHead.ForwardLink;
           Entry != &Current->ChildHead;
           Entry = Entry->ForwardLink)
      {
        Child = BASE_CR (Entry, YAFFS2_OBJECT, SiblingLink);
        if ((AsciiStrnCmp (Child->Name, p, CompLen) == 0) &&
            (Child->Name[CompLen] == '\0'))
        {
          Current = Child;
          Found   = TRUE;
          break;
        }
      }

      if (!Found) {
        return EFI_NOT_FOUND;
      }

      //
      // Follow hardlinks
      //
      Current = ResolveHardlinks (Volume, Current);
      if (Current == NULL) {
        return EFI_NOT_FOUND;
      }
    }

    p = End;
    if (*p == '\\') {
      p++;
    }
  }

  //
  // Allocate a new IFILE for the resolved object
  //
  NewIFile = Yaffs2AllocateIFile (Volume, Current);
  if (NewIFile == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *NewHandle = &NewIFile->Handle;
  return EFI_SUCCESS;
}

/**
  Close an open file handle.
**/
EFI_STATUS
EFIAPI
Yaffs2Close (
  IN  EFI_FILE_PROTOCOL  *This
  )
{
  YAFFS2_IFILE  *IFile;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  IFile = YAFFS2_IFILE_FROM_HANDLE (This);
  FreePool (IFile);

  return EFI_SUCCESS;
}

/**
  Delete is not supported on a read-only filesystem.
**/
EFI_STATUS
EFIAPI
Yaffs2Delete (
  IN  EFI_FILE_PROTOCOL  *This
  )
{
  //
  // Close the file and return warning
  //
  Yaffs2Close (This);
  return EFI_WARN_DELETE_FAILURE;
}
