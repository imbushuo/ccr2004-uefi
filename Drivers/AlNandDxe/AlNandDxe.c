/** @file
  Annapurna Labs NAND controller DXE driver for MikroTik CCR2004.

  Drives the AL NAND controller at 0xFA100000 on the Alpine V2 SoC.
  Supports Toshiba BENAND (built-in ECC) flash as a read-only block device.

  Uses the official AL HAL for NAND access. Page reads are accelerated via
  the SSM RAID DMA engine (memory-copy DMA) when available, falling back
  to PIO otherwise.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AlNandDxe.h"

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;

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
// DMA initialization
// ---------------------------------------------------------------------------

/**
  Allocate DMA-safe memory: below 4GB, uncacheable.
  Returns both virtual and physical addresses.
**/
STATIC
EFI_STATUS
AlNandAllocDmaBuffer (
  IN  UINTN                 Size,
  OUT VOID                  **VirtAddr,
  OUT EFI_PHYSICAL_ADDRESS  *PhysAddr
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Pages;
  UINTN                 NumPages;

  NumPages = EFI_SIZE_TO_PAGES (Size);
  Pages = 0xFFFFFFFF;

  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData,
                               NumPages, &Pages);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *VirtAddr = (VOID *)(UINTN)Pages;
  *PhysAddr = Pages;

  //
  // Zero while cacheable, flush, then set to UC for DMA coherency
  //
  ZeroMem (*VirtAddr, EFI_PAGES_TO_SIZE (NumPages));
  WriteBackInvalidateDataCacheRange (*VirtAddr, EFI_PAGES_TO_SIZE (NumPages));

  if (mCpu != NULL) {
    mCpu->SetMemoryAttributes (mCpu, Pages,
                               EFI_PAGES_TO_SIZE (NumPages),
                               EFI_MEMORY_UC);
  }

  return EFI_SUCCESS;
}

