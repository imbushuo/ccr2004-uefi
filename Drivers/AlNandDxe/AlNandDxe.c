/** @file
  Annapurna Labs NAND controller DXE driver for MikroTik CCR2004.

  Uses the official AL HAL for NAND controller access (PIO, no DMA).
  Targets Toshiba BENAND (built-in ECC) flash as a read-only block device.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AlNandDxe.h"

//
// Device path template
//
STATIC AL_NAND_DEVICE_PATH  mAlNandDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (AL_NAND_DEVICE_PATH) - sizeof (EFI_DEVICE_PATH_PROTOCOL)),
        (UINT8)((sizeof (AL_NAND_DEVICE_PATH) - sizeof (EFI_DEVICE_PATH_PROTOCOL)) >> 8),
      },
    },
    { 0x8b3e4d5c, 0xf2a1, 0x4b07, { 0xa8, 0xea, 0x3f, 0xbc, 0x2e, 0x5d, 0x7f, 0x94 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
  Wait for NFC command buffer to drain, with timeout.
**/
STATIC
EFI_STATUS
AlNandWaitCmdEmpty (
  IN struct al_nand_ctrl_obj  *Obj
  )
{
  UINTN  Iter;

  for (Iter = 0; Iter < 1000; Iter++) {
    if (al_nand_cmd_buff_is_empty (Obj)) {
      return EFI_SUCCESS;
    }
  }

  for (Iter = 0; Iter < AL_NAND_POLL_TIMEOUT_US; Iter++) {
    if (al_nand_cmd_buff_is_empty (Obj)) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  DEBUG ((DEBUG_ERROR, "AlNandDxe: cmd_buff_is_empty timeout\n"));
  return EFI_TIMEOUT;
}

// ---------------------------------------------------------------------------
// NAND device operations
// ---------------------------------------------------------------------------

/**
  Send NAND RESET command and wait for device ready.
**/
STATIC
EFI_STATUS
AlNandDeviceReset (
  IN AL_NAND_CONTEXT  *Ctx
  )
{
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0xFF));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_WAIT_FOR_READY, 0));

  return AlNandWaitCmdEmpty (&Ctx->NandObj);
}

/**
  Read NAND device ID bytes.
**/
STATIC
EFI_STATUS
AlNandReadId (
  IN  AL_NAND_CONTEXT  *Ctx,
  OUT UINT8            *IdBuf,
  IN  UINT32           IdLen
  )
{
  UINT32  CwSize;
  int     Err;

  CwSize = (IdLen + 3) & ~3U;

  al_nand_cw_config (&Ctx->NandObj, CwSize, 1);

  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0x90));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_ADDRESS, 0x00));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_DATA_READ_COUNT, CwSize & 0xFF));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_DATA_READ_COUNT, (CwSize >> 8) & 0xFF));

  Err = al_nand_data_buff_read (&Ctx->NandObj, (int)CwSize, 0, (int)(CwSize - IdLen), IdBuf);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read a single NAND page (data area only).
**/
STATIC
EFI_STATUS
AlNandReadPage (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *Buffer
  )
{
  uint32_t  SeqBuf[AL_NAND_MAX_CMD_SEQ];
  int       NumEntries;
  uint32_t  CwSize;
  uint32_t  CwCount;
  int       Err;

  NumEntries = AL_NAND_MAX_CMD_SEQ;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj, 0, (int)Page, (int)Ctx->PageSize, 0,
          SeqBuf, &NumEntries, &CwSize, &CwCount);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);
  al_nand_cmd_seq_execute (&Ctx->NandObj, SeqBuf, NumEntries);

  Err = al_nand_data_buff_read (&Ctx->NandObj, (int)Ctx->PageSize, 0, 0, Buffer);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: Page %u read failed\n", Page));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read the first byte of OOB area (bad block marker).
**/
STATIC
EFI_STATUS
AlNandReadOobByte (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *OobByte
  )
{
  uint32_t  SeqBuf[AL_NAND_MAX_CMD_SEQ];
  int       NumEntries;
  uint32_t  CwSize;
  uint32_t  CwCount;
  UINT8     Tmp[4];
  int       Err;

  NumEntries = AL_NAND_MAX_CMD_SEQ;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj, (int)Ctx->PageSize, (int)Page, 4, 0,
          SeqBuf, &NumEntries, &CwSize, &CwCount);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);
  al_nand_cmd_seq_execute (&Ctx->NandObj, SeqBuf, NumEntries);

  Err = al_nand_data_buff_read (&Ctx->NandObj, 4, 0, 3, Tmp);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  *OobByte = Tmp[0];
  return EFI_SUCCESS;
}

