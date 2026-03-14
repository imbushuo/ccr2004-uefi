/** @file
  DesignWare I2C controller DXE driver for MikroTik CCR2004.

  Drives the two snps,designware-i2c controllers on the Alpine V2 SoC
  (i2c-pld at 0xFD880000 and i2c-gen at 0xFD894000). Polling-based,
  no interrupts.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "DwI2cDxe.h"
#include <Guid/EventGroup.h>

//
// Controller base addresses (from Alpine V2 DTS)
//
STATIC CONST UINTN  mDwI2cBase[DW_I2C_BUS_COUNT] = {
  0xFD880000,   // i2c-pld
  0xFD894000,   // i2c-gen
};

STATIC CONST UINT32  mDwI2cSpeed[DW_I2C_BUS_COUNT] = {
  100000,       // i2c-pld: 100 kHz
  100000,       // i2c-gen: 100 kHz
};

//
// Device path template
//
STATIC DW_I2C_DEVICE_PATH  mDwI2cDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(OFFSET_OF (DW_I2C_DEVICE_PATH, End)),
        (UINT8)(OFFSET_OF (DW_I2C_DEVICE_PATH, End) >> 8),
      },
    },
    // GUID matches FILE_GUID from INF
    { 0x3a8e7b2c, 0xd4f1, 0x4905, { 0xb6, 0xc8, 0x1e, 0x9a, 0x0f, 0x3d, 0x5c, 0x72 } }
  },
  0,  // Instance (filled per controller)
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

STATIC
UINT32
DwI2cRead (
  IN DW_I2C_CONTEXT  *Ctx,
  IN UINTN           Offset
  )
{
  return MmioRead32 (Ctx->BaseAddress + Offset);
}

STATIC
VOID
DwI2cWrite (
  IN DW_I2C_CONTEXT  *Ctx,
  IN UINTN           Offset,
  IN UINT32          Value
  )
{
  MmioWrite32 (Ctx->BaseAddress + Offset, Value);
}

/**
  Enable or disable the I2C controller, waiting for ENABLE_STATUS confirmation.
**/
STATIC
EFI_STATUS
DwI2cEnable (
  IN DW_I2C_CONTEXT  *Ctx,
  IN UINT32          Enable
  )
{
  UINTN  Count;

  DwI2cWrite (Ctx, DW_IC_ENABLE, Enable);

  for (Count = 0; Count < DW_MAX_STATUS_POLL_COUNT; Count++) {
    if ((DwI2cRead (Ctx, DW_IC_ENABLE_STATUS) & 0x01) == Enable) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (Ctx->PollingTimeUs);
  }

  DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u enable/disable timeout\n", Ctx->BusIndex));
  return EFI_TIMEOUT;
}

/**
  Check for errors and clear them.
**/
STATIC
UINT32
DwI2cCheckErrors (
  IN DW_I2C_CONTEXT  *Ctx
  )
{
  UINT32  Errors;

  Errors = DwI2cRead (Ctx, DW_IC_RAW_INTR_STAT) & DW_IC_ERR_CONDITION;

  if ((Errors & DW_IC_INTR_RX_UNDER) != 0) {
    DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u RX_UNDER\n", Ctx->BusIndex));
    DwI2cRead (Ctx, DW_IC_CLR_RX_UNDER);
  }
  if ((Errors & DW_IC_INTR_RX_OVER) != 0) {
    DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u RX_OVER\n", Ctx->BusIndex));
    DwI2cRead (Ctx, DW_IC_CLR_RX_OVER);
  }
  if ((Errors & DW_IC_INTR_TX_ABRT) != 0) {
    DEBUG ((DEBUG_VERBOSE, "DwI2cDxe: Bus %u TX_ABRT source=0x%08x\n",
      Ctx->BusIndex, DwI2cRead (Ctx, DW_IC_TX_ABRT_SOURCE)));
    DwI2cRead (Ctx, DW_IC_CLR_TX_ABRT);
  }

  return Errors;
}

