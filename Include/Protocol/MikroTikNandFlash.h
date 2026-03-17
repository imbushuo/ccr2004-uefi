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

struct _MIKROTIK_NAND_FLASH_PROTOCOL {
  UINT32                    PageSize;       ///< Bytes per page (2048)
  UINT32                    PagesPerBlock;  ///< Pages per erase block (64)
  UINT32                    NumBlocks;      ///< Total erase blocks (1024)
  MIKROTIK_NAND_READ_PAGE   ReadPage;
};

extern EFI_GUID  gMikroTikNandFlashProtocolGuid;

#endif // MIKROTIK_NAND_FLASH_H_
