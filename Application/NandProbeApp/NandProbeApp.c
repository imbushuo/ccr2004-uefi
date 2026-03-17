/** @file
  NandProbeApp — Scan NAND flash to identify filesystem format.

  Scans erase block boundaries on the NAND block device looking for
  non-erased data.  Identifies UBI (magic "UBI#") or YAFFS2 (inband
  tags + object header inspection).  Decodes YAFFS2 object headers
  to list files and directories found on flash.

  Usage: NandProbeApp [start_block [count]]

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
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ShellParameters.h>

/* NAND geometry (must match AlNandDxe) */
#define NAND_PAGE_SIZE        2048
#define NAND_PAGES_PER_BLOCK  64
#define NAND_BLOCK_SIZE       (NAND_PAGE_SIZE * NAND_PAGES_PER_BLOCK)  /* 128KB */
#define NAND_NUM_BLOCKS       1024

/* UBI EC header magic: "UBI#" = 0x55424923 big-endian */
#define UBI_EC_HDR_MAGIC  0x55424923

/* YAFFS2 constants */
#define YAFFS_INBAND_TAG_OFF   (NAND_PAGE_SIZE - 16)  /* last 16 bytes */
#define YAFFS_DATA_BYTES       (NAND_PAGE_SIZE - 16)   /* 2032 usable data bytes */

#define YAFFS_OBJECT_TYPE_UNKNOWN   0
#define YAFFS_OBJECT_TYPE_FILE      1
#define YAFFS_OBJECT_TYPE_SYMLINK   2
#define YAFFS_OBJECT_TYPE_DIRECTORY 3
#define YAFFS_OBJECT_TYPE_HARDLINK  4
#define YAFFS_OBJECT_TYPE_SPECIAL   5

#define YAFFS_OBJECTID_ROOT         1
#define YAFFS_OBJECTID_LOSTNFOUND   2
#define YAFFS_OBJECTID_UNLINKED     3
#define YAFFS_OBJECTID_DELETED      4

/* Extra flags packed into tag fields */
#define YAFFS_EXTRA_HEADER_FLAG   0x80000000
#define YAFFS_EXTRA_SHRINK_FLAG   0x40000000
#define YAFFS_EXTRA_SHADOWS_FLAG  0x20000000
#define YAFFS_EXTRA_SPARE_FLAGS   0x10000000
#define YAFFS_EXTRA_PARENT_MASK   0x0FFFFFFF
#define YAFFS_EXTRA_OBJTYPE_SHIFT 28
#define YAFFS_EXTRA_OBJID_MASK    0x0FFFFFFF

/* Sequence number validity range */
#define YAFFS_LOWEST_SEQ   0x00001000
#define YAFFS_HIGHEST_SEQ  0xEFFFFF00

/* YAFFS2 checkpoint magic */
#define YAFFS_MAGIC  0x5941FF53

/* Max objects to track for listing */
#define MAX_YAFFS_OBJECTS  256

/* AlNandDxe FILE_GUID used in its vendor device path */
STATIC EFI_GUID  mAlNandDevicePathGuid = {
  0x8b3e4d5c, 0xf2a1, 0x4b07,
  { 0xa8, 0xea, 0x3f, 0xbc, 0x2e, 0x5d, 0x7f, 0x94 }
};

#pragma pack(1)
/* UBI Erase Counter header (big-endian on flash) */
typedef struct {
  UINT32  Magic;
  UINT8   Version;
  UINT8   Padding[3];
  UINT64  Ec;
  UINT32  VidHdrOff;
  UINT32  DataOff;
  UINT32  ImageSeq;
  UINT8   Padding2[32];
  UINT32  HdrCrc;
} UBI_EC_HDR;

/* YAFFS2 inband packed tags (last 16 bytes of page data) */
typedef struct {
  UINT32  SeqNumber;   /* block allocation sequence */
  UINT32  ObjId;       /* object ID (top 4 bits = type for headers) */
  UINT32  ChunkId;     /* chunk within object; 0 = object header; flags in top bits */
  UINT32  NBytes;      /* data byte count or file size for headers */
} YAFFS_PACKED_TAGS;

