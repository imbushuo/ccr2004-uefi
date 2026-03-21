/** @file
  Annapurna Labs NAND controller DXE driver header for MikroTik CCR2004.

  Supports Toshiba BENAND (built-in ECC) flash. Read-only block device.
  Uses AL HAL with SSM RAID DMA for accelerated page reads.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef AL_NAND_DXE_H_
#define AL_NAND_DXE_H_

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Cpu.h>
#include <Protocol/DevicePath.h>
#include <Protocol/MikroTikNandFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "al_hal_nand.h"
#include "al_hal_ssm.h"
#include "al_hal_ssm_raid.h"

//
// Alpine V2 SoC memory map addresses
//
#define AL_NAND_BASE                  0xFA100000

// SSM RAID DMA engine (for NAND DMA acceleration)
// AL_SB_BASE=0xFC000000, AL_SSM_DEV_NUM(1)=5, RAID unit = SB_BASE + 5*0x100000
#define AL_SSM_RAID_UDMA_BASE         0xFC520000  // AL_SSM_UDMA_BASE(1,0)
#define AL_SSM_RAID_APP_BASE          0xFC505000  // AL_SSM_BASE(1) + 0x5000

// SSM RAID adapter base (AL_SSM_BASE(1) = SB_BASE + 5*0x100000)
// PCI config space is at the start of this region
#define AL_SSM_RAID_ADAPTER_BASE      0xFC500000

//
// Toshiba BENAND 1Gbit (128MB) parameters
//
#define BENAND_PAGE_SIZE              2048
#define BENAND_OOB_SIZE               64
#define BENAND_PAGES_PER_BLOCK        64
#define BENAND_NUM_BLOCKS             1024
#define BENAND_NUM_PAGES              (BENAND_NUM_BLOCKS * BENAND_PAGES_PER_BLOCK)
#define BENAND_COL_CYCLES             2
#define BENAND_ROW_CYCLES             2

//
// Toshiba BENAND: ID byte 4 bit 7 indicates BENAND
//
#define TOSHIBA_NAND_ID4_IS_BENAND    BIT7

//
// ONFI Mode 4 timing at 500 MHz sbclk (in controller cycles)
//
#define AL_NAND_TIM_SETUP       6
#define AL_NAND_TIM_HOLD        3
#define AL_NAND_TIM_WH          5
#define AL_NAND_TIM_WRP         6
#define AL_NAND_TIM_INTCMD     10
#define AL_NAND_TIM_RR         10
#define AL_NAND_TIM_WB         50
#define AL_NAND_TIM_READ_DLY    3

//
// Polling
//
#define AL_NAND_POLL_TIMEOUT_US       1000000

//
// DMA descriptor ring configuration
//
#define AL_NAND_DMA_DESCS_PER_Q       32
#define AL_NAND_DMA_DESC_SIZE         16   // sizeof(union al_udma_desc)
#define AL_NAND_DMA_RING_SIZE         (AL_NAND_DMA_DESCS_PER_Q * AL_NAND_DMA_DESC_SIZE)
#define AL_NAND_DMA_TOTAL_RING_SIZE   (4 * AL_NAND_DMA_RING_SIZE)  // tx_sub, tx_comp, rx_sub, rx_comp

//
// Maximum command sequence entries for a page read
//
#define AL_NAND_MAX_CMD_SEQ_ENTRIES    64

//
// Bad block map size (1 bit per block)
//
#define AL_NAND_BB_MAP_SIZE           (BENAND_NUM_BLOCKS / 8)

//
// Codeword size for reads
//
#define AL_NAND_CW_SIZE               512

//
// Context structure
//
#define AL_NAND_SIGNATURE  SIGNATURE_32 ('A', 'L', 'N', 'D')

typedef struct {
  UINT32                    Signature;
  EFI_HANDLE                Handle;

  //
  // HAL NAND controller object
  //
  struct al_nand_ctrl_obj   NandObj;

  //
  // SSM RAID DMA engine
  //
  struct al_ssm_dma         SsmDma;
  BOOLEAN                   DmaAvailable;

  //
  // DMA memory (UC, below 4GB)
  //
  VOID                      *DescRingBase;      // Descriptor rings virtual
  EFI_PHYSICAL_ADDRESS      DescRingPhys;       // Descriptor rings physical
  VOID                      *DmaBufBase;        // DMA page buffer virtual
  EFI_PHYSICAL_ADDRESS      DmaBufPhys;         // DMA page buffer physical
  VOID                      *CmdSeqBase;        // DMA cmd sequence buffer virtual
  EFI_PHYSICAL_ADDRESS      CmdSeqPhys;         // DMA cmd sequence buffer physical

  //
  // Device parameters
  //
  UINT32                    PageSize;
  UINT32                    OobSize;
  UINT32                    PagesPerBlock;
  UINT32                    NumBlocks;
  UINT32                    NumPages;
  UINT8                     BadBlockMap[AL_NAND_BB_MAP_SIZE];

  //
  // Protocols
  //
  EFI_BLOCK_IO_PROTOCOL           BlockIo;
  EFI_BLOCK_IO_MEDIA              Media;
  MIKROTIK_NAND_FLASH_PROTOCOL    NandFlash;
} AL_NAND_CONTEXT;

#define AL_NAND_FROM_BLOCKIO(a)    CR (a, AL_NAND_CONTEXT, BlockIo, AL_NAND_SIGNATURE)
#define AL_NAND_FROM_NANDFLASH(a)  CR (a, AL_NAND_CONTEXT, NandFlash, AL_NAND_SIGNATURE)

//
// Device path
//
typedef struct {
  VENDOR_DEVICE_PATH          Guid;
  EFI_DEVICE_PATH_PROTOCOL    End;
} AL_NAND_DEVICE_PATH;

#endif // AL_NAND_DXE_H_
