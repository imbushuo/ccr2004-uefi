/** @file
  FormatBlockApp — Create GPT + FAT32 on a block device.

  Usage:
    FormatBlockApp ramdisk <SizeMB>   — create a RAM disk and format it
    FormatBlockApp <HandleHex>        — format an existing block device

  Writes a protective MBR, GPT header+entries (primary and backup),
  and a FAT32 filesystem on the single ESP partition spanning the
  whole usable area.  Then reconnects the handle so PartitionDxe
  and the FAT driver pick up the new layout.

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
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/RamDisk.h>
#include <Protocol/ShellParameters.h>
#include <Uefi/UefiGpt.h>
#include <Guid/Gpt.h>

/* Well-known GUIDs */
STATIC EFI_GUID  mEspTypeGuid = {
  0xC12A7328, 0xF81F, 0x11D2,
  { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};

/* Fixed GUIDs for the disk and partition (deterministic, not random) */
STATIC EFI_GUID  mDiskGuid = {
  0xA0A1A2A3, 0xB0B1, 0xC0C1,
  { 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7 }
};

STATIC EFI_GUID  mPartGuid = {
  0xE0E1E2E3, 0xF0F1, 0x0001,
  { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 }
};

#define GPT_ENTRY_COUNT  128
#define GPT_ENTRY_SIZE   128
#define GPT_ENTRIES_LBAS(BlkSz) \
  ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + (BlkSz) - 1) / (BlkSz))

/* ---- Protective MBR ---- */
#pragma pack(1)
typedef struct {
  UINT8   BootIndicator;
  UINT8   StartHead;
  UINT8   StartSector;
  UINT8   StartTrack;
  UINT8   OSIndicator;
  UINT8   EndHead;
  UINT8   EndSector;
  UINT8   EndTrack;
  UINT32  StartingLBA;
  UINT32  SizeInLBA;
} MBR_PARTITION_RECORD;

typedef struct {
  UINT8                Bootstrap[440];
  UINT32               UniqueMbrSignature;
  UINT16               Unknown;
  MBR_PARTITION_RECORD Partition[4];
  UINT16               Signature;     /* 0xAA55 */
} MASTER_BOOT_RECORD;
#pragma pack()

/* ---- FAT Boot Sector (common BPB + FAT16 or FAT32 extension) ---- */
#pragma pack(1)
typedef struct {
  UINT8   JmpBoot[3];
  UINT8   OEMName[8];
  UINT16  BytsPerSec;
  UINT8   SecPerClus;
  UINT16  RsvdSecCnt;
  UINT8   NumFATs;
  UINT16  RootEntCnt;
  UINT16  TotSec16;
  UINT8   Media;
  UINT16  FATSz16;
  UINT16  SecPerTrk;
  UINT16  NumHeads;
  UINT32  HiddSec;
  UINT32  TotSec32;
  /* FAT16 extended BPB (offset 36) */
  UINT8   DrvNum16;
  UINT8   Reserved16;
  UINT8   BootSig16;
  UINT32  VolID16;
  UINT8   VolLab16[11];
  UINT8   FilSysType16[8];
  UINT8   BootCode16[448];
  UINT16  Signature;     /* 0xAA55 */
} FAT16_BOOT_SECTOR;

typedef struct {
  UINT8   JmpBoot[3];
  UINT8   OEMName[8];
  UINT16  BytsPerSec;
  UINT8   SecPerClus;
  UINT16  RsvdSecCnt;
  UINT8   NumFATs;
  UINT16  RootEntCnt;
  UINT16  TotSec16;
  UINT8   Media;
  UINT16  FATSz16;
  UINT16  SecPerTrk;
  UINT16  NumHeads;
  UINT32  HiddSec;
  UINT32  TotSec32;
  /* FAT32-specific (offset 36) */
  UINT32  FATSz32;
  UINT16  ExtFlags;
  UINT16  FSVer;
  UINT32  RootClus;
  UINT16  FSInfo;
  UINT16  BkBootSec;
  UINT8   Reserved[12];
  UINT8   DrvNum;
  UINT8   Reserved1;
  UINT8   BootSig;
  UINT32  VolID;
  UINT8   VolLab[11];
  UINT8   FilSysType[8];
  UINT8   BootCode[420];
  UINT16  Signature;     /* 0xAA55 */
} FAT32_BOOT_SECTOR;