/* YAFFS2 object header (stored in page data when chunk_id == 0) */
typedef struct {
  UINT32  Type;           /* yaffs_obj_type enum */
  UINT32  ParentObjId;
  UINT16  SumNoLongerUsed;
  CHAR8   Name[256];      /* null-terminated ASCII name */
  UINT32  YstMode;        /* Unix permissions */
  UINT32  YstUid;
  UINT32  YstGid;
  UINT32  YstAtime;
  UINT32  YstMtime;
  UINT32  YstCtime;
  UINT32  FileSizeLow;
  UINT32  EquivId;
  CHAR8   Alias[160];     /* symlink target */
  UINT32  YstRdev;
  UINT32  WinCtime[2];
  UINT32  WinAtime[2];
  UINT32  WinMtime[2];
  UINT32  InbandShadowedObjId;
  UINT32  InbandIsShrink;
  UINT32  FileSizeHigh;
  UINT32  Reserved;
  UINT32  ShadowsObj;
  UINT32  IsShrink;
} YAFFS_OBJ_HDR;
#pragma pack()

/* Object record for listing */
typedef struct {
  UINT32  ObjId;
  UINT32  ParentId;
  UINT32  Type;
  UINT32  Mode;
  UINT64  FileSize;
  UINT32  Mtime;
  CHAR8   Name[64];
  UINT32  SeqNumber;
} YAFFS_OBJ_RECORD;

