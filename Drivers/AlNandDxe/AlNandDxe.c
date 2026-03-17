/** @file
  Annapurna Labs NAND controller DXE driver for MikroTik CCR2004.

  Drives the AL NAND controller at 0xFA100000 on the Alpine V2 SoC.
  Supports Toshiba BENAND (built-in ECC) flash as a read-only block device.
  Polling-based PIO, no interrupts, no DMA.

  Block device exposes raw NAND pages (2048 bytes per block).
  Bad blocks return EFI_DEVICE_ERROR on read.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AlNandDxe.h"

//
// Device path template (GUID matches FILE_GUID from INF)
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
    // GUID matches FILE_GUID from INF
    { 0x8b3e4d5c, 0xf2a1, 0x4b07, { 0xa8, 0xea, 0x3f, 0xbc, 0x2e, 0x5d, 0x7f, 0x94 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

// ---------------------------------------------------------------------------
// Low-level register helpers
// ---------------------------------------------------------------------------

STATIC
VOID
AlNandWriteMasked32 (
  IN UINTN   Addr,
  IN UINT32  Mask,
  IN UINT32  Value
  )
{
  UINT32  Tmp;

  Tmp = MmioRead32 (Addr);
  Tmp = (Tmp & ~Mask) | (Value & Mask);
  MmioWrite32 (Addr, Tmp);
}

// ---------------------------------------------------------------------------
// Command buffer helpers
// ---------------------------------------------------------------------------

/**
  Write a single command to the command buffer FIFO.
**/
STATIC
VOID
AlNandCmdExec (
  IN AL_NAND_CONTEXT  *Ctx,
  IN UINT32           Cmd
  )
{
  MmioWrite32 (Ctx->CmdBuffBase, Cmd);
}

/**
  Send a 16-bit byte count as two command buffer entries (LSB first).
**/
STATIC
VOID
AlNandSendByteCount (
  IN AL_NAND_CONTEXT  *Ctx,
  IN UINT32           CmdType,
  IN UINT16           Count
  )
{
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (CmdType, Count & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (CmdType, (Count >> 8) & 0xFF));
}

