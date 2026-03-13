/** @file
  ArmPlatformLib implementation for MikroTik CCR2004.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
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

/**
  Return the current Boot Mode.
**/
EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

/**
  Initialize controllers that must setup in the normal world.
**/
RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  return RETURN_SUCCESS;
}

/**
  Return the Platform specific PPIs.
**/
VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  *PpiListSize = sizeof (mPlatformPpiTable);
  *PpiList     = mPlatformPpiTable;
}
