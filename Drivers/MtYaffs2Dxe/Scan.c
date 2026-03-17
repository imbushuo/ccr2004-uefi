/** @file
  MikroTik YAFFS2 filesystem driver — NAND detection and full scan.

  Implements YAFFS2 detection (quick probe of first blocks) and
  full NAND scan with multi-version resolution.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"

/**
  Check if a page is erased (all 0xFF in tag area).
**/
STATIC
BOOLEAN
IsPageErased (
  IN  UINT8  *Page
  )
{
  UINT32  *Tags;

  Tags = (UINT32 *)(Page + YAFFS2_DATA_PER_PAGE);
  return (Tags[0] == 0xFFFFFFFF) &&
         (Tags[1] == 0xFFFFFFFF) &&
         (Tags[2] == 0xFFFFFFFF) &&
         (Tags[3] == 0xFFFFFFFF);
}

/**
  Parse inline tags from the last 16 bytes of a NAND page.

  Tag layout (4 x UINT32):
    Word 0: seq_number
    Word 1: (type<<28)|obj_id  (header) or obj_id (data)
    Word 2: 0x80000000|parent_id (header) or chunk_id (data, 1-based)
    Word 3: file_size (header) or n_bytes (data)
**/
STATIC
VOID
ParseTags (
  IN  UINT8        *Page,
  OUT YAFFS2_TAGS  *Tags
  )
{
  UINT32  *Raw;

  Raw = (UINT32 *)(Page + YAFFS2_DATA_PER_PAGE);

  Tags->SeqNumber = Raw[0];
  Tags->IsHeader  = (Raw[2] & 0x80000000) != 0;

  if (Tags->IsHeader) {
    Tags->ObjId   = Raw[1] & 0x0FFFFFFF;
    Tags->ChunkId = 0;
    Tags->NBytes  = Raw[3];
  } else {
    Tags->ObjId   = Raw[1];
    Tags->ChunkId = Raw[2];
    Tags->NBytes  = Raw[3];
  }
}

/**
  Ensure Volume->Objects array can hold at least ObjId entries.
**/
STATIC
EFI_STATUS
EnsureObjectsCapacity (
  IN  YAFFS2_VOLUME  *Volume,
  IN  UINT32         ObjId
  )
{
  UINT32          NewMax;
  YAFFS2_OBJECT   **NewObjects;

  if (ObjId <= Volume->MaxObjId) {
    return EFI_SUCCESS;
  }

  NewMax = (Volume->MaxObjId + 1) * 2;
  if (NewMax < 256) {
    NewMax = 256;
  }
  if (NewMax <= ObjId) {
    NewMax = ObjId + 64;
  }

  NewObjects = AllocateZeroPool ((NewMax + 1) * sizeof (YAFFS2_OBJECT *));
  if (NewObjects == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Volume->Objects != NULL) {
    CopyMem (NewObjects, Volume->Objects,
             (Volume->MaxObjId + 1) * sizeof (YAFFS2_OBJECT *));
    FreePool (Volume->Objects);
  }

  Volume->Objects  = NewObjects;
  Volume->MaxObjId = NewMax;
  return EFI_SUCCESS;
}

/**
  Find or create an object entry in the volume.
**/
STATIC
YAFFS2_OBJECT *
FindOrCreateObject (
  IN  YAFFS2_VOLUME  *Volume,
  IN  UINT32         ObjId
  )
{
  EFI_STATUS     Status;
  YAFFS2_OBJECT  *Obj;

  Status = EnsureObjectsCapacity (Volume, ObjId);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Obj = Volume->Objects[ObjId];
  if (Obj != NULL) {
    return Obj;
  }

  Obj = AllocateZeroPool (sizeof (YAFFS2_OBJECT));
  if (Obj == NULL) {
    return NULL;
  }

  Obj->ObjId = ObjId;
  InitializeListHead (&Obj->ChildHead);
  InitializeListHead (&Obj->SiblingLink);

  Volume->Objects[ObjId] = Obj;
  return Obj;
}