/**
  Poll NFC_INT_STAT for a given bit mask, with timeout.
**/
STATIC
EFI_STATUS
AlNandPollIntStatus (
  IN AL_NAND_CONTEXT  *Ctx,
  IN UINT32           Mask
  )
{
  UINTN   Elapsed;
  UINT32  Status;

  for (Elapsed = 0; Elapsed < AL_NAND_POLL_TIMEOUT_US; Elapsed += AL_NAND_POLL_INTERVAL_US) {
    Status = MmioRead32 (Ctx->CtrlBase + AL_NAND_NFC_INT_STAT);
    if ((Status & Mask) != 0) {
      //
      // Clear the matched bits (W1C register)
      //
      MmioWrite32 (Ctx->CtrlBase + AL_NAND_NFC_INT_STAT, Status & Mask);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (AL_NAND_POLL_INTERVAL_US);
  }

  DEBUG ((DEBUG_ERROR, "AlNandDxe: Int status poll timeout (mask=0x%x)\n", Mask));
  return EFI_TIMEOUT;
}

/**
  Wait for command buffer to drain (CMD_BUF_EMPTY).
**/
STATIC
EFI_STATUS
AlNandWaitCmdEmpty (
  IN AL_NAND_CONTEXT  *Ctx
  )
{
  return AlNandPollIntStatus (Ctx, AL_NAND_INT_CMD_BUF_EMPTY);
}

// ---------------------------------------------------------------------------
// Codeword configuration
// ---------------------------------------------------------------------------

/**
  Configure codeword size and count for data transfers.
**/
STATIC
VOID
AlNandCwConfig (
  IN AL_NAND_CONTEXT  *Ctx,
  IN UINT32           CwSize,
  IN UINT32           CwCount
  )
{
  AlNandWriteMasked32 (
    Ctx->CtrlBase + AL_NAND_CW_SIZE_CNT_REG,
    AL_NAND_CW_SIZE_MASK | AL_NAND_CW_COUNT_MASK,
    (CwSize << AL_NAND_CW_SIZE_SHIFT) | (CwCount << AL_NAND_CW_COUNT_SHIFT)
    );

  //
  // Wrapper codeword size
  //
  MmioWrite32 (Ctx->WrapBase + AL_NAND_WRAP_CW_SIZE, CwSize);
}

// ---------------------------------------------------------------------------
// Hardware initialization
// ---------------------------------------------------------------------------

/**
  Initialize the AL NAND controller.

  Sets bank types, timing, mode, disables ECC (BENAND has internal ECC).
**/
STATIC
VOID
AlNandHwInit (
  IN AL_NAND_CONTEXT  *Ctx
  )
{
  UINT32  TimParams0;
  UINT32  TimParams1;
  UINT32  CtlReg0;
  UINT32  Twb;

  //
  // Set all 6 banks to NAND type
  //
  MmioWrite32 (Ctx->CtrlBase + AL_NAND_FLASH_CTL_3, AL_NAND_FLASH_CTL_3_ALL_NAND);

  //
  // SDR mode, ONFI timing mode 0 (manual via RMN 2903)
  //
  AlNandWriteMasked32 (
    Ctx->CtrlBase + AL_NAND_MODE_SELECT_REG,
    0x7FFF,    // mode + all timing fields
    AL_NAND_MODE_SELECT_SDR | (6 << AL_NAND_MODE_SDR_TIM_SHIFT)  // 6 = manual timing mode
    );

  //
  // Configure ctl_reg0: CS0, 8-bit DQ, 2 col cycles, 2 row cycles, 2K page, RX mode
  //
  CtlReg0 = (0 << AL_NAND_CTL_REG0_CS_SHIFT) |                // CS0
            (0 << AL_NAND_CTL_REG0_CS2_SHIFT) |                // CS2 = 0
            (BENAND_COL_CYCLES << AL_NAND_CTL_REG0_COL_SHIFT) |
            (BENAND_ROW_CYCLES << AL_NAND_CTL_REG0_ROW_SHIFT) |
            (AL_NAND_PAGE_SIZE_2K << AL_NAND_CTL_REG0_PAGE_SHIFT);
            // DQ_WIDTH bit clear = 8-bit, TX_MODE bit clear = RX
  MmioWrite32 (Ctx->CtrlBase + AL_NAND_CTL_REG0, CtlReg0);

  //
  // SDR timing parameters for ONFI mode 0 at 500 MHz
  //
  TimParams0 = (AL_NAND_TIM_SETUP  << AL_NAND_SDR_T0_SETUP_SHIFT) |
               (AL_NAND_TIM_HOLD   << AL_NAND_SDR_T0_HOLD_SHIFT) |
               (AL_NAND_TIM_WH     << AL_NAND_SDR_T0_WH_SHIFT) |
               (AL_NAND_TIM_WRP    << AL_NAND_SDR_T0_WRP_SHIFT) |
               (AL_NAND_TIM_INTCMD << AL_NAND_SDR_T0_INTCMD_SHIFT);
  MmioWrite32 (Ctx->CtrlBase + AL_NAND_SDR_TIM_PARAMS_0, TimParams0);

  //
  // tWB is 7 bits split: low 6 bits in [11:6], MSB in bit [14]
  //
  Twb = AL_NAND_TIM_WB;
  TimParams1 = (AL_NAND_TIM_RR       << AL_NAND_SDR_T1_RR_SHIFT) |
               ((Twb & 0x3F)         << AL_NAND_SDR_T1_WB_SHIFT) |
               (AL_NAND_TIM_READ_DLY << AL_NAND_SDR_T1_READ_DLY_SHIFT) |
               (((Twb >> 6) & 1)     << AL_NAND_SDR_T1_WB_MSB_SHIFT);
  MmioWrite32 (Ctx->CtrlBase + AL_NAND_SDR_TIM_PARAMS_1, TimParams1);

  //
  // Ready/busy timeout: max value, enabled
  //
  MmioWrite32 (
    Ctx->CtrlBase + AL_NAND_RDY_BUSY_WAIT_CNT,
    (0xFFFF << AL_NAND_RDY_TOUT_SHIFT) | AL_NAND_RDYBSYEN
    );

  //
  // Disable ECC (BENAND has internal ECC)
  //
  AlNandWriteMasked32 (
    Ctx->CtrlBase + AL_NAND_BCH_CTRL_REG_0,
    AL_NAND_BCH_ECC_ON_OFF,
    0
    );

  //
  // Disable all interrupts (we poll)
  //
  MmioWrite32 (Ctx->CtrlBase + AL_NAND_NFC_INT_EN, 0);

  //
  // Clear any pending interrupt status
  //
  MmioWrite32 (Ctx->CtrlBase + AL_NAND_NFC_INT_STAT, 0xFFFFFFFF);
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
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_CMD, NAND_CMD_RESET));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_WAIT_READY, 0));

  return AlNandWaitCmdEmpty (Ctx);
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
  EFI_STATUS  Status;
  UINT32      CwSize;
  UINT32      CwCount;
  UINT32      i;
  UINT32      Word;

  //
  // Align to 4 bytes for codeword
  //
  CwSize = (IdLen + 3) & ~3U;
  CwCount = 1;

  //
  // Configure codeword
  //
  AlNandCwConfig (Ctx, CwSize, CwCount);

  //
  // Send READID command sequence
  //
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_CMD, NAND_CMD_READID));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, 0x00));

  //
  // Request data read
  //
  AlNandSendByteCount (Ctx, AL_NAND_CMD_TYPE_DATA_READ, (UINT16)CwSize);

  //
  // Wait for data ready
  //
  Status = AlNandPollIntStatus (Ctx, AL_NAND_INT_BUF_RDRDY);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Read data from buffer (FIFO reads, 4 bytes at a time)
  //
  for (i = 0; i < CwSize / 4; i++) {
    Word = MmioRead32 (Ctx->DataBuffBase);
    if (i * 4 < IdLen) {
      UINT32  Remaining = IdLen - i * 4;
      if (Remaining > 4) {
        Remaining = 4;
      }
      CopyMem (IdBuf + i * 4, &Word, Remaining);
    }
  }

  return EFI_SUCCESS;
}

