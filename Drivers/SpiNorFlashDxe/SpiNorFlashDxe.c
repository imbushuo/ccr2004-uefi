/** @file
  SPI NOR Flash driver for Winbond W25Q128JWS on MikroTik CCR2004.

  Produces EFI_SPI_NOR_FLASH_PROTOCOL using standard SPI NOR commands
  (RDID, READ, PP, SE, WREN, RDSR) over the EFI_SPI_IO_PROTOCOL from
  SpiBusDxe.  Does not use SFDP.

  Flash geometry (W25Q128JWS):
    - 16 MB (128 Mbit)
    - 256-byte pages
    - 4 KB sectors (smallest erase unit)
    - 3-byte addressing

  Write-protected by default: the flash WEL bit is only set when
  WriteData or Erase is explicitly called.

  Modeled after Silicon/Marvell/Drivers/Spi/MvSpiFlashDxe.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/SpiIo.h>
#include <Protocol/SpiNorFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

/* SPI NOR standard commands */
#define CMD_READ_ID         0x9F
#define CMD_READ_DATA       0x03  /* Normal Read (up to ~50MHz) */
#define CMD_READ_FAST       0x0B  /* Fast Read (+ 1 dummy byte) */
#define CMD_WRITE_ENABLE    0x06
#define CMD_WRITE_DISABLE   0x04
#define CMD_PAGE_PROGRAM    0x02
#define CMD_SECTOR_ERASE    0x20  /* 4KB sector erase */
#define CMD_READ_STATUS     0x05
#define CMD_WRITE_STATUS    0x01

/* Status register bits */
#define SR_WIP              BIT0  /* Write In Progress */
#define SR_WEL              BIT1  /* Write Enable Latch */

/* W25Q128JWS geometry */
#define FLASH_SIZE          (16 * 1024 * 1024)  /* 16 MB */
#define FLASH_PAGE_SIZE     256
#define FLASH_SECTOR_SIZE   (4 * 1024)          /* 4 KB */
#define FLASH_ADDR_BYTES    3

/* Timeouts */
#define WIP_POLL_DELAY_US   100
#define WIP_POLL_MAX        100000  /* 10 seconds max */

/* Maximum bytes per SPI read transaction.
 * Use 4KB chunks for progress logging and bounded transaction sizes. */
#define SPI_READ_CHUNK_SIZE  4096

/* Driver instance */
typedef struct {
  EFI_SPI_NOR_FLASH_PROTOCOL  Protocol;
  EFI_SPI_IO_PROTOCOL         *SpiIo;
  UINT8                       DeviceId[3];
} SPI_NOR_FLASH_INSTANCE;

/**
  Write a formatted message directly to the serial port.
**/
STATIC
VOID
SpiLog (
  IN CONST CHAR8  *Fmt,
  ...
  )
{
#ifdef LOG_SPI_ACCESS
  VA_LIST  Args;
  CHAR8    Buf[128];
  UINTN    Len;

  VA_START (Args, Fmt);
  Len = AsciiVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);

  SerialPortWrite ((UINT8 *)Buf, Len);
#endif
}

/* ---------- Low-level SPI helpers ---------- */

/**
  Execute a WRITE_THEN_READ transaction (command + address → read data).
**/
STATIC
EFI_STATUS
SpiWriteThenRead (
  IN  EFI_SPI_IO_PROTOCOL  *SpiIo,
  IN  UINT8                *Cmd,
  IN  UINT32               CmdLen,
  OUT UINT8                *Data,
  IN  UINT32               DataLen
  )
{
  return SpiIo->Transaction (
                   SpiIo,
                   SPI_TRANSACTION_WRITE_THEN_READ,
                   FALSE,
                   0,     /* use max clock */
                   1,     /* bus width 1 bit */
                   8,     /* frame size 8 bits */
                   CmdLen,
                   Cmd,
                   DataLen,
                   Data
                   );
}