typedef struct {
  UINT32  LeadSig;       /* 0x41615252 */
  UINT8   Reserved1[480];
  UINT32  StrucSig;      /* 0x61417272 */
  UINT32  Free_Count;
  UINT32  Nxt_Free;
  UINT8   Reserved2[12];
  UINT32  TrailSig;      /* 0xAA550000 */
} FAT32_FSINFO;
#pragma pack()

/* Root directory entries for FAT16 — 512 entries = 32 sectors at 512 bytes/sector */
#define FAT16_ROOT_ENTRY_COUNT  512
#define FAT16_ROOT_DIR_SECTORS(BlkSz) \
  ((FAT16_ROOT_ENTRY_COUNT * 32 + (BlkSz) - 1) / (BlkSz))

STATIC
EFI_STATUS
WriteProtectiveMbr (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT32                 BlockSize,
  IN UINT64                 TotalBlocks
  )
{
  MASTER_BOOT_RECORD  *Mbr;
  EFI_STATUS          Status;
  UINT32              EndLba;

  Mbr = AllocateZeroPool (BlockSize);
  if (Mbr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  EndLba = (TotalBlocks - 1 > 0xFFFFFFFF) ? 0xFFFFFFFF : (UINT32)(TotalBlocks - 1);

  Mbr->Partition[0].BootIndicator = 0x00;
  Mbr->Partition[0].OSIndicator  = 0xEE;  /* GPT protective */
  Mbr->Partition[0].StartHead    = 0x00;
  Mbr->Partition[0].StartSector  = 0x02;
  Mbr->Partition[0].StartTrack   = 0x00;
  Mbr->Partition[0].EndHead      = 0xFF;
  Mbr->Partition[0].EndSector    = 0xFF;
  Mbr->Partition[0].EndTrack     = 0xFF;
  Mbr->Partition[0].StartingLBA  = 1;
  Mbr->Partition[0].SizeInLBA    = EndLba;
  Mbr->Signature                 = 0xAA55;

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, BlockSize, Mbr);
  FreePool (Mbr);
  return Status;
}