STATIC
BOOLEAN
IsPageErased (
  IN UINT8   *Buf,
  IN UINTN   Size
  )
{
  UINTN   i;
  UINT64  *Ptr64 = (UINT64 *)Buf;
  UINTN   Count64 = Size / sizeof (UINT64);

  for (i = 0; i < Count64; i++) {
    if (Ptr64[i] != 0xFFFFFFFFFFFFFFFFULL) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
VOID
HexDump (
  IN UINT8   *Data,
  IN UINTN   Len,
  IN UINTN   BaseAddr
  )
{
  UINTN  i, j;

  for (i = 0; i < Len; i += 16) {
    Print (L"  %04x: ", BaseAddr + i);
    for (j = 0; j < 16 && (i + j) < Len; j++) {
      Print (L"%02x ", Data[i + j]);
    }
    for (; j < 16; j++) {
      Print (L"   ");
    }
    Print (L" ");
    for (j = 0; j < 16 && (i + j) < Len; j++) {
      UINT8  Ch = Data[i + j];
      Print (L"%c", (Ch >= 0x20 && Ch < 0x7F) ? Ch : L'.');
    }
    Print (L"\n");
  }
}

STATIC
CONST CHAR16 *
YaffsTypeName (
  IN UINT32  Type
  )
{
  switch (Type) {
    case YAFFS_OBJECT_TYPE_FILE:      return L"FILE";
    case YAFFS_OBJECT_TYPE_SYMLINK:   return L"SYMLINK";
    case YAFFS_OBJECT_TYPE_DIRECTORY: return L"DIR";
    case YAFFS_OBJECT_TYPE_HARDLINK:  return L"HARDLINK";
    case YAFFS_OBJECT_TYPE_SPECIAL:   return L"SPECIAL";
    default:                          return L"UNKNOWN";
  }
}

/**
  Validate YAFFS2 inband tags heuristically.
  Returns TRUE if this looks like a valid YAFFS2 tag.
**/
STATIC
BOOLEAN
IsValidYaffsTags (
  IN YAFFS_PACKED_TAGS  *Tags
  )
{
  UINT32  RawObjId;

  /* All-FF = erased */
  if (Tags->SeqNumber == 0xFFFFFFFF) {
    return FALSE;
  }

  /* All-zero = not valid either */
  if (Tags->SeqNumber == 0 && Tags->ObjId == 0 && Tags->ChunkId == 0) {
    return FALSE;
  }

  /* Sequence number should be in valid range */
  if (Tags->SeqNumber < YAFFS_LOWEST_SEQ || Tags->SeqNumber > YAFFS_HIGHEST_SEQ) {
    return FALSE;
  }

  /* Object ID (lower 28 bits) should be nonzero */
  RawObjId = Tags->ObjId & YAFFS_EXTRA_OBJID_MASK;
  if (RawObjId == 0) {
    return FALSE;
  }

  return TRUE;
}

/**
  Check if this page is a YAFFS2 object header (chunk_id == 0).
  For headers with extra flags, chunk_id has EXTRA_HEADER_FLAG set
  and the lower bits contain parent_obj_id.
**/
STATIC
BOOLEAN
IsYaffsObjectHeader (
  IN YAFFS_PACKED_TAGS  *Tags
  )
{
  /* chunk_id == 0 means object header (simple case) */
  if (Tags->ChunkId == 0) {
    return TRUE;
  }

  /* With extra header info: EXTRA_HEADER_FLAG set, no regular chunk_id */
  if (Tags->ChunkId & YAFFS_EXTRA_HEADER_FLAG) {
    return TRUE;
  }

  return FALSE;
}

STATIC
EFI_HANDLE
FindNandBlockIoHandle (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     i;
  EFI_DEVICE_PATH_PROTOCOL  *DevPath;
  VENDOR_DEVICE_PATH        *VendorNode;
  EFI_HANDLE                Found;

  Found = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (i = 0; i < HandleCount; i++) {
    Status = gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&DevPath);
    if (EFI_ERROR (Status)) {
      continue;
    }

    while (!IsDevicePathEnd (DevPath)) {
      if (DevicePathType (DevPath) == HARDWARE_DEVICE_PATH &&
          DevicePathSubType (DevPath) == HW_VENDOR_DP) {
        VendorNode = (VENDOR_DEVICE_PATH *)DevPath;
        if (CompareGuid (&VendorNode->Guid, &mAlNandDevicePathGuid)) {
          Found = Handles[i];
          break;
        }
      }
      DevPath = NextDevicePathNode (DevPath);
    }

    if (Found != NULL) {
      break;
    }
  }

  FreePool (Handles);
  return Found;
}

/**
  Deep-scan a block for YAFFS2 content.
  Reads all pages in the block, validates inband tags, decodes object headers.
  Returns the number of valid YAFFS2 pages found.
**/
STATIC
UINT32
ScanBlockForYaffs (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  UINT32                 Block,
  IN  BOOLEAN                Verbose,
  IN  YAFFS_OBJ_RECORD       *ObjList,
  IN  UINT32                 *ObjCount,
  IN  UINT32                 ObjMax
  )
{
  UINT8              *PageBuf;
  UINT32             Page;
  UINT32             ValidPages;
  YAFFS_PACKED_TAGS  *Tags;
  EFI_STATUS         Status;
  EFI_LBA            BaseLba;

  PageBuf = AllocatePool (NAND_PAGE_SIZE);
  if (PageBuf == NULL) {
    return 0;
  }

  ValidPages = 0;
  BaseLba = (EFI_LBA)Block * NAND_PAGES_PER_BLOCK;

  for (Page = 0; Page < NAND_PAGES_PER_BLOCK; Page++) {
    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        BaseLba + Page,
                        NAND_PAGE_SIZE,
                        PageBuf
                        );
    if (EFI_ERROR (Status)) {
      break;
    }

    if (IsPageErased (PageBuf, NAND_PAGE_SIZE)) {
      continue;  /* rest of block is likely erased too */
    }

    Tags = (YAFFS_PACKED_TAGS *)(PageBuf + YAFFS_INBAND_TAG_OFF);

    if (!IsValidYaffsTags (Tags)) {
      continue;
    }

    ValidPages++;

    if (IsYaffsObjectHeader (Tags)) {
      YAFFS_OBJ_HDR  *Hdr = (YAFFS_OBJ_HDR *)PageBuf;
      UINT32         ObjId = Tags->ObjId & YAFFS_EXTRA_OBJID_MASK;

      /* Validate object header minimally */
      if (Hdr->Type >= YAFFS_OBJECT_TYPE_FILE && Hdr->Type <= YAFFS_OBJECT_TYPE_SPECIAL) {
        /* Ensure name is printable and null-terminated within bounds */
        Hdr->Name[sizeof (Hdr->Name) - 1] = '\0';

        if (Verbose) {
          Print (L"    Page %3u: obj=%u type=%s parent=%u name=\"%a\"",
                 Page, ObjId, YaffsTypeName (Hdr->Type),
                 Hdr->ParentObjId, Hdr->Name);

          if (Hdr->Type == YAFFS_OBJECT_TYPE_FILE) {
            UINT64 Size = ((UINT64)Hdr->FileSizeHigh << 32) | Hdr->FileSizeLow;
            Print (L" size=%lu", Size);
          } else if (Hdr->Type == YAFFS_OBJECT_TYPE_SYMLINK) {
            Hdr->Alias[sizeof (Hdr->Alias) - 1] = '\0';
            Print (L" -> \"%a\"", Hdr->Alias);
          }

          Print (L" mode=0%o seq=%u\n", Hdr->YstMode, Tags->SeqNumber);
        }

        /* Record object if we have space and it's not a duplicate
         * (keep the one with highest sequence number) */
        if (ObjList != NULL && *ObjCount < ObjMax) {
          UINT32  k;
          BOOLEAN Found = FALSE;

          for (k = 0; k < *ObjCount; k++) {
            if (ObjList[k].ObjId == ObjId) {
              /* Keep newer version (higher seq) */
              if (Tags->SeqNumber > ObjList[k].SeqNumber) {
                ObjList[k].ParentId = Hdr->ParentObjId;
                ObjList[k].Type = Hdr->Type;
                ObjList[k].Mode = Hdr->YstMode;
                ObjList[k].FileSize = ((UINT64)Hdr->FileSizeHigh << 32) | Hdr->FileSizeLow;
                ObjList[k].Mtime = Hdr->YstMtime;
                AsciiStrnCpyS (ObjList[k].Name, sizeof (ObjList[k].Name), Hdr->Name, sizeof (ObjList[k].Name) - 1);
                ObjList[k].SeqNumber = Tags->SeqNumber;
              }
              Found = TRUE;
              break;
            }
          }

          if (!Found) {
            YAFFS_OBJ_RECORD  *Rec = &ObjList[*ObjCount];
            Rec->ObjId = ObjId;
            Rec->ParentId = Hdr->ParentObjId;
            Rec->Type = Hdr->Type;
            Rec->Mode = Hdr->YstMode;
            Rec->FileSize = ((UINT64)Hdr->FileSizeHigh << 32) | Hdr->FileSizeLow;
            Rec->Mtime = Hdr->YstMtime;
            AsciiStrnCpyS (Rec->Name, sizeof (Rec->Name), Hdr->Name, sizeof (Rec->Name) - 1);
            Rec->SeqNumber = Tags->SeqNumber;
            (*ObjCount)++;
          }
        }
      }
    } else if (Verbose) {
      /* Data chunk */
      UINT32  ObjId = Tags->ObjId & YAFFS_EXTRA_OBJID_MASK;
      UINT32  ChunkId = Tags->ChunkId;

      Print (L"    Page %3u: obj=%u chunk=%u bytes=%u seq=%u\n",
             Page, ObjId, ChunkId, Tags->NBytes, Tags->SeqNumber);
    }
  }

  FreePool (PageBuf);
  return ValidPages;
}

