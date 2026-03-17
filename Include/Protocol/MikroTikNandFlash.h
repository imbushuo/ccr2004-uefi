/** @file
  MikroTik NAND Flash Protocol definition.

  NAND-aware interface for direct page access, produced by AlNandDxe,
  consumed by filesystem drivers (MtYaffs2Dxe).

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MIKROTIK_NAND_FLASH_H_
#define MIKROTIK_NAND_FLASH_H_

#define MIKROTIK_NAND_FLASH_PROTOCOL_GUID \
  { 0x7a3b8c1d, 0x2e4f, 0x4a56, { 0xb8, 0x9d, 0x1c, 0x3e, 0x5f, 0x7a, 0x9b, 0x02 } }

typedef struct _MIKROTIK_NAND_FLASH_PROTOCOL MIKROTIK_NAND_FLASH_PROTOCOL;

/**
  Read a single NAND page (full page including OOB/tag area).

  @param[in]  This       Protocol instance.
  @param[in]  PageIndex  Zero-based page number.
  @param[out] Buffer     Caller-allocated buffer of at least PageSize bytes.

  @retval EFI_SUCCESS    Page read successfully.
  @retval EFI_DEVICE_ERROR  Hardware read failure.
  @retval EFI_INVALID_PARAMETER  PageIndex out of range or Buffer is NULL.
**/
typedef
EFI_STATUS
(EFIAPI *MIKROTIK_NAND_READ_PAGE)(
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *This,
  IN  UINT32                         PageIndex,
  OUT VOID                           *Buffer
  );

/**
  Read only the inline tags (last 16 bytes) of a NAND page.

  Much faster than ReadPage for metadata-only access: reads 16 bytes
  via column-addressed NAND read instead of the full 2048-byte page.

  @param[in]  This       Protocol instance.
  @param[in]  PageIndex  Zero-based page number.
  @param[out] Buffer     Caller-allocated buffer of at least 16 bytes.

  @retval EFI_SUCCESS    Tags read successfully.
  @retval EFI_DEVICE_ERROR  Hardware read failure.
**/
typedef
EFI_STATUS
(EFIAPI *MIKROTIK_NAND_READ_TAGS)(
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *This,
  IN  UINT32                         PageIndex,
  OUT VOID                           *Buffer
  );

struct _MIKROTIK_NAND_FLASH_PROTOCOL {
  UINT32                    PageSize;       ///< Bytes per page (2048)
  UINT32                    PagesPerBlock;  ///< Pages per erase block (64)
  UINT32                    NumBlocks;      ///< Total erase blocks (1024)
  MIKROTIK_NAND_READ_PAGE   ReadPage;
  MIKROTIK_NAND_READ_TAGS   ReadTags;       ///< Fast tag-only read
};

extern EFI_GUID  gMikroTikNandFlashProtocolGuid;

#endif // MIKROTIK_NAND_FLASH_H_