STATIC
EFI_STATUS
WriteGpt (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT32                 BlockSize,
  IN UINT64                 TotalBlocks
  )
{
  EFI_STATUS                Status;
  EFI_PARTITION_TABLE_HEADER *Hdr;
  EFI_PARTITION_ENTRY       *Entries;
  UINT32                    EntriesLbas;
  UINT32                    EntriesBytes;
  UINT64                    FirstUsable;
  UINT64                    LastUsable;
  UINT64                    BackupHdrLba;
  UINT64                    BackupEntriesLba;
  UINT32                    EntriesCrc;
  UINT32                    HdrCrc;
  UINT8                     *LbaBlock;

  EntriesLbas  = GPT_ENTRIES_LBAS (BlockSize);
  EntriesBytes = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE;
  FirstUsable  = 2 + EntriesLbas;             /* LBA 0=MBR, 1=GPT hdr, 2..N=entries */
  LastUsable   = TotalBlocks - 2 - EntriesLbas; /* backup entries + backup header */
  BackupHdrLba = TotalBlocks - 1;
  BackupEntriesLba = LastUsable + 1;

  if (LastUsable <= FirstUsable) {
    Print (L"Error: Device too small for GPT\n");
    return EFI_VOLUME_FULL;
  }

  /* Build partition entries */
  Entries = AllocateZeroPool (EntriesBytes);
  if (Entries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyGuid (&Entries[0].PartitionTypeGUID, &mEspTypeGuid);
  CopyGuid (&Entries[0].UniquePartitionGUID, &mPartGuid);
  Entries[0].StartingLBA = FirstUsable;
  Entries[0].EndingLBA   = LastUsable;
  Entries[0].Attributes  = 0;
  UnicodeSPrint (Entries[0].PartitionName, sizeof (Entries[0].PartitionName), L"EFI System");

  /* Compute entries CRC */
  gBS->CalculateCrc32 (Entries, EntriesBytes, &EntriesCrc);

  /* Build primary GPT header (LBA 1) */
  Hdr = AllocateZeroPool (BlockSize);
  if (Hdr == NULL) {
    FreePool (Entries);
    return EFI_OUT_OF_RESOURCES;
  }

  Hdr->Header.Signature  = EFI_PTAB_HEADER_ID;
  Hdr->Header.Revision   = 0x00010000;
  Hdr->Header.HeaderSize = sizeof (EFI_PARTITION_TABLE_HEADER);
  Hdr->Header.CRC32      = 0;
  Hdr->Header.Reserved   = 0;
  Hdr->MyLBA             = PRIMARY_PART_HEADER_LBA;
  Hdr->AlternateLBA      = BackupHdrLba;
  Hdr->FirstUsableLBA    = FirstUsable;
  Hdr->LastUsableLBA     = LastUsable;
  CopyGuid (&Hdr->DiskGUID, &mDiskGuid);
  Hdr->PartitionEntryLBA       = 2;
  Hdr->NumberOfPartitionEntries = GPT_ENTRY_COUNT;
  Hdr->SizeOfPartitionEntry    = GPT_ENTRY_SIZE;
  Hdr->PartitionEntryArrayCRC32 = EntriesCrc;

  /* CRC the header */
  Hdr->Header.CRC32 = 0;
  gBS->CalculateCrc32 (Hdr, Hdr->Header.HeaderSize, &HdrCrc);
  Hdr->Header.CRC32 = HdrCrc;

  /* Write primary GPT header (LBA 1) */
  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 1, BlockSize, Hdr);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  /* Write primary partition entries (LBA 2+) */
  {
    UINT32  Remaining = EntriesBytes;
    UINT64  Lba = 2;
    UINT8   *Ptr = (UINT8 *)Entries;

    LbaBlock = AllocateZeroPool (BlockSize);
    if (LbaBlock == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    while (Remaining > 0) {
      UINT32 ChunkSize = (Remaining > BlockSize) ? BlockSize : Remaining;
      ZeroMem (LbaBlock, BlockSize);
      CopyMem (LbaBlock, Ptr, ChunkSize);
      Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, Lba, BlockSize, LbaBlock);
      if (EFI_ERROR (Status)) {
        FreePool (LbaBlock);
        goto Done;
      }
      Ptr += ChunkSize;
      Remaining -= ChunkSize;
      Lba++;
    }

    FreePool (LbaBlock);
  }

  /* Write backup partition entries */
  {
    UINT32  Remaining = EntriesBytes;
    UINT64  Lba = BackupEntriesLba;
    UINT8   *Ptr = (UINT8 *)Entries;

    LbaBlock = AllocateZeroPool (BlockSize);
    if (LbaBlock == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    while (Remaining > 0) {
      UINT32 ChunkSize = (Remaining > BlockSize) ? BlockSize : Remaining;
      ZeroMem (LbaBlock, BlockSize);
      CopyMem (LbaBlock, Ptr, ChunkSize);
      Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, Lba, BlockSize, LbaBlock);
      if (EFI_ERROR (Status)) {
        FreePool (LbaBlock);
        goto Done;
      }
      Ptr += ChunkSize;
      Remaining -= ChunkSize;
      Lba++;
    }

    FreePool (LbaBlock);
  }

  /* Build and write backup GPT header */
  Hdr->MyLBA             = BackupHdrLba;
  Hdr->AlternateLBA      = PRIMARY_PART_HEADER_LBA;
  Hdr->PartitionEntryLBA = BackupEntriesLba;
  Hdr->Header.CRC32      = 0;
  gBS->CalculateCrc32 (Hdr, Hdr->Header.HeaderSize, &HdrCrc);
  Hdr->Header.CRC32 = HdrCrc;

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, BackupHdrLba, BlockSize, Hdr);

Done:
  FreePool (Hdr);
  FreePool (Entries);
  return Status;
}

