/** @file
  Board I2C EEPROM probe application for MikroTik CCR2004.

  UEFI shell application that scans both I2C buses (i2c-pld bus 0,
  i2c-gen bus 1) for EEPROM devices at addresses 0x50-0x57.
  For each EEPROM found, dumps its full contents to the console.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>

/* EEPROM I2C addresses: 0x50-0x57 (A0-A2 select) */
#define EEPROM_BASE_ADDRESS     0x50
#define EEPROM_ADDRESS_COUNT    8

/* M24C64: 64Kbit = 8192 bytes, 2-byte address, 32-byte page */
#define EEPROM_SIZE             8192
#define EEPROM_READ_CHUNK       32

/* I2C bus count */
#define I2C_BUS_COUNT           2

/* DwI2cDxe vendor GUID (must match DwI2cDxe.inf FILE_GUID) */
#define DW_I2C_DEVICE_PATH_GUID \
  { 0x3a8e7b2c, 0xd4f1, 0x4905, { 0xb6, 0xc8, 0x1e, 0x9a, 0x0f, 0x3d, 0x5c, 0x72 } }

typedef struct {
  VENDOR_DEVICE_PATH        Guid;
  UINT32                    Instance;
  EFI_DEVICE_PATH_PROTOCOL  End;
} DW_I2C_DEVICE_PATH;

/**
  Locate the EFI_I2C_MASTER_PROTOCOL for a given DwI2cDxe bus instance.
**/
STATIC
EFI_STATUS
FindI2cMaster (
  IN  UINT32                     BusInstance,
  OUT EFI_I2C_MASTER_PROTOCOL    **I2cMaster
  )
{
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  EFI_HANDLE                *HandleBuffer;
  UINTN                     Index;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  VENDOR_DEVICE_PATH        *VendorDp;
  DW_I2C_DEVICE_PATH        *DwDevPath;
  EFI_GUID                  DwI2cGuid = DW_I2C_DEVICE_PATH_GUID;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiI2cMasterProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (DevicePathType (DevicePath) != HARDWARE_DEVICE_PATH ||
        DevicePathSubType (DevicePath) != HW_VENDOR_DP) {
      continue;
    }

    VendorDp = (VENDOR_DEVICE_PATH *)DevicePath;
    if (!CompareGuid (&VendorDp->Guid, &DwI2cGuid)) {
      continue;
    }

    DwDevPath = (DW_I2C_DEVICE_PATH *)DevicePath;
    if (DwDevPath->Instance != BusInstance) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiI2cMasterProtocolGuid,
                    (VOID **)I2cMaster
                    );
    FreePool (HandleBuffer);
    return Status;
  }

  FreePool (HandleBuffer);
  return EFI_NOT_FOUND;
}

/**
  Probe an EEPROM at the given address by attempting a 2-byte address
  write followed by a 1-byte read.  Returns EFI_SUCCESS if the device ACKs.
**/
STATIC
EFI_STATUS
EepromProbe (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINTN                    SlaveAddr
  )
{
  EFI_STATUS  Status;
  UINT8       AddrBuf[2];
  UINT8       DataBuf;

  typedef struct {
    UINTN                OperationCount;
    EFI_I2C_OPERATION    Operation[2];
  } I2C_RW_PACKET;

  I2C_RW_PACKET  Packet;

  /* Set internal address to 0x0000 */
  AddrBuf[0] = 0x00;
  AddrBuf[1] = 0x00;

  Packet.OperationCount             = 2;
  Packet.Operation[0].Flags         = 0;          /* write */
  Packet.Operation[0].LengthInBytes = 2;
  Packet.Operation[0].Buffer        = AddrBuf;
  Packet.Operation[1].Flags         = I2C_FLAG_READ;
  Packet.Operation[1].LengthInBytes = 1;
  Packet.Operation[1].Buffer        = &DataBuf;

  Status = I2cMaster->StartRequest (
                         I2cMaster,
                         SlaveAddr,
                         (EFI_I2C_REQUEST_PACKET *)&Packet,
                         NULL,
                         NULL
                         );

  return Status;
}