/**
  Process a header page: parse object header fields and update if newer.
**/
STATIC
VOID
ProcessHeader (
  IN  YAFFS2_VOLUME  *Volume,
  IN  UINT8          *PageBuf,
  IN  YAFFS2_TAGS    *Tags,
  IN  UINT32         PageIdx
  )
{
  YAFFS2_OBJECT  *Obj;
  UINT8          *Data;
  UINT32         SizeLow;
  UINT32         SizeHigh;

  Obj = FindOrCreateObject (Volume, Tags->ObjId);
  if (Obj == NULL) {
    return;
  }

  //
  // Check if existing header is newer
  //
  if (Obj->HeaderSeq != 0) {
    if (Tags->SeqNumber < Obj->HeaderSeq) {
      return;
    }
    if ((Tags->SeqNumber == Obj->HeaderSeq) && (PageIdx <= Obj->HeaderPage)) {
      return;
    }
  }

  //
  // Parse object header from the data area of the page.
  //
  // Layout (MikroTik modified YAFFS2):
  //   0x000: type (UINT32)
  //   0x004: parent_id (UINT32)
  //   0x008: sum (UINT16, unused)
  //   0x00A: name (256 bytes, null-terminated ASCII)
  //   0x10A: padding (2 bytes, MikroTik addition)
  //   0x10C: yst_mode (UINT32)
  //   0x110: uid (UINT32, unused here)
  //   0x114: gid (UINT32, unused here)
  //   0x118: atime (UINT32, unused here)
  //   0x11C: mtime (UINT32)
  //   0x120: ctime (UINT32, unused here)
  //   0x124: file_size_low (UINT32)
  //   0x128: file_size_high (UINT32)
  //   0x12C: equiv_id (UINT32)
  //   0x130: alias (160 bytes, symlinks)
  //
  Data = PageBuf;

  Obj->Type     = *(UINT32 *)(Data + 0x00);
  Obj->ParentId = *(UINT32 *)(Data + 0x04);

  CopyMem (Obj->Name, Data + 0x0A, 255);
  Obj->Name[255] = '\0';

  Obj->Mode  = *(UINT32 *)(Data + 0x10C);
  Obj->MTime = *(UINT32 *)(Data + 0x11C);
  if (Obj->MTime == 0xFFFFFFFF) {
    Obj->MTime = 0;
  }

  SizeLow  = *(UINT32 *)(Data + 0x124);
  SizeHigh = *(UINT32 *)(Data + 0x128);
  if (SizeLow == 0xFFFFFFFF) {
    SizeLow = 0;
  }
  if (SizeHigh == 0xFFFFFFFF) {
    SizeHigh = 0;
  }
  Obj->FileSize = (UINT64)SizeLow | ((UINT64)SizeHigh << 32);

  Obj->EquivId = *(UINT32 *)(Data + 0x12C);
  if (Obj->EquivId == 0xFFFFFFFF) {
    Obj->EquivId = 0;
  }

  if (Obj->Type == YAFFS2_TYPE_SYMLINK) {
    CopyMem (Obj->Alias, Data + 0x130, 159);
    Obj->Alias[159] = '\0';
  }

  Obj->HeaderSeq  = Tags->SeqNumber;
  Obj->HeaderPage = PageIdx;
}

/**
  Process a data chunk page: record chunk location if newer.
**/
STATIC
VOID
ProcessDataChunk (
  IN  YAFFS2_VOLUME  *Volume,
  IN  YAFFS2_TAGS    *Tags,
  IN  UINT32         PageIdx
  )
{
  YAFFS2_OBJECT     *Obj;
  UINT32            ChunkIdx;
  UINT32            NewAlloc;
  YAFFS2_CHUNK_LOC  *NewChunks;
  YAFFS2_CHUNK_LOC  *Existing;

  if (Tags->ChunkId == 0) {
    return;
  }

  Obj = FindOrCreateObject (Volume, Tags->ObjId);
  if (Obj == NULL) {
    return;
  }

  ChunkIdx = Tags->ChunkId - 1;

  //
  // Grow chunks array if needed
  //
  if (Tags->ChunkId > Obj->ChunkAlloc) {
    NewAlloc = Obj->ChunkAlloc * 2;
    if (NewAlloc < 8) {
      NewAlloc = 8;
    }
    if (NewAlloc < Tags->ChunkId) {
      NewAlloc = Tags->ChunkId + 8;
    }

    NewChunks = AllocatePool (NewAlloc * sizeof (YAFFS2_CHUNK_LOC));
    if (NewChunks == NULL) {
      return;
    }
    //
    // Initialize all entries as unset (0xFF → PageIndex = 0xFFFFFFFF)
    //
    SetMem (NewChunks, NewAlloc * sizeof (YAFFS2_CHUNK_LOC), 0xFF);

    if (Obj->Chunks != NULL) {
      CopyMem (NewChunks, Obj->Chunks,
               Obj->ChunkAlloc * sizeof (YAFFS2_CHUNK_LOC));
      FreePool (Obj->Chunks);
    }

    Obj->Chunks     = NewChunks;
    Obj->ChunkAlloc = NewAlloc;
  }

  //
  // Update if this chunk is newer than existing
  //
  Existing = &Obj->Chunks[ChunkIdx];
  if ((Existing->PageIndex == YAFFS2_CHUNK_UNSET) ||
      (Tags->SeqNumber > Existing->SeqNumber) ||
      ((Tags->SeqNumber == Existing->SeqNumber) && (PageIdx > Existing->PageIndex)))
  {
    Existing->PageIndex  = PageIdx;
    Existing->NBytes     = Tags->NBytes;
    Existing->SeqNumber  = Tags->SeqNumber;
  }

  //
  // Track maximum chunk_id
  //
  if (Tags->ChunkId > Obj->ChunkCount) {
    Obj->ChunkCount = Tags->ChunkId;
  }
}