/**
  Read a single NAND page (data area only, no OOB).

  @param[in]  Ctx       Controller context
  @param[in]  Page      Page number (0-based)
  @param[out] Buffer    Output buffer, must be PageSize bytes
**/
STATIC
EFI_STATUS
AlNandReadPage (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *Buffer
  )
{
  EFI_STATUS  Status;
  UINT32      CwSize;
  UINT32      CwCount;
  UINT32      i;
  UINT32      CwIdx;

  CwSize  = AL_NAND_CW_SIZE;
  CwCount = Ctx->PageSize / CwSize;

  //
  // Configure codewords
  //
  AlNandCwConfig (Ctx, CwSize, CwCount);

  //
  // Send page read command: CMD(0x00), COL0, COL1, ROW0, ROW1, CMD(0x30), WAIT_READY
  //
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_CMD, NAND_CMD_READ0));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, 0));        // Column low
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, 0));        // Column high
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, Page & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, (Page >> 8) & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_CMD, NAND_CMD_READ1));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_WAIT_READY, 0));

  //
  // Queue all DATA_READ_COUNT commands
  //
  for (CwIdx = 0; CwIdx < CwCount; CwIdx++) {
    AlNandSendByteCount (Ctx, AL_NAND_CMD_TYPE_DATA_READ, (UINT16)CwSize);
  }

  //
  // Read each codeword as it becomes available
  //
  for (CwIdx = 0; CwIdx < CwCount; CwIdx++) {
    Status = AlNandPollIntStatus (Ctx, AL_NAND_INT_BUF_RDRDY);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "AlNandDxe: Page %u codeword %u read timeout\n", Page, CwIdx));
      return Status;
    }

    //
    // Read codeword from data buffer FIFO
    //
    for (i = 0; i < CwSize / 4; i++) {
      ((UINT32 *)Buffer)[CwIdx * (CwSize / 4) + i] = MmioRead32 (Ctx->DataBuffBase);
    }
  }

  return EFI_SUCCESS;
}

