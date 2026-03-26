/** @file
  SPI NOR Flash driver for Winbond W25Q128JWS on MikroTik CCR2004.

  Uses the Alpine HAL SPI driver (al_hal_spi) for all low-level SPI
  transactions.  Produces EFI_SPI_NOR_FLASH_PROTOCOL.

  Flash geometry (W25Q128JWS):
    - 16 MB (128 Mbit), 256-byte pages, 4 KB sectors, 3-byte addressing

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/SpiNorFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include "al_hal_spi.h"

/* SPI NOR standard commands */
#define CMD_READ_ID         0x9F
#define CMD_READ_DATA       0x03
#define CMD_WRITE_ENABLE    0x06
#define CMD_PAGE_PROGRAM    0x02
#define CMD_SECTOR_ERASE    0x20
#define CMD_READ_STATUS     0x05
#define CMD_READ_STATUS2    0x35
#define CMD_READ_STATUS3    0x15
#define CMD_WRITE_STATUS    0x01
#define CMD_WRITE_STATUS2   0x31
#define CMD_WRITE_STATUS3   0x11

#define SR_WIP              BIT0
#define SR_WEL              BIT1

/* W25Q128JWS geometry */
#define FLASH_SIZE          (16 * 1024 * 1024)
#define FLASH_PAGE_SIZE     256
#define FLASH_SECTOR_SIZE   (4 * 1024)

/* Alpine V2 SPI Master base and clock */
#define AL_SPI_BASE         0xFD882000ULL
#define AL_SPI_CLK_HZ       500000000U
#define AL_SPI_FLASH_HZ     30000000U
#define AL_SPI_CS            0
#define AL_SPI_TIMEOUT_US    1000000U

#define WIP_POLL_MAX        10000
#define WIP_POLL_DELAY_US   100

typedef struct {
  EFI_SPI_NOR_FLASH_PROTOCOL  Protocol;
  struct al_spi_interface     Spi;
  UINT8                       DeviceId[3];
} SPI_NOR_FLASH_INSTANCE;

/* ---- HAL helpers ---- */

