/** @file
  DesignWare SPI (SSI) controller DXE driver header for MikroTik CCR2004.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_SPI_DXE_H_
#define DW_SPI_DXE_H_

#include <Uefi.h>
#include <Protocol/SpiHc.h>
#include <Protocol/SpiConfiguration.h>
#include <Protocol/SpiIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// DesignWare SSI register offsets (from Linux spi-dw.h)
//
#define DW_SPI_CTRL0     0x00
#define DW_SPI_CTRL1     0x04
#define DW_SPI_SSIENR    0x08
#define DW_SPI_SER       0x10
#define DW_SPI_BAUDR     0x14
#define DW_SPI_TXFLTR    0x18
#define DW_SPI_RXFLTR    0x1C
#define DW_SPI_TXFLR     0x20
#define DW_SPI_RXFLR     0x24
#define DW_SPI_SR        0x28
#define DW_SPI_IMR       0x2C
#define DW_SPI_ISR       0x30
#define DW_SPI_RISR      0x34
#define DW_SPI_TXOICR    0x38
#define DW_SPI_RXOICR    0x3C
#define DW_SPI_RXUICR    0x40
#define DW_SPI_MSTICR    0x44
#define DW_SPI_ICR       0x48
#define DW_SPI_IDR       0x58
#define DW_SPI_VERSION   0x5C
#define DW_SPI_DR        0x60

//
// CTRL0 bit fields
//
#define DW_SPI_CTRL0_DFS_MASK    0x0F        // [3:0] Data frame size
#define DW_SPI_CTRL0_FRF_SHIFT   4           // [5:4] Frame format
#define DW_SPI_CTRL0_SCPH        BIT6        // Clock phase
#define DW_SPI_CTRL0_SCOL        BIT7        // Clock polarity
#define DW_SPI_CTRL0_TMOD_SHIFT  8           // [9:8] Transfer mode

//
// Transfer modes (TMOD field values)
//
#define DW_SPI_TMOD_TR           0           // Transmit & Receive
#define DW_SPI_TMOD_TO           1           // Transmit Only
#define DW_SPI_TMOD_RO           2           // Receive Only
#define DW_SPI_TMOD_EPROMREAD    3           // EEPROM Read

//
// SR (Status Register) bits
//
#define DW_SPI_SR_BUSY   BIT0
#define DW_SPI_SR_TFNF   BIT1    // TX FIFO not full
#define DW_SPI_SR_TFE    BIT2    // TX FIFO empty
#define DW_SPI_SR_RFNE   BIT3    // RX FIFO not empty
#define DW_SPI_SR_RFF    BIT4    // RX FIFO full

//
// Platform constants (Alpine V2 SoC)
//
#define DW_SPI_BASE           0xFD882000
#define DW_SPI_REF_CLOCK_HZ  500000000    // 500 MHz sbclk
#define DW_SPI_NUM_CS         4

//
// Polling constants
//
#define DW_SPI_POLL_TIMEOUT_US     1000000   // 1 second
#define DW_SPI_POLL_INTERVAL_US    10

//
// Context structure
//
#define DW_SPI_SIGNATURE  SIGNATURE_32 ('D', 'W', 'S', 'P')

typedef struct {
  UINT32                    Signature;
  EFI_HANDLE                Handle;
  EFI_LOCK                  Lock;
  UINTN                     BaseAddress;
  UINT32                    FifoLen;
  UINT32                    ActiveCs;       // Currently selected CS (0-3), or 0xFF=none
  EFI_SPI_HC_PROTOCOL       HcProtocol;
} DW_SPI_CONTEXT;

#define DW_SPI_FROM_HC(a)  CR (a, DW_SPI_CONTEXT, HcProtocol, DW_SPI_SIGNATURE)

//
// Device path
//
typedef struct {
  VENDOR_DEVICE_PATH          Guid;
  EFI_DEVICE_PATH_PROTOCOL    End;
} DW_SPI_DEVICE_PATH;

#endif // DW_SPI_DXE_H_