/**
  Execute a WRITE_ONLY transaction (command only, or command + data).
**/
STATIC
EFI_STATUS
SpiWriteOnly (
  IN  EFI_SPI_IO_PROTOCOL  *SpiIo,
  IN  UINT8                *Data,
  IN  UINT32               DataLen
  )
{
  return SpiIo->Transaction (
                   SpiIo,
                   SPI_TRANSACTION_WRITE_ONLY,
                   FALSE,
                   0,
                   1,
                   8,
                   DataLen,
                   Data,
                   0,
                   NULL
                   );
}

/**
  Send Write Enable command (0x06).
**/
STATIC
EFI_STATUS
FlashWriteEnable (
  IN  EFI_SPI_IO_PROTOCOL  *SpiIo
  )
{
  UINT8  Cmd = CMD_WRITE_ENABLE;
  return SpiWriteOnly (SpiIo, &Cmd, 1);
}

/**
  Read Status Register and return the value.
**/
STATIC
EFI_STATUS
FlashReadStatus (
  IN  EFI_SPI_IO_PROTOCOL  *SpiIo,
  OUT UINT8                *Status
  )
{
  UINT8  Cmd = CMD_READ_STATUS;
  return SpiWriteThenRead (SpiIo, &Cmd, 1, Status, 1);
}

/**
  Poll the status register until WIP clears or timeout.
**/
STATIC
EFI_STATUS
FlashWaitReady (
  IN  EFI_SPI_IO_PROTOCOL  *SpiIo
  )
{
  EFI_STATUS  Status;
  UINT8       Sr;
  UINT32      Retry;

  for (Retry = 0; Retry < WIP_POLL_MAX; Retry++) {
    Status = FlashReadStatus (SpiIo, &Sr);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((Sr & SR_WIP) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (WIP_POLL_DELAY_US);
  }

  DEBUG ((DEBUG_ERROR, "SpiNorFlash: WIP timeout\n"));
  return EFI_TIMEOUT;
}

/* ---------- EFI_SPI_NOR_FLASH_PROTOCOL methods ---------- */

/**
  GetFlashid — Read 3-byte JEDEC ID.
**/
STATIC
EFI_STATUS
EFIAPI
SpiNorGetFlashId (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  OUT UINT8                              *Buffer
  )
{
  SPI_NOR_FLASH_INSTANCE  *Instance;
  UINT8                   Cmd;
  EFI_STATUS              Status;

  if (This == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = (SPI_NOR_FLASH_INSTANCE *)This;
  Cmd = CMD_READ_ID;

  Status = SpiWriteThenRead (Instance->SpiIo, &Cmd, 1, Buffer, 3);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_VERBOSE, "SpiNorFlash: JEDEC ID = %02x %02x %02x\n",
            Buffer[0], Buffer[1], Buffer[2]));
  }

  return Status;
}