EFI_STATUS
EFIAPI
NandProbeAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_SHELL_PARAMETERS_PROTOCOL *ShellParams;
  EFI_HANDLE                    NandHandle;
  EFI_BLOCK_IO_PROTOCOL         *BlockIo;
  UINT8                         *PageBuf;
  UINT32                        Block;
  UINT32                        MaxBlocks;
  UINT32                        StartBlock;
  UINT32                        ErasedCount;
  UINT32                        BadCount;
  UINT32                        UbiCount;
  UINT32                        YaffsCount;
  UINT32                        DataCount;
  UINT32                        FirstDataBlock;
  BOOLEAN                       UbiDetailShown;
  BOOLEAN                       YaffsDetailShown;
  UINTN                         Argc;
  CHAR16                        **Argv;
  YAFFS_OBJ_RECORD              *ObjList;
  UINT32                        ObjCount;
  UINT32                        YaffsPageTotal;

  StartBlock = 0;
  MaxBlocks  = NAND_NUM_BLOCKS;

  /* Parse arguments: NandProbeApp [start_block [count]] */
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    Argc = ShellParams->Argc;
    Argv = ShellParams->Argv;
    if (Argc >= 2) {
      UINTN  Val;
      Status = StrDecimalToUintnS (Argv[1], NULL, &Val);
      if (!EFI_ERROR (Status) && Val < NAND_NUM_BLOCKS) {
        StartBlock = (UINT32)Val;
      }
    }
    if (Argc >= 3) {
      UINTN  Val;
      Status = StrDecimalToUintnS (Argv[2], NULL, &Val);
      if (!EFI_ERROR (Status) && Val > 0) {
        MaxBlocks = StartBlock + (UINT32)Val;
        if (MaxBlocks > NAND_NUM_BLOCKS) {
          MaxBlocks = NAND_NUM_BLOCKS;
        }
      }
    } else if (StartBlock > 0) {
      MaxBlocks = NAND_NUM_BLOCKS;
    }
  }

  /* Find NAND block device */
  NandHandle = FindNandBlockIoHandle ();
  if (NandHandle == NULL) {
    Print (L"Error: NAND block device not found\n");
    Print (L"  (looking for AlNandDxe vendor device path)\n");
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol (NandHandle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to open BlockIo: %r\n", Status);
    return Status;
  }

  Print (L"NAND Probe: scanning blocks %u-%u (%u KB each)\n",
         StartBlock, MaxBlocks - 1, NAND_BLOCK_SIZE / 1024);
  Print (L"  Page size: %u  Pages/block: %u  Inband tag offset: %u\n",
         NAND_PAGE_SIZE, NAND_PAGES_PER_BLOCK, YAFFS_INBAND_TAG_OFF);
  Print (L"\n");

  PageBuf = AllocatePool (NAND_PAGE_SIZE);
  if (PageBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ObjList = AllocateZeroPool (MAX_YAFFS_OBJECTS * sizeof (YAFFS_OBJ_RECORD));
  ObjCount = 0;

  ErasedCount     = 0;
  BadCount        = 0;
  UbiCount        = 0;
  YaffsCount      = 0;
  DataCount       = 0;
  FirstDataBlock  = 0xFFFFFFFF;
  UbiDetailShown  = FALSE;
  YaffsDetailShown = FALSE;
  YaffsPageTotal  = 0;

  for (Block = StartBlock; Block < MaxBlocks; Block++) {
    EFI_LBA  Lba = (EFI_LBA)Block * NAND_PAGES_PER_BLOCK;

    /* Read first page of this erase block */
    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        Lba,
                        NAND_PAGE_SIZE,
                        PageBuf
                        );
    if (EFI_ERROR (Status)) {
      Print (L"  Block %4u (0x%06x): BAD (read error: %r)\n",
             Block, Block * NAND_BLOCK_SIZE, Status);
      BadCount++;
      continue;
    }

    /* Check if erased */
    if (IsPageErased (PageBuf, NAND_PAGE_SIZE)) {
      ErasedCount++;
      continue;
    }

    /* Found non-erased data */
    DataCount++;
    if (FirstDataBlock == 0xFFFFFFFF) {
      FirstDataBlock = Block;
    }

    /* Check for UBI magic (big-endian "UBI#") */
    {
      UBI_EC_HDR  *EcHdr = (UBI_EC_HDR *)PageBuf;
      UINT32       Magic = SwapBytes32 (EcHdr->Magic);

      if (Magic == UBI_EC_HDR_MAGIC) {
        UbiCount++;

        if (!UbiDetailShown) {
          UINT64  Ec       = SwapBytes64 (EcHdr->Ec);
          UINT32  VidOff   = SwapBytes32 (EcHdr->VidHdrOff);
          UINT32  DataOff  = SwapBytes32 (EcHdr->DataOff);
          UINT32  ImgSeq   = SwapBytes32 (EcHdr->ImageSeq);

          Print (L"  Block %4u (0x%06x): UBI EC header\n",
                 Block, Block * NAND_BLOCK_SIZE);
          Print (L"    Version: %u  EC: %lu  VID offset: 0x%x  Data offset: 0x%x\n",
                 EcHdr->Version, Ec, VidOff, DataOff);
          Print (L"    Image seq: 0x%08x\n", ImgSeq);
          Print (L"\n    First 64 bytes:\n");
          HexDump (PageBuf, 64, 0);
          Print (L"\n");
          UbiDetailShown = TRUE;
        }
        continue;
      }
    }

    /* Check for YAFFS2 inband tags */
    {
      YAFFS_PACKED_TAGS  *Tags = (YAFFS_PACKED_TAGS *)(PageBuf + YAFFS_INBAND_TAG_OFF);

      if (IsValidYaffsTags (Tags)) {
        YaffsCount++;

        /* Show detail for first YAFFS block found */
        BOOLEAN ShowDetail = !YaffsDetailShown;

        if (ShowDetail) {
          Print (L"  Block %4u (0x%06x): YAFFS2 data detected\n",
                 Block, Block * NAND_BLOCK_SIZE);
          Print (L"    First page inband tags: seq=%u obj=%u chunk=%u nbytes=%u\n",
                 Tags->SeqNumber, Tags->ObjId & YAFFS_EXTRA_OBJID_MASK,
                 Tags->ChunkId & ~(YAFFS_EXTRA_HEADER_FLAG | YAFFS_EXTRA_SHRINK_FLAG |
                                   YAFFS_EXTRA_SHADOWS_FLAG | YAFFS_EXTRA_SPARE_FLAGS),
                 Tags->NBytes);

          if (IsYaffsObjectHeader (Tags)) {
            YAFFS_OBJ_HDR  *Hdr = (YAFFS_OBJ_HDR *)PageBuf;
            Hdr->Name[sizeof (Hdr->Name) - 1] = '\0';
            Print (L"    Object header: type=%s name=\"%a\" parent=%u\n",
                   YaffsTypeName (Hdr->Type), Hdr->Name, Hdr->ParentObjId);
          }

          Print (L"\n    First 64 bytes:\n");
          HexDump (PageBuf, 64, 0);
          Print (L"\n    Inband tags (last 16 bytes):\n");
          HexDump (PageBuf + YAFFS_INBAND_TAG_OFF, 16, YAFFS_INBAND_TAG_OFF);
          Print (L"\n");
          YaffsDetailShown = TRUE;
        }

        /* Deep-scan all pages in this block for objects */
        {
          UINT32  ValidPages;
          ValidPages = ScanBlockForYaffs (
                         BlockIo, Block, ShowDetail,
                         ObjList, &ObjCount, MAX_YAFFS_OBJECTS
                         );
          YaffsPageTotal += ValidPages;
        }

        continue;
      }
    }

    /* Unknown non-erased data (not UBI, not YAFFS) */
    if (!UbiDetailShown && !YaffsDetailShown) {
      Print (L"  Block %4u (0x%06x): Unknown data\n", Block, Block * NAND_BLOCK_SIZE);
      Print (L"\n    First 64 bytes:\n");
      HexDump (PageBuf, 64, 0);
      Print (L"\n    Last 32 bytes:\n");
      HexDump (PageBuf + NAND_PAGE_SIZE - 32, 32, NAND_PAGE_SIZE - 32);
      Print (L"\n");
    }
  }

  FreePool (PageBuf);

  /* Summary */
  Print (L"--- Summary ---\n");
  Print (L"  Blocks scanned:  %u (%u-%u)\n", MaxBlocks - StartBlock, StartBlock, MaxBlocks - 1);
  Print (L"  Erased (0xFF):   %u\n", ErasedCount);
  Print (L"  Bad blocks:      %u\n", BadCount);
  Print (L"  UBI blocks:      %u\n", UbiCount);
  Print (L"  YAFFS2 blocks:   %u (%u valid pages)\n", YaffsCount, YaffsPageTotal);
  Print (L"  Other data:      %u\n", DataCount - UbiCount - YaffsCount);

  if (UbiCount > 0) {
    Print (L"\n  ==> Format: UBI (%u blocks with EC headers)\n", UbiCount);
  }

  if (YaffsCount > 0) {
    Print (L"\n  ==> Format: YAFFS2 (%u blocks, inband tags, BENAND)\n", YaffsCount);

    /* Print object listing */
    if (ObjCount > 0) {
      UINT32  i;

      Print (L"\n--- YAFFS2 Objects (%u found) ---\n", ObjCount);
      Print (L"  %5s  %7s  %8s  %6s  %s\n", L"ObjId", L"Type", L"Size", L"Parent", L"Name");
      Print (L"  %5s  %7s  %8s  %6s  %s\n", L"-----", L"-------", L"--------", L"------", L"----");

      for (i = 0; i < ObjCount; i++) {
        YAFFS_OBJ_RECORD  *Rec = &ObjList[i];

        if (Rec->Type == YAFFS_OBJECT_TYPE_FILE) {
          Print (L"  %5u  %7s  %8lu  %6u  %a\n",
                 Rec->ObjId, YaffsTypeName (Rec->Type),
                 Rec->FileSize, Rec->ParentId, Rec->Name);
        } else if (Rec->Type == YAFFS_OBJECT_TYPE_DIRECTORY) {
          Print (L"  %5u  %7s  %8s  %6u  %a/\n",
                 Rec->ObjId, YaffsTypeName (Rec->Type),
                 L"-", Rec->ParentId, Rec->Name);
        } else {
          Print (L"  %5u  %7s  %8s  %6u  %a\n",
                 Rec->ObjId, YaffsTypeName (Rec->Type),
                 L"-", Rec->ParentId, Rec->Name);
        }
      }
    }
  }

  if (UbiCount == 0 && YaffsCount == 0 && DataCount > 0) {
    Print (L"\n  ==> Format: Unknown (not UBI or YAFFS2)\n");
    Print (L"      First data at block %u (0x%06x)\n",
           FirstDataBlock, FirstDataBlock * NAND_BLOCK_SIZE);
  }

  if (DataCount == 0) {
    Print (L"\n  ==> NAND appears empty (all erased)\n");
  }

  if (ObjList != NULL) {
    FreePool (ObjList);
  }

  return EFI_SUCCESS;
}
