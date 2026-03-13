/** @file
  SEC phase header for MikroTik CCR2004.

  Copyright (c) 2011 - 2020, Arm Limited. All rights reserved.<BR>
  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <PiPei.h>

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PerformanceLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/PrePiLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>

#include <Ppi/GuidedSectionExtraction.h>
#include <Ppi/SecPerformance.h>

extern UINT64  mSystemMemoryEnd;

/**
  Entrypoint of the memory PEIM driver.

  @param[in]  UefiMemoryBase  The base of the PI/UEFI memory region
  @param[in]  UefiMemorySize  The size of the PI/UEFI memory region

  @return  Whether the memory PEIM driver executed successfully
**/
EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS  UefiMemoryBase,
  IN UINT64                UefiMemorySize
  );

/**
  Entrypoint of platform PEIM driver.

  @return  Whether the platform PEIM driver executed successfully
**/
EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  );

/**
  Architecture specific initialization routine.
**/
VOID
ArchInitialize (
  VOID
  );