/**
  Initialize the SSM RAID DMA engine for use with NAND.

  Sets up UDMA descriptor rings, initializes SSM DMA and RAID.
**/
STATIC
EFI_STATUS
AlNandDmaInit (
  IN AL_NAND_CONTEXT  *Ctx
  )
{
  EFI_STATUS                  Status;
  struct al_ssm_dma_params    SsmParams;
  struct al_udma_q_params     TxQParams;
  struct al_udma_q_params     RxQParams;
  int                         Err;

  //
  // Allocate descriptor rings (UC, below 4GB)
  //
  Status = AlNandAllocDmaBuffer (AL_NAND_DMA_TOTAL_RING_SIZE,
                                 &Ctx->DescRingBase, &Ctx->DescRingPhys);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: DMA ring alloc failed: %r\n", Status));
    return Status;
  }

  //
  // Allocate DMA page buffer (for bounce buffer reads)
  //
  Status = AlNandAllocDmaBuffer (EFI_PAGE_SIZE,
                                 &Ctx->DmaBufBase, &Ctx->DmaBufPhys);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: DMA buf alloc failed: %r\n", Status));
    return Status;
  }

  //
  // Allocate DMA command sequence buffer
  //
  Status = AlNandAllocDmaBuffer (EFI_PAGE_SIZE,
                                 &Ctx->CmdSeqBase, &Ctx->CmdSeqPhys);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: DMA cmd seq alloc failed: %r\n", Status));
    return Status;
  }

  //
  // Enable bus master on the SSM RAID unit via its adapter config space
  // (at the device base, NOT the ECAM which is inaccessible on this board)
  //
  {
    UINT32  PciCmd;
    PciCmd = MmioRead32 (AL_SSM_RAID_ADAPTER_BASE + 0x04);
    DEBUG ((DEBUG_WARN, "AlNandDxe: SSM RAID adapter CMD=0x%08x\n", PciCmd));
    PciCmd |= BIT2;  // Bus Master Enable
    MmioWrite32 (AL_SSM_RAID_ADAPTER_BASE + 0x04, PciCmd);
    PciCmd = MmioRead32 (AL_SSM_RAID_ADAPTER_BASE + 0x04);
    DEBUG ((DEBUG_WARN, "AlNandDxe: SSM RAID adapter CMD after BME=0x%08x\n", PciCmd));
  }

  //
  // Initialize SSM DMA
  //
  ZeroMem (&SsmParams, sizeof (SsmParams));
  SsmParams.rev_id          = AL_SSM_REV_ID_REV1;
  SsmParams.udma_regs_base  = (void *)(UINTN)AL_SSM_RAID_UDMA_BASE;
  SsmParams.name            = "nand-raid";
  SsmParams.num_of_queues   = 1;
  SsmParams.unit_adapter    = NULL;

  Err = al_ssm_dma_init (&Ctx->SsmDma, &SsmParams);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: al_ssm_dma_init failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  //
  // Initialize queue 0
  //
  ZeroMem (&TxQParams, sizeof (TxQParams));
  TxQParams.size           = AL_NAND_DMA_DESCS_PER_Q;
  TxQParams.desc_base      = (union al_udma_desc *)
                             ((UINTN)Ctx->DescRingBase + 0 * AL_NAND_DMA_RING_SIZE);
  TxQParams.desc_phy_base  = Ctx->DescRingPhys + 0 * AL_NAND_DMA_RING_SIZE;
  TxQParams.cdesc_base     = (uint8_t *)
                             ((UINTN)Ctx->DescRingBase + 1 * AL_NAND_DMA_RING_SIZE);
  TxQParams.cdesc_phy_base = Ctx->DescRingPhys + 1 * AL_NAND_DMA_RING_SIZE;
  TxQParams.cdesc_size     = AL_NAND_DMA_DESC_SIZE;

  ZeroMem (&RxQParams, sizeof (RxQParams));
  RxQParams.size           = AL_NAND_DMA_DESCS_PER_Q;
  RxQParams.desc_base      = (union al_udma_desc *)
                             ((UINTN)Ctx->DescRingBase + 2 * AL_NAND_DMA_RING_SIZE);
  RxQParams.desc_phy_base  = Ctx->DescRingPhys + 2 * AL_NAND_DMA_RING_SIZE;
  RxQParams.cdesc_base     = (uint8_t *)
                             ((UINTN)Ctx->DescRingBase + 3 * AL_NAND_DMA_RING_SIZE);
  RxQParams.cdesc_phy_base = Ctx->DescRingPhys + 3 * AL_NAND_DMA_RING_SIZE;
  RxQParams.cdesc_size     = AL_NAND_DMA_DESC_SIZE;

  Err = al_ssm_dma_q_init (&Ctx->SsmDma, 0, &TxQParams, &RxQParams, AL_RAID_Q);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: al_ssm_dma_q_init failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  //
  // Enable DMA
  //
  Err = al_ssm_dma_state_set (&Ctx->SsmDma, UDMA_NORMAL);
  if (Err != 0) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: al_ssm_dma_state_set failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  //
  // Initialize RAID engine (loads GF tables)
  //
  al_raid_init (&Ctx->SsmDma, (void *)(UINTN)AL_SSM_RAID_APP_BASE);

  //
  // DMA self-test: memory-to-memory copy via RAID engine
  //
  {
    UINT32                     *SrcBuf;
    UINT32                     *DstBuf;
    struct al_raid_transaction Xaction;
    struct al_block            SrcBlock;
    struct al_block            DstBlock;
    struct al_buf              SrcAlBuf;
    struct al_buf              DstAlBuf;
    uint32_t                   CompStatus;
    int                        Ret;
    UINTN                      i;
    BOOLEAN                    Pass;

    SrcBuf = (UINT32 *)Ctx->CmdSeqBase;
    DstBuf = (UINT32 *)((UINTN)Ctx->DmaBufBase + 2048);  // use upper half

    //
    // Fill source with pattern, clear destination
    //
    for (i = 0; i < 16; i++) {
      SrcBuf[i] = 0xDEAD0000 | (UINT32)i;
    }
    for (i = 0; i < 16; i++) {
      DstBuf[i] = 0;
    }

    SrcAlBuf.addr = (al_phys_addr_t)Ctx->CmdSeqPhys;
    SrcAlBuf.len  = 64;
    DstAlBuf.addr = (al_phys_addr_t)(Ctx->DmaBufPhys + 2048);
    DstAlBuf.len  = 64;

    SrcBlock.bufs = &SrcAlBuf;
    SrcBlock.num  = 1;
    DstBlock.bufs = &DstAlBuf;
    DstBlock.num  = 1;

    ZeroMem (&Xaction, sizeof (Xaction));
    Xaction.op             = AL_RAID_OP_MEM_CPY;
    Xaction.flags          = AL_SSM_BARRIER | AL_SSM_INTERRUPT;
    Xaction.srcs_blocks    = &SrcBlock;
    Xaction.num_of_srcs    = 1;
    Xaction.total_src_bufs = 1;
    Xaction.dsts_blocks    = &DstBlock;
    Xaction.num_of_dsts    = 1;
    Xaction.total_dst_bufs = 1;

    Err = al_raid_dma_prepare (&Ctx->SsmDma, 0, &Xaction);
    if (Err != 0) {
      DEBUG ((DEBUG_WARN, "AlNandDxe: DMA self-test prepare failed: %d\n", Err));
      return EFI_DEVICE_ERROR;
    }

    Err = al_raid_dma_action (&Ctx->SsmDma, 0, Xaction.tx_descs_count);
    if (Err != 0) {
      DEBUG ((DEBUG_WARN, "AlNandDxe: DMA self-test action failed: %d\n", Err));
      return EFI_DEVICE_ERROR;
    }

    //
    // Poll for completion (short timeout since it's just 64 bytes)
    //
    Ret = 0;
    for (i = 0; i < 100000 && Ret == 0; i++) {
      Ret = al_raid_dma_completion (&Ctx->SsmDma, 0, &CompStatus);
      if (Ret == 0 && i >= 1000) {
        MicroSecondDelay (1);
      }
    }

    if (Ret <= 0) {
      DEBUG ((DEBUG_WARN, "AlNandDxe: DMA self-test FAILED: no completion\n"));
      return EFI_DEVICE_ERROR;
    }

    //
    // Verify destination
    //
    Pass = TRUE;
    for (i = 0; i < 16; i++) {
      if (DstBuf[i] != (0xDEAD0000 | (UINT32)i)) {
        Pass = FALSE;
        break;
      }
    }

    DEBUG ((DEBUG_WARN, "AlNandDxe: DMA self-test %a (status=0x%x)\n",
      Pass ? "PASSED" : "FAILED", CompStatus));

    if (!Pass) {
      return EFI_DEVICE_ERROR;
    }
  }

  DEBUG ((DEBUG_WARN, "AlNandDxe: SSM RAID DMA initialized\n"));
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// NFC interrupt status helpers
// ---------------------------------------------------------------------------