/**
  Post-scan: remove pseudo/deleted objects and build the directory tree.
**/
STATIC
VOID
BuildTree (
  IN  YAFFS2_VOLUME  *Volume
  )
{
  UINT32         i;
  YAFFS2_OBJECT  *Obj;
  YAFFS2_OBJECT  *Parent;
  UINT32         LiveCount;

  LiveCount = 0;

  //
  // First pass: remove invalid objects
  //
  for (i = 1; i <= Volume->MaxObjId; i++) {
    Obj = Volume->Objects[i];
    if (Obj == NULL) {
      continue;
    }

    //
    // Skip objects without a valid header
    //
    if (Obj->Type == 0) {
      if (Obj->Chunks != NULL) {
        FreePool (Obj->Chunks);
      }
      FreePool (Obj);
      Volume->Objects[i] = NULL;
      continue;
    }

    //
    // Skip pseudo-objects (unlinked dir = 3, deleted dir = 4)
    //
    if ((i == 3) || (i == 4)) {
      if (Obj->Chunks != NULL) {
        FreePool (Obj->Chunks);
      }
      FreePool (Obj);
      Volume->Objects[i] = NULL;
      continue;
    }

    //
    // Skip objects parented under unlinked/deleted directories
    //
    if ((Obj->ParentId == 3) || (Obj->ParentId == 4)) {
      if (Obj->Chunks != NULL) {
        FreePool (Obj->Chunks);
      }
      FreePool (Obj);
      Volume->Objects[i] = NULL;
      continue;
    }

    LiveCount++;
  }

  //
  // Second pass: resolve parent pointers and build child lists
  //
  for (i = 1; i <= Volume->MaxObjId; i++) {
    Obj = Volume->Objects[i];
    if (Obj == NULL) {
      continue;
    }

    //
    // Root object (obj_id 1) has no parent
    //
    if (i == 1) {
      Obj->Parent = NULL;
      continue;
    }

    //
    // Link to parent
    //
    if ((Obj->ParentId == 0) ||
        (Obj->ParentId > Volume->MaxObjId) ||
        (Volume->Objects[Obj->ParentId] == NULL))
    {
      //
      // Orphan — attach to root as fallback
      //
      if (Volume->Objects[1] != NULL) {
        Obj->Parent = Volume->Objects[1];
        InsertTailList (&Volume->Objects[1]->ChildHead, &Obj->SiblingLink);
      }
      continue;
    }

    Parent = Volume->Objects[Obj->ParentId];
    Obj->Parent = Parent;
    InsertTailList (&Parent->ChildHead, &Obj->SiblingLink);
  }

  Volume->Root = (Volume->MaxObjId >= 1) ? Volume->Objects[1] : NULL;

  DEBUG ((DEBUG_INFO, "[MtYaffs2] Tree built: %u live objects\n", LiveCount));
}

/**
  Quick YAFFS2 detection: probe first blocks for valid YAFFS2 signatures.

  @param[in]  Nand        NAND flash protocol instance.
  @param[in]  PageBuffer  Caller-provided buffer of at least Nand->PageSize bytes.

  @retval EFI_SUCCESS      YAFFS2 filesystem detected.
  @retval EFI_UNSUPPORTED  No YAFFS2 found.
**/
EFI_STATUS
Yaffs2DetectFilesystem (
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *Nand,
  IN  UINT8                         *PageBuffer
  )
{
  EFI_STATUS   Status;
  UINT32       Block;
  UINT32       BasePage;
  UINT32       PageInBlock;
  YAFFS2_TAGS  Tags;
  UINT32       FirstSeq;
  BOOLEAN      SeqOk;
  BOOLEAN      FoundHeader;
  UINT32       ObjType;

  for (Block = 0; (Block < 10) && (Block < Nand->NumBlocks); Block++) {
    BasePage = Block * Nand->PagesPerBlock;

    Status = Nand->ReadPage (Nand, BasePage, PageBuffer);
    if (EFI_ERROR (Status) || IsPageErased (PageBuffer)) {
      continue;
    }

    ParseTags (PageBuffer, &Tags);
    if ((Tags.SeqNumber < YAFFS2_SEQ_MIN) || (Tags.SeqNumber > YAFFS2_SEQ_MAX)) {
      continue;
    }

    //
    // Found a non-erased block with valid seq. Verify consistency + look for headers.
    //
    FirstSeq    = Tags.SeqNumber;
    SeqOk       = TRUE;
    FoundHeader = FALSE;

    if (Tags.IsHeader) {
      ObjType = *(UINT32 *)PageBuffer;
      if ((ObjType >= YAFFS2_TYPE_FILE) && (ObjType <= YAFFS2_TYPE_SPECIAL)) {
        FoundHeader = TRUE;
      }
    }

    for (PageInBlock = 1; PageInBlock < Nand->PagesPerBlock; PageInBlock++) {
      Status = Nand->ReadPage (Nand, BasePage + PageInBlock, PageBuffer);
      if (EFI_ERROR (Status) || IsPageErased (PageBuffer)) {
        continue;
      }

      ParseTags (PageBuffer, &Tags);
      if (Tags.SeqNumber != FirstSeq) {
        SeqOk = FALSE;
        break;
      }

      if (!FoundHeader && Tags.IsHeader) {
        ObjType = *(UINT32 *)PageBuffer;
        if ((ObjType >= YAFFS2_TYPE_FILE) && (ObjType <= YAFFS2_TYPE_SPECIAL)) {
          FoundHeader = TRUE;
        }
      }
    }

    if (SeqOk && FoundHeader) {
      DEBUG ((DEBUG_INFO, "[MtYaffs2] Detected YAFFS2 on NAND (block %u, seq 0x%x)\n",
              Block, FirstSeq));
      return EFI_SUCCESS;
    }
  }

  return EFI_UNSUPPORTED;
}

