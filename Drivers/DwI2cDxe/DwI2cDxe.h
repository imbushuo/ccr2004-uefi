/** @file
  DesignWare I2C controller DXE driver header for MikroTik CCR2004.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_I2C_DXE_H_
#define DW_I2C_DXE_H_

#include <Uefi.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/I2cEnumerate.h>
#include <Protocol/I2cBusConfigurationManagement.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// DesignWare I2C register offsets
//
#define DW_IC_CON                0x00
#define DW_IC_TAR                0x04
#define DW_IC_SAR                0x08
#define DW_IC_DATA_CMD           0x10
#define DW_IC_SS_SCL_HCNT       0x14
#define DW_IC_SS_SCL_LCNT       0x18
#define DW_IC_FS_SCL_HCNT       0x1C
#define DW_IC_FS_SCL_LCNT       0x20
#define DW_IC_HS_SCL_HCNT       0x24
#define DW_IC_HS_SCL_LCNT       0x28
#define DW_IC_INTR_STAT          0x2C
#define DW_IC_INTR_MASK          0x30
#define DW_IC_RAW_INTR_STAT     0x34
#define DW_IC_RX_TL             0x38
#define DW_IC_TX_TL             0x3C
#define DW_IC_CLR_INTR          0x40
#define DW_IC_CLR_RX_UNDER      0x44
#define DW_IC_CLR_RX_OVER       0x48
#define DW_IC_CLR_TX_OVER       0x4C
#define DW_IC_CLR_RD_REQ        0x50
#define DW_IC_CLR_TX_ABRT       0x54
#define DW_IC_CLR_RX_DONE       0x58
#define DW_IC_CLR_ACTIVITY      0x5C
#define DW_IC_CLR_STOP_DET      0x60
#define DW_IC_CLR_START_DET     0x64
#define DW_IC_CLR_GEN_CALL      0x68
#define DW_IC_ENABLE             0x6C
#define DW_IC_STATUS             0x70
#define DW_IC_TXFLR              0x74
#define DW_IC_RXFLR              0x78
#define DW_IC_SDA_HOLD           0x7C
#define DW_IC_TX_ABRT_SOURCE    0x80
#define DW_IC_ENABLE_STATUS     0x9C
#define DW_IC_COMP_PARAM_1      0xF4
#define DW_IC_COMP_VERSION      0xF8
#define DW_IC_COMP_TYPE          0xFC

//
// DW_IC_CON bits
//
#define DW_IC_CON_MASTER         BIT0
#define DW_IC_CON_SPEED_STD      BIT1
#define DW_IC_CON_SPEED_FAST     BIT2
#define DW_IC_CON_SPEED_MASK     (BIT2 | BIT1)
#define DW_IC_CON_10BITADDR_MASTER  BIT4
#define DW_IC_CON_RESTART_EN     BIT5
#define DW_IC_CON_SLAVE_DISABLE  BIT6

//
// DW_IC_DATA_CMD bits
//
#define DW_IC_DATA_CMD_DAT_MASK  0xFF
#define DW_IC_DATA_CMD_CMD       BIT8   // 1 = read, 0 = write
#define DW_IC_DATA_CMD_STOP      BIT9
#define DW_IC_DATA_CMD_RESTART   BIT10

//
// DW_IC_RAW_INTR_STAT / DW_IC_INTR_MASK bits
//
#define DW_IC_INTR_RX_UNDER     BIT0
#define DW_IC_INTR_RX_OVER      BIT1
#define DW_IC_INTR_RX_FULL      BIT2
#define DW_IC_INTR_TX_OVER      BIT3
#define DW_IC_INTR_TX_EMPTY     BIT4
#define DW_IC_INTR_RD_REQ       BIT5
#define DW_IC_INTR_TX_ABRT      BIT6
#define DW_IC_INTR_RX_DONE      BIT7
#define DW_IC_INTR_ACTIVITY     BIT8
#define DW_IC_INTR_STOP_DET     BIT9
#define DW_IC_INTR_START_DET    BIT10
#define DW_IC_INTR_GEN_CALL     BIT11

#define DW_IC_ERR_CONDITION      (DW_IC_INTR_RX_UNDER | DW_IC_INTR_RX_OVER | DW_IC_INTR_TX_ABRT)

//
// DW_IC_STATUS bits
//
#define DW_IC_STATUS_ACTIVITY    BIT0
#define DW_IC_STATUS_TFNF        BIT1   // TX FIFO not full
#define DW_IC_STATUS_TFE         BIT2   // TX FIFO empty
#define DW_IC_STATUS_RFNE        BIT3   // RX FIFO not empty
#define DW_IC_STATUS_RFF         BIT4   // RX FIFO full
#define DW_IC_STATUS_MST_ACTIVITY  BIT5

//
// COMP_PARAM_1 field extraction
//
#define DW_IC_COMP_PARAM_1_RX_BUFFER_DEPTH(x)  ((((x) >> 8) & 0xFF) + 1)
#define DW_IC_COMP_PARAM_1_TX_BUFFER_DEPTH(x)  ((((x) >> 16) & 0xFF) + 1)

//
// SDA hold minimum version
//
#define DW_IC_SDA_HOLD_MIN_VERS  0x3131312A

//
// DW_IC_COMP_TYPE expected value
//
#define DW_IC_COMP_TYPE_VALUE    0x44570140

//
// Polling constants
//
#define DW_MAX_TRANSFER_POLL_COUNT   100000
#define DW_MAX_STATUS_POLL_COUNT     100
#define DW_POLL_MST_ACTIVITY_INTERVAL_US  1000
#define DW_MAX_MST_ACTIVITY_POLL_COUNT    20

//
// Compute polling interval: 10x the signaling period for the bus speed
//
#define DW_POLL_INTERVAL_US(Speed)  (10 * (1000000 / (Speed)))

//
// Context structure
//
#define DW_I2C_SIGNATURE  SIGNATURE_32 ('D', 'W', 'I', 'C')

typedef struct {
  UINT32                                           Signature;
  EFI_HANDLE                                       Handle;
  EFI_LOCK                                         Lock;
  UINTN                                            BaseAddress;
  UINT32                                           BusIndex;
  UINT32                                           BusSpeedHz;
  UINT32                                           TxFifo;
  UINT32                                           RxFifo;
  UINT32                                           PollingTimeUs;
  EFI_I2C_MASTER_PROTOCOL                          I2cMaster;
  EFI_I2C_ENUMERATE_PROTOCOL                       I2cEnumerate;
  EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL    I2cBusConf;
  EFI_I2C_CONTROLLER_CAPABILITIES                  Capabilities;
} DW_I2C_CONTEXT;

#define DW_I2C_FROM_MASTER(a)     CR (a, DW_I2C_CONTEXT, I2cMaster, DW_I2C_SIGNATURE)
#define DW_I2C_FROM_ENUMERATE(a)  CR (a, DW_I2C_CONTEXT, I2cEnumerate, DW_I2C_SIGNATURE)
#define DW_I2C_FROM_BUSCONF(a)    CR (a, DW_I2C_CONTEXT, I2cBusConf, DW_I2C_SIGNATURE)

//
// Device path
//
typedef struct {
  VENDOR_DEVICE_PATH          Guid;
  UINT32                      Instance;
  EFI_DEVICE_PATH_PROTOCOL    End;
} DW_I2C_DEVICE_PATH;

//
// Controller table: base addresses and bus speeds
//
#define DW_I2C_BUS_COUNT  2

//
// Alpine V2 SCL timing (sbclk = 500 MHz, from DTS)
//   SS: HCNT = 0x855, LCNT = 0xB0B (100 kHz)
//   FS: HCNT = 0x19D, LCNT = 0x320 (400 kHz)
//   SDA hold = 300ns = 150 ticks at 500 MHz
//
#define DW_I2C_SS_SCL_HCNT_VAL   0x855
#define DW_I2C_SS_SCL_LCNT_VAL   0xB0B
#define DW_I2C_FS_SCL_HCNT_VAL   0x19D
#define DW_I2C_FS_SCL_LCNT_VAL   0x320
#define DW_I2C_SDA_HOLD_VAL      150

#endif // DW_I2C_DXE_H_
