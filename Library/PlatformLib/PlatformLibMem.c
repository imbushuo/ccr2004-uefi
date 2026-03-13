/** @file
  Virtual memory map for MikroTik CCR2004 (Alpine V2 SoC).

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

#define CCR2004_DRAM_PRIMARY_BASE   0x00000000ULL
#define CCR2004_DRAM_PRIMARY_SIZE   0x80000000ULL  // 2GB
#define CCR2004_DRAM_SECONDARY_BASE 0x80000000ULL
#define CCR2004_DRAM_SECONDARY_SIZE 0x40000000ULL  // 1GB
#define CCR2004_PERIPH_BASE         0xF0000000ULL
#define CCR2004_PERIPH_SIZE         0x10000000ULL  // 256MB

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  4

/**
  Return the Virtual Memory Map of the platform.
**/
VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;
  UINTN                         Index;

  ASSERT (VirtualMemoryMap != NULL);

  VirtualMemoryTable = AllocatePool (
                         sizeof (ARM_MEMORY_REGION_DESCRIPTOR) *
                         MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS
                         );
  if (VirtualMemoryTable == NULL) {
    DEBUG ((DEBUG_ERROR, "ArmPlatformGetVirtualMemoryMap: failed to allocate memory\n"));
    ASSERT (FALSE);
    return;
  }

  Index = 0;

  // DRAM primary region (2GB)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_DRAM_PRIMARY_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_DRAM_PRIMARY_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_DRAM_PRIMARY_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // DRAM secondary region (1GB)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_DRAM_SECONDARY_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_DRAM_SECONDARY_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_DRAM_SECONDARY_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // SoC peripherals (GIC, UART, etc.)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PERIPH_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PERIPH_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PERIPH_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // End of table sentinel
  VirtualMemoryTable[Index].PhysicalBase = 0;
  VirtualMemoryTable[Index].VirtualBase  = 0;
  VirtualMemoryTable[Index].Length       = 0;
  VirtualMemoryTable[Index].Attributes   = 0;

  *VirtualMemoryMap = VirtualMemoryTable;
}
