/** @file
  Virtual memory map for MikroTik CCR2004 (Alpine V2 SoC).

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

//
// DRAM layout (4 GB total):
//   0x00000000  - 0x0FFFFFFF  : DRAM below PCIe EP SHMEM (256 MB)
//   0x10000000  - 0x10000FFF  : PCIe EP shared memory (4 KB, uncached)
//   0x10001000  - 0x7FFFFFFF  : DRAM above PCIe EP SHMEM (~1.75 GB)
//   0x80000000  - 0xBFFFFFFF  : DRAM secondary (1 GB)
//   0x200000000 - 0x23FFFFFFF : DRAM high (1 GB, above 4 GB boundary)
//
#define CCR2004_DRAM_LOW_BASE       0x00000000ULL
#define CCR2004_DRAM_LOW_SIZE       0x10000000ULL  // 256 MB

#define CCR2004_PCIE_EP_SHMEM_BASE  0x10000000ULL
#define CCR2004_PCIE_EP_SHMEM_SIZE  0x00001000ULL  // 4 KB

#define CCR2004_DRAM_HIGH_BASE      0x10001000ULL
#define CCR2004_DRAM_HIGH_SIZE      0x6FFFF000ULL  // ~1.75 GB

#define CCR2004_DRAM_SECONDARY_BASE 0x80000000ULL
#define CCR2004_DRAM_SECONDARY_SIZE 0x40000000ULL  // 1 GB

#define CCR2004_DRAM_HIGHMEM_BASE   0x200000000ULL
#define CCR2004_DRAM_HIGHMEM_SIZE   0x040000000ULL // 1 GB

//
// Peripheral space split around PBS SRAM:
//   0xF0000000 - 0xFD8A3FFF : peripherals (device)
//   0xFD8A4000 - 0xFD8A7FFF : PBS SRAM 16KB (uncached normal)
//   0xFD8A8000 - 0xFFFFFFFF : peripherals (device)
//
#define CCR2004_PERIPH1_BASE        0xF0000000ULL
#define CCR2004_PERIPH1_SIZE        0x0D8A4000ULL

#define CCR2004_PBS_SRAM_BASE       0xFD8A4000ULL
#define CCR2004_PBS_SRAM_SIZE       0x00004000ULL

#define CCR2004_PERIPH2_BASE        0xFD8A8000ULL
#define CCR2004_PERIPH2_SIZE        0x02758000ULL

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  10

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

  // DRAM below PCIe EP shared memory (256 MB)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_DRAM_LOW_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_DRAM_LOW_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_DRAM_LOW_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // PCIe EP shared memory (4 KB, uncached — accessed by PCIe inbound ATU)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PCIE_EP_SHMEM_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PCIE_EP_SHMEM_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PCIE_EP_SHMEM_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  Index++;

  // DRAM above PCIe EP shared memory (~1.75 GB)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_DRAM_HIGH_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_DRAM_HIGH_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_DRAM_HIGH_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // DRAM secondary (1 GB)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_DRAM_SECONDARY_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_DRAM_SECONDARY_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_DRAM_SECONDARY_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // DRAM high (1 GB, above 4 GB boundary)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_DRAM_HIGHMEM_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_DRAM_HIGHMEM_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_DRAM_HIGHMEM_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // Peripherals below PBS SRAM (device)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PERIPH1_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PERIPH1_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PERIPH1_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // PBS SRAM (uncached normal)
  VirtualMemoryTable[Index].PhysicalBase = CCR2004_PBS_SRAM_BASE;
  VirtualMemoryTable[Index].VirtualBase  = CCR2004_PBS_SRAM_BASE;
  VirtualMemoryTable[Index].Length       = CCR2004_PBS_SRAM_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  Index++;

  // Peripherals above PBS SRAM (device)
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
