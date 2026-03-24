/** @file
  ArmPlatformLib implementation for MikroTik CCR2004.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/IoLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Guid/ArmMpCoreInfo.h>
#include <Ppi/ArmMpCoreInfo.h>

//
// CCR2004 has 4x Cortex-A72 cores in a single cluster.
// MPIDR Aff0 values: 0x0, 0x1, 0x2, 0x3.
//
STATIC ARM_CORE_INFO  mCoreInfoTable[] = {
  { 0x000 },  // Cluster 0, Core 0
  { 0x001 },  // Cluster 0, Core 1
  { 0x002 },  // Cluster 0, Core 2
  { 0x003 },  // Cluster 0, Core 3
};

STATIC
EFI_STATUS
EFIAPI
GetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **ArmCoreTable
  )
{
  *CoreCount    = ARRAY_SIZE (mCoreInfoTable);
  *ArmCoreTable = mCoreInfoTable;
  return EFI_SUCCESS;
}

STATIC ARM_MP_CORE_INFO_PPI  mMpCoreInfoPpi = {
  GetMpCoreInfo
};

STATIC EFI_PEI_PPI_DESCRIPTOR  mPlatformPpiTable[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI,
    &gArmMpCoreInfoPpiGuid,
    &mMpCoreInfoPpi
  }
};

// =====================================================================
// Alpine V2 PBS MUIO MUX configuration
// =====================================================================
//
// When booting directly from SPI flash (not chainloaded by MikroTik's
// bootloader), the pin mux registers are at reset defaults (all GPIO).
// Writing the correct values is harmless when already configured.
//
// PBS register file base: 0xFD8A8000
// Alpine V2: 4 bits per pin, 8 pins per 32-bit register
//   FUNC_0 = GPIO (reset default)
//   FUNC_1 = ETH / SGPO
//   FUNC_2 = UART / I2C
//   FUNC_3 = NAND / NOR
//   FUNC_4 = SGPO
//

#define AL_PBS_BASE    0xFD8A8000ULL
#define PBS_MUX_SEL_0  0x138  // pins  0- 7
#define PBS_MUX_SEL_1  0x13C  // pins  8-15
#define PBS_MUX_SEL_2  0x140  // pins 16-23
#define PBS_MUX_SEL_3  0x144  // pins 24-31
#define PBS_MUX_SEL_4  0x220  // pins 32-39
#define PBS_MUX_SEL_5  0x224  // pins 40-47
#define PBS_MUX_SEL_6  0x244  // pins 48-55

STATIC
VOID
AlpineMuioMuxInit (
  VOID
  )
{
  //
  // CCR2004 pin mux from DTS pinctrl-0 allocation:
  //   NAND_8 + NAND_CS_0       pins  6-19  FUNC_3
  //   UART_1                    pins 20-21  FUNC_2
  //   SGPO_DS_1                 pin  22     FUNC_4
  //   SGPO_CLK                  pins 24-25  FUNC_4
  //   UART_2                    pins 26-27  FUNC_2
  //   UART_3                    pins 28-29  FUNC_2
  //   I2C_GEN                   pins 30-31  FUNC_2
  //   ETH_GPIO (EC)             pins 32-43  FUNC_1
  //   RGMII_B                   pins 44-55  FUNC_1
  //
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_0, 0x33000000);
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_1, 0x33333333);
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_2, 0x04223333);
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_3, 0x22222244);
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_4, 0x11111111);
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_5, 0x11111111);
  MmioWrite32 (AL_PBS_BASE + PBS_MUX_SEL_6, 0x11111111);
}

// =====================================================================
// PCA9555 GPIO expander early init (raw MMIO on I2C_PLD)
// =====================================================================
//
// Replicates RouterBoot sequence: drive Port1 bits 2 and 5 low
// (board control lines), everything else input/high.
//
// Uses raw DesignWare I2C at 0xFD880000 (I2C_PLD, bus 0).
//

#define I2C_PLD_BASE          0xFD880000ULL
#define IC_CON                0x00
#define IC_TAR                0x04
#define IC_DATA_CMD           0x10
#define IC_SS_SCL_HCNT        0x14
#define IC_SS_SCL_LCNT        0x18
#define IC_INTR_MASK          0x30
#define IC_RAW_INTR_STAT      0x34
#define IC_CLR_INTR           0x40
#define IC_CLR_TX_ABRT        0x54
#define IC_ENABLE             0x6C
#define IC_STATUS             0x70
#define IC_ENABLE_STATUS      0x9C

#define IC_CON_MASTER         (1U << 0)
#define IC_CON_SPEED_STD      (1U << 1)
#define IC_CON_RESTART_EN     (1U << 5)
#define IC_CON_SLAVE_DISABLE  (1U << 6)
#define IC_DATA_CMD_STOP      (1U << 9)
#define IC_STATUS_TFNF        (1U << 1)
#define IC_STATUS_TFE         (1U << 2)
#define IC_INTR_TX_ABRT       (1U << 6)

#define PCA9555_ADDR          0x20
#define PCA9555_OUTPUT_0      0x02
#define PCA9555_OUTPUT_1      0x03
#define PCA9555_CONFIG_0      0x06
#define PCA9555_CONFIG_1      0x07

// 500 MHz sbclk, 100 kHz I2C → HCNT=LCNT=2500
#define SS_SCL_HCNT_VAL       2500
#define SS_SCL_LCNT_VAL       2500

#define I2C_TIMEOUT_US        100000
#define I2C_POLL_US           10

STATIC
BOOLEAN
I2cWaitTfnf (
  VOID
  )
{
  UINTN  E = 0;
  while (E < I2C_TIMEOUT_US) {
    if (MmioRead32 (I2C_PLD_BASE + IC_STATUS) & IC_STATUS_TFNF)
      return TRUE;
    MicroSecondDelay (I2C_POLL_US);
    E += I2C_POLL_US;
  }
  return FALSE;
}

STATIC
BOOLEAN
I2cWaitTfe (
  VOID
  )
{
  UINTN  E = 0;
  while (E < I2C_TIMEOUT_US) {
    if (MmioRead32 (I2C_PLD_BASE + IC_STATUS) & IC_STATUS_TFE)
      return TRUE;
    if (MmioRead32 (I2C_PLD_BASE + IC_RAW_INTR_STAT) & IC_INTR_TX_ABRT) {
      MmioRead32 (I2C_PLD_BASE + IC_CLR_TX_ABRT);
      return FALSE;
    }
    MicroSecondDelay (I2C_POLL_US);
    E += I2C_POLL_US;
  }
  return FALSE;
}

STATIC
BOOLEAN
Pca9555WriteReg (
  UINT8  Reg,
  UINT8  Value
  )
{
  if (!I2cWaitTfnf ()) return FALSE;
  MmioWrite32 (I2C_PLD_BASE + IC_DATA_CMD, (UINT32)Reg);
  if (!I2cWaitTfnf ()) return FALSE;
  MmioWrite32 (I2C_PLD_BASE + IC_DATA_CMD, (UINT32)Value | IC_DATA_CMD_STOP);
  return I2cWaitTfe ();
}

STATIC
VOID
Pca9555Init (
  VOID
  )
{
  //
  // Disable, configure, enable I2C_PLD controller
  //
  MmioWrite32 (I2C_PLD_BASE + IC_ENABLE, 0);
  while (MmioRead32 (I2C_PLD_BASE + IC_ENABLE_STATUS) & 1)
    ;

  MmioWrite32 (I2C_PLD_BASE + IC_CON,
    IC_CON_MASTER | IC_CON_SPEED_STD | IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE);
  MmioWrite32 (I2C_PLD_BASE + IC_SS_SCL_HCNT, SS_SCL_HCNT_VAL);
  MmioWrite32 (I2C_PLD_BASE + IC_SS_SCL_LCNT, SS_SCL_LCNT_VAL);
  MmioWrite32 (I2C_PLD_BASE + IC_INTR_MASK, 0);
  MmioWrite32 (I2C_PLD_BASE + IC_TAR, PCA9555_ADDR);
  MmioRead32  (I2C_PLD_BASE + IC_CLR_INTR);
  MmioWrite32 (I2C_PLD_BASE + IC_ENABLE, 1);

  //
  // RouterBoot sequence:
  //   Output Port 0 = 0xFF  (all high)
  //   Output Port 1 = 0xDB  (bits 2,5 low)
  //   Config Port 0 = 0xFF  (all input)
  //   Config Port 1 = 0xDB  (bits 2,5 output, rest input)
  //
  Pca9555WriteReg (PCA9555_OUTPUT_0, 0xFF);
  Pca9555WriteReg (PCA9555_OUTPUT_1, 0xDB);
  Pca9555WriteReg (PCA9555_CONFIG_0, 0xFF);
  Pca9555WriteReg (PCA9555_CONFIG_1, 0xDB);

  //
  // Disable controller so DwI2cDxe can reinitialize cleanly
  //
  MmioWrite32 (I2C_PLD_BASE + IC_ENABLE, 0);
  while (MmioRead32 (I2C_PLD_BASE + IC_ENABLE_STATUS) & 1)
    ;
}

// =====================================================================
// ArmPlatformLib interface
// =====================================================================

EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

/**
  Initialize controllers that must setup in the normal world.
  Called by PeilessSec after serial port is initialized.
**/
RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  AlpineMuioMuxInit ();
  SerialPortWrite ((UINT8 *)"SEC: PBS MUIO mux configured\n\r", 30);

  Pca9555Init ();
  SerialPortWrite ((UINT8 *)"SEC: PCA9555 GPIO expander configured\n\r", 39);

  return RETURN_SUCCESS;
}

VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  *PpiListSize = sizeof (mPlatformPpiTable);
  *PpiList     = mPlatformPpiTable;
}