/**
  Read a chunk from EEPROM using 2-byte addressing.
  Writes the 16-bit address, then reads DataLen bytes.
**/
STATIC
EFI_STATUS
EepromRead (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN  UINTN                    SlaveAddr,
  IN  UINT16                   Offset,
  OUT UINT8                    *Data,
  IN  UINTN                    DataLen
  )
{
  UINT8  AddrBuf[2];

  typedef struct {
    UINTN                OperationCount;
    EFI_I2C_OPERATION    Operation[2];
  } I2C_RW_PACKET;

  I2C_RW_PACKET  Packet;

  AddrBuf[0] = (UINT8)(Offset >> 8);    /* address high byte */
  AddrBuf[1] = (UINT8)(Offset & 0xFF);  /* address low byte */

  Packet.OperationCount             = 2;
  Packet.Operation[0].Flags         = 0;
  Packet.Operation[0].LengthInBytes = 2;
  Packet.Operation[0].Buffer        = AddrBuf;
  Packet.Operation[1].Flags         = I2C_FLAG_READ;
  Packet.Operation[1].LengthInBytes = (UINT32)DataLen;
  Packet.Operation[1].Buffer        = Data;

  return I2cMaster->StartRequest (
                       I2cMaster,
                       SlaveAddr,
                       (EFI_I2C_REQUEST_PACKET *)&Packet,
                       NULL,
                       NULL
                       );
}

/**
  Dump a buffer as hex + ASCII to serial via DEBUG.
**/
STATIC
VOID
HexDump (
  IN UINTN        BaseOffset,
  IN CONST UINT8  *Data,
  IN UINTN        Length
  )
{
  UINTN   i;
  UINTN   j;
  CHAR8   Line[80];
  CHAR8   *p;
  UINT8   c;

  for (i = 0; i < Length; i += 16) {
    p = Line;

    /* offset */
    p += AsciiSPrint (p, 12, "%04x: ", BaseOffset + i);

    /* hex bytes */
    for (j = 0; j < 16; j++) {
      if (i + j < Length) {
        p += AsciiSPrint (p, 4, "%02x ", Data[i + j]);
      } else {
        p += AsciiSPrint (p, 4, "   ");
      }
    }

    /* ASCII */
    *p++ = ' ';
    for (j = 0; j < 16 && (i + j) < Length; j++) {
      c = Data[i + j];
      *p++ = (c >= 0x20 && c <= 0x7e) ? (CHAR8)c : '.';
    }
    *p = '\0';

    Print (L"%a\n", Line);
  }
}

/**
  Entry point.  Scan both I2C buses for EEPROMs and dump contents.
**/
EFI_STATUS
EFIAPI
BoardI2cEepromProbeAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC CONST CHAR8  *BusName[I2C_BUS_COUNT] = { "i2c-pld", "i2c-gen" };

  EFI_I2C_MASTER_PROTOCOL  *I2cMaster;
  EFI_STATUS               Status;
  UINT32                   Bus;
  UINTN                    Addr;
  UINT8                    *Buffer;
  UINT16                   Offset;
  BOOLEAN                  Found;

  Print (L"Scanning for EEPROM devices on I2C buses...\n");

  Found = FALSE;

  for (Bus = 0; Bus < I2C_BUS_COUNT; Bus++) {
    Status = FindI2cMaster (Bus, &I2cMaster);
    if (EFI_ERROR (Status)) {
      Print (L"  Bus %u (%a): not available\n", Bus, BusName[Bus]);
      continue;
    }

    for (Addr = EEPROM_BASE_ADDRESS; Addr < EEPROM_BASE_ADDRESS + EEPROM_ADDRESS_COUNT; Addr++) {
      Status = EepromProbe (I2cMaster, Addr);
      if (EFI_ERROR (Status)) {
        continue;
      }

      Found = TRUE;
      Print (L"  Found EEPROM at bus %u (%a) addr 0x%02x\n", Bus, BusName[Bus], Addr);

      Buffer = AllocatePool (EEPROM_SIZE);
      if (Buffer == NULL) {
        Print (L"  ERROR: Failed to allocate %u bytes\n", EEPROM_SIZE);
        continue;
      }

      SetMem (Buffer, EEPROM_SIZE, 0xFF);

      for (Offset = 0; Offset < EEPROM_SIZE; Offset += EEPROM_READ_CHUNK) {
        UINTN ChunkLen = EEPROM_SIZE - Offset;
        if (ChunkLen > EEPROM_READ_CHUNK) {
          ChunkLen = EEPROM_READ_CHUNK;
        }

        Status = EepromRead (I2cMaster, Addr, Offset, &Buffer[Offset], ChunkLen);
        if (EFI_ERROR (Status)) {
          Print (L"  ERROR: Read failed at offset 0x%04x: %r\n", Offset, Status);
          break;
        }
      }

      if (!EFI_ERROR (Status)) {
        Print (L"  Dump of %u bytes from bus %u addr 0x%02x:\n", EEPROM_SIZE, Bus, Addr);
        HexDump (0, Buffer, EEPROM_SIZE);
      }

      FreePool (Buffer);
    }
  }

  if (!Found) {
    Print (L"  No EEPROM found on any bus\n");
  }

  return EFI_SUCCESS;
}
