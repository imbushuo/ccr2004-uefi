/** @file
  Alpine Ethernet UEFI SNP Driver — HAL-based.

  Uses the Alpine HAL library for hardware initialization, including
  EC forwarding table configuration that enables immediate RX.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef AL_ETH_NEXT_DXE_H_
#define AL_ETH_NEXT_DXE_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <IndustryStandard/Pci.h>
#include <Protocol/Cpu.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleNetwork.h>

/* Alpine HAL headers */
#include "al_hal_eth.h"
#include "al_hal_udma.h"

/* PCI identification */
#define AL_ETH_VENDOR_ID  0x1C36
#define AL_ETH_DEVICE_ID  0x0001

/* BAR indices */
#define AL_ETH_BAR_UDMA  0
#define AL_ETH_BAR_MAC   2
#define AL_ETH_BAR_EC    4

/* Ring configuration */
#define AL_ETH_NUM_TX_DESC   32
#define AL_ETH_NUM_RX_DESC   32
#define AL_ETH_RX_BUF_SIZE   2048
#define AL_ETH_MAX_PKT_SIZE  1518
#define AL_ETH_CDESC_SIZE    16
#define AL_ETH_DESC_SIZE     16

/* Descriptor ring sizing (same layout as U-Boot) */
#define AL_ETH_DESCS_PER_Q    (AL_ETH_NUM_RX_DESC + 1)
#define AL_ETH_Q_DESCS_SIZE   (AL_ETH_DESCS_PER_Q * AL_ETH_DESC_SIZE)

#define TX_SDESC_OFFSET  (0 * AL_ETH_Q_DESCS_SIZE)
#define TX_CDESC_OFFSET  (1 * AL_ETH_Q_DESCS_SIZE)
#define RX_SDESC_OFFSET  (2 * AL_ETH_Q_DESCS_SIZE)
#define RX_CDESC_OFFSET  (3 * AL_ETH_Q_DESCS_SIZE)
#define TOTAL_DESC_SIZE  (4 * AL_ETH_Q_DESCS_SIZE)

/* RGMII status register for link detection */
#define MAC_GEN_BASE              0x00000
#define MAC_GEN_RGMII_STAT        0x00064
#define RGMII_STAT_LINK           BIT4

/* MAC 1G register for promiscuous */
#define MAC_1G_CMD_CFG            0x00808
#define MAC_1G_CMD_PROMIS_EN      BIT4

/* TX completion poll retries (each ~= 1us) */
#define TX_DONE_POLL_RETRIES  500000

/* Context signature */
#define AL_ETH_NEXT_SIGNATURE  SIGNATURE_32('A','L','N','X')

/* Device path structure */
typedef struct {
  MAC_ADDR_DEVICE_PATH       MacAddrNode;
  EFI_DEVICE_PATH_PROTOCOL   End;
} AL_ETH_NEXT_DEVICE_PATH;

/* Driver context */
typedef struct {
  UINT32                          Signature;
  EFI_HANDLE                      ControllerHandle;
  EFI_HANDLE                      ChildHandle;
  EFI_LOCK                        Lock;
  EFI_PCI_IO_PROTOCOL             *PciIo;
  UINT64                          OriginalPciAttributes;

  /* Direct MMIO base addresses */
  UINTN                           UdmaBase;
  UINTN                           MacBase;
  UINTN                           EcBase;

  /* HAL adapter instance */
  struct al_hal_eth_adapter       HalAdapter;
  struct al_udma_q                *TxDmaQ;
  struct al_udma_q                *RxDmaQ;

  /* DMA ring memory (single UC allocation) */
  VOID                            *DescRingBase;
  EFI_PHYSICAL_ADDRESS            DescRingPhys;

  /* RX buffers */
  VOID                            *RxBuffers[AL_ETH_NUM_RX_DESC];
  EFI_PHYSICAL_ADDRESS            RxBuffersPhys[AL_ETH_NUM_RX_DESC];

  /* RX buffer tracking for re-submit */
  UINT32                          RxBufTailIdx;

  /* TX buffer tracking */
  VOID                            *TxBufInFlight;

  /* PHY address */
  UINT32                          PhyAddr;

  /* SNP protocol and mode */
  EFI_SIMPLE_NETWORK_PROTOCOL     Snp;
  EFI_SIMPLE_NETWORK_MODE         SnpMode;
  EFI_EVENT                       ExitBootServicesEvent;

  /* Network statistics */
  EFI_NETWORK_STATISTICS          Stats;

  /* Device path */
  AL_ETH_NEXT_DEVICE_PATH         *DevicePath;
} AL_ETH_NEXT_CONTEXT;

#define AL_ETH_NEXT_FROM_SNP(a) \
  CR(a, AL_ETH_NEXT_CONTEXT, Snp, AL_ETH_NEXT_SIGNATURE)

#endif /* AL_ETH_NEXT_DXE_H_ */