STATIC
EFI_STATUS
FormatFat (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT32                 BlockSize,
  IN UINT64                 TotalBlocks
  )
{
  EFI_STATUS          Status;
  UINT32              EntriesLbas;
  UINT64              FirstUsable;
  UINT64              LastUsable;
  UINT64              PartSectors;
  UINT32              SecPerClus;
  UINT32              FatSz;
  UINT32              RsvdSecCnt;
  UINT32              RootDirSectors;
  UINT32              TotalClusters;
  BOOLEAN             UseFat32;
  UINT8               *Sector;
  UINT64              FatStartLba;
  UINT64              Fat2StartLba;
  UINT64              RootDirLba;
  UINT64              DataStartLba;
  UINT32              i;

  EntriesLbas = GPT_ENTRIES_LBAS (BlockSize);
  FirstUsable = 2 + EntriesLbas;
  LastUsable  = TotalBlocks - 2 - EntriesLbas;
  PartSectors = LastUsable - FirstUsable + 1;

  /* Decide FAT type based on partition size.
   * Try FAT16 first with small clusters to maximize cluster count.
   * FAT16: 4085-65524 clusters, FAT32: >= 65525 clusters. */

  /* For FAT16: pick cluster size to stay within 4085-65524 clusters */
  SecPerClus = 0;
  RootDirSectors = FAT16_ROOT_DIR_SECTORS (BlockSize);
  RsvdSecCnt = 1;  /* FAT16 typically uses 1 reserved sector */

  /* Try increasing cluster sizes: 1, 2, 4, 8, 16, 32, 64 sectors */
  for (i = 0; i <= 6; i++) {
    UINT32 TryClus = 1U << i;
    UINT32 TryFatSz;
    UINT32 TryClusters;
    UINT32 DataSectors;

    /* FAT16: 2 bytes per entry */
    TryClusters = (UINT32)((PartSectors - RsvdSecCnt - RootDirSectors) / TryClus);
    TryFatSz = ((TryClusters + 2) * 2 + BlockSize - 1) / BlockSize;
    DataSectors = (UINT32)PartSectors - RsvdSecCnt - 2 * TryFatSz - RootDirSectors;
    TryClusters = DataSectors / TryClus;

    if (TryClusters >= 4085 && TryClusters <= 65524) {
      SecPerClus = TryClus;
      FatSz = TryFatSz;
      TotalClusters = TryClusters;
      break;
    }
  }

  if (SecPerClus > 0) {
    UseFat32 = FALSE;
  } else {
    /* FAT16 won't work, use FAT32 */
    UseFat32 = TRUE;
    RsvdSecCnt = 32;
    RootDirSectors = 0;

    /* Pick cluster size for FAT32 */
    if (BlockSize >= 32768) {
      SecPerClus = 1;
    } else {
      SecPerClus = 32768 / BlockSize;
    }
    if (SecPerClus > 128) {
      SecPerClus = 128;
    }

    /* Calculate FAT32 size (4 bytes per entry) */
    TotalClusters = (UINT32)((PartSectors - RsvdSecCnt) / SecPerClus);
    FatSz = ((TotalClusters + 2) * 4 + BlockSize - 1) / BlockSize;
    TotalClusters = (UINT32)((PartSectors - RsvdSecCnt - 2 * FatSz) / SecPerClus);
    FatSz = ((TotalClusters + 2) * 4 + BlockSize - 1) / BlockSize;
  }

  FatStartLba  = FirstUsable + RsvdSecCnt;
  Fat2StartLba = FatStartLba + FatSz;
  RootDirLba   = Fat2StartLba + FatSz;
  DataStartLba = RootDirLba + RootDirSectors;

  Print (L"  Format: %a\n", UseFat32 ? "FAT32" : "FAT16");
  Print (L"  Partition: LBA %lu - %lu (%lu sectors)\n", FirstUsable, LastUsable, PartSectors);
  Print (L"  Cluster size: %u bytes (%u sectors)\n", SecPerClus * BlockSize, SecPerClus);
  Print (L"  FAT size: %u sectors, clusters: %u\n", FatSz, TotalClusters);

  /* Write boot sector */
  Sector = AllocateZeroPool (BlockSize);
  if (Sector == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (UseFat32) {
    FAT32_BOOT_SECTOR  *Bpb = (FAT32_BOOT_SECTOR *)Sector;

    Bpb->JmpBoot[0]  = 0xEB;
    Bpb->JmpBoot[1]  = 0x58;
    Bpb->JmpBoot[2]  = 0x90;
    CopyMem (Bpb->OEMName, "MTIKUEFI", 8);
    Bpb->BytsPerSec  = (UINT16)BlockSize;
    Bpb->SecPerClus  = (UINT8)SecPerClus;
    Bpb->RsvdSecCnt  = (UINT16)RsvdSecCnt;
    Bpb->NumFATs     = 2;
    Bpb->RootEntCnt  = 0;
    Bpb->TotSec16    = 0;
    Bpb->Media       = 0xF8;
    Bpb->FATSz16     = 0;
    Bpb->SecPerTrk   = 63;
    Bpb->NumHeads    = 255;
    Bpb->HiddSec     = (UINT32)FirstUsable;
    Bpb->TotSec32    = (UINT32)PartSectors;
    Bpb->FATSz32     = FatSz;
    Bpb->ExtFlags    = 0;
    Bpb->FSVer       = 0;
    Bpb->RootClus    = 2;
    Bpb->FSInfo      = 1;
    Bpb->BkBootSec   = 6;
    Bpb->DrvNum      = 0x80;
    Bpb->BootSig     = 0x29;
    Bpb->VolID       = 0xDEADBEEF;
    CopyMem (Bpb->VolLab, "EFI SYSTEM ", 11);
    CopyMem (Bpb->FilSysType, "FAT32   ", 8);
    Bpb->Signature   = 0xAA55;
  } else {
    FAT16_BOOT_SECTOR  *Bpb = (FAT16_BOOT_SECTOR *)Sector;

    Bpb->JmpBoot[0]  = 0xEB;
    Bpb->JmpBoot[1]  = 0x3C;
    Bpb->JmpBoot[2]  = 0x90;
    CopyMem (Bpb->OEMName, "MTIKUEFI", 8);
    Bpb->BytsPerSec  = (UINT16)BlockSize;
    Bpb->SecPerClus  = (UINT8)SecPerClus;
    Bpb->RsvdSecCnt  = (UINT16)RsvdSecCnt;
    Bpb->NumFATs     = 2;
    Bpb->RootEntCnt  = FAT16_ROOT_ENTRY_COUNT;
    Bpb->Media       = 0xF8;
    Bpb->FATSz16     = (UINT16)FatSz;
    Bpb->SecPerTrk   = 63;
    Bpb->NumHeads    = 255;
    Bpb->HiddSec     = (UINT32)FirstUsable;
    if (PartSectors <= 0xFFFF) {
      Bpb->TotSec16  = (UINT16)PartSectors;
      Bpb->TotSec32  = 0;
    } else {
      Bpb->TotSec16  = 0;
      Bpb->TotSec32  = (UINT32)PartSectors;
    }
    Bpb->DrvNum16    = 0x80;
    Bpb->BootSig16   = 0x29;
    Bpb->VolID16     = 0xDEADBEEF;
    CopyMem (Bpb->VolLab16, "EFI SYSTEM ", 11);
    CopyMem (Bpb->FilSysType16, "FAT16   ", 8);
    Bpb->Signature   = 0xAA55;
  }

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, FirstUsable, BlockSize, Sector);
  if (EFI_ERROR (Status)) {
    FreePool (Sector);
    return Status;
  }

  if (UseFat32) {
    /* Write backup boot sector at sector 6 */
    Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, FirstUsable + 6, BlockSize, Sector);
    if (EFI_ERROR (Status)) {
      FreePool (Sector);
      return Status;
    }
  }

  FreePool (Sector);

  /* FSInfo (FAT32 only) */
  if (UseFat32) {
    FAT32_FSINFO  *FsInfo;

    FsInfo = AllocateZeroPool (BlockSize);
    if (FsInfo == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    FsInfo->LeadSig    = 0x41615252;
    FsInfo->StrucSig   = 0x61417272;
    FsInfo->Free_Count = TotalClusters - 1;
    FsInfo->Nxt_Free   = 3;
    FsInfo->TrailSig   = 0xAA550000;

    BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, FirstUsable + 1, BlockSize, FsInfo);
    BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, FirstUsable + 7, BlockSize, FsInfo);
    FreePool (FsInfo);
  }

  /* Write FAT tables */
  Sector = AllocateZeroPool (BlockSize);
  if (Sector == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (UseFat32) {
    ((UINT32 *)Sector)[0] = 0x0FFFFFF8;
    ((UINT32 *)Sector)[1] = 0x0FFFFFFF;
    ((UINT32 *)Sector)[2] = 0x0FFFFFFF;  /* root dir cluster */
  } else {
    ((UINT16 *)Sector)[0] = 0xFFF8;
    ((UINT16 *)Sector)[1] = 0xFFFF;
  }

  BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, FatStartLba, BlockSize, Sector);
  BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, Fat2StartLba, BlockSize, Sector);

  /* Zero remaining FAT sectors */
  ZeroMem (Sector, BlockSize);
  for (i = 1; i < FatSz; i++) {
    BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, FatStartLba + i, BlockSize, Sector);
    BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, Fat2StartLba + i, BlockSize, Sector);
  }

  /* Zero root directory area */
  if (UseFat32) {
    UINT32 j;
    for (j = 0; j < SecPerClus; j++) {
      BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, DataStartLba + j, BlockSize, Sector);
    }
  } else {
    for (i = 0; i < RootDirSectors; i++) {
      BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, RootDirLba + i, BlockSize, Sector);
    }
  }

  FreePool (Sector);

  return EFI_SUCCESS;
}

