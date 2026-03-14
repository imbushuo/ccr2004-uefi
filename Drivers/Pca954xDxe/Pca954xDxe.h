/** @file
  PCA954x I2C mux/switch DXE driver header for MikroTik CCR2004.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef PCA954X_DXE_H_
#define PCA954X_DXE_H_

#include <Uefi.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/I2cEnumerate.h>
#include <Protocol/I2cBusConfigurationManagement.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiLib.h>

//
// PCA9546 configuration
//
#define PCA9546_SLAVE_ADDRESS   0x70
#define PCA9546_CHANNEL_COUNT   4
#define PCA954X_NO_CHANNEL      0xFF

//
// Parent bus instance (i2c-gen = bus 1)
//
#define PCA954X_PARENT_BUS_INSTANCE  1

//
// DwI2cDxe vendor GUID (must match DwI2cDxe.inf FILE_GUID)
//
#define DW_I2C_DEVICE_PATH_GUID \
  { 0x3a8e7b2c, 0xd4f1, 0x4905, { 0xb6, 0xc8, 0x1e, 0x9a, 0x0f, 0x3d, 0x5c, 0x72 } }

//
// Replicate DW_I2C_DEVICE_PATH for parent handle matching
//
typedef struct {
  VENDOR_DEVICE_PATH          Guid;
  UINT32                      Instance;
  EFI_DEVICE_PATH_PROTOCOL    End;
} DW_I2C_DEVICE_PATH;

//
// Pca954xDxe device path (FILE_GUID as vendor GUID + Channel)
//
typedef struct {
  VENDOR_DEVICE_PATH          Guid;
  UINT32                      Channel;
  EFI_DEVICE_PATH_PROTOCOL    End;
} PCA954X_DEVICE_PATH;

//
// Shared mux context (one per physical PCA9546)
//
#define PCA954X_MUX_SIGNATURE  SIGNATURE_32 ('P', 'M', 'U', 'X')

typedef struct {
  UINT32                       Signature;
  EFI_I2C_MASTER_PROTOCOL      *ParentI2cMaster;
  EFI_LOCK                     Lock;
  UINT32                       ActiveChannel;
} PCA954X_MUX_CONTEXT;

//
// Per-channel context (one per child handle)
//
#define PCA954X_CHANNEL_SIGNATURE  SIGNATURE_32 ('P', 'C', 'H', 'N')

typedef struct {
  UINT32                                           Signature;
  EFI_HANDLE                                       Handle;
  UINT32                                           Channel;
  PCA954X_MUX_CONTEXT                              *Mux;
  EFI_I2C_MASTER_PROTOCOL                          I2cMaster;
  EFI_I2C_ENUMERATE_PROTOCOL                       I2cEnumerate;
  EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL    I2cBusConf;
  EFI_I2C_CONTROLLER_CAPABILITIES                  Capabilities;
} PCA954X_CHANNEL_CONTEXT;

//
// CR macros for protocol -> context recovery
//
#define PCA954X_CHAN_FROM_MASTER(a) \
  CR (a, PCA954X_CHANNEL_CONTEXT, I2cMaster, PCA954X_CHANNEL_SIGNATURE)
#define PCA954X_CHAN_FROM_ENUMERATE(a) \
  CR (a, PCA954X_CHANNEL_CONTEXT, I2cEnumerate, PCA954X_CHANNEL_SIGNATURE)
#define PCA954X_CHAN_FROM_BUSCONF(a) \
  CR (a, PCA954X_CHANNEL_CONTEXT, I2cBusConf, PCA954X_CHANNEL_SIGNATURE)

#endif // PCA954X_DXE_H_
