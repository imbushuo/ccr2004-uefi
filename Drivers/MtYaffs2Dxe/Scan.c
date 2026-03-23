/** @file
  MikroTik YAFFS2 filesystem driver — NAND detection and full scan.

  Implements YAFFS2 detection (quick probe of first blocks) and
  full NAND scan with multi-version resolution.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "MtYaffs2Dxe.h"
#include <Library/TimerLib.h>

/**
  Check if raw tag bytes (16 bytes) indicate an erased page.
**/
STATIC
BOOLEAN
AreTagsErased (
  IN  UINT32  *Raw
  )
{
  return (Raw[0] == 0xFFFFFFFF) &&
         (Raw[1] == 0xFFFFFFFF) &&
         (Raw[2] == 0xFFFFFFFF) &&
         (Raw[3] == 0xFFFFFFFF);
}

/**
  Parse raw tag words (4 x UINT32) into YAFFS2_TAGS.

  Tag layout:
    Word 0: seq_number
    Word 1: (type<<28)|obj_id  (header) or obj_id (data)
    Word 2: 0x80000000|parent_id (header) or chunk_id (data, 1-based)
    Word 3: file_size (header) or n_bytes (data)
**/
STATIC
VOID
ParseTagWords (
  IN  UINT32       *Raw,
  OUT YAFFS2_TAGS  *Tags
  )
{
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
    if ((i == YAFFS2_OBJECTID_UNLINKED) || (i == YAFFS2_OBJECTID_DELETED)) {
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
    if ((Obj->ParentId == YAFFS2_OBJECTID_UNLINKED) || (Obj->ParentId == YAFFS2_OBJECTID_DELETED)) {
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
  UINT32       RawTags[4];
  UINT32       FirstSeq;
  BOOLEAN      SeqOk;
  BOOLEAN      FoundHeader;
  UINT32       ObjType;

  for (Block = 0; (Block < 10) && (Block < Nand->NumBlocks); Block++) {
    BasePage = Block * Nand->PagesPerBlock;

    //
    // Use fast tag-only read for detection
    //
    Status = Nand->ReadTags (Nand, BasePage, RawTags);
    if (EFI_ERROR (Status) || AreTagsErased (RawTags)) {
      continue;
    }

    ParseTagWords (RawTags, &Tags);
    if ((Tags.SeqNumber < YAFFS2_SEQ_MIN) || (Tags.SeqNumber > YAFFS2_SEQ_MAX)) {
      continue;
    }

    //
    // Found a non-erased block with valid seq. Verify consistency + look for headers.
    //
    FirstSeq    = Tags.SeqNumber;
    SeqOk       = TRUE;
    FoundHeader = FALSE;

    //
    // For header detection we need the page data to check obj_type
    //
    if (Tags.IsHeader) {
      Status = Nand->ReadPage (Nand, BasePage, PageBuffer);
      if (!EFI_ERROR (Status)) {
        ObjType = *(UINT32 *)PageBuffer;
        if ((ObjType >= YAFFS2_TYPE_FILE) && (ObjType <= YAFFS2_TYPE_SPECIAL)) {
          FoundHeader = TRUE;
        }
      }
    }

    for (PageInBlock = 1; PageInBlock < Nand->PagesPerBlock; PageInBlock++) {
      Status = Nand->ReadTags (Nand, BasePage + PageInBlock, RawTags);
      if (EFI_ERROR (Status) || AreTagsErased (RawTags)) {
        break;  // Sequential write — rest of block is erased
      }

      ParseTagWords (RawTags, &Tags);
      if (Tags.SeqNumber != FirstSeq) {
        SeqOk = FALSE;
        break;
      }

      if (!FoundHeader && Tags.IsHeader) {
        Status = Nand->ReadPage (Nand, BasePage + PageInBlock, PageBuffer);
        if (!EFI_ERROR (Status)) {
          ObjType = *(UINT32 *)PageBuffer;
          if ((ObjType >= YAFFS2_TYPE_FILE) && (ObjType <= YAFFS2_TYPE_SPECIAL)) {
            FoundHeader = TRUE;
          }
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

//
// Block summary format — matches mainline YAFFS2 (yaffs_summary.c).
//
// Summary page tags (packed_tags2): obj_id = YAFFS_OBJECTID_SUMMARY (0x10),
// chunk_id = 1.  Stored on the last page of each block.
//
// Data layout:
//   yaffs_summary_header (16 bytes):
//     UINT32 version  — must be YAFFS_SUMMARY_VERSION (1)
//     UINT32 block    — block number (internal, not validated here)
//     UINT32 seq      — must match block's sequence number
//     UINT32 sum      — byte-sum checksum of all summary tag data
//
//   (PagesPerBlock-1) × yaffs_summary_tags (12 bytes each):
//     UINT32 obj_id   — packed: (type<<28)|obj_id for headers, plain for data
//     UINT32 chunk_id — packed: EXTRA_HEADER_INFO_FLAG|parent_id for headers
//     UINT32 n_bytes  — file_size for headers, valid bytes for data
//
#define YAFFS2_SUMMARY_OBJ_ID    0x10    // YAFFS_OBJECTID_SUMMARY
#define YAFFS2_SUMMARY_HEADER_SZ 16
#define YAFFS2_SUMMARY_ENTRY_SZ  12
#define YAFFS2_SUMMARY_VERSION   1
#define YAFFS2_MAX_PAGES_PER_BLK 64      // Stack array sizing

/**
  Compute the mainline YAFFS2 summary checksum: byte-sum of all
  summary entry data (matching yaffs_summary_sum in yaffs_summary.c).
**/
STATIC
UINT32
SummaryChecksum (
  IN  UINT8   *EntryData,
  IN  UINT32  EntryBytes
  )
{
  UINT32  Sum;
  UINT32  i;

  Sum = 0;
  for (i = 0; i < EntryBytes; i++) {
    Sum += EntryData[i];
  }
  return Sum;
}

/**
  Process a block using its summary page.

  Reads the summary from the last page, validates it (version, seq,
  checksum, stale-entry detection), copies entries to stack, then
  selectively reads only header pages that need full object data.

  @param[in]     Volume       Volume context.
  @param[in]     Block        Block number.
  @param[in,out] PageCount    Incremented by pages processed.
  @param[in,out] HeaderCount  Incremented by headers processed.

  @retval TRUE   Summary was valid and block was fully processed.
  @retval FALSE  Summary invalid/stale — caller should fall back.
**/
STATIC
BOOLEAN
ScanBlockViaSummary (
  IN     YAFFS2_VOLUME  *Volume,
  IN     UINT32         Block,
  IN OUT UINT32         *PageCount,
  IN OUT UINT32         *HeaderCount
  )
{
  MIKROTIK_NAND_FLASH_PROTOCOL  *Nand;
  UINT8                         *PageBuf;
  UINT32                        SummaryPage;
  UINT32                        RawTags[4];
  EFI_STATUS                    Status;
  UINT32                        Version;
  UINT32                        BlockSeq;
  UINT32                        StoredSum;
  UINT32                        NumEntries;
  UINT32                        SummaryBytes;
  UINT32                        i;
  UINT32                        EntryObjId;
  UINT32                        EntryChunkId;
  UINT32                        EntryNBytes;
  BOOLEAN                       EntryIsHeader;
  YAFFS2_TAGS                   Tags;
  UINT32                        BasePage;
  UINT32                        EntryStore[3 * YAFFS2_MAX_PAGES_PER_BLK];
  UINT32                        EntryCount;

  Nand    = Volume->Nand;
  PageBuf = Volume->PageBuffer;

  //
  // Read tags of the last page in the block.
  //
  BasePage    = Block * Nand->PagesPerBlock;
  SummaryPage = BasePage + (Nand->PagesPerBlock - 1);

  Status = Nand->ReadTags (Nand, SummaryPage, RawTags);
  if (EFI_ERROR (Status) || AreTagsErased (RawTags)) {
    return FALSE;
  }

  //
  // Validate summary marker using reference logic:
  //   Word 1 (obj_id field) = YAFFS_OBJECTID_SUMMARY (0x10)
  //   Word 2 (chunk_id field): bit 31 clear, value = 1
  //
  // Since YAFFS_OBJECTID_SUMMARY (0x10) has no type bits in upper nibble
  // and chunk_id = 1 has no EXTRA_HEADER_INFO_FLAG, this is always a
  // non-header chunk when unpacked.
  //
  if (RawTags[1] != YAFFS2_SUMMARY_OBJ_ID) {
    return FALSE;
  }
  if ((RawTags[2] & 0x80000000) != 0) {
    return FALSE;
  }
  if (RawTags[2] != 1) {
    return FALSE;
  }

  //
  // Read summary page body (reuse NAND buffer — no 2nd tR).
  //
  Status = Nand->ReadPageBody (Nand, PageBuf);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  //
  // Validate summary header fields.
  //
  Version   = *(UINT32 *)(PageBuf + 0);
  BlockSeq  = *(UINT32 *)(PageBuf + 8);
  StoredSum = *(UINT32 *)(PageBuf + 12);

  if (Version != YAFFS2_SUMMARY_VERSION) {
    return FALSE;
  }
  if ((BlockSeq < YAFFS2_SEQ_MIN) || (BlockSeq > YAFFS2_SEQ_MAX)) {
    return FALSE;
  }

  NumEntries = Nand->PagesPerBlock - 1;
  SummaryBytes = NumEntries * YAFFS2_SUMMARY_ENTRY_SZ;
  if (YAFFS2_SUMMARY_HEADER_SZ + SummaryBytes > YAFFS2_DATA_PER_PAGE) {
    return FALSE;
  }

  //
  // Validate checksum (byte-sum of entry data, matching yaffs_summary.c).
  //
  if (SummaryChecksum (PageBuf + YAFFS2_SUMMARY_HEADER_SZ, SummaryBytes) != StoredSum) {
    return FALSE;
  }

  //
  // Detect stale/incomplete summaries.
  //
  // MikroTik's YAFFS2 can write summaries with zero entries {0,0,0} for
  // pages that actually contain valid data.  If the first entry is all-zero
  // but the block has data on page 0 (confirmed by the erased-block check
  // in the caller), the summary is stale — fall back to page-by-page.
  //
  {
    UINT8  *FirstEntry;

    FirstEntry = PageBuf + YAFFS2_SUMMARY_HEADER_SZ;
    if ((*(UINT32 *)(FirstEntry + 0) == 0) &&
        (*(UINT32 *)(FirstEntry + 4) == 0) &&
        (*(UINT32 *)(FirstEntry + 8) == 0))
    {
      return FALSE;
    }
  }

  //
  // Copy entries to stack BEFORE reading any header pages (which
  // will overwrite PageBuf).  Max 63 entries × 12 bytes = 756 bytes.
  //
  EntryCount = 0;
  for (i = 0; i < NumEntries; i++) {
    UINT8  *Ep;

    Ep = PageBuf + YAFFS2_SUMMARY_HEADER_SZ + i * YAFFS2_SUMMARY_ENTRY_SZ;

    EntryObjId   = *(UINT32 *)(Ep + 0);
    EntryChunkId = *(UINT32 *)(Ep + 4);
    EntryNBytes  = *(UINT32 *)(Ep + 8);

    if ((EntryObjId == 0xFFFFFFFF) &&
        (EntryChunkId == 0xFFFFFFFF) &&
        (EntryNBytes == 0xFFFFFFFF))
    {
      break;
    }

    EntryStore[EntryCount * 3 + 0] = EntryObjId;
    EntryStore[EntryCount * 3 + 1] = EntryChunkId;
    EntryStore[EntryCount * 3 + 2] = EntryNBytes;
    EntryCount++;
  }

  //
  // Process entries — PageBuf is now free for header reads.
  //
  for (i = 0; i < EntryCount; i++) {
    EntryObjId   = EntryStore[i * 3 + 0];
    EntryChunkId = EntryStore[i * 3 + 1];
    EntryNBytes  = EntryStore[i * 3 + 2];

    //
    // Unpack using reference packed_tags2 logic:
    //   EXTRA_HEADER_INFO_FLAG (bit 31 of chunk_id) → header chunk
    //   obj_id upper 4 bits → object type (masked off for real obj_id)
    //
    EntryIsHeader = (EntryChunkId & 0x80000000) != 0;

    Tags.SeqNumber = BlockSeq;
    Tags.IsHeader  = EntryIsHeader;
    if (EntryIsHeader) {
      Tags.ObjId   = EntryObjId & 0x0FFFFFFF;
      Tags.ChunkId = 0;
      Tags.NBytes  = EntryNBytes;
    } else {
      Tags.ObjId   = EntryObjId;
      Tags.ChunkId = EntryChunkId;
      Tags.NBytes  = EntryNBytes;
    }

    //
    // Skip pseudo-objects and summary chunks
    //
    if ((Tags.ObjId == 0) ||
        (Tags.ObjId == YAFFS2_OBJECTID_UNLINKED) ||
        (Tags.ObjId == YAFFS2_OBJECTID_DELETED) ||
        (Tags.ObjId == YAFFS2_SUMMARY_OBJ_ID))
    {
      continue;
    }

    (*PageCount)++;

    if (EntryIsHeader) {
      Status = Nand->ReadPage (Nand, BasePage + i, PageBuf);
      if (EFI_ERROR (Status)) {
        continue;
      }
      (*HeaderCount)++;
      ProcessHeader (Volume, PageBuf, &Tags, BasePage + i);
    } else {
      ProcessDataChunk (Volume, &Tags, BasePage + i);
    }
  }

  return TRUE;
}

/**
  Scan a single block page-by-page (fallback when no valid summary exists).
**/
STATIC
VOID
ScanBlockPageByPage (
  IN     YAFFS2_VOLUME  *Volume,
  IN     UINT32         Block,
  IN OUT UINT32         *PageCount,
  IN OUT UINT32         *HeaderCount
  )
{
  MIKROTIK_NAND_FLASH_PROTOCOL  *Nand;
  UINT8                         *PageBuf;
  UINT32                        BasePage;
  UINT32                        PageInBlock;
  UINT32                        Page;
  EFI_STATUS                    Status;
  YAFFS2_TAGS                   Tags;
  UINT32                        RawTags[4];

  Nand    = Volume->Nand;
  PageBuf = Volume->PageBuffer;
  BasePage = Block * Nand->PagesPerBlock;

  for (PageInBlock = 0; PageInBlock < Nand->PagesPerBlock; PageInBlock++) {
    Page = BasePage + PageInBlock;

    Status = Nand->ReadTags (Nand, Page, RawTags);
    if (EFI_ERROR (Status) || AreTagsErased (RawTags)) {
      break;
    }

    ParseTagWords (RawTags, &Tags);

    if ((Tags.SeqNumber < YAFFS2_SEQ_MIN) || (Tags.SeqNumber > YAFFS2_SEQ_MAX)) {
      continue;
    }

    if ((Tags.ObjId == 0) ||
        (Tags.ObjId == YAFFS2_OBJECTID_UNLINKED) ||
        (Tags.ObjId == YAFFS2_OBJECTID_DELETED) ||
        (Tags.ObjId == YAFFS2_SUMMARY_OBJ_ID))
    {
      continue;
    }

    (*PageCount)++;

    if (Tags.IsHeader) {
      //
      // Use ReadPageBody to reuse NAND buffer from ReadTags — avoids 2nd tR.
      //
      Status = Nand->ReadPageBody (Nand, PageBuf);
      if (EFI_ERROR (Status)) {
        continue;
      }
      (*HeaderCount)++;
      ProcessHeader (Volume, PageBuf, &Tags, Page);
    } else {
      ProcessDataChunk (Volume, &Tags, Page);
    }
  }
}

/**
  Full NAND scan: read block summaries (or fall back to page-by-page),
  parse YAFFS2 structures, and build the filesystem tree.

  Uses MikroTik's YAFFS2 block summary feature: the last page of each
  block (page 63) contains a condensed copy of all page tags, allowing
  the scan to process an entire block with just 1-2 NAND reads instead
  of 64.

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
  UINT32                        Block;
  UINT32                        BasePage;
  EFI_STATUS                    Status;
  UINT32                        RawTags[4];
  UINT32                        UsedBlocks;
  UINT32                        SummaryBlocks;
  UINT32                        FallbackBlocks;
  UINT32                        PageCount;
  UINT32                        HeaderCount;

  Nand           = Volume->Nand;
  UsedBlocks     = 0;
  SummaryBlocks  = 0;
  FallbackBlocks = 0;
  PageCount      = 0;
  HeaderCount    = 0;

  DEBUG ((DEBUG_INFO, "[MtYaffs2] Scanning %u blocks (summary-accelerated)...\n",
          Nand->NumBlocks));

  for (Block = 0; Block < Nand->NumBlocks; Block++) {
    BasePage = Block * Nand->PagesPerBlock;

    //
    // Quick erased-block check: read tags of page 0.
    //
    Status = Nand->ReadTags (Nand, BasePage, RawTags);
    if (EFI_ERROR (Status) || AreTagsErased (RawTags)) {
      continue;
    }

    UsedBlocks++;

    //
    // Try summary-based scan first (reads 1-2 pages per block).
    // Falls back to page-by-page if summary is missing or invalid.
    //
    if (ScanBlockViaSummary (Volume, Block, &PageCount, &HeaderCount)) {
      SummaryBlocks++;
    } else {
      FallbackBlocks++;
      ScanBlockPageByPage (Volume, Block, &PageCount, &HeaderCount);
    }
  }

  DEBUG ((DEBUG_INFO,
          "[MtYaffs2] Scanned %u used blocks (%u via summary, %u fallback), "
          "%u pages (%u headers)\n",
          UsedBlocks, SummaryBlocks, FallbackBlocks, PageCount, HeaderCount));

  BuildTree (Volume);

  if ((Volume->Root == NULL) || (Volume->Root->Type != YAFFS2_TYPE_DIR)) {
    DEBUG ((DEBUG_ERROR, "[MtYaffs2] Root object not found or not a directory\n"));
    return EFI_VOLUME_CORRUPTED;
  }

  DEBUG ((DEBUG_INFO, "[MtYaffs2] Scan complete, root obj_id=1\n"));
  return EFI_SUCCESS;
}