/**
  Poll NFC interrupt status for a given bit mask, with timeout.
  Uses a tight busy-loop for the first 1000 iterations, then falls
  back to 1us delays.
**/
STATIC
EFI_STATUS
AlNandPollIntStatus (
  IN struct al_nand_ctrl_obj  *Obj,
  IN UINT32                   Mask
  )
{
  UINTN   Iter;
  UINT32  Status;

  for (Iter = 0; Iter < 1000; Iter++) {
    Status = al_nand_int_status_get (Obj);
    if ((Status & Mask) != 0) {
      return EFI_SUCCESS;
    }
  }

  for (Iter = 0; Iter < AL_NAND_POLL_TIMEOUT_US; Iter++) {
    Status = al_nand_int_status_get (Obj);
    if ((Status & Mask) != 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  DEBUG ((DEBUG_ERROR, "AlNandDxe: Int status poll timeout (mask=0x%x)\n", Mask));
  return EFI_TIMEOUT;
}

/**
  Wait for DMA transaction completions. Polls until the expected number
  of completions have been acknowledged.
**/
STATIC
EFI_STATUS
AlNandWaitDmaCompletion (
  IN struct al_nand_ctrl_obj  *Obj,
  IN INT32                    NumCompletions
  )
{
  UINTN     Iter;
  uint32_t  CompStatus;
  INT32     Done;
  int       Ret;

  Done = 0;
  for (Iter = 0; Iter < AL_NAND_POLL_TIMEOUT_US && Done < NumCompletions; Iter++) {
    Ret = al_nand_transaction_completion (Obj, &CompStatus);
    if (Ret > 0) {
      Done++;
      if (CompStatus != 0) {
        DEBUG ((DEBUG_WARN, "AlNandDxe: DMA completion status 0x%x\n", CompStatus));
      }
      continue;
    }
    if (Iter >= 1000) {
      MicroSecondDelay (1);
    }
  }

  if (Done < NumCompletions) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: DMA completion timeout (%d/%d)\n", Done, NumCompletions));
    return EFI_TIMEOUT;
  }

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// NAND device operations
// ---------------------------------------------------------------------------

/**
  Read a single NAND page via DMA.

  Follows the HAL-recommended full-DMA flow:
    1. al_nand_cw_config (PIO register writes, fast)
    2. al_nand_cmd_seq_execute_dma (DMA cmd seq to NFC, no interrupt)
    3. al_nand_data_buff_read_dma (DMA data from NFC, with interrupt)
    4. al_nand_transaction_completion x2

  Both commands and data go through the RAID DMA engine with BARRIER
  flag ensuring sequential execution.
**/
STATIC
EFI_STATUS
AlNandReadPageDma (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *Buffer
  )
{
  uint32_t       *CmdSeq;
  int            NumEntries;
  uint32_t       CwSize;
  uint32_t       CwCount;
  struct al_buf  CmdBuf;
  struct al_buf  DataBuf;
  int            Err;
  EFI_STATUS     Status;

  //
  // Generate page read command sequence into DMA-accessible buffer
  //
  CmdSeq = (uint32_t *)Ctx->CmdSeqBase;
  NumEntries = AL_NAND_MAX_CMD_SEQ_ENTRIES;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj,
          0,              // column = 0
          (int)Page,      // row = page number
          (int)Ctx->PageSize,
          0,              // ecc_enabled = 0 (BENAND)
          CmdSeq,
          &NumEntries,
          &CwSize,
          &CwCount
          );
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: cmd_seq_gen_page_read failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  //
  // Configure codeword (PIO register writes, fast)
  //
  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);

  //
  // DMA command sequence to NFC (no interrupt, barrier ensures ordering)
  //
  CmdBuf.addr = (al_phys_addr_t)Ctx->CmdSeqPhys;
  CmdBuf.len  = (uint32_t)(NumEntries * sizeof(uint32_t));

  Err = al_nand_cmd_seq_execute_dma (&Ctx->NandObj, &CmdBuf, 0);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: cmd_seq_execute_dma failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  //
  // DMA read from NFC data buffer (with interrupt for completion)
  //
  DataBuf.addr = (al_phys_addr_t)Ctx->DmaBufPhys;
  DataBuf.len  = Ctx->PageSize;

  Err = al_nand_data_buff_read_dma (&Ctx->NandObj, &DataBuf, 1);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: data_buff_read_dma failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  //
  // Wait for both completions (cmd_seq + data_read)
  //
  Status = AlNandWaitDmaCompletion (&Ctx->NandObj, 2);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Copy from DMA bounce buffer to caller's buffer
  //
  CopyMem (Buffer, Ctx->DmaBufBase, Ctx->PageSize);

  return EFI_SUCCESS;
}

