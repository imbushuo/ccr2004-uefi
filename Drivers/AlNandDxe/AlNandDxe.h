/** @file
  Annapurna Labs NAND controller DXE driver header for MikroTik CCR2004.

  Supports Toshiba BENAND (built-in ECC) flash. Read-only block device.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef AL_NAND_DXE_H_
#define AL_NAND_DXE_H_

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/MikroTikNandFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// Memory map offsets from NAND base (0xFA100000)
//
#define AL_NAND_DATA_BUFF_OFFSET    0x000000
#define AL_NAND_CMD_BUFF_OFFSET     0x100000
#define AL_NAND_WRAP_BASE_OFFSET    0x200000
#define AL_NAND_CTRL_BASE_OFFSET    0x201000

//
// Control register offsets (from CtrlBase)
//
#define AL_NAND_FLASH_CTL_3         0x0044
#define AL_NAND_MODE_SELECT_REG     0x0400
#define AL_NAND_CTL_REG0            0x0404
#define AL_NAND_BCH_CTRL_REG_0      0x0408
#define AL_NAND_BCH_CTRL_REG_1      0x040C
#define AL_NAND_CW_SIZE_CNT_REG     0x041C
#define AL_NAND_SPARE_OFFSET_REG    0x0428
#define AL_NAND_SDR_TIM_PARAMS_0    0x042C
#define AL_NAND_SDR_TIM_PARAMS_1    0x0430
#define AL_NAND_RDY_BUSY_WAIT_CNT   0x0444
#define AL_NAND_NFC_INT_EN          0x0448
#define AL_NAND_RDY_BUSY_STATUS     0x044C
#define AL_NAND_NFC_INT_STAT        0x0450
#define AL_NAND_RESET_REG           0x0460
#define AL_NAND_RESET_STATUS_REG    0x048C

//
// Wrapper register offsets (from WrapBase)
//
#define AL_NAND_WRAP_CW_SIZE        0x0004

//
// flash_ctl_3: set all 6 banks to NAND type (value 1, 2 bits per bank)
//
#define AL_NAND_FLASH_CTL_3_ALL_NAND  0x555

//
// mode_select_reg fields
//
#define AL_NAND_MODE_SELECT_SDR       0
#define AL_NAND_MODE_SDR_TIM_SHIFT    3
#define AL_NAND_MODE_SDR_TIM_MASK     (0xF << AL_NAND_MODE_SDR_TIM_SHIFT)

//
// ctl_reg0 fields
//
#define AL_NAND_CTL_REG0_CS_MASK      0x07
#define AL_NAND_CTL_REG0_CS_SHIFT     0
#define AL_NAND_CTL_REG0_DQ_WIDTH     BIT4
#define AL_NAND_CTL_REG0_COL_SHIFT    5
#define AL_NAND_CTL_REG0_COL_MASK     (0xF << AL_NAND_CTL_REG0_COL_SHIFT)
#define AL_NAND_CTL_REG0_ROW_SHIFT    9
#define AL_NAND_CTL_REG0_ROW_MASK     (0xF << AL_NAND_CTL_REG0_ROW_SHIFT)
#define AL_NAND_CTL_REG0_PAGE_SHIFT   13
#define AL_NAND_CTL_REG0_PAGE_MASK    (0x7 << AL_NAND_CTL_REG0_PAGE_SHIFT)
#define AL_NAND_CTL_REG0_TX_MODE      BIT16
#define AL_NAND_CTL_REG0_CS2_SHIFT    19
#define AL_NAND_CTL_REG0_CS2_MASK     (0x7 << AL_NAND_CTL_REG0_CS2_SHIFT)

//
// Page size enum values (for ctl_reg0 PAGE_SIZE field)
//
#define AL_NAND_PAGE_SIZE_2K          0
#define AL_NAND_PAGE_SIZE_4K          1
#define AL_NAND_PAGE_SIZE_8K          2
#define AL_NAND_PAGE_SIZE_16K         3
#define AL_NAND_PAGE_SIZE_512         4

//
// bch_ctrl_reg_0 fields
//
#define AL_NAND_BCH_ECC_ON_OFF        BIT0

//
// codeword_size_cnt_reg fields
//
#define AL_NAND_CW_SIZE_SHIFT         0
#define AL_NAND_CW_SIZE_MASK          0xFFFF
#define AL_NAND_CW_COUNT_SHIFT        16
#define AL_NAND_CW_COUNT_MASK         (0xFFFF << AL_NAND_CW_COUNT_SHIFT)

//
// sdr_timing_params_0 fields
//
#define AL_NAND_SDR_T0_SETUP_SHIFT    0
#define AL_NAND_SDR_T0_HOLD_SHIFT     6
#define AL_NAND_SDR_T0_WH_SHIFT       12
#define AL_NAND_SDR_T0_WRP_SHIFT      18
#define AL_NAND_SDR_T0_INTCMD_SHIFT   24

//
// sdr_timing_params_1 fields
//
#define AL_NAND_SDR_T1_RR_SHIFT       0
#define AL_NAND_SDR_T1_WB_SHIFT       6
#define AL_NAND_SDR_T1_READ_DLY_SHIFT 12
#define AL_NAND_SDR_T1_WB_MSB_SHIFT   14

//
// rdy_busy_wait_cnt_reg fields
//
#define AL_NAND_RDY_TOUT_SHIFT        0
#define AL_NAND_RDY_TOUT_MASK         0xFFFF
#define AL_NAND_RDYBSYEN              BIT16

//
// NFC interrupt status bits (nfc_int_stat, write-1-to-clear)
//
#define AL_NAND_INT_CMD_BUF_EMPTY     BIT0
#define AL_NAND_INT_CMD_BUF_FULL      BIT1
#define AL_NAND_INT_BUF_RDRDY         BIT7
#define AL_NAND_INT_WRRD_DONE         BIT8

//
// Reset masks
//
#define AL_NAND_RESET_SOFT            BIT0
#define AL_NAND_RESET_CMD_FIFO        BIT1
#define AL_NAND_RESET_DATA_FIFO       BIT2

//
// NAND command types (for command buffer entries)
//
#define AL_NAND_CMD_TYPE_NOP          0
#define AL_NAND_CMD_TYPE_CMD          2
#define AL_NAND_CMD_TYPE_ADDRESS      3
#define AL_NAND_CMD_TYPE_WAIT_CYCLE   4
#define AL_NAND_CMD_TYPE_WAIT_READY   5
#define AL_NAND_CMD_TYPE_DATA_READ    6
#define AL_NAND_CMD_TYPE_STATUS_READ  8

//
// Command buffer entry macro
//
#define AL_NAND_CMD_ENTRY(type, arg)  (((type) << 8) | ((arg) & 0xFF))

//
// Standard NAND commands
//
#define NAND_CMD_READ0                0x00
#define NAND_CMD_READ1                0x30
#define NAND_CMD_READID               0x90
#define NAND_CMD_RESET                0xFF
#define NAND_CMD_STATUS               0x70

//
// Toshiba BENAND: ID byte 4 bit 7 indicates BENAND
//
#define TOSHIBA_NAND_ID4_IS_BENAND    BIT7

//
// Platform constants
//
#define AL_NAND_BASE                  0xFA100000

//
// ONFI Mode 0 timing at 500 MHz sbclk (in controller cycles)
// Computed as: ceil(ns * 500MHz / 1e9)
//
#define AL_NAND_TIM_SETUP             7     // 14 ns
#define AL_NAND_TIM_HOLD              11    // 22 ns
#define AL_NAND_TIM_WH                16    // 32 ns
#define AL_NAND_TIM_WRP               27    // 54 ns
#define AL_NAND_TIM_INTCMD            43    // 86 ns
#define AL_NAND_TIM_RR                22    // 43 ns (21.5, rounded up)
#define AL_NAND_TIM_WB                103   // 206 ns
#define AL_NAND_TIM_READ_DLY          3

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
// Polling
//
#define AL_NAND_POLL_TIMEOUT_US       1000000   // 1 second
#define AL_NAND_POLL_INTERVAL_US      10

//
// Codeword size for reads (matches Linux driver's ecc.size for BENAND)
//
#define AL_NAND_CW_SIZE               512

//
// Bad block map size (1 bit per block, max 1024 blocks)
//
#define AL_NAND_BB_MAP_SIZE           (BENAND_NUM_BLOCKS / 8)

//
// Context structure
//
#define AL_NAND_SIGNATURE  SIGNATURE_32 ('A', 'L', 'N', 'D')

typedef struct {
  UINT32                    Signature;
  EFI_HANDLE                Handle;
  UINTN                     NandBase;
  UINTN                     CtrlBase;
  UINTN                     WrapBase;
  UINTN                     CmdBuffBase;
  UINTN                     DataBuffBase;
  UINT32                    PageSize;
  UINT32                    OobSize;
  UINT32                    PagesPerBlock;
  UINT32                    NumBlocks;
  UINT32                    NumPages;
  UINT8                     BadBlockMap[AL_NAND_BB_MAP_SIZE];
  EFI_BLOCK_IO_PROTOCOL           BlockIo;
  EFI_BLOCK_IO_MEDIA              Media;
  MIKROTIK_NAND_FLASH_PROTOCOL    NandFlash;
} AL_NAND_CONTEXT;

#define AL_NAND_FROM_BLOCKIO(a)  CR (a, AL_NAND_CONTEXT, BlockIo, AL_NAND_SIGNATURE)

//
// Device path
//
typedef struct {
  VENDOR_DEVICE_PATH          Guid;
  EFI_DEVICE_PATH_PROTOCOL    End;
} AL_NAND_DEVICE_PATH;

#endif // AL_NAND_DXE_H_