/* Use the proper extern from MdePkg */

STATIC
EFI_STATUS
FormatBlockDevice (
  IN EFI_HANDLE   ImageHandle,
  IN EFI_HANDLE   TargetHandle
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINT32                 BlockSize;
  UINT64                 TotalBlocks;

  Status = gBS->OpenProtocol (
                  TargetHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Error: Handle does not have BlockIo: %r\n", Status);
    return Status;
  }

  if (BlockIo->Media->ReadOnly) {
    Print (L"Error: Device is read-only\n");
    return EFI_WRITE_PROTECTED;
  }

  BlockSize   = BlockIo->Media->BlockSize;
  TotalBlocks = BlockIo->Media->LastBlock + 1;

  Print (L"Device: blocks=%lu  blocksize=%u  total=%lu MB\n",
         TotalBlocks, BlockSize,
         (TotalBlocks * BlockSize) / (1024 * 1024));

  if (BlockSize < 512) {
    Print (L"Error: Block size %u too small (minimum 512)\n", BlockSize);
    return EFI_UNSUPPORTED;
  }

  Print (L"Writing protective MBR...\n");
  Status = WriteProtectiveMbr (BlockIo, BlockSize, TotalBlocks);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to write MBR: %r\n", Status);
    return Status;
  }

  Print (L"Writing GPT (primary + backup)...\n");
  Status = WriteGpt (BlockIo, BlockSize, TotalBlocks);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to write GPT: %r\n", Status);
    return Status;
  }

  Print (L"Formatting FAT32...\n");
  Status = FormatFat (BlockIo, BlockSize, TotalBlocks);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to format FAT32: %r\n", Status);
    return Status;
  }

  BlockIo->FlushBlocks (BlockIo);

  Print (L"Reconnecting controllers...\n");
  gBS->DisconnectController (TargetHandle, NULL, NULL);
  gBS->ConnectController (TargetHandle, NULL, NULL, TRUE);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CreateAndFormatRamDisk (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       SizeMB
  )
{
  EFI_STATUS                Status;
  EFI_RAM_DISK_PROTOCOL     *RamDisk;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_PHYSICAL_ADDRESS      RamBase;
  UINT64                    RamSize;
  UINTN                     NumPages;
  EFI_HANDLE                BlockHandle;

  if (SizeMB < 34) {
    Print (L"Error: Minimum RAM disk size is 34 MB (for GPT + FAT32)\n");
    return EFI_INVALID_PARAMETER;
  }

  /* Locate RAM disk protocol */
  Status = gBS->LocateProtocol (&gEfiRamDiskProtocolGuid, NULL, (VOID **)&RamDisk);
  if (EFI_ERROR (Status)) {
    Print (L"Error: RAM disk protocol not found: %r\n", Status);
    return Status;
  }

  /* Allocate memory for the RAM disk */
  RamSize  = (UINT64)SizeMB * 1024 * 1024;
  NumPages = EFI_SIZE_TO_PAGES ((UINTN)RamSize);

  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, NumPages, &RamBase);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to allocate %u MB: %r\n", SizeMB, Status);
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)RamBase, (UINTN)RamSize);

  Print (L"RAM disk: %u MB at 0x%lx\n", SizeMB, RamBase);

  /* Register the RAM disk */
  DevicePath = NULL;
  Status = RamDisk->Register (RamBase, RamSize, &gEfiVirtualDiskGuid, NULL, &DevicePath);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Failed to register RAM disk: %r\n", Status);
    gBS->FreePages (RamBase, NumPages);
    return Status;
  }

  /* Find the handle that was created with the returned device path */
  BlockHandle = NULL;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &DevicePath, &BlockHandle);
  if (EFI_ERROR (Status)) {
    Print (L"Error: Could not find BlockIo for RAM disk: %r\n", Status);
    return Status;
  }

  Print (L"Found RAM disk BlockIo at handle %p\n", BlockHandle);

  /* Format it */
  Status = FormatBlockDevice (ImageHandle, BlockHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"Done. Use 'map -r' to refresh mappings.\n");
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FormatBlockAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  UINTN                  Argc;
  CHAR16                 **Argv;
  EFI_SHELL_PARAMETERS_PROTOCOL *ShellParams;

  /* Get shell parameters */
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

  if (Argc < 2) {
    Print (L"Usage:\n");
    Print (L"  FormatBlockApp ramdisk <SizeMB>  — create RAM disk and format\n");
    Print (L"  FormatBlockApp <HandleHex>       — format existing block device\n");
    Print (L"\n");
    Print (L"Creates GPT partition table with a single FAT32 EFI System Partition.\n");
    Print (L"\n");
    Print (L"Examples:\n");
    Print (L"  FormatBlockApp ramdisk 64        — 64 MB RAM disk\n");
    Print (L"  FormatBlockApp 5A                — format handle 0x5A\n");
    return EFI_INVALID_PARAMETER;
  }

  /* Check for "ramdisk" subcommand */
  if (StrCmp (Argv[1], L"ramdisk") == 0) {
    UINTN  SizeMB;

    if (Argc != 3) {
      Print (L"Usage: FormatBlockApp ramdisk <SizeMB>\n");
      return EFI_INVALID_PARAMETER;
    }

    Status = StrDecimalToUintnS (Argv[2], NULL, &SizeMB);
    if (EFI_ERROR (Status) || SizeMB == 0) {
      Print (L"Error: Invalid size '%s'\n", Argv[2]);
      return EFI_INVALID_PARAMETER;
    }

    return CreateAndFormatRamDisk (ImageHandle, SizeMB);
  }

  /* Otherwise treat argument as a block device handle in hex */
  {
    UINTN       HandleValue;
    EFI_HANDLE  TargetHandle;

    HandleValue = 0;
    Status = StrHexToUintnS (Argv[1], NULL, &HandleValue);
    if (EFI_ERROR (Status) || HandleValue == 0) {
      Print (L"Error: Invalid handle '%s'\n", Argv[1]);
      return EFI_INVALID_PARAMETER;
    }

    TargetHandle = (EFI_HANDLE)(UINTN)HandleValue;

    Status = FormatBlockDevice (ImageHandle, TargetHandle);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Print (L"Done. Use 'map -r' to refresh mappings.\n");
    return EFI_SUCCESS;
  }
}
