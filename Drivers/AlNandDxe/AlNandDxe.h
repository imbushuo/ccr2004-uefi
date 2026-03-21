/** @file
  Annapurna Labs NAND controller DXE driver header for MikroTik CCR2004.

  Supports Toshiba BENAND (built-in ECC) flash. Read-only block device.
  Uses AL HAL for NAND controller access.

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

#include "al_hal_nand.h"

//
// Alpine V2 SoC memory map
//
#define AL_NAND_BASE                  0xFA100000

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
// Toshiba BENAND: ID byte 5 bit 7 indicates BENAND
//
#define TOSHIBA_NAND_ID5_IS_BENAND    BIT7

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
