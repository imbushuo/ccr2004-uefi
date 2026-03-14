/** @file
  PCA954x I2C mux/switch DXE driver for MikroTik CCR2004.

  Creates 4 child I2C bus handles (one per channel) behind the PCA9546
  4-channel I2C switch at address 0x70 on the i2c-gen bus (bus 1).
  Each child handle produces I2cMaster, I2cEnumerate,
  I2cBusConfigurationManagement, and DevicePath protocols so that
  I2cHostDxe can auto-bind.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Pca954xDxe.h"
#include <Guid/EventGroup.h>

//
// Static storage for child handles
//
STATIC EFI_HANDLE  mChildHandles[PCA9546_CHANNEL_COUNT];
STATIC UINT32      mChildHandleCount;

//
// Device path template for child handles.
// GUID is this driver's FILE_GUID from the INF.
//
STATIC PCA954X_DEVICE_PATH  mPca954xDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(OFFSET_OF (PCA954X_DEVICE_PATH, End)),
        (UINT8)(OFFSET_OF (PCA954X_DEVICE_PATH, End) >> 8),
      },
    },
    // FILE_GUID for Pca954xDxe
    { 0xb5e2a6d1, 0x7c3f, 0x4e8a, { 0x91, 0x0d, 0x2b, 0x4f, 0x6c, 0x8e, 0xa3, 0x57 } }
  },
  0,  // Channel (filled per child)
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
// Find parent I2C master (bus 1)
// ---------------------------------------------------------------------------

/**
  Locate the EFI_I2C_MASTER_PROTOCOL for DwI2cDxe bus instance 1 (i2c-gen).

  Iterates all handles with I2cMaster, checks the device path for the
  DwI2cDxe vendor GUID and Instance == PCA954X_PARENT_BUS_INSTANCE.

  @param[out] ParentI2cMaster  Pointer to the parent I2C master protocol.

  @retval EFI_SUCCESS    Found and returned.
  @retval EFI_NOT_FOUND  No matching handle.
**/
STATIC
EFI_STATUS
FindParentI2cMaster (
  OUT EFI_I2C_MASTER_PROTOCOL  **ParentI2cMaster
  )
{
  EFI_STATUS                 Status;
  UINTN                      HandleCount;
  EFI_HANDLE                 *HandleBuffer;
  UINTN                      Index;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  VENDOR_DEVICE_PATH         *VendorDp;
  DW_I2C_DEVICE_PATH         *DwDevPath;
  EFI_GUID                   DwI2cGuid = DW_I2C_DEVICE_PATH_GUID;

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

    //
    // Check if this is a vendor device path with the DwI2cDxe GUID
    //
    if (DevicePathType (DevicePath) != HARDWARE_DEVICE_PATH ||
        DevicePathSubType (DevicePath) != HW_VENDOR_DP) {
      continue;
    }

    VendorDp = (VENDOR_DEVICE_PATH *)DevicePath;
    if (!CompareGuid (&VendorDp->Guid, &DwI2cGuid)) {
      continue;
    }

    //
    // Check Instance field
    //
    DwDevPath = (DW_I2C_DEVICE_PATH *)DevicePath;
    if (DwDevPath->Instance != PCA954X_PARENT_BUS_INSTANCE) {
      continue;
    }

    //
    // Found it
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiI2cMasterProtocolGuid,
                    (VOID **)ParentI2cMaster
                    );
    FreePool (HandleBuffer);
    return Status;
  }

  FreePool (HandleBuffer);
  return EFI_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Mux channel selection
// ---------------------------------------------------------------------------

/**
  Select a mux channel by writing to the PCA9546.

  @param[in] Mux      Shared mux context.
  @param[in] Channel  Channel to select (0..3).

  @retval EFI_SUCCESS  Channel selected (or already active).
**/
STATIC
EFI_STATUS
Pca954xSelectChannel (
  IN PCA954X_MUX_CONTEXT  *Mux,
  IN UINT32               Channel
  )
{
  EFI_STATUS              Status;
  UINT8                   RegVal;
  EFI_I2C_REQUEST_PACKET  Packet;

  if (Mux->ActiveChannel == Channel) {
    return EFI_SUCCESS;
  }

  RegVal = (UINT8)(1U << Channel);

  Packet.OperationCount = 1;
  Packet.Operation[0].Flags        = 0;  // write
  Packet.Operation[0].LengthInBytes = sizeof (RegVal);
  Packet.Operation[0].Buffer       = &RegVal;

  Status = Mux->ParentI2cMaster->StartRequest (
                                    Mux->ParentI2cMaster,
                                    PCA9546_SLAVE_ADDRESS,
                                    &Packet,
                                    NULL,
                                    NULL
                                    );
  if (EFI_ERROR (Status)) {
    Mux->ActiveChannel = PCA954X_NO_CHANNEL;
    DEBUG ((DEBUG_ERROR, "Pca954xDxe: Channel select %u failed: %r\n", Channel, Status));
  } else {
    Mux->ActiveChannel = Channel;
  }

  return Status;
}