/**
  Read the first byte of OOB area for a given page (bad block marker).

  @param[in]  Ctx       Controller context
  @param[in]  Page      Page number
  @param[out] OobByte   First OOB byte
**/
STATIC
EFI_STATUS
AlNandReadOobByte (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *OobByte
  )
{
  EFI_STATUS  Status;
  UINT32      Col;
  UINT32      Word;

  //
  // Column address = page data size (start of OOB)
  //
  Col = Ctx->PageSize;

  //
  // Configure codeword: 4 bytes (minimum), 1 codeword
  //
  AlNandCwConfig (Ctx, 4, 1);

  //
  // Send page read with column pointing to OOB area
  //
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_CMD, NAND_CMD_READ0));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, Col & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, (Col >> 8) & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, Page & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_ADDRESS, (Page >> 8) & 0xFF));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_CMD, NAND_CMD_READ1));
  AlNandCmdExec (Ctx, AL_NAND_CMD_ENTRY (AL_NAND_CMD_TYPE_WAIT_READY, 0));

  //
  // Read 4 bytes
  //
  AlNandSendByteCount (Ctx, AL_NAND_CMD_TYPE_DATA_READ, 4);

  Status = AlNandPollIntStatus (Ctx, AL_NAND_INT_BUF_RDRDY);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Word = MmioRead32 (Ctx->DataBuffBase);
  *OobByte = (UINT8)(Word & 0xFF);

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
      //
      // Mark block as bad
      //
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

/**
  Check if a block is marked bad.
**/
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
  AL_NAND_CONTEXT  *Ctx;

  Ctx = AL_NAND_FROM_BLOCKIO (This);
  return AlNandDeviceReset (Ctx);
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
    //
    // Check if this page's block is bad
    //
    Block = Page / Ctx->PagesPerBlock;
    if (AlNandIsBlockBad (Ctx, Block)) {
      //
      // Return all-FF for bad block pages (erased pattern) so
      // sequential reads can proceed past bad blocks.
      //
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
  EFI_STATUS           Status;
  AL_NAND_CONTEXT      *Ctx;
  AL_NAND_DEVICE_PATH  *DevicePath;
  UINT8                IdBuf[5];

  //
  // Allocate context
  //
  Ctx = AllocateZeroPool (sizeof (AL_NAND_CONTEXT));
  if (Ctx == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ctx->Signature    = AL_NAND_SIGNATURE;
  Ctx->Handle       = NULL;
  Ctx->NandBase     = AL_NAND_BASE;
  Ctx->CtrlBase     = AL_NAND_BASE + AL_NAND_CTRL_BASE_OFFSET;
  Ctx->WrapBase     = AL_NAND_BASE + AL_NAND_WRAP_BASE_OFFSET;
  Ctx->CmdBuffBase  = AL_NAND_BASE + AL_NAND_CMD_BUFF_OFFSET;
  Ctx->DataBuffBase = AL_NAND_BASE + AL_NAND_DATA_BUFF_OFFSET;

  //
  // Set Toshiba BENAND parameters
  //
  Ctx->PageSize      = BENAND_PAGE_SIZE;
  Ctx->OobSize       = BENAND_OOB_SIZE;
  Ctx->PagesPerBlock = BENAND_PAGES_PER_BLOCK;
  Ctx->NumBlocks     = BENAND_NUM_BLOCKS;
  Ctx->NumPages      = BENAND_NUM_PAGES;

  //
  // Initialize controller hardware
  //
  AlNandHwInit (Ctx);

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
  // Read device ID
  //
  Status = AlNandReadId (Ctx, IdBuf, 5);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: ReadID failed: %r\n", Status));
    FreePool (Ctx);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "AlNandDxe: NAND ID: %02x %02x %02x %02x %02x\n",
    IdBuf[0], IdBuf[1], IdBuf[2], IdBuf[3], IdBuf[4]));

  //
  // Verify manufacturer (Toshiba = 0x98) and BENAND flag
  //
  if (IdBuf[0] != 0x98) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: Non-Toshiba NAND (mfr=0x%02x), proceeding anyway\n", IdBuf[0]));
  }

  if ((IdBuf[3] & TOSHIBA_NAND_ID4_IS_BENAND) != 0) {
    DEBUG ((DEBUG_INFO, "AlNandDxe: Toshiba BENAND detected\n"));
  } else {
    DEBUG ((DEBUG_WARN, "AlNandDxe: BENAND flag not set in ID byte 4, proceeding\n"));
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
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: Protocol install failed: %r\n", Status));
    FreePool (DevicePath);
    FreePool (Ctx);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "AlNandDxe: NAND at 0x%lx, %u pages, %u bytes/page, block device ready\n",
    (UINT64)Ctx->NandBase, Ctx->NumPages, Ctx->PageSize));

  return EFI_SUCCESS;
}
