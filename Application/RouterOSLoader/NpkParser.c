/** @file
  NPK (MikroTik package) TLV parser — extracts the "boot/kernel" file entry.

  NPK file layout:
    - 8-byte main header: uint32 magic (0xBAD0F11E LE) + uint32 remain_size
    - Sequence of TLV partitions: uint16 type + uint32 size + data[size]
    - Type 4 = files container, containing 30-byte file entry headers

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RouterOSLoader.h"

#define NPK_MAGIC          0xBAD0F11EU
#define NPK_MAIN_HDR_SIZE  8

/**
  Search type-4 partition data for a file entry named "boot/kernel".

  Rather than iterating entries sequentially (which requires knowing the
  exact sub-header format of the files container), we scan for the
  "boot/kernel" string and validate the surrounding file entry header.
**/
STATIC
EFI_STATUS
FindBootKernelInFilesPartition (
  IN  CONST UINT8  *PartData,
  IN  UINTN        PartSize,
  IN  CONST UINT8  *NpkBase,
  OUT CONST UINT8  **KernelData,
  OUT UINTN        *KernelSize
  )
{
  STATIC CONST CHAR8  BootKernelName[] = "boot/kernel";
  UINTN               NameLen;
  UINTN               Pos;

  NameLen = sizeof (BootKernelName) - 1;  // 11

  //
  // Scan for "boot/kernel" string preceded by name_len=11 (LE16)
  // and a plausible data_size (LE32).
  //
  for (Pos = NPK_FILE_ENTRY_HEADER_SIZE; Pos + NameLen <= PartSize; Pos++) {
    UINT16  FoundNameLen;
    UINT32  DataSize;
    UINTN   FileDataStart;

    if (CompareMem (&PartData[Pos], BootKernelName, NameLen) != 0) {
      continue;
    }

    //
    // Verify name_len field at Pos - 2
    //
    if (Pos < 2) {
      continue;
    }

    FoundNameLen = PartData[Pos - 2] | ((UINT16)PartData[Pos - 1] << 8);
    if (FoundNameLen != NameLen) {
      continue;
    }

    //
    // Read data_size at Pos - 6
    //
    if (Pos < 6) {
      continue;
    }

    DataSize = PartData[Pos - 6]
             | ((UINT32)PartData[Pos - 5] << 8)
             | ((UINT32)PartData[Pos - 4] << 16)
             | ((UINT32)PartData[Pos - 3] << 24);

    FileDataStart = Pos + NameLen;
    if (FileDataStart + DataSize > PartSize) {
      DEBUG ((DEBUG_WARN, "[RouterOS] boot/kernel data_size 0x%x overflows partition\n",
              DataSize));
      continue;
    }

    //
    // Verify the file data starts with ELF magic
    //
    if (DataSize >= 4 &&
        PartData[FileDataStart] == 0x7F &&
        PartData[FileDataStart + 1] == 'E' &&
        PartData[FileDataStart + 2] == 'L' &&
        PartData[FileDataStart + 3] == 'F') {
      *KernelData = &PartData[FileDataStart];
      *KernelSize = DataSize;
      DEBUG ((DEBUG_WARN, "[RouterOS] Found boot/kernel: offset 0x%x, size %u bytes\n",
              (UINTN)(*KernelData - NpkBase), DataSize));
      return EFI_SUCCESS;
    }

    DEBUG ((DEBUG_WARN, "[RouterOS] boot/kernel at offset 0x%x but not ELF (0x%02x)\n",
            Pos, PartData[FileDataStart]));
  }

  return EFI_NOT_FOUND;
}

/**
  Parse NPK data to find the "boot/kernel" file entry.
**/
EFI_STATUS
NpkFindBootKernel (
  IN  CONST UINT8  *NpkData,
  IN  UINTN        NpkSize,
  OUT CONST UINT8  **KernelData,
  OUT UINTN        *KernelSize
  )
{
  UINT32  Magic;
  UINTN   Offset;

  //
  // Validate main NPK header (8 bytes: magic + remain_size)
  //
  if (NpkSize < NPK_MAIN_HDR_SIZE) {
    DEBUG ((DEBUG_WARN, "[RouterOS] NPK too small (%u bytes)\n", NpkSize));
    return EFI_INVALID_PARAMETER;
  }

  Magic = NpkData[0]
        | ((UINT32)NpkData[1] << 8)
        | ((UINT32)NpkData[2] << 16)
        | ((UINT32)NpkData[3] << 24);

  if (Magic != NPK_MAGIC) {
    DEBUG ((DEBUG_WARN, "[RouterOS] Bad NPK magic: 0x%08x (expected 0x%08x)\n",
            Magic, NPK_MAGIC));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Walk TLV partitions after the 8-byte header
  //
  Offset = NPK_MAIN_HDR_SIZE;
  while (Offset + NPK_HEADER_SIZE <= NpkSize) {
    UINT16  ItemType;
    UINT32  ItemSize;

    ItemType = NpkData[Offset] | ((UINT16)NpkData[Offset + 1] << 8);
    ItemSize = NpkData[Offset + 2]
             | ((UINT32)NpkData[Offset + 3] << 8)
             | ((UINT32)NpkData[Offset + 4] << 16)
             | ((UINT32)NpkData[Offset + 5] << 24);

    if (Offset + NPK_HEADER_SIZE + ItemSize > NpkSize) {
      DEBUG ((DEBUG_WARN, "[RouterOS] NPK partition at 0x%x overflows (type=%d, size=0x%x)\n",
              Offset, ItemType, ItemSize));
      break;
    }

    if (ItemType == NPK_TYPE_FILE_PAYLOAD) {
      CONST UINT8  *PartData;
      EFI_STATUS   Status;

      PartData = NpkData + Offset + NPK_HEADER_SIZE;

      DEBUG ((DEBUG_WARN, "[RouterOS] NPK type=4 partition at offset 0x%x, size 0x%x\n",
              Offset, ItemSize));

      Status = FindBootKernelInFilesPartition (
                 PartData,
                 ItemSize,
                 NpkData,
                 KernelData,
                 KernelSize
                 );
      if (!EFI_ERROR (Status)) {
        return Status;
      }
    }

    Offset += NPK_HEADER_SIZE + ItemSize;
  }

  DEBUG ((DEBUG_WARN, "[RouterOS] boot/kernel not found in NPK\n"));
  return EFI_NOT_FOUND;
}