// ---------------------------------------------------------------------------
// EFI_I2C_MASTER_PROTOCOL implementation (child)
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
Pca954xSetBusFrequency (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This,
  IN OUT UINTN                      *BusClockHertz
  )
{
  PCA954X_CHANNEL_CONTEXT  *Chan;

  if (This == NULL || BusClockHertz == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Chan = PCA954X_CHAN_FROM_MASTER (This);
  return Chan->Mux->ParentI2cMaster->SetBusFrequency (
                                        Chan->Mux->ParentI2cMaster,
                                        BusClockHertz
                                        );
}

STATIC
EFI_STATUS
EFIAPI
Pca954xReset (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This
  )
{
  PCA954X_CHANNEL_CONTEXT  *Chan;
  EFI_STATUS               Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Chan = PCA954X_CHAN_FROM_MASTER (This);
  Status = Chan->Mux->ParentI2cMaster->Reset (Chan->Mux->ParentI2cMaster);
  Chan->Mux->ActiveChannel = PCA954X_NO_CHANNEL;
  return Status;
}

/**
  Start an I2C transaction on a mux channel.

  Acquires the mux lock, selects the channel, forwards the transaction
  to the parent I2C master synchronously, then releases the lock.
**/
STATIC
EFI_STATUS
EFIAPI
Pca954xStartRequest (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This,
  IN UINTN                          SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET         *RequestPacket,
  IN EFI_EVENT                      Event      OPTIONAL,
  OUT EFI_STATUS                    *I2cStatus OPTIONAL
  )
{
  PCA954X_CHANNEL_CONTEXT  *Chan;
  EFI_STATUS               Status;

  if (This == NULL || RequestPacket == NULL) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  Chan = PCA954X_CHAN_FROM_MASTER (This);

  EfiAcquireLock (&Chan->Mux->Lock);

  //
  // Select mux channel
  //
  Status = Pca954xSelectChannel (Chan->Mux, Chan->Channel);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&Chan->Mux->Lock);
    goto Done;
  }

  //
  // Forward transaction to parent (always synchronous)
  //
  Status = Chan->Mux->ParentI2cMaster->StartRequest (
                                          Chan->Mux->ParentI2cMaster,
                                          SlaveAddress,
                                          RequestPacket,
                                          NULL,
                                          NULL
                                          );

  EfiReleaseLock (&Chan->Mux->Lock);

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
Pca954xEnumerate (
  IN CONST EFI_I2C_ENUMERATE_PROTOCOL  *This,
  IN OUT CONST EFI_I2C_DEVICE          **Device
  )
{
  if (Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Device = NULL;
  return EFI_NO_MAPPING;
}

STATIC
EFI_STATUS
EFIAPI
Pca954xGetBusFrequency (
  IN CONST EFI_I2C_ENUMERATE_PROTOCOL  *This,
  IN UINTN                             I2cBusConfiguration,
  OUT UINTN                            *BusClockHertz
  )
{
  if (This == NULL || BusClockHertz == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (I2cBusConfiguration != 0) {
    return EFI_NO_MAPPING;
  }

  *BusClockHertz = 100000;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL (stub)
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
Pca954xEnableBusConfiguration (
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
// EndOfDxe callback: connect I2cHostDxe to child handles
// ---------------------------------------------------------------------------

STATIC
VOID
EFIAPI
Pca954xOnEndOfDxe (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32      Index;
  EFI_STATUS  Status;

  gBS->CloseEvent (Event);

  for (Index = 0; Index < mChildHandleCount; Index++) {
    Status = gBS->ConnectController (mChildHandles[Index], NULL, NULL, TRUE);
    DEBUG ((DEBUG_INFO, "Pca954xDxe: ConnectController channel %u => %r\n",
      Index, Status));
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
Pca954xDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS               Status;
  EFI_I2C_MASTER_PROTOCOL  *ParentI2cMaster;
  PCA954X_MUX_CONTEXT      *Mux;
  UINT32                   Index;
  PCA954X_CHANNEL_CONTEXT  *Chan;
  PCA954X_DEVICE_PATH      *DevicePath;
  EFI_EVENT                EndOfDxeEvent;
  UINT8                    ProbeVal;
  EFI_I2C_REQUEST_PACKET   ProbePacket;

  //
  // Find the parent I2C master (bus 1, i2c-gen)
  //
  Status = FindParentI2cMaster (&ParentI2cMaster);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Pca954xDxe: Parent I2C master for bus %u not found: %r\n",
      PCA954X_PARENT_BUS_INSTANCE, Status));
    return EFI_NOT_FOUND;
  }
  DEBUG ((DEBUG_INFO, "Pca954xDxe: Found parent I2C master for bus %u\n",
    PCA954X_PARENT_BUS_INSTANCE));

  //
  // Allocate shared mux context
  //
  Mux = AllocateZeroPool (sizeof (PCA954X_MUX_CONTEXT));
  if (Mux == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Mux->Signature       = PCA954X_MUX_SIGNATURE;
  Mux->ParentI2cMaster = ParentI2cMaster;
  Mux->ActiveChannel   = PCA954X_NO_CHANNEL;
  EfiInitializeLock (&Mux->Lock, TPL_NOTIFY);

  //
  // Probe: write 0x00 to PCA9546 (deselect all channels)
  //
  ProbeVal = 0x00;
  ProbePacket.OperationCount = 1;
  ProbePacket.Operation[0].Flags        = 0;
  ProbePacket.Operation[0].LengthInBytes = sizeof (ProbeVal);
  ProbePacket.Operation[0].Buffer       = &ProbeVal;

  Status = ParentI2cMaster->StartRequest (
                              ParentI2cMaster,
                              PCA9546_SLAVE_ADDRESS,
                              &ProbePacket,
                              NULL,
                              NULL
                              );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Pca954xDxe: PCA9546 at 0x%02x not responding: %r\n",
      PCA9546_SLAVE_ADDRESS, Status));
    FreePool (Mux);
    return EFI_NOT_FOUND;
  }
  DEBUG ((DEBUG_INFO, "Pca954xDxe: PCA9546 at 0x%02x probed OK\n",
    PCA9546_SLAVE_ADDRESS));

  //
  // Create child handles for each channel
  //
  for (Index = 0; Index < PCA9546_CHANNEL_COUNT; Index++) {
    Chan = AllocateZeroPool (sizeof (PCA954X_CHANNEL_CONTEXT));
    if (Chan == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Chan->Signature = PCA954X_CHANNEL_SIGNATURE;
    Chan->Handle    = NULL;
    Chan->Channel   = Index;
    Chan->Mux       = Mux;

    //
    // Wire up I2cMaster protocol
    //
    Chan->I2cMaster.SetBusFrequency          = Pca954xSetBusFrequency;
    Chan->I2cMaster.Reset                    = Pca954xReset;
    Chan->I2cMaster.StartRequest             = Pca954xStartRequest;
    Chan->I2cMaster.I2cControllerCapabilities = &Chan->Capabilities;

    Chan->Capabilities.StructureSizeInBytes = sizeof (EFI_I2C_CONTROLLER_CAPABILITIES);
    Chan->Capabilities.MaximumReceiveBytes  = 0;
    Chan->Capabilities.MaximumTransmitBytes = 0;
    Chan->Capabilities.MaximumTotalBytes    = 0;

    //
    // Wire up I2cEnumerate protocol
    //
    Chan->I2cEnumerate.Enumerate       = Pca954xEnumerate;
    Chan->I2cEnumerate.GetBusFrequency = Pca954xGetBusFrequency;

    //
    // Wire up I2cBusConf protocol
    //
    Chan->I2cBusConf.EnableI2cBusConfiguration = Pca954xEnableBusConfiguration;

    //
    // Allocate device path
    //
    DevicePath = AllocateCopyPool (sizeof (mPca954xDevicePathTemplate),
                                   &mPca954xDevicePathTemplate);
    if (DevicePath == NULL) {
      FreePool (Chan);
      return EFI_OUT_OF_RESOURCES;
    }
    DevicePath->Channel = Index;

    //
    // Install protocols on a new handle
    //
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Chan->Handle,
                    &gEfiI2cMasterProtocolGuid,
                    &Chan->I2cMaster,
                    &gEfiI2cEnumerateProtocolGuid,
                    &Chan->I2cEnumerate,
                    &gEfiI2cBusConfigurationManagementProtocolGuid,
                    &Chan->I2cBusConf,
                    &gEfiDevicePathProtocolGuid,
                    (EFI_DEVICE_PATH_PROTOCOL *)DevicePath,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Pca954xDxe: Channel %u protocol install failed: %r\n",
        Index, Status));
      FreePool (DevicePath);
      FreePool (Chan);
      return Status;
    }

    mChildHandles[Index] = Chan->Handle;
    mChildHandleCount++;

    DEBUG ((DEBUG_INFO, "Pca954xDxe: Created channel %u\n", Index));
  }

  //
  // Register EndOfDxe event so I2cHostDxe can auto-bind to child handles
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  Pca954xOnEndOfDxe,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