/**
  Scan all blocks for bad block markers and build bitmap.
**/
STATIC
VOID
AlNandScanBadBlocks (
  IN AL_NAND_CONTEXT  *Ctx
  )
{
  UINT32      Block;
  UINT32      Page;
  UINT8       OobByte;
  EFI_STATUS  Status;
  UINT32      BadCount;

  ZeroMem (Ctx->BadBlockMap, sizeof (Ctx->BadBlockMap));
  BadCount = 0;

  for (Block = 0; Block < Ctx->NumBlocks; Block++) {
    Page = Block * Ctx->PagesPerBlock;

    Status = AlNandReadOobByte (Ctx, Page, &OobByte);
    if (EFI_ERROR (Status) || (OobByte != 0xFF)) {
      Ctx->BadBlockMap[Block / 8] |= (UINT8)(1 << (Block % 8));
      BadCount++;
      if (!EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "AlNandDxe: Bad block %u (OOB=0x%02x)\n", Block, OobByte));
      } else {
        DEBUG ((DEBUG_WARN, "AlNandDxe: Bad block %u (read error)\n", Block));
      }
    }
  }

  DEBUG ((DEBUG_INFO, "AlNandDxe: Bad block scan complete, %u bad blocks found\n", BadCount));
}

STATIC
BOOLEAN
AlNandIsBlockBad (
  IN AL_NAND_CONTEXT  *Ctx,
  IN UINT32           Block
  )
{
  if (Block >= Ctx->NumBlocks) {
    return TRUE;
  }
  return (Ctx->BadBlockMap[Block / 8] & (1 << (Block % 8))) != 0;
}

// ---------------------------------------------------------------------------
// MIKROTIK_NAND_FLASH_PROTOCOL implementation
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
AlNandFlashReadPage (
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *This,
  IN  UINT32                         PageIndex,
  OUT VOID                           *Buffer
  )
{
  AL_NAND_CONTEXT  *Ctx;
  UINT32           Block;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = AL_NAND_FROM_NANDFLASH (This);

  if (PageIndex >= Ctx->NumPages) {
    return EFI_INVALID_PARAMETER;
  }

  Block = PageIndex / Ctx->PagesPerBlock;
  if (AlNandIsBlockBad (Ctx, Block)) {
    SetMem (Buffer, Ctx->PageSize, 0xFF);
    return EFI_SUCCESS;
  }

  return AlNandReadPage (Ctx, PageIndex, Buffer);
}

/**
  Read only the inline tags (last 16 bytes) of a NAND page.
**/
STATIC
EFI_STATUS
EFIAPI
AlNandFlashReadTags (
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *This,
  IN  UINT32                         PageIndex,
  OUT VOID                           *Buffer
  )
{
  AL_NAND_CONTEXT  *Ctx;
  UINT32           Block;
  uint32_t         SeqBuf[AL_NAND_MAX_CMD_SEQ];
  int              NumEntries;
  uint32_t         CwSize;
  uint32_t         CwCount;
  int              Err;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = AL_NAND_FROM_NANDFLASH (This);

  if (PageIndex >= Ctx->NumPages) {
    return EFI_INVALID_PARAMETER;
  }

  Block = PageIndex / Ctx->PagesPerBlock;
  if (AlNandIsBlockBad (Ctx, Block)) {
    SetMem (Buffer, 16, 0xFF);
    return EFI_SUCCESS;
  }

  NumEntries = AL_NAND_MAX_CMD_SEQ;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj,
          (int)(Ctx->PageSize - 16),  // column = start of tags
          (int)PageIndex, 16, 0,
          SeqBuf, &NumEntries, &CwSize, &CwCount);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);
  al_nand_cmd_seq_execute (&Ctx->NandObj, SeqBuf, NumEntries);

  Err = al_nand_data_buff_read (&Ctx->NandObj, 16, 0, 0, Buffer);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read full page data using Change Read Column.
  Must be called immediately after ReadTags on the same page.
**/
STATIC
EFI_STATUS
EFIAPI
AlNandFlashReadPageBody (
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *This,
  OUT VOID                           *Buffer
  )
{
  AL_NAND_CONTEXT  *Ctx;
  uint32_t         CwSize;
  uint32_t         CwCount;
  uint32_t         i;
  int              Err;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = AL_NAND_FROM_NANDFLASH (This);

  CwSize  = AL_NAND_CW_SIZE;
  CwCount = Ctx->PageSize / CwSize;

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);

  //
  // Change Read Column: CMD(0x05), COL(0x0000), CMD(0xE0)
  //
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0x05));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_ADDRESS, 0));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_ADDRESS, 0));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0xE0));

  for (i = 0; i < CwCount; i++) {
    al_nand_cmd_single_execute (&Ctx->NandObj,
      AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_DATA_READ_COUNT, CwSize & 0xFF));
    al_nand_cmd_single_execute (&Ctx->NandObj,
      AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_DATA_READ_COUNT, (CwSize >> 8) & 0xFF));
  }

  Err = al_nand_data_buff_read (&Ctx->NandObj, (int)Ctx->PageSize, 0, 0, Buffer);
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_BLOCK_IO_PROTOCOL implementation
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
AlNandBlockIoReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return AlNandDeviceReset (AL_NAND_FROM_BLOCKIO (This));
}

