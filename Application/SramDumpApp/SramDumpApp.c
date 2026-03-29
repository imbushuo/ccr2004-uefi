/** @file
  SramDumpApp — dumps the Alpine V2 PBS SRAM general shared data struct.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>

#include "al_general_shared_data.h"

#define AL_PBS_SRAM_BASE                  0xFD8A4000ULL
#define SRAM_GENERAL_SHARED_DATA_OFFSET   0x180

EFI_STATUS
EFIAPI
SramDumpAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  struct al_general_shared_data  *Sd;
  UINTN   i;
  uint8_t Mac[AL_GENERAL_SHARED_MAC_ADDR_LEN];

  Sd = (struct al_general_shared_data *)(UINTN)(AL_PBS_SRAM_BASE + SRAM_GENERAL_SHARED_DATA_OFFSET);

  Print (L"=== Alpine V2 PBS SRAM General Shared Data ===\n");
  Print (L"Address: 0x%lx\n\n", (UINT64)(UINTN)Sd);

  Print (L"magic_num:       0x%02x (%a)\n",
         Sd->magic_num,
         (Sd->magic_num == AL_GENERAL_SHARED_DATA_MN) ? "VALID" : "INVALID");

  Print (L"\nBoot app version: %u.%u.%u\n",
         Sd->boot_app_major_ver, Sd->boot_app_minor_ver, Sd->boot_app_fix_ver);
  Print (L"U-Boot version:   %u.%u.%u\n",
         Sd->uboot_major_ver, Sd->uboot_minor_ver, Sd->uboot_fix_ver);

  Print (L"\nPort status:     [%u] [%u] [%u] [%u]\n",
         Sd->port_status[0], Sd->port_status[1],
         Sd->port_status[2], Sd->port_status[3]);

  Print (L"Board rev ID:    0x%08x\n", Sd->board_rev_id);
  Print (L"PCI Vendor:Device: %04x:%04x\n", Sd->vendor_id, Sd->device_id);

  Print (L"\nAPCEA version:   %u.%u.%u\n",
         Sd->apcea_major_ver, Sd->apcea_minor_ver, Sd->apcea_fix_ver);
  Print (L"iPXE version:    %u.%u.%u\n",
         Sd->ipxe_major_ver, Sd->ipxe_minor_ver, Sd->ipxe_fix_ver);

  Print (L"\nDiag indication: mn=0x%08x status=0x%08x\n",
         Sd->diag_indication_mn, Sd->diag_indication_status);

  Print (L"\nCurrent sensors:\n");
  for (i = 0; i < CURRENT_SENSOR_MAX_CHANNELS; i++) {
    Print (L"  ch%u: current=%u  voltage=%u\n", i,
           Sd->current_sensor_ch[i].electric_current,
           Sd->current_sensor_ch[i].electric_voltage);
  }

  Print (L"\nTemp sensor:     %u\n", Sd->temp_sensor);

  Print (L"\nError counters:\n");
  Print (L"  critical_err:    %u\n", Sd->critical_err);
  Print (L"  uncritical_err:  %u\n", Sd->uncritical_err);
  Print (L"  boot_err:        %u\n", Sd->boot_err);
  Print (L"  watchdog:        %u\n", Sd->watchdog);
  Print (L"  mem_corr_err:    %u\n", Sd->mem_corr_err);
  Print (L"  mem_uncorr_err:  %u\n", Sd->mem_uncorr_err);
  Print (L"  pcie_corr_err:   %u\n", Sd->pcie_corr_err);
  Print (L"  pcie_uncorr_err: %u\n", Sd->pcie_uncorr_err);

  Print (L"\nStage2 shared data:\n");
  Print (L"  magic_num:          0x%02x\n", Sd->stage2_shared_data.magic_num);
  Print (L"  flags:              0x%02x\n", Sd->stage2_shared_data.flags);
  Print (L"  secure_fail_reason: 0x%02x\n", Sd->stage2_shared_data.secure_fail_reason);

  Print (L"\nMAC addresses (via HAL):\n");
  for (i = 0; i < AL_GENERAL_SHARED_MAC_ADDR_NUM; i++) {
    if (al_general_shared_data_mac_addr_get (Sd, (unsigned int)i, Mac)) {
      Print (L"  [%u] %02x:%02x:%02x:%02x:%02x:%02x\n",
             i, Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]);
    } else {
      Print (L"  [%u] (not set)\n", i);
    }
  }

  Print (L"\n=== Raw hex dump (first 128 bytes) ===\n");
  {
    volatile UINT8  *Raw = (volatile UINT8 *)Sd;
    UINTN           Offset;

    for (Offset = 0; Offset < 128; Offset++) {
      if ((Offset % 16) == 0) Print (L"%04x: ", Offset);
      Print (L"%02x ", Raw[Offset]);
      if ((Offset % 16) == 15) Print (L"\n");
    }
  }

  return EFI_SUCCESS;
}
