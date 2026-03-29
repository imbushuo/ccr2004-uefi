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

//
// Peripheral space split around PBS SRAM:
//   0xF0000000 - 0xFD8A3FFF : peripherals (device)
//   0xFD8A4000 - 0xFD8A7FFF : PBS SRAM 16KB (uncached normal — allows unaligned access)
//   0xFD8A8000 - 0xFFFFFFFF : peripherals (device) — includes PBS regfile, NAND, etc.
//
#define CCR2004_PERIPH1_BASE        0xF0000000ULL
#define CCR2004_PERIPH1_SIZE        0x0D8A4000ULL  // F0000000..FD8A3FFF

#define CCR2004_PBS_SRAM_BASE       0xFD8A4000ULL
#define CCR2004_PBS_SRAM_SIZE       0x00004000ULL  // 16KB

#define CCR2004_PERIPH2_BASE        0xFD8A8000ULL
#define CCR2004_PERIPH2_SIZE        0x02758000ULL  // FD8A8000..FFFFFFFF

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  6

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

  // Peripherals below PBS SRAM (device memory)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PERIPH1_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PERIPH1_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PERIPH1_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // PBS SRAM (uncached normal — permits unaligned access for shared data structs)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PBS_SRAM_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PBS_SRAM_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PBS_SRAM_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  Index++;

  // Peripherals above PBS SRAM (device memory)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PERIPH2_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PERIPH2_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PERIPH2_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // End of table sentinel
  VirtualMemoryTable[Index].PhysicalBase = 0;
  VirtualMemoryTable[Index].VirtualBase  = 0;
  VirtualMemoryTable[Index].Length       = 0;
  VirtualMemoryTable[Index].Attributes   = 0;

  *VirtualMemoryMap = VirtualMemoryTable;
}