STATIC
EFI_STATUS
EFIAPI
AlNandBlockIoReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  AL_NAND_CONTEXT  *Ctx;
  EFI_STATUS       Status;
  UINT32           Page;
  UINT32           NumPages;
  UINT32           Block;
  UINT8            *Buf;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = AL_NAND_FROM_BLOCKIO (This);

  if (MediaId != Ctx->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  if ((BufferSize % Ctx->PageSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  NumPages = (UINT32)(BufferSize / Ctx->PageSize);
  Page = (UINT32)Lba;

  if (Page + NumPages > Ctx->NumPages) {
    return EFI_INVALID_PARAMETER;
  }

  Buf = (UINT8 *)Buffer;

  while (NumPages > 0) {
    Block = Page / Ctx->PagesPerBlock;
    if (AlNandIsBlockBad (Ctx, Block)) {
      SetMem (Buf, Ctx->PageSize, 0xFF);
    } else {
      Status = AlNandReadPage (Ctx, Page, Buf);
      if (EFI_ERROR (Status)) {
        return EFI_DEVICE_ERROR;
      }
    }

    Buf += Ctx->PageSize;
    Page++;
    NumPages--;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlNandBlockIoWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
AlNandBlockIoFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
AlNandDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                      Status;
  AL_NAND_CONTEXT                 *Ctx;
  AL_NAND_DEVICE_PATH             *DevicePath;
  UINT8                           IdBuf[8];
  int                             Err;
  struct al_nand_dev_properties   DevProps;
  struct al_nand_ecc_config       EccCfg;

  Ctx = AllocateZeroPool (sizeof (AL_NAND_CONTEXT));
  if (Ctx == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ctx->Signature     = AL_NAND_SIGNATURE;
  Ctx->Handle        = NULL;
  Ctx->PageSize      = BENAND_PAGE_SIZE;
  Ctx->OobSize       = BENAND_OOB_SIZE;
  Ctx->PagesPerBlock = BENAND_PAGES_PER_BLOCK;
  Ctx->NumBlocks     = BENAND_NUM_BLOCKS;
  Ctx->NumPages      = BENAND_NUM_PAGES;

  //
  // Initialize HAL NAND controller (PIO only, no DMA)
  //
  Err = al_nand_init (&Ctx->NandObj, (void *)(UINTN)AL_NAND_BASE, NULL, 0);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: al_nand_init failed: %d\n", Err));
    FreePool (Ctx);
    return EFI_DEVICE_ERROR;
  }

  al_nand_dev_select (&Ctx->NandObj, 0);

  //
  // Configure device: BENAND, ONFI Mode 4, no ECC
  //
  ZeroMem (&DevProps, sizeof (DevProps));
  DevProps.timingMode        = AL_NAND_DEVICE_TIMING_MODE_MANUAL;
  DevProps.sdrDataWidth      = AL_NAND_DEVICE_SDR_DATA_WIDTH_8;
  DevProps.timing.tSETUP     = AL_NAND_TIM_SETUP;
  DevProps.timing.tHOLD      = AL_NAND_TIM_HOLD;
  DevProps.timing.tWH        = AL_NAND_TIM_WH;
  DevProps.timing.tWRP       = AL_NAND_TIM_WRP;
  DevProps.timing.tINTCMD    = AL_NAND_TIM_INTCMD;
  DevProps.timing.tRR        = AL_NAND_TIM_RR;
  DevProps.timing.tWB        = AL_NAND_TIM_WB;
  DevProps.timing.readDelay  = AL_NAND_TIM_READ_DLY;
  DevProps.readyBusyTimeout  = 0;   // R/B# pin mode (HAL sets RDYBSYEN=1)
  DevProps.num_col_cyc       = BENAND_COL_CYCLES;
  DevProps.num_row_cyc       = BENAND_ROW_CYCLES;
  DevProps.pageSize          = AL_NAND_DEVICE_PAGE_SIZE_2K;

  ZeroMem (&EccCfg, sizeof (EccCfg));

  Err = al_nand_dev_config (&Ctx->NandObj, &DevProps, &EccCfg);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: al_nand_dev_config failed: %d\n", Err));
    FreePool (Ctx);
    return EFI_DEVICE_ERROR;
  }

  //
  // Disable ECC (BENAND has on-die ECC)
  //
  al_nand_ecc_set_enabled (&Ctx->NandObj, 0);

  //
  // Clear stale RouterBOOT state that the HAL's read-modify-write preserves:
  // - BCH control registers: prevents parity byte injection
  // - Spare offset register: prevents OOB byte insertion into data stream
  //
  al_reg_write32 (&Ctx->NandObj.regs_base->bch_ctrl_reg_0, 0);
  al_reg_write32 (&Ctx->NandObj.regs_base->bch_ctrl_reg_1, 0);
  al_reg_write32 (&Ctx->NandObj.regs_base->nflash_spare_offset, 0);

  //
  // Disable all NFC interrupts (polling mode)
  //
  al_nand_int_disable (&Ctx->NandObj, 0xFFFFFFFF);
  al_reg_write32 (&Ctx->NandObj.regs_base->nfc_int_stat, 0xFFFFFFFF);

  //
  // Reset NAND device
  //
  Status = AlNandDeviceReset (Ctx);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: Device reset failed: %r\n", Status));
    FreePool (Ctx);
    return Status;
  }

  //
  // Read and verify device ID
  //
  Status = AlNandReadId (Ctx, IdBuf, 8);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: ReadID failed\n"));
    FreePool (Ctx);
    return Status;
  }

  DEBUG ((DEBUG_WARN, "AlNandDxe: NAND ID: %02x %02x %02x %02x %02x %02x %02x %02x\n",
    IdBuf[0], IdBuf[1], IdBuf[2], IdBuf[3], IdBuf[4], IdBuf[5], IdBuf[6], IdBuf[7]));

  if (IdBuf[0] != 0x98) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: Non-Toshiba NAND (mfr=0x%02x)\n", IdBuf[0]));
  }

  if ((IdBuf[4] & TOSHIBA_NAND_ID5_IS_BENAND) != 0) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: Toshiba BENAND detected (ID5=0x%02x)\n", IdBuf[4]));
  } else {
    DEBUG ((DEBUG_WARN, "AlNandDxe: BENAND flag not set (ID5=0x%02x)\n", IdBuf[4]));
  }

  //
  // Scan for bad blocks
  //
  AlNandScanBadBlocks (Ctx);

  //
  // Set up BLOCK_IO protocol
  //
  Ctx->Media.MediaId          = 0;
  Ctx->Media.RemovableMedia   = FALSE;
  Ctx->Media.MediaPresent     = TRUE;
  Ctx->Media.LogicalPartition = FALSE;
  Ctx->Media.ReadOnly         = TRUE;
  Ctx->Media.WriteCaching     = FALSE;
  Ctx->Media.BlockSize        = Ctx->PageSize;
  Ctx->Media.IoAlign          = 4;
  Ctx->Media.LastBlock        = Ctx->NumPages - 1;

  Ctx->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Ctx->BlockIo.Media       = &Ctx->Media;
  Ctx->BlockIo.Reset       = AlNandBlockIoReset;
  Ctx->BlockIo.ReadBlocks  = AlNandBlockIoReadBlocks;
  Ctx->BlockIo.WriteBlocks = AlNandBlockIoWriteBlocks;
  Ctx->BlockIo.FlushBlocks = AlNandBlockIoFlushBlocks;

  //
  // Set up NAND Flash protocol
  //
  Ctx->NandFlash.PageSize      = Ctx->PageSize;
  Ctx->NandFlash.PagesPerBlock = Ctx->PagesPerBlock;
  Ctx->NandFlash.NumBlocks     = Ctx->NumBlocks;
  Ctx->NandFlash.ReadPage      = AlNandFlashReadPage;
  Ctx->NandFlash.ReadTags      = AlNandFlashReadTags;
  Ctx->NandFlash.ReadPageBody  = AlNandFlashReadPageBody;

  //
  // Allocate device path
  //
  DevicePath = AllocateCopyPool (sizeof (mAlNandDevicePathTemplate),
                                 &mAlNandDevicePathTemplate);
  if (DevicePath == NULL) {
    FreePool (Ctx);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Install protocols
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Ctx->Handle,
                  &gEfiBlockIoProtocolGuid,
                  &Ctx->BlockIo,
                  &gEfiDevicePathProtocolGuid,
                  (EFI_DEVICE_PATH_PROTOCOL *)DevicePath,
                  &gMikroTikNandFlashProtocolGuid,
                  &Ctx->NandFlash,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: Protocol install failed: %r\n", Status));
    FreePool (DevicePath);
    FreePool (Ctx);
    return Status;
  }

  DEBUG ((DEBUG_WARN, "AlNandDxe: NAND at 0x%lx, %u pages, %u bytes/page\n",
    (UINT64)AL_NAND_BASE, Ctx->NumPages, Ctx->PageSize));

  return EFI_SUCCESS;
}