/**
  ReadData — Read data from flash.
  Uses normal READ (0x03) with 3-byte address.
  Chunks large reads to stay within DW SSI CTRL1.NDF limits.
**/
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
  SPI_NOR_FLASH_INSTANCE  *Instance;
  EFI_STATUS              Status;
  UINT32                  Offset;
  UINT32                  Chunk;
  UINT8                   Cmd[4];

  if (This == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (FlashAddress + LengthInBytes > FLASH_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = (SPI_NOR_FLASH_INSTANCE *)This;

  SpiLog ("[SpiNor] ReadData: addr=0x%06x len=0x%x\r\n", FlashAddress, LengthInBytes);

  for (Offset = 0; Offset < LengthInBytes; Offset += Chunk) {
    UINT32  Addr;

    Chunk = LengthInBytes - Offset;
    if (Chunk > SPI_READ_CHUNK_SIZE) {
      Chunk = SPI_READ_CHUNK_SIZE;
    }

    Addr = FlashAddress + Offset;
    Cmd[0] = CMD_READ_DATA;
    Cmd[1] = (UINT8)(Addr >> 16);
    Cmd[2] = (UINT8)(Addr >> 8);
    Cmd[3] = (UINT8)(Addr);

    Status = SpiWriteThenRead (Instance->SpiIo, Cmd, 4, Buffer + Offset, Chunk);
    if (EFI_ERROR (Status)) {
      SpiLog ("[SpiNor] ReadData: FAIL at offset 0x%x: %d\r\n", Offset, (INT32)Status);
      return Status;
    }
  }

  SpiLog ("[SpiNor] ReadData: done\r\n");
  return EFI_SUCCESS;
}

/**
  LfReadData — Low-frequency read (same as ReadData for this device).
**/
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

/**
  ReadStatus — Read flash status register.
**/
STATIC
EFI_STATUS
EFIAPI
SpiNorReadStatus (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            LengthInBytes,
  OUT UINT8                             *FlashStatus
  )
{
  SPI_NOR_FLASH_INSTANCE  *Instance;

  if (This == NULL || FlashStatus == NULL || LengthInBytes < 1) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = (SPI_NOR_FLASH_INSTANCE *)This;
  return FlashReadStatus (Instance->SpiIo, FlashStatus);
}

/**
  WriteStatus — Write flash status register.
**/
STATIC
EFI_STATUS
EFIAPI
SpiNorWriteStatus (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            LengthInBytes,
  IN  UINT8                             *FlashStatus
  )
{
  SPI_NOR_FLASH_INSTANCE  *Instance;
  UINT8                   Buf[2];
  EFI_STATUS              Status;

  if (This == NULL || FlashStatus == NULL || LengthInBytes < 1) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = (SPI_NOR_FLASH_INSTANCE *)This;

  Status = FlashWriteEnable (Instance->SpiIo);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Buf[0] = CMD_WRITE_STATUS;
  Buf[1] = FlashStatus[0];
  Status = SpiWriteOnly (Instance->SpiIo, Buf, 2);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return FlashWaitReady (Instance->SpiIo);
}

/**
  WriteData — Program data to flash using Page Program (0x02).
  Handles page-boundary splitting.
**/
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
  SPI_NOR_FLASH_INSTANCE  *Instance;
  EFI_STATUS              Status;
  UINT32                  ChunkSize;
  UINT32                  PageOffset;
  UINT8                   *CmdBuf;

  if (This == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (FlashAddress + LengthInBytes > FLASH_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = (SPI_NOR_FLASH_INSTANCE *)This;

  /* Allocate buffer for command header + page data */
  CmdBuf = AllocatePool (4 + FLASH_PAGE_SIZE);
  if (CmdBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  while (LengthInBytes > 0) {
    /* Respect page boundaries */
    PageOffset = FlashAddress & (FLASH_PAGE_SIZE - 1);
    ChunkSize = FLASH_PAGE_SIZE - PageOffset;
    if (ChunkSize > LengthInBytes) {
      ChunkSize = LengthInBytes;
    }

    /* Write Enable */
    Status = FlashWriteEnable (Instance->SpiIo);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    /* Build Page Program command: [0x02, A23-A16, A15-A8, A7-A0, data...] */
    CmdBuf[0] = CMD_PAGE_PROGRAM;
    CmdBuf[1] = (UINT8)(FlashAddress >> 16);
    CmdBuf[2] = (UINT8)(FlashAddress >> 8);
    CmdBuf[3] = (UINT8)(FlashAddress);
    CopyMem (&CmdBuf[4], Buffer, ChunkSize);

    Status = SpiWriteOnly (Instance->SpiIo, CmdBuf, 4 + ChunkSize);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    /* Wait for program to complete */
    Status = FlashWaitReady (Instance->SpiIo);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    FlashAddress += ChunkSize;
    Buffer       += ChunkSize;
    LengthInBytes -= ChunkSize;
  }

  Status = EFI_SUCCESS;

Done:
  FreePool (CmdBuf);
  return Status;
}

/**
  Erase — Erase flash in 4KB sectors.
**/
STATIC
EFI_STATUS
EFIAPI
SpiNorErase (
  IN  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                            FlashAddress,
  IN  UINT32                            BlockCount
  )
{
  SPI_NOR_FLASH_INSTANCE  *Instance;
  EFI_STATUS              Status;
  UINT32                  Idx;
  UINT8                   Cmd[4];

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((FlashAddress & (FLASH_SECTOR_SIZE - 1)) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (FlashAddress + BlockCount * FLASH_SECTOR_SIZE > FLASH_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = (SPI_NOR_FLASH_INSTANCE *)This;

  for (Idx = 0; Idx < BlockCount; Idx++) {
    UINT32  Addr = FlashAddress + Idx * FLASH_SECTOR_SIZE;

    /* Write Enable */
    Status = FlashWriteEnable (Instance->SpiIo);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    /* Sector Erase command */
    Cmd[0] = CMD_SECTOR_ERASE;
    Cmd[1] = (UINT8)(Addr >> 16);
    Cmd[2] = (UINT8)(Addr >> 8);
    Cmd[3] = (UINT8)(Addr);

    Status = SpiWriteOnly (Instance->SpiIo, Cmd, 4);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    /* Wait for erase to complete */
    Status = FlashWaitReady (Instance->SpiIo);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/* ---------- Entry point ---------- */

extern EFI_GUID  gEdk2JedecSfdpSpiDxeDriverGuid;

EFI_STATUS
EFIAPI
SpiNorFlashDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  EFI_SPI_IO_PROTOCOL     *SpiIo;
  SPI_NOR_FLASH_INSTANCE  *Instance;
  UINT8                   JedecId[3];
  EFI_HANDLE              FlashHandle;

  DEBUG ((DEBUG_VERBOSE, "SpiNorFlash: Locating SPI IO protocol...\n"));

  /* The SpiBusDxe installs EFI_SPI_IO_PROTOCOL with the peripheral driver
   * GUID (gEdk2JedecSfdpSpiDxeDriverGuid) as the protocol GUID. */
  Status = gBS->LocateProtocol (
                  &gEdk2JedecSfdpSpiDxeDriverGuid,
                  NULL,
                  (VOID **)&SpiIo
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlash: SPI IO protocol not found: %r\n", Status));
    return Status;
  }

  /* Allocate instance */
  Instance = AllocateZeroPool (sizeof (SPI_NOR_FLASH_INSTANCE));
  if (Instance == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Instance->SpiIo = SpiIo;

  /* Read JEDEC ID to verify flash is present */
  Status = SpiNorGetFlashId (&Instance->Protocol, JedecId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNorFlash: Failed to read JEDEC ID: %r\n", Status));
    FreePool (Instance);
    return Status;
  }

  CopyMem (Instance->DeviceId, JedecId, 3);

  /* W25Q128JWS: Manufacturer=0xEF, Device=0x6018 */
  DEBUG ((DEBUG_VERBOSE, "SpiNorFlash: Detected flash: MFR=0x%02x DEV=0x%02x%02x",
          JedecId[0], JedecId[1], JedecId[2]));
  if (JedecId[0] == 0xEF) {
    DEBUG ((DEBUG_VERBOSE, " (Winbond)\n"));
  } else {
    DEBUG ((DEBUG_VERBOSE, "\n"));
  }

  /* Wire up protocol */
  Instance->Protocol.SpiPeripheral  = SpiIo->SpiPeripheral;
  Instance->Protocol.FlashSize      = FLASH_SIZE;
  Instance->Protocol.EraseBlockBytes = FLASH_SECTOR_SIZE;
  CopyMem (Instance->Protocol.Deviceid, JedecId, 3);
  Instance->Protocol.GetFlashid     = SpiNorGetFlashId;
  Instance->Protocol.ReadData       = SpiNorReadData;
  Instance->Protocol.LfReadData     = SpiNorLfReadData;
  Instance->Protocol.ReadStatus     = SpiNorReadStatus;
  Instance->Protocol.WriteStatus    = SpiNorWriteStatus;
  Instance->Protocol.WriteData      = SpiNorWriteData;
  Instance->Protocol.Erase          = SpiNorErase;

  /* Install on a new handle */
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

  DEBUG ((DEBUG_VERBOSE, "SpiNorFlash: %u MB flash ready (4KB erase, write-protected by default)\n",
          FLASH_SIZE / (1024 * 1024)));

  return EFI_SUCCESS;
}