/**
  Read a single NAND page via PIO (fallback path).
**/
STATIC
EFI_STATUS
AlNandReadPagePio (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *Buffer
  )
{
  uint32_t    CmdSeqBuf[AL_NAND_MAX_CMD_SEQ_ENTRIES];
  int         NumEntries;
  uint32_t    CwSize;
  uint32_t    CwCount;
  int         Err;

  NumEntries = AL_NAND_MAX_CMD_SEQ_ENTRIES;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj,
          0, (int)Page, (int)Ctx->PageSize,
          0, CmdSeqBuf, &NumEntries, &CwSize, &CwCount
          );
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);
  al_nand_cmd_seq_execute (&Ctx->NandObj, CmdSeqBuf, NumEntries);

  //
  // Read data via PIO (blocking HAL call)
  //
  Err = al_nand_data_buff_read (
          &Ctx->NandObj,
          (int)Ctx->PageSize,
          0, 0,  // no skip
          Buffer
          );
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: PIO page %u read failed: %d\n", Page, Err));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read a single NAND page. Uses DMA if available, otherwise PIO.
**/
STATIC
EFI_STATUS
AlNandReadPage (
  IN  AL_NAND_CONTEXT  *Ctx,
  IN  UINT32           Page,
  OUT UINT8            *Buffer
  )
{
  if (Ctx->DmaAvailable) {
    return AlNandReadPageDma (Ctx, Page, Buffer);
  }
  return AlNandReadPagePio (Ctx, Page, Buffer);
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
  uint32_t    CmdSeqBuf[AL_NAND_MAX_CMD_SEQ_ENTRIES];
  int         NumEntries;
  uint32_t    CwSize;
  uint32_t    CwCount;
  UINT8       Tmp[4];
  int         Err;

  //
  // Read 4 bytes from column = PageSize (start of OOB)
  //
  NumEntries = AL_NAND_MAX_CMD_SEQ_ENTRIES;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj,
          (int)Ctx->PageSize,   // column = OOB start
          (int)Page,
          4,                    // read 4 bytes
          0,                    // no ECC
          CmdSeqBuf,
          &NumEntries,
          &CwSize,
          &CwCount
          );
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);
  al_nand_cmd_seq_execute (&Ctx->NandObj, CmdSeqBuf, NumEntries);

  Err = al_nand_data_buff_read (&Ctx->NandObj, 4, 0, 0, Tmp);
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
  Uses column-addressed read for efficiency.
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
  uint32_t         CmdSeqBuf[AL_NAND_MAX_CMD_SEQ_ENTRIES];
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

  //
  // Column address = PageSize - 16 (start of inline tags)
  //
  NumEntries = AL_NAND_MAX_CMD_SEQ_ENTRIES;
  Err = al_nand_cmd_seq_gen_page_read (
          &Ctx->NandObj,
          (int)(Ctx->PageSize - 16),  // column
          (int)PageIndex,              // row
          16,                          // 16 bytes
          0,                           // no ECC
          CmdSeqBuf,
          &NumEntries,
          &CwSize,
          &CwCount
          );
  if (Err != 0) {
    return EFI_DEVICE_ERROR;
  }

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);
  al_nand_cmd_seq_execute (&Ctx->NandObj, CmdSeqBuf, NumEntries);

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
  uint32_t         CmdSeqBuf[16];
  int              Idx;
  uint32_t         CwSize;
  uint32_t         CwCount;
  uint32_t         CwIdx;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = AL_NAND_FROM_NANDFLASH (This);

  CwSize  = AL_NAND_CW_SIZE;
  CwCount = Ctx->PageSize / CwSize;

  al_nand_cw_config (&Ctx->NandObj, CwSize, CwCount);

  //
  // Build Change Read Column command sequence:
  // CMD(0x05), COL(0x00), COL(0x00), CMD(0xE0), then DATA_READ_COUNT * CwCount
  //
  Idx = 0;
  CmdSeqBuf[Idx++] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0x05);
  CmdSeqBuf[Idx++] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_ADDRESS, 0);
  CmdSeqBuf[Idx++] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_ADDRESS, 0);
  CmdSeqBuf[Idx++] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0xE0);

  for (CwIdx = 0; CwIdx < CwCount; CwIdx++) {
    CmdSeqBuf[Idx++] = AL_NAND_CMD_SEQ_ENTRY (
                          AL_NAND_COMMAND_TYPE_DATA_READ_COUNT,
                          CwSize & 0xFF);
    CmdSeqBuf[Idx++] = AL_NAND_CMD_SEQ_ENTRY (
                          AL_NAND_COMMAND_TYPE_DATA_READ_COUNT,
                          (CwSize >> 8) & 0xFF);
  }

  //
  // Read data - use DMA if available, PIO otherwise
  //
  if (Ctx->DmaAvailable) {
    struct al_buf  CmdBuf;
    struct al_buf  DataBuf;
    EFI_STATUS     Status;

    //
    // Copy command sequence to DMA buffer and execute via DMA
    //
    CopyMem (Ctx->CmdSeqBase, CmdSeqBuf, Idx * sizeof(uint32_t));

    CmdBuf.addr = (al_phys_addr_t)Ctx->CmdSeqPhys;
    CmdBuf.len  = (uint32_t)(Idx * sizeof(uint32_t));

    if (al_nand_cmd_seq_execute_dma (&Ctx->NandObj, &CmdBuf, 0) != 0) {
      return EFI_DEVICE_ERROR;
    }

    DataBuf.addr = (al_phys_addr_t)Ctx->DmaBufPhys;
    DataBuf.len  = Ctx->PageSize;

    if (al_nand_data_buff_read_dma (&Ctx->NandObj, &DataBuf, 1) != 0) {
      return EFI_DEVICE_ERROR;
    }

    Status = AlNandWaitDmaCompletion (&Ctx->NandObj, 2);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    CopyMem (Buffer, Ctx->DmaBufBase, Ctx->PageSize);
  } else {
    al_nand_cmd_seq_execute (&Ctx->NandObj, CmdSeqBuf, Idx);
    int Err = al_nand_data_buff_read (&Ctx->NandObj, (int)Ctx->PageSize,
                                       0, 0, Buffer);
    if (Err != 0) {
      return EFI_DEVICE_ERROR;
    }
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
  AL_NAND_CONTEXT  *Ctx;

  Ctx = AL_NAND_FROM_BLOCKIO (This);

  //
  // Send NAND RESET command via HAL
  //
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0xFF));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_WAIT_FOR_READY, 0));

  return AlNandPollIntStatus (&Ctx->NandObj, AL_NAND_INTR_STATUS_CMD_BUF_EMPTY);
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
  UINT8                           IdBuf[5];
  int                             Err;
  struct al_nand_dev_properties   DevProps;
  struct al_nand_ecc_config       EccCfg;

  //
  // Get CPU Arch protocol for setting memory attributes
  //
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: CPU Arch protocol not found, DMA may not work\n"));
    mCpu = NULL;
  }

  //
  // Allocate context
  //
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
  Ctx->DmaAvailable  = FALSE;

  //
  // Try to initialize DMA
  //
  Status = AlNandDmaInit (Ctx);
  if (!EFI_ERROR (Status)) {
    Ctx->DmaAvailable = TRUE;
    DEBUG ((DEBUG_INFO, "AlNandDxe: DMA acceleration enabled\n"));
  } else {
    DEBUG ((DEBUG_WARN, "AlNandDxe: DMA init failed, using PIO fallback\n"));
  }

  //
  // Initialize HAL NAND controller
  //
  Err = al_nand_init (
          &Ctx->NandObj,
          (void *)(UINTN)AL_NAND_BASE,
          Ctx->DmaAvailable ? &Ctx->SsmDma : NULL,
          0  // queue ID
          );
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: al_nand_init failed: %d\n", Err));
    FreePool (Ctx);
    return EFI_DEVICE_ERROR;
  }

  //
  // Select device 0
  //
  al_nand_dev_select (&Ctx->NandObj, 0);

  //
  // Configure device properties for Toshiba BENAND
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
  DevProps.readyBusyTimeout  = 0xFFFF;
  DevProps.num_col_cyc       = BENAND_COL_CYCLES;
  DevProps.num_row_cyc       = BENAND_ROW_CYCLES;
  DevProps.pageSize          = AL_NAND_DEVICE_PAGE_SIZE_2K;

  ZeroMem (&EccCfg, sizeof (EccCfg));
  EccCfg.algorithm       = AL_NAND_ECC_ALGORITHM_BCH;
  EccCfg.num_corr_bits   = AL_NAND_ECC_BCH_NUM_CORR_BITS_8;
  EccCfg.messageSize     = AL_NAND_ECC_BCH_MESSAGE_SIZE_512;
  EccCfg.spareAreaOffset = 0;

  Err = al_nand_dev_config (&Ctx->NandObj, &DevProps, &EccCfg);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: al_nand_dev_config failed: %d\n", Err));
    FreePool (Ctx);
    return EFI_DEVICE_ERROR;
  }

  //
  // Disable ECC (BENAND has built-in ECC)
  //
  al_nand_ecc_set_enabled (&Ctx->NandObj, 0);

  //
  // Disable all NFC interrupts (we poll status)
  //
  al_nand_int_disable (&Ctx->NandObj, 0xFFFFFFFF);

  //
  // Reset NAND device
  //
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0xFF));
  al_nand_cmd_single_execute (&Ctx->NandObj,
    AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_WAIT_FOR_READY, 0));

  Status = AlNandPollIntStatus (&Ctx->NandObj, AL_NAND_INTR_STATUS_CMD_BUF_EMPTY);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlNandDxe: Device reset failed: %r\n", Status));
    FreePool (Ctx);
    return Status;
  }

  //
  // Read device ID (8 bytes, use all for diagnostics)
  //
  {
    UINT8    RawId[8];
    uint32_t CmdSeq[4];
    CmdSeq[0] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_CMD, 0x90);
    CmdSeq[1] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_ADDRESS, 0x00);
    CmdSeq[2] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_DATA_READ_COUNT, 8);
    CmdSeq[3] = AL_NAND_CMD_SEQ_ENTRY (AL_NAND_COMMAND_TYPE_DATA_READ_COUNT, 0);

    al_nand_cw_config (&Ctx->NandObj, 8, 1);
    al_nand_cmd_seq_execute (&Ctx->NandObj, CmdSeq, 4);

    Err = al_nand_data_buff_read (&Ctx->NandObj, 8, 0, 0, RawId);
    if (Err != 0) {
      DEBUG ((DEBUG_ERROR, "AlNandDxe: ReadID failed\n"));
      FreePool (Ctx);
      return EFI_DEVICE_ERROR;
    }

    CopyMem (IdBuf, RawId, 5);

    DEBUG ((DEBUG_WARN, "AlNandDxe: NAND ID: %02x %02x %02x %02x %02x %02x %02x %02x\n",
      RawId[0], RawId[1], RawId[2], RawId[3], RawId[4], RawId[5], RawId[6], RawId[7]));
  }

  if (IdBuf[0] != 0x98) {
    DEBUG ((DEBUG_WARN, "AlNandDxe: Non-Toshiba NAND (mfr=0x%02x)\n", IdBuf[0]));
  }

  if ((IdBuf[4] & TOSHIBA_NAND_ID4_IS_BENAND) != 0) {
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

  DEBUG ((DEBUG_INFO, "AlNandDxe: NAND at 0x%lx, %u pages, %u bytes/page, %a mode\n",
    (UINT64)AL_NAND_BASE, Ctx->NumPages, Ctx->PageSize,
    Ctx->DmaAvailable ? "DMA" : "PIO"));

  return EFI_SUCCESS;
}