/**
  Wait for bus master activity to cease.
**/
STATIC
EFI_STATUS
DwI2cWaitBusNotBusy (
  IN DW_I2C_CONTEXT  *Ctx
  )
{
  UINTN  Count;

  for (Count = 0; Count < DW_MAX_MST_ACTIVITY_POLL_COUNT; Count++) {
    if ((DwI2cRead (Ctx, DW_IC_STATUS) & DW_IC_STATUS_MST_ACTIVITY) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (DW_POLL_MST_ACTIVITY_INTERVAL_US);
  }

  DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u busy timeout\n", Ctx->BusIndex));
  return EFI_TIMEOUT;
}

/**
  Wait for TX FIFO to have space.
**/
STATIC
EFI_STATUS
DwI2cWaitTxData (
  IN DW_I2C_CONTEXT  *Ctx
  )
{
  UINTN  Count;

  for (Count = 0; Count < DW_MAX_TRANSFER_POLL_COUNT; Count++) {
    if (DwI2cRead (Ctx, DW_IC_TXFLR) < Ctx->TxFifo) {
      return EFI_SUCCESS;
    }
    if ((DwI2cCheckErrors (Ctx) & DW_IC_INTR_TX_ABRT) != 0) {
      return EFI_ABORTED;
    }
    MicroSecondDelay (Ctx->PollingTimeUs);
  }

  DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u TX FIFO full timeout\n", Ctx->BusIndex));
  return EFI_TIMEOUT;
}

/**
  Wait for RX FIFO to have data.
**/
STATIC
EFI_STATUS
DwI2cWaitRxData (
  IN DW_I2C_CONTEXT  *Ctx
  )
{
  UINTN  Count;

  for (Count = 0; Count < DW_MAX_TRANSFER_POLL_COUNT; Count++) {
    if ((DwI2cRead (Ctx, DW_IC_STATUS) & DW_IC_STATUS_RFNE) != 0) {
      return EFI_SUCCESS;
    }
    if ((DwI2cCheckErrors (Ctx) & DW_IC_INTR_TX_ABRT) != 0) {
      return EFI_ABORTED;
    }
    MicroSecondDelay (Ctx->PollingTimeUs);
  }

  DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u RX FIFO empty timeout\n", Ctx->BusIndex));
  return EFI_TIMEOUT;
}

/**
  Wait for TX FIFO empty and STOP_DET.
**/
STATIC
EFI_STATUS
DwI2cFinish (
  IN DW_I2C_CONTEXT  *Ctx
  )
{
  UINTN  Count;

  //
  // Wait for TX FIFO empty
  //
  for (Count = 0; Count < DW_MAX_TRANSFER_POLL_COUNT; Count++) {
    if ((DwI2cRead (Ctx, DW_IC_STATUS) & DW_IC_STATUS_TFE) != 0) {
      break;
    }
    MicroSecondDelay (Ctx->PollingTimeUs);
  }
  if (Count >= DW_MAX_TRANSFER_POLL_COUNT) {
    DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u TX empty timeout\n", Ctx->BusIndex));
    return EFI_TIMEOUT;
  }

  //
  // Wait for STOP detected
  //
  for (Count = 0; Count < DW_MAX_TRANSFER_POLL_COUNT; Count++) {
    if ((DwI2cRead (Ctx, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_STOP_DET) != 0) {
      DwI2cRead (Ctx, DW_IC_CLR_STOP_DET);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (Ctx->PollingTimeUs);
  }

  DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u STOP_DET timeout\n", Ctx->BusIndex));
  return EFI_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Hardware initialization
// ---------------------------------------------------------------------------

/**
  Initialize the DesignWare I2C controller hardware.
**/
STATIC
VOID
DwI2cHwInit (
  IN DW_I2C_CONTEXT  *Ctx
  )
{
  UINT32  Version;
  UINT32  IcCon;

  //
  // Disable controller and mask all interrupts
  //
  DwI2cEnable (Ctx, 0);
  DwI2cWrite (Ctx, DW_IC_INTR_MASK, 0);

  //
  // Program SCL timing for both SS and FS (pre-calculated for 500 MHz sbclk)
  //
  DwI2cWrite (Ctx, DW_IC_SS_SCL_HCNT, DW_I2C_SS_SCL_HCNT_VAL);
  DwI2cWrite (Ctx, DW_IC_SS_SCL_LCNT, DW_I2C_SS_SCL_LCNT_VAL);
  DwI2cWrite (Ctx, DW_IC_FS_SCL_HCNT, DW_I2C_FS_SCL_HCNT_VAL);
  DwI2cWrite (Ctx, DW_IC_FS_SCL_LCNT, DW_I2C_FS_SCL_LCNT_VAL);

  //
  // Program SDA hold time (version check per DW spec)
  //
  Version = DwI2cRead (Ctx, DW_IC_COMP_VERSION);
  if (Version >= DW_IC_SDA_HOLD_MIN_VERS) {
    DwI2cWrite (Ctx, DW_IC_SDA_HOLD, DW_I2C_SDA_HOLD_VAL);
  }

  //
  // Configure: master mode, slave disabled, restart enabled, standard speed
  //
  IcCon = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE |
          DW_IC_CON_RESTART_EN | DW_IC_CON_SPEED_STD;
  DwI2cWrite (Ctx, DW_IC_CON, IcCon);

  //
  // Clear all pending interrupts
  //
  DwI2cRead (Ctx, DW_IC_CLR_INTR);
}

// ---------------------------------------------------------------------------
// EFI_I2C_MASTER_PROTOCOL implementation
// ---------------------------------------------------------------------------

/**
  Set the I2C bus frequency.
**/
STATIC
EFI_STATUS
EFIAPI
DwI2cSetBusFrequency (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This,
  IN OUT UINTN                      *BusClockHertz
  )
{
  DW_I2C_CONTEXT  *Ctx;
  UINT32          IcCon;

  if (This == NULL || BusClockHertz == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = DW_I2C_FROM_MASTER (This);
  EfiAcquireLock (&Ctx->Lock);

  DwI2cEnable (Ctx, 0);

  IcCon = DwI2cRead (Ctx, DW_IC_CON);
  IcCon &= ~DW_IC_CON_SPEED_MASK;

  if (*BusClockHertz <= 100000) {
    IcCon |= DW_IC_CON_SPEED_STD;
    *BusClockHertz = 100000;
  } else {
    IcCon |= DW_IC_CON_SPEED_FAST;
    *BusClockHertz = 400000;
  }

  DwI2cWrite (Ctx, DW_IC_CON, IcCon);
  Ctx->BusSpeedHz = (UINT32)*BusClockHertz;
  Ctx->PollingTimeUs = DW_POLL_INTERVAL_US (Ctx->BusSpeedHz);

  DwI2cEnable (Ctx, 1);
  EfiReleaseLock (&Ctx->Lock);

  return EFI_SUCCESS;
}

/**
  Reset the I2C controller.
**/
STATIC
EFI_STATUS
EFIAPI
DwI2cReset (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This
  )
{
  DW_I2C_CONTEXT  *Ctx;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = DW_I2C_FROM_MASTER (This);
  EfiAcquireLock (&Ctx->Lock);
  DwI2cHwInit (Ctx);
  EfiReleaseLock (&Ctx->Lock);

  return EFI_SUCCESS;
}

/**
  Start an I2C transaction.
**/
STATIC
EFI_STATUS
EFIAPI
DwI2cStartRequest (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This,
  IN UINTN                          SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET         *RequestPacket,
  IN EFI_EVENT                      Event      OPTIONAL,
  OUT EFI_STATUS                    *I2cStatus OPTIONAL
  )
{
  DW_I2C_CONTEXT      *Ctx;
  EFI_STATUS          Status;
  UINTN               OpIdx;
  EFI_I2C_OPERATION   *Op;
  UINTN               ByteIdx;
  UINT32              CmdData;
  BOOLEAN             IsLastOp;
  BOOLEAN             IsLastByte;
  BOOLEAN             NeedRestart;

  if (This == NULL || RequestPacket == NULL) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }
  if (RequestPacket->OperationCount == 0) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  Ctx = DW_I2C_FROM_MASTER (This);
  EfiAcquireLock (&Ctx->Lock);

  //
  // Wait for bus not busy
  //
  Status = DwI2cWaitBusNotBusy (Ctx);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&Ctx->Lock);
    goto Done;
  }

  //
  // Disable, set target address, enable
  //
  DwI2cEnable (Ctx, 0);
  DwI2cWrite (Ctx, DW_IC_TAR, (UINT32)(SlaveAddress & 0x3FF));
  DwI2cEnable (Ctx, 1);

  //
  // Clear any pending interrupts
  //
  DwI2cRead (Ctx, DW_IC_CLR_INTR);

  //
  // Process each operation
  //
  for (OpIdx = 0; OpIdx < RequestPacket->OperationCount; OpIdx++) {
    Op = &RequestPacket->Operation[OpIdx];
    IsLastOp = (OpIdx == RequestPacket->OperationCount - 1);
    NeedRestart = (OpIdx > 0);

    if ((Op->Flags & I2C_FLAG_READ) != 0) {
      //
      // Read operation: issue read commands byte-at-a-time
      //
      for (ByteIdx = 0; ByteIdx < Op->LengthInBytes; ByteIdx++) {
        Status = DwI2cWaitTxData (Ctx);
        if (EFI_ERROR (Status)) {
          goto Abort;
        }

        IsLastByte = IsLastOp && (ByteIdx == Op->LengthInBytes - 1);
        CmdData = DW_IC_DATA_CMD_CMD;  // read bit
        if (IsLastByte) {
          CmdData |= DW_IC_DATA_CMD_STOP;
        }
        if (NeedRestart && ByteIdx == 0) {
          CmdData |= DW_IC_DATA_CMD_RESTART;
        }

        DwI2cWrite (Ctx, DW_IC_DATA_CMD, CmdData);

        Status = DwI2cWaitRxData (Ctx);
        if (EFI_ERROR (Status)) {
          goto Abort;
        }

        Op->Buffer[ByteIdx] = (UINT8)(DwI2cRead (Ctx, DW_IC_DATA_CMD) & DW_IC_DATA_CMD_DAT_MASK);
      }
    } else {
      //
      // Write operation
      //
      for (ByteIdx = 0; ByteIdx < Op->LengthInBytes; ByteIdx++) {
        Status = DwI2cWaitTxData (Ctx);
        if (EFI_ERROR (Status)) {
          goto Abort;
        }

        IsLastByte = IsLastOp && (ByteIdx == Op->LengthInBytes - 1);
        CmdData = Op->Buffer[ByteIdx] & DW_IC_DATA_CMD_DAT_MASK;
        if (IsLastByte) {
          CmdData |= DW_IC_DATA_CMD_STOP;
        }
        if (NeedRestart && ByteIdx == 0) {
          CmdData |= DW_IC_DATA_CMD_RESTART;
        }

        DwI2cWrite (Ctx, DW_IC_DATA_CMD, CmdData);
      }
    }
  }

  //
  // Wait for transaction to complete
  //
  Status = DwI2cFinish (Ctx);
  if (EFI_ERROR (Status)) {
    goto Abort;
  }

  //
  // Check for errors
  //
  {
    UINT32  Errors;
    Errors = DwI2cCheckErrors (Ctx);
    if ((Errors & DW_IC_INTR_TX_ABRT) != 0) {
      Status = EFI_NO_RESPONSE;
    } else if ((Errors & (DW_IC_INTR_RX_UNDER | DW_IC_INTR_RX_OVER)) != 0) {
      Status = EFI_DEVICE_ERROR;
    } else {
      Status = EFI_SUCCESS;
    }
  }

  DwI2cWaitBusNotBusy (Ctx);
  DwI2cEnable (Ctx, 0);
  EfiReleaseLock (&Ctx->Lock);
  goto Done;

Abort:
  //
  // Send STOP on abort
  //
  DwI2cWrite (Ctx, DW_IC_DATA_CMD, DW_IC_DATA_CMD_STOP);
  DwI2cFinish (Ctx);
  DwI2cCheckErrors (Ctx);
  DwI2cWaitBusNotBusy (Ctx);
  DwI2cEnable (Ctx, 0);
  EfiReleaseLock (&Ctx->Lock);

  if (Status == EFI_ABORTED) {
    Status = EFI_NO_RESPONSE;
  }

Done:
  if (I2cStatus != NULL) {
    *I2cStatus = Status;
  }
  if (Event != NULL) {
    gBS->SignalEvent (Event);
  }
  return Status;
}

// ---------------------------------------------------------------------------
// EFI_I2C_ENUMERATE_PROTOCOL (stub)
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
DwI2cEnumerate (
  IN CONST EFI_I2C_ENUMERATE_PROTOCOL  *This,
  IN OUT CONST EFI_I2C_DEVICE          **Device
  )
{
  //
  // No devices enumerated at the bus controller level.
  // Peripheral drivers will be added later.
  //
  if (Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Device = NULL;
  return EFI_NO_MAPPING;
}

STATIC
EFI_STATUS
EFIAPI
DwI2cGetBusFrequency (
  IN CONST EFI_I2C_ENUMERATE_PROTOCOL  *This,
  IN UINTN                             I2cBusConfiguration,
  OUT UINTN                            *BusClockHertz
  )
{
  DW_I2C_CONTEXT  *Ctx;

  if (This == NULL || BusClockHertz == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (I2cBusConfiguration != 0) {
    return EFI_NO_MAPPING;
  }

  Ctx = DW_I2C_FROM_ENUMERATE (This);
  *BusClockHertz = Ctx->BusSpeedHz;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL (stub)
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
DwI2cEnableBusConfiguration (
  IN CONST EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL  *This,
  IN UINTN                                                I2cBusConfiguration,
  IN EFI_EVENT                                            Event      OPTIONAL,
  IN EFI_STATUS                                           *I2cStatus OPTIONAL
  )
{
  if (I2cBusConfiguration != 0) {
    return EFI_NO_MAPPING;
  }

  if (I2cStatus != NULL) {
    *I2cStatus = EFI_SUCCESS;
  }
  if (Event != NULL) {
    gBS->SignalEvent (Event);
  }
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EndOfDxe callback: connect I2cHostDxe
// ---------------------------------------------------------------------------

STATIC
VOID
EFIAPI
DwI2cOnEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DW_I2C_DEVICE_PATH        *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePathPointer;
  EFI_HANDLE                DeviceHandle;
  EFI_STATUS                Status;

  gBS->CloseEvent (Event);

  DevicePath = AllocateCopyPool (sizeof (mDwI2cDevicePathTemplate),
                                 &mDwI2cDevicePathTemplate);
  if (DevicePath == NULL) {
    return;
  }

  do {
    DevicePathPointer = (EFI_DEVICE_PATH_PROTOCOL *)DevicePath;
    Status = gBS->LocateDevicePath (
                    &gEfiI2cMasterProtocolGuid,
                    &DevicePathPointer,
                    &DeviceHandle
                    );
    if (EFI_ERROR (Status)) {
      break;
    }

    Status = gBS->ConnectController (DeviceHandle, NULL, NULL, TRUE);
    DEBUG ((DEBUG_INFO, "DwI2cDxe: ConnectController Bus %u => %r\n",
      DevicePath->Instance, Status));

    DevicePath->Instance++;
  } while (TRUE);

  FreePool (DevicePath);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
DwI2cDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS          Status;
  UINTN               Index;
  DW_I2C_CONTEXT      *Ctx;
  DW_I2C_DEVICE_PATH  *DevicePath;
  UINT32              Param;
  EFI_EVENT           EndOfDxeEvent;

  for (Index = 0; Index < DW_I2C_BUS_COUNT; Index++) {
    //
    // Allocate context
    //
    Ctx = AllocateZeroPool (sizeof (DW_I2C_CONTEXT));
    if (Ctx == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Ctx->Signature   = DW_I2C_SIGNATURE;
    Ctx->Handle      = NULL;
    Ctx->BaseAddress = mDwI2cBase[Index];
    Ctx->BusIndex    = (UINT32)Index;
    Ctx->BusSpeedHz  = mDwI2cSpeed[Index];
    Ctx->PollingTimeUs = DW_POLL_INTERVAL_US (Ctx->BusSpeedHz);
    EfiInitializeLock (&Ctx->Lock, TPL_NOTIFY);

    //
    // Read FIFO depths from hardware
    //
    Param = MmioRead32 (Ctx->BaseAddress + DW_IC_COMP_PARAM_1);
    Ctx->TxFifo = DW_IC_COMP_PARAM_1_TX_BUFFER_DEPTH (Param);
    Ctx->RxFifo = DW_IC_COMP_PARAM_1_RX_BUFFER_DEPTH (Param);

    DEBUG ((DEBUG_INFO, "DwI2cDxe: Bus %u at 0x%lx, TX FIFO %u, RX FIFO %u\n",
      Ctx->BusIndex, (UINT64)Ctx->BaseAddress, Ctx->TxFifo, Ctx->RxFifo));

    //
    // Initialize hardware
    //
    DwI2cHwInit (Ctx);

    //
    // Wire up protocol functions
    //
    Ctx->I2cMaster.SetBusFrequency = DwI2cSetBusFrequency;
    Ctx->I2cMaster.Reset           = DwI2cReset;
    Ctx->I2cMaster.StartRequest    = DwI2cStartRequest;
    Ctx->I2cMaster.I2cControllerCapabilities = &Ctx->Capabilities;

    Ctx->Capabilities.StructureSizeInBytes = sizeof (EFI_I2C_CONTROLLER_CAPABILITIES);
    Ctx->Capabilities.MaximumReceiveBytes  = 0;  // no limit
    Ctx->Capabilities.MaximumTransmitBytes = 0;  // no limit
    Ctx->Capabilities.MaximumTotalBytes    = 0;  // no limit

    Ctx->I2cEnumerate.Enumerate       = DwI2cEnumerate;
    Ctx->I2cEnumerate.GetBusFrequency = DwI2cGetBusFrequency;

    Ctx->I2cBusConf.EnableI2cBusConfiguration = DwI2cEnableBusConfiguration;

    //
    // Allocate device path
    //
    DevicePath = AllocateCopyPool (sizeof (mDwI2cDevicePathTemplate),
                                   &mDwI2cDevicePathTemplate);
    if (DevicePath == NULL) {
      FreePool (Ctx);
      return EFI_OUT_OF_RESOURCES;
    }
    DevicePath->Instance = (UINT32)Index;

    //
    // Install protocols on a new handle
    //
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Ctx->Handle,
                    &gEfiI2cMasterProtocolGuid,
                    &Ctx->I2cMaster,
                    &gEfiI2cEnumerateProtocolGuid,
                    &Ctx->I2cEnumerate,
                    &gEfiI2cBusConfigurationManagementProtocolGuid,
                    &Ctx->I2cBusConf,
                    &gEfiDevicePathProtocolGuid,
                    (EFI_DEVICE_PATH_PROTOCOL *)DevicePath,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "DwI2cDxe: Bus %u protocol install failed: %r\n",
        Index, Status));
      FreePool (DevicePath);
      FreePool (Ctx);
      return Status;
    }
  }

  //
  // Register EndOfDxe event so I2cHostDxe can auto-bind
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  DwI2cOnEndOfDxe,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