STATIC
EFI_STATUS
HalRead (
  IN  SPI_NOR_FLASH_INSTANCE  *Inst,
  IN  UINT8                   *Cmd,
  IN  UINT8                   CmdLen,
  OUT UINT8                   *Buf,
  IN  UINT32                  BufLen
  )
{
  int Err = al_spi_read (&Inst->Spi, Cmd, CmdLen, Buf, BufLen,
                          AL_SPI_CS, AL_SPI_TIMEOUT_US);
  return (Err == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
HalWrite (
  IN  SPI_NOR_FLASH_INSTANCE  *Inst,
  IN  UINT8                   *Cmd,
  IN  UINT8                   CmdLen,
  IN  UINT8                   *Data,
  IN  UINT32                  DataLen
  )
{
  int Err = al_spi_write (&Inst->Spi, Cmd, CmdLen, Data, DataLen,
                           AL_SPI_CS, AL_SPI_TIMEOUT_US);
  return (Err == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
HalCmdOnly (
  IN  SPI_NOR_FLASH_INSTANCE  *Inst,
  IN  UINT8                   *Cmd,
  IN  UINT8                   CmdLen
  )
{
  int Err = al_spi_write (&Inst->Spi, Cmd, CmdLen, NULL, 0,
                           AL_SPI_CS, AL_SPI_TIMEOUT_US);
  return (Err == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/* ---- Flash operations ---- */

STATIC
EFI_STATUS
FlashReadStatus (
  IN  SPI_NOR_FLASH_INSTANCE  *Inst,
  IN  UINT8                   StatusCmd,
  OUT UINT8                   *Sr
  )
{
  return HalRead (Inst, &StatusCmd, 1, Sr, 1);
}

STATIC
EFI_STATUS
FlashWriteEnable (
  IN  SPI_NOR_FLASH_INSTANCE  *Inst
  )
{
  EFI_STATUS  Status;
  UINT8       Sr;
  UINT32      Retry;

  Status = HalCmdOnly (Inst, (UINT8[]){ CMD_WRITE_ENABLE }, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Verify WEL is set */
  for (Retry = 0; Retry < 100; Retry++) {
    Status = FlashReadStatus (Inst, CMD_READ_STATUS, &Sr);
    if (EFI_ERROR (Status)) return Status;
    if (Sr & SR_WEL) return EFI_SUCCESS;
    MicroSecondDelay (10);
  }

  DEBUG ((DEBUG_ERROR, "SpiNorFlash: WEL not set after WREN (SR=0x%02x)\n", Sr));
  return EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
FlashWaitReady (
  IN  SPI_NOR_FLASH_INSTANCE  *Inst
  )
{
  UINT8   Sr;
  UINT32  Retry;

  for (Retry = 0; Retry < WIP_POLL_MAX; Retry++) {
    if (EFI_ERROR (FlashReadStatus (Inst, CMD_READ_STATUS, &Sr))) {
      return EFI_DEVICE_ERROR;
    }
    if ((Sr & SR_WIP) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (WIP_POLL_DELAY_US);
  }

  DEBUG ((DEBUG_ERROR, "SpiNorFlash: WIP timeout\n"));
  return EFI_TIMEOUT;
}

/* ---- EFI_SPI_NOR_FLASH_PROTOCOL methods ---- */

STATIC
EFI_STATUS
EFIAPI
SpiNorGetFlashId (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  OUT UINT8                             *Buffer
  )
{
  SPI_NOR_FLASH_INSTANCE  *Inst = (SPI_NOR_FLASH_INSTANCE *)This;
  UINT8                   Cmd = CMD_READ_ID;
  return HalRead (Inst, &Cmd, 1, Buffer, 3);
}

STATIC
EFI_STATUS
EFIAPI
SpiNorReadData (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            FlashAddress,
  IN  UINT32                            LengthInBytes,
  OUT UINT8                             *Buffer
  )
{
  SPI_NOR_FLASH_INSTANCE  *Inst = (SPI_NOR_FLASH_INSTANCE *)This;
  UINT32                  Offset = 0;

  if (FlashAddress + LengthInBytes > FLASH_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  /* Read in 4KB chunks to avoid SPI FIFO issues */
  while (Offset < LengthInBytes) {
    UINT32  Chunk = LengthInBytes - Offset;
    UINT32  Addr  = FlashAddress + Offset;
    UINT8   Cmd[4];

    if (Chunk > FLASH_SECTOR_SIZE) {
      Chunk = FLASH_SECTOR_SIZE;
    }

    Cmd[0] = CMD_READ_DATA;
    Cmd[1] = (UINT8)(Addr >> 16);
    Cmd[2] = (UINT8)(Addr >> 8);
    Cmd[3] = (UINT8)(Addr);

    if (EFI_ERROR (HalRead (Inst, Cmd, 4, Buffer + Offset, Chunk))) {
      return EFI_DEVICE_ERROR;
    }

    Offset += Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SpiNorLfReadData (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            FlashAddress,
  IN  UINT32                            LengthInBytes,
  OUT UINT8                             *Buffer
  )
{
  return SpiNorReadData (This, FlashAddress, LengthInBytes, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
SpiNorReadStatus (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            LengthInBytes,
  OUT UINT8                             *FlashStatus
  )
{
  SPI_NOR_FLASH_INSTANCE  *Inst = (SPI_NOR_FLASH_INSTANCE *)This;
  if (LengthInBytes < 1) return EFI_INVALID_PARAMETER;
  return FlashReadStatus (Inst, CMD_READ_STATUS, FlashStatus);
}

STATIC
EFI_STATUS
EFIAPI
SpiNorWriteStatus (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            LengthInBytes,
  IN  UINT8                             *FlashStatus
  )
{
  SPI_NOR_FLASH_INSTANCE  *Inst = (SPI_NOR_FLASH_INSTANCE *)This;
  EFI_STATUS              Status;
  UINT8                   Cmd;

  if (LengthInBytes < 1) return EFI_INVALID_PARAMETER;

  Status = FlashWriteEnable (Inst);
  if (EFI_ERROR (Status)) return Status;

  Cmd = CMD_WRITE_STATUS;
  Status = HalWrite (Inst, &Cmd, 1, FlashStatus, 1);
  if (EFI_ERROR (Status)) return Status;

  return FlashWaitReady (Inst);
}

STATIC
EFI_STATUS
EFIAPI
SpiNorWriteData (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            FlashAddress,
  IN  UINT32                            LengthInBytes,
  IN  UINT8                             *Buffer
  )
{
  SPI_NOR_FLASH_INSTANCE  *Inst = (SPI_NOR_FLASH_INSTANCE *)This;
  EFI_STATUS              Status;
  UINT32                  ChunkSize;
  UINT32                  PageOffset;

  if (FlashAddress + LengthInBytes > FLASH_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  while (LengthInBytes > 0) {
    PageOffset = FlashAddress & (FLASH_PAGE_SIZE - 1);
    ChunkSize  = FLASH_PAGE_SIZE - PageOffset;
    if (ChunkSize > LengthInBytes) {
      ChunkSize = LengthInBytes;
    }

    Status = FlashWriteEnable (Inst);
    if (EFI_ERROR (Status)) return Status;

    {
      UINT8  Cmd[4];
      Cmd[0] = CMD_PAGE_PROGRAM;
      Cmd[1] = (UINT8)(FlashAddress >> 16);
      Cmd[2] = (UINT8)(FlashAddress >> 8);
      Cmd[3] = (UINT8)(FlashAddress);

      Status = HalWrite (Inst, Cmd, 4, Buffer, ChunkSize);
      if (EFI_ERROR (Status)) return Status;
    }

    Status = FlashWaitReady (Inst);
    if (EFI_ERROR (Status)) return Status;

    FlashAddress  += ChunkSize;
    Buffer        += ChunkSize;
    LengthInBytes -= ChunkSize;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SpiNorErase (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            FlashAddress,
  IN  UINT32                            BlockCount
  )
{
  SPI_NOR_FLASH_INSTANCE  *Inst = (SPI_NOR_FLASH_INSTANCE *)This;
  EFI_STATUS              Status;
  UINT32                  Idx;

  if ((FlashAddress & (FLASH_SECTOR_SIZE - 1)) != 0) return EFI_INVALID_PARAMETER;
  if (FlashAddress + BlockCount * FLASH_SECTOR_SIZE > FLASH_SIZE) return EFI_INVALID_PARAMETER;

  for (Idx = 0; Idx < BlockCount; Idx++) {
    UINT32  Addr = FlashAddress + Idx * FLASH_SECTOR_SIZE;

    Status = FlashWriteEnable (Inst);
    if (EFI_ERROR (Status)) return Status;

    {
      UINT8  Cmd[4];
      Cmd[0] = CMD_SECTOR_ERASE;
      Cmd[1] = (UINT8)(Addr >> 16);
      Cmd[2] = (UINT8)(Addr >> 8);
      Cmd[3] = (UINT8)(Addr);

      Status = HalCmdOnly (Inst, Cmd, 4);
      if (EFI_ERROR (Status)) return Status;
    }

    Status = FlashWaitReady (Inst);
    if (EFI_ERROR (Status)) return Status;
  }

  return EFI_SUCCESS;
}

/* ---- Entry point ---- */

EFI_STATUS
EFIAPI
SpiNorFlashDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  SPI_NOR_FLASH_INSTANCE  *Instance;
  UINT8                   JedecId[3];
  EFI_HANDLE              FlashHandle;

  Instance = AllocateZeroPool (sizeof (*Instance));
  if (Instance == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* Initialize Alpine HAL SPI */
  al_spi_init (&Instance->Spi, (void *)AL_SPI_BASE, AL_SPI_CLK_HZ);
  al_spi_claim_bus (&Instance->Spi, AL_SPI_FLASH_HZ,
                    AL_SPI_PHASE_SLAVE_SELECT, AL_SPI_POLARITY_INACTIVE_LOW,
                    AL_SPI_CS);

  DEBUG ((DEBUG_WARN, "SpiNorFlash: HAL SPI initialized at 0x%lx\n", AL_SPI_BASE));

  /* Read JEDEC ID */
  Status = SpiNorGetFlashId (&Instance->Protocol, JedecId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlash: Failed to read JEDEC ID: %r\n", Status));
    FreePool (Instance);
    return Status;
  }

  CopyMem (Instance->DeviceId, JedecId, 3);

  DEBUG ((DEBUG_WARN, "SpiNorFlash: JEDEC ID: %02x %02x %02x%a\n",
          JedecId[0], JedecId[1], JedecId[2],
          (JedecId[0] == 0xEF) ? " (Winbond)" : ""));

  /* Dump and clear Winbond status registers */
  if (JedecId[0] == 0xEF) {
    UINT8  Sr1, Sr2, Sr3;

    FlashReadStatus (Instance, CMD_READ_STATUS,  &Sr1);
    FlashReadStatus (Instance, CMD_READ_STATUS2, &Sr2);
    FlashReadStatus (Instance, CMD_READ_STATUS3, &Sr3);

    DEBUG ((DEBUG_WARN, "SpiNorFlash: SR1=0x%02x SR2=0x%02x SR3=0x%02x\n", Sr1, Sr2, Sr3));

    /* Clear protection: SR1 BP bits, SR2 CMP, SR3 WPS */
    if ((Sr1 & 0x7C) || (Sr2 & 0x40) || (Sr3 & 0x04)) {
      DEBUG ((DEBUG_WARN, "SpiNorFlash: Clearing write protection bits\n"));

      if (Sr1 & 0x7C) {
        UINT8 Val = Sr1 & ~0x7C;
        FlashWriteEnable (Instance);
        HalWrite (Instance, (UINT8[]){ CMD_WRITE_STATUS }, 1, &Val, 1);
        FlashWaitReady (Instance);
      }
      if (Sr2 & 0x40) {
        UINT8 Val = Sr2 & ~0x40;
        FlashWriteEnable (Instance);
        HalWrite (Instance, (UINT8[]){ CMD_WRITE_STATUS2 }, 1, &Val, 1);
        FlashWaitReady (Instance);
      }
      if (Sr3 & 0x04) {
        UINT8 Val = Sr3 & ~0x04;
        FlashWriteEnable (Instance);
        HalWrite (Instance, (UINT8[]){ CMD_WRITE_STATUS3 }, 1, &Val, 1);
        FlashWaitReady (Instance);
      }
    }
  }

  /* Wire up protocol */
  Instance->Protocol.SpiPeripheral   = NULL;
  Instance->Protocol.FlashSize       = FLASH_SIZE;
  Instance->Protocol.EraseBlockBytes = FLASH_SECTOR_SIZE;
  CopyMem (Instance->Protocol.Deviceid, JedecId, 3);
  Instance->Protocol.GetFlashid      = SpiNorGetFlashId;
  Instance->Protocol.ReadData        = SpiNorReadData;
  Instance->Protocol.LfReadData      = SpiNorLfReadData;
  Instance->Protocol.ReadStatus      = SpiNorReadStatus;
  Instance->Protocol.WriteStatus     = SpiNorWriteStatus;
  Instance->Protocol.WriteData       = SpiNorWriteData;
  Instance->Protocol.Erase           = SpiNorErase;

  /* Install protocol */
  FlashHandle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &FlashHandle,
                  &gEfiSpiNorFlashProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Instance->Protocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlash: Failed to install protocol: %r\n", Status));
    FreePool (Instance);
    return Status;
  }

  DEBUG ((DEBUG_WARN, "SpiNorFlash: %u MB flash ready (HAL-based)\n",
          FLASH_SIZE / (1024 * 1024)));

  return EFI_SUCCESS;
}
