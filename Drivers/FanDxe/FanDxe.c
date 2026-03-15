/** @file
  Fan controller keepalive DXE driver for MikroTik CCR2004.

  Sends periodic PWM commands to the MikroTik MCU fan controller at I2C
  address 0x0d on bus 1 (i2c-gen).  The MCU firmware has an internal
  watchdog that spins fans to 100 % if it receives no I2C traffic for
  several seconds.  This driver keeps the fans at a moderate speed while
  UEFI firmware is running.

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
#include <Library/UefiLib.h>

//
// MCU I2C address and registers
//
#define FAN_MCU_SLAVE_ADDRESS   0x0d
#define FAN_MCU_REG_VERSION     0x90
#define FAN_MCU_REG_COMMAND     0x98
#define FAN_MCU_CMD_PWM         0x23
#define FAN_PWM_VALUE           51        // ~20% (51/255)
#define FAN_KEEPALIVE_INTERVAL  40000000  // 4 seconds in 100ns units

//
// Parent bus instance (i2c-gen = bus 1)
//
#define FAN_PARENT_BUS_INSTANCE  1

//
// DwI2cDxe vendor GUID (must match DwI2cDxe.inf FILE_GUID)
//
#define DW_I2C_DEVICE_PATH_GUID \
  { 0x3a8e7b2c, 0xd4f1, 0x4905, { 0xb6, 0xc8, 0x1e, 0x9a, 0x0f, 0x3d, 0x5c, 0x72 } }

//
// Replicate DW_I2C_DEVICE_PATH for parent handle matching
//
typedef struct {
  VENDOR_DEVICE_PATH        Guid;
  UINT32                    Instance;
  EFI_DEVICE_PATH_PROTOCOL  End;
} DW_I2C_DEVICE_PATH;

//
// Module globals
//
STATIC EFI_I2C_MASTER_PROTOCOL  *mI2cMaster;
STATIC EFI_EVENT                mKeepaliveEvent;

// ---------------------------------------------------------------------------
// Find I2C master for bus 1
// ---------------------------------------------------------------------------

/**
  Locate the EFI_I2C_MASTER_PROTOCOL for DwI2cDxe bus instance 1 (i2c-gen).

  @param[out] I2cMaster  Pointer to the I2C master protocol.

  @retval EFI_SUCCESS    Found and returned.
  @retval EFI_NOT_FOUND  No matching handle.
**/
STATIC
EFI_STATUS
FindI2cMasterBus1 (
  OUT EFI_I2C_MASTER_PROTOCOL  **I2cMaster
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
    if (DwDevPath->Instance != FAN_PARENT_BUS_INSTANCE) {
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

// ---------------------------------------------------------------------------
// MCU I2C helpers
// ---------------------------------------------------------------------------

/**
  Write data bytes to an MCU register.

  Sends a single I2C write operation: [reg, data[0], data[1], ...].

  @param[in] Reg       Register address.
  @param[in] Data      Data bytes to write.
  @param[in] DataLen   Number of data bytes.

  @retval EFI_SUCCESS  Write completed.
**/
STATIC
EFI_STATUS
FanMcuWriteReg (
  IN UINT8  Reg,
  IN UINT8  *Data,
  IN UINTN  DataLen
  )
{
  EFI_I2C_REQUEST_PACKET  *Packet;
  UINT8                   *Buffer;
  EFI_STATUS              Status;

  Buffer = AllocatePool (1 + DataLen);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Buffer[0] = Reg;
  CopyMem (&Buffer[1], Data, DataLen);

  Packet = AllocateZeroPool (sizeof (EFI_I2C_REQUEST_PACKET));
  if (Packet == NULL) {
    FreePool (Buffer);
    return EFI_OUT_OF_RESOURCES;
  }

  Packet->OperationCount             = 1;
  Packet->Operation[0].Flags         = 0;
  Packet->Operation[0].LengthInBytes = (UINT32)(1 + DataLen);
  Packet->Operation[0].Buffer        = Buffer;

  Status = mI2cMaster->StartRequest (
                          mI2cMaster,
                          FAN_MCU_SLAVE_ADDRESS,
                          Packet,
                          NULL,
                          NULL
                          );

  FreePool (Packet);
  FreePool (Buffer);
  return Status;
}

/**
  Read data bytes from an MCU register.

  Sends a write operation with the register address, then a read operation
  to receive the data (I2C repeated start).

  @param[in]  Reg       Register address.
  @param[out] Data      Buffer to receive data.
  @param[in]  DataLen   Number of bytes to read.

  @retval EFI_SUCCESS   Read completed.
**/
STATIC
EFI_STATUS
FanMcuReadReg (
  IN  UINT8  Reg,
  OUT UINT8  *Data,
  IN  UINTN  DataLen
  )
{
  EFI_STATUS  Status;
  UINT8       RegBuf;

  //
  // We need a packet with 2 operations.  EFI_I2C_REQUEST_PACKET contains
  // Operation[1] inline, so allocate space for the extra operation.
  //
  typedef struct {
    UINTN                OperationCount;
    EFI_I2C_OPERATION    Operation[2];
  } I2C_READ_PACKET;

  I2C_READ_PACKET  ReadPacket;

  RegBuf = Reg;

  ReadPacket.OperationCount             = 2;
  ReadPacket.Operation[0].Flags         = 0;
  ReadPacket.Operation[0].LengthInBytes = 1;
  ReadPacket.Operation[0].Buffer        = &RegBuf;
  ReadPacket.Operation[1].Flags         = I2C_FLAG_READ;
  ReadPacket.Operation[1].LengthInBytes = (UINT32)DataLen;
  ReadPacket.Operation[1].Buffer        = Data;

  Status = mI2cMaster->StartRequest (
                          mI2cMaster,
                          FAN_MCU_SLAVE_ADDRESS,
                          (EFI_I2C_REQUEST_PACKET *)&ReadPacket,
                          NULL,
                          NULL
                          );

  return Status;
}

/**
  Read the MCU version from register 0x90.

  The version register returns 4 bytes: [ver_lo, ver_hi, 0x01, xor_checksum].
  Validates that buf[2] == 1 and (buf[0] ^ buf[1] ^ buf[2]) == buf[3].

  @param[out] Version  MCU version as UINT16 (ver_hi << 8 | ver_lo).

  @retval EFI_SUCCESS            Version read and validated.
  @retval EFI_DEVICE_ERROR       Checksum validation failed.
  @retval EFI_NOT_FOUND          MCU not responding.
**/
STATIC
EFI_STATUS
FanMcuReadVersion (
  OUT UINT16  *Version
  )
{
  EFI_STATUS  Status;
  UINT8       Buf[4];

  Status = FanMcuReadReg (FAN_MCU_REG_VERSION, Buf, sizeof (Buf));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Buf[2] != 1) {
    DEBUG ((DEBUG_ERROR, "FanDxe: Version buf[2] = 0x%02x, expected 0x01\n", Buf[2]));
    return EFI_DEVICE_ERROR;
  }

  if ((Buf[0] ^ Buf[1] ^ Buf[2]) != Buf[3]) {
    DEBUG ((DEBUG_ERROR, "FanDxe: Version checksum mismatch\n"));
    return EFI_DEVICE_ERROR;
  }

  *Version = (UINT16)((Buf[1] << 8) | Buf[0]);
  return EFI_SUCCESS;
}

/**
  Send a PWM command to the MCU.

  Writes 3 bytes to register 0x98: [MCU_CMD_PWM, pwm_value, pwm_value ^ MCU_CMD_PWM].

  @param[in] PwmValue  PWM value (0-255).

  @retval EFI_SUCCESS  Command sent.
**/
STATIC
EFI_STATUS
FanMcuSendPwm (
  IN UINT8  PwmValue
  )
{
  UINT8  Data[3];

  Data[0] = FAN_MCU_CMD_PWM;
  Data[1] = PwmValue;
  Data[2] = PwmValue ^ FAN_MCU_CMD_PWM;

  return FanMcuWriteReg (FAN_MCU_REG_COMMAND, Data, sizeof (Data));
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

/**
  Periodic timer callback to send PWM keepalive.

  @param[in] Event    Timer event.
  @param[in] Context  Not used.
**/
STATIC
VOID
EFIAPI
FanKeepaliveTimerCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  Status = FanMcuSendPwm (FAN_PWM_VALUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FanDxe: Keepalive PWM failed: %r\n", Status));
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
FanDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT16      McuVersion;

  //
  // Find the I2C master for bus 1 (i2c-gen)
  //
  Status = FindI2cMasterBus1 (&mI2cMaster);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FanDxe: I2C master for bus %u not found: %r\n",
      FAN_PARENT_BUS_INSTANCE, Status));
    return EFI_NOT_FOUND;
  }

  //
  // Read MCU version to verify the fan controller is present
  //
  Status = FanMcuReadVersion (&McuVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FanDxe: MCU version read failed: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "FanDxe: MCU version 0x%04x\n", McuVersion));

  //
  // Send initial PWM command
  //
  Status = FanMcuSendPwm (FAN_PWM_VALUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FanDxe: Initial PWM command failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "FanDxe: PWM set to %u\n", FAN_PWM_VALUE));

  //
  // Create periodic timer for keepalive
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  FanKeepaliveTimerCallback,
                  NULL,
                  &mKeepaliveEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FanDxe: CreateEvent failed: %r\n", Status));
    return Status;
  }

  Status = gBS->SetTimer (mKeepaliveEvent, TimerPeriodic, FAN_KEEPALIVE_INTERVAL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FanDxe: SetTimer failed: %r\n", Status));
    gBS->CloseEvent (mKeepaliveEvent);
    return Status;
  }

  return EFI_SUCCESS;
}