/**
  Full NAND scan: read all pages, parse YAFFS2 structures, build filesystem tree.

  @param[in]  Volume  Volume with Nand and PageBuffer already set up.

  @retval EFI_SUCCESS          Scan complete, Root is valid.
  @retval EFI_VOLUME_CORRUPTED No valid root object found.
  @retval EFI_OUT_OF_RESOURCES Allocation failure.
**/
EFI_STATUS
Yaffs2ScanNand (
  IN  YAFFS2_VOLUME  *Volume
  )
{
  MIKROTIK_NAND_FLASH_PROTOCOL  *Nand;
  UINT8                         *PageBuf;
  UINT32                        Block;
  UINT32                        BasePage;
  UINT32                        PageInBlock;
  UINT32                        Page;
  EFI_STATUS                    Status;
  YAFFS2_TAGS                   Tags;
  UINT32                        UsedBlocks;
  UINT32                        PageCount;

  Nand       = Volume->Nand;
  PageBuf    = Volume->PageBuffer;
  UsedBlocks = 0;
  PageCount  = 0;

  DEBUG ((DEBUG_INFO, "[MtYaffs2] Scanning %u blocks...\n", Nand->NumBlocks));

  for (Block = 0; Block < Nand->NumBlocks; Block++) {
    BasePage = Block * Nand->PagesPerBlock;

    //
    // Quick check: if first page of block is erased, skip entire block.
    //
    Status = Nand->ReadPage (Nand, BasePage, PageBuf);
    if (EFI_ERROR (Status) || IsPageErased (PageBuf)) {
      continue;
    }

    UsedBlocks++;

    for (PageInBlock = 0; PageInBlock < Nand->PagesPerBlock; PageInBlock++) {
      Page = BasePage + PageInBlock;

      if (PageInBlock > 0) {
        Status = Nand->ReadPage (Nand, Page, PageBuf);
        if (EFI_ERROR (Status)) {
          continue;
        }
      }

      //
      // YAFFS2 writes pages sequentially within a block.
      // Once we hit an erased page, the rest of the block is also erased.
      //
      if (IsPageErased (PageBuf)) {
        break;
      }

      PageCount++;
      ParseTags (PageBuf, &Tags);

      //
      // Validate sequence number
      //
      if ((Tags.SeqNumber < YAFFS2_SEQ_MIN) || (Tags.SeqNumber > YAFFS2_SEQ_MAX)) {
        continue;
      }

      //
      // Skip invalid or pseudo object IDs
      //
      if ((Tags.ObjId == 0) || (Tags.ObjId == 3) || (Tags.ObjId == 4)) {
        continue;
      }

      if (Tags.IsHeader) {
        ProcessHeader (Volume, PageBuf, &Tags, Page);
      } else {
        ProcessDataChunk (Volume, &Tags, Page);
      }
    }
  }

  DEBUG ((DEBUG_INFO, "[MtYaffs2] Scanned %u used blocks, %u pages\n",
          UsedBlocks, PageCount));

  //
  // Build the directory tree
  //
  BuildTree (Volume);

  if ((Volume->Root == NULL) || (Volume->Root->Type != YAFFS2_TYPE_DIR)) {
    DEBUG ((DEBUG_ERROR, "[MtYaffs2] Root object not found or not a directory\n"));
    return EFI_VOLUME_CORRUPTED;
  }

  DEBUG ((DEBUG_INFO, "[MtYaffs2] Scan complete, root obj_id=1\n"));
  return EFI_SUCCESS;
}
