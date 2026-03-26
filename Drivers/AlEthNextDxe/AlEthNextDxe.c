/** @file
  Alpine Ethernet UEFI SNP Driver — HAL-based.

  Ports the U-Boot al_eth driver pattern to UEFI, using the Alpine HAL
  library for full hardware initialization including EC forwarding tables.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AlEthNextDxe.h"
#include <Protocol/EmbeddedGpio.h>
#include "al_hal_pll.h"

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu = NULL;

/* Forward declarations */
STATIC EFI_STATUS EFIAPI AlEthNextSupported (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

STATIC EFI_STATUS EFIAPI AlEthNextStart (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

STATIC EFI_STATUS EFIAPI AlEthNextStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  );

STATIC EFI_DRIVER_BINDING_PROTOCOL mDriverBinding = {
  AlEthNextSupported,
  AlEthNextStart,
  AlEthNextStop,
  0x10,
  NULL,
  NULL
};

/* ---------- MMIO helpers (bypass PciIo for direct access) ---------- */

#define MacRead32(Ctx, Off)        MmioRead32 ((Ctx)->MacBase + (Off))
#define MacWrite32(Ctx, Off, Val)  MmioWrite32 ((Ctx)->MacBase + (Off), (Val))

/* ---------- PHY helpers using HAL MDIO ---------- */

STATIC
EFI_STATUS
AlEthPhyInit (
  IN AL_ETH_NEXT_CONTEXT  *Ctx
  )
{
  UINT32   Addr;
  UINT16   PhyId1, PhyId2;
  UINT16   Bmcr;
  UINT32   Timeout;
  int      Err;

  /* Scan MDIO addresses 0-31 for PHY */
  Ctx->PhyAddr = 0xFF;
  for (Addr = 0; Addr < 32; Addr++) {
    Err = al_eth_mdio_read (&Ctx->HalAdapter, Addr, 0, 2, &PhyId1);
    if (Err) continue;
    Err = al_eth_mdio_read (&Ctx->HalAdapter, Addr, 0, 3, &PhyId2);
    if (Err) continue;
    if ((PhyId1 != 0xFFFF) && (PhyId2 != 0xFFFF) && (PhyId1 != 0)) {
      Ctx->PhyAddr = Addr;
      DEBUG ((DEBUG_INFO, "AlEthNext: PHY found at MDIO addr %u (ID %04x:%04x)\n",
              Addr, PhyId1, PhyId2));
      break;
    }
  }

  if (Ctx->PhyAddr == 0xFF) {
    DEBUG ((DEBUG_INFO, "AlEthNext: No PHY found on MDIO bus\n"));
    return EFI_NOT_FOUND;
  }

  /* Dump PHY registers before any modification */
  {
    UINT16  Val;
    UINT32  Reg;
    STATIC CONST CHAR8 *RegNames[] = {
      "BMCR", "BMSR", "PHYID1", "PHYID2",     // 0-3
      "ANAR", "ANLPAR", "ANER", "ANNPTR",       // 4-7
      "ANNPRR", "GBCR", "GBSR", "R11",          // 8-11
      "R12", "MMD_CTRL", "MMD_DATA", "EXSR",    // 12-15
    };

    DEBUG ((DEBUG_INFO, "AlEthNext: PHY register dump (addr=%u) before init:\n", Ctx->PhyAddr));
    for (Reg = 0; Reg < 16; Reg++) {
      Err = al_eth_mdio_read (&Ctx->HalAdapter, Ctx->PhyAddr, 0, Reg, &Val);
      if (Err) {
        DEBUG ((DEBUG_INFO, "  [%2u] %-8a = READ ERROR\n", Reg, RegNames[Reg]));
      } else {
        DEBUG ((DEBUG_INFO, "  [%2u] %-8a = 0x%04x\n", Reg, RegNames[Reg], Val));
      }
    }

    /* Also dump AR8035-specific registers if accessible */
    for (Reg = 16; Reg < 32; Reg++) {
      Err = al_eth_mdio_read (&Ctx->HalAdapter, Ctx->PhyAddr, 0, Reg, &Val);
      if (!Err && Val != 0x0000 && Val != 0xFFFF) {
        DEBUG ((DEBUG_INFO, "  [%2u]          = 0x%04x\n", Reg, Val));
      }
    }

    /*
     * Dump AR8035 debug registers via indirect access:
     *   Write debug address to reg 0x1D, read data from reg 0x1E.
     *   Key debug regs:
     *     0x00 = RX clock delay (bit 15 = RGMII RX delay enable)
     *     0x05 = TX clock delay (bit 8 = RGMII TX delay enable)
     *     0x0B = Hibernate control
     *     0x1F = Chip config (RGMII/SGMII mode, etc.)
     */
    {
      STATIC CONST struct { UINT16 Addr; CONST CHAR8 *Name; } DbgRegs[] = {
        { 0x00, "RxClkDly" },
        { 0x05, "TxClkDly" },
        { 0x0B, "Hibernate" },
        { 0x1F, "ChipCfg"  },
      };
      UINTN  d;

      DEBUG ((DEBUG_INFO, "  AR8035 debug registers:\n"));
      for (d = 0; d < ARRAY_SIZE (DbgRegs); d++) {
        al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0x1D, DbgRegs[d].Addr);
        al_eth_mdio_read  (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0x1E, &Val);
        DEBUG ((DEBUG_INFO, "    dbg[0x%02x] %-8a = 0x%04x\n", DbgRegs[d].Addr, DbgRegs[d].Name, Val));
      }
    }
  }

  /*
   * AR8035 RGMII TX clock delay configuration.
   *
   * Confirmed from RouterBoot disassembly: only the TX clock delay is
   * enabled (debug reg 0x05 bit 8).  RX delay is NOT set.
   *
   * Access: write debug address to reg 0x1D, read-modify-write via reg 0x1E.
   */
  {
    UINT16  DbgVal;

    /* Enable RGMII TX clock delay (debug reg 0x05, set bit 8) */
    al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0x1D, 0x0005);
    al_eth_mdio_read  (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0x1E, &DbgVal);
    DbgVal |= BIT8;
    al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0x1E, DbgVal);

    DEBUG ((DEBUG_INFO, "AlEthNext: AR8035 RGMII TX clock delay enabled\n"));
  }

  /* Software reset */
  al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0, BIT15);

  for (Timeout = 0; Timeout < 1000; Timeout++) {
    MicroSecondDelay (1000);
    al_eth_mdio_read (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0, &Bmcr);
    if ((Bmcr & BIT15) == 0) break;
  }

  if (Timeout >= 1000) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: PHY reset timeout\n"));
    return EFI_TIMEOUT;
  }

  /* Advertise 10/100 */
  al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 4, 0x01E1);
  /* Advertise 1000BASE-T full duplex */
  al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 9, 0x0200);
  /* Enable auto-negotiation and restart */
  al_eth_mdio_write (&Ctx->HalAdapter, Ctx->PhyAddr, 0, 0, 0x1200);

  return EFI_SUCCESS;
}

/* ---------- Hardware Init / Shutdown ---------- */

STATIC
EFI_STATUS
AlEthHwInitialize (
  IN AL_ETH_NEXT_CONTEXT  *Ctx
  )
{
  EFI_STATUS               Status;
  int                      Err;
  UINT32                   Idx;
  EFI_PHYSICAL_ADDRESS     BufPages;

  struct al_eth_adapter_params  AdapterParams;
  struct al_udma_q_params       TxQParams;
  struct al_udma_q_params       RxQParams;

  DEBUG ((DEBUG_INFO, "AlEthNext: [1/15] Allocating DMA descriptor rings\n"));

  /* 1. Allocate DMA descriptor rings (UC, below 4GB) */
  {
    EFI_PHYSICAL_ADDRESS  DescPages = 0xFFFFFFFF;
    UINTN                 NumPages  = EFI_SIZE_TO_PAGES (TOTAL_DESC_SIZE);

    Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, NumPages, &DescPages);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "AlEthNext: Failed to allocate desc rings: %r\n", Status));
      return Status;
    }

    Ctx->DescRingBase = (VOID *)(UINTN)DescPages;
    Ctx->DescRingPhys = DescPages;

    /* Zero while cacheable, then switch to UC */
    ZeroMem (Ctx->DescRingBase, TOTAL_DESC_SIZE);
    WriteBackInvalidateDataCacheRange (Ctx->DescRingBase, TOTAL_DESC_SIZE);
    mCpu->SetMemoryAttributes (mCpu, DescPages, EFI_PAGES_TO_SIZE (NumPages), EFI_MEMORY_UC);
  }

  DEBUG ((DEBUG_INFO, "AlEthNext: [2/15] Allocating %u RX buffers (%u bytes each)\n",
          AL_ETH_NUM_RX_DESC, AL_ETH_RX_BUF_SIZE));

  /* 2. Allocate RX buffers (UC, below 4GB) */
  for (Idx = 0; Idx < AL_ETH_NUM_RX_DESC; Idx++) {
    BufPages = 0xFFFFFFFF;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData,
                                 EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE), &BufPages);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "AlEthNext: Failed to allocate RX buf %u: %r\n", Idx, Status));
      return Status;
    }

    Ctx->RxBuffers[Idx]     = (VOID *)(UINTN)BufPages;
    Ctx->RxBuffersPhys[Idx] = BufPages;

    ZeroMem (Ctx->RxBuffers[Idx], AL_ETH_RX_BUF_SIZE);
    WriteBackInvalidateDataCacheRange (Ctx->RxBuffers[Idx], AL_ETH_RX_BUF_SIZE);
    mCpu->SetMemoryAttributes (mCpu, BufPages,
                               EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE)),
                               EFI_MEMORY_UC);
  }

  /* Clean up stale RouterBOOT state — equivalent to U-Boot al_eth_halt().
   * Initialize a temporary adapter handle to get access to the UDMA queues,
   * then: mac_stop → q_reset(tx) → q_reset(rx) → adapter_stop. */
  DEBUG ((DEBUG_INFO, "AlEthNext: [3a/15] Cleaning up stale RouterBOOT state\n"));
  {
    struct al_hal_eth_adapter  TmpAdapter;
    struct al_eth_adapter_params  TmpParams;
    struct al_udma_q  *TmpQ;

    ZeroMem (&TmpAdapter, sizeof (TmpAdapter));
    ZeroMem (&TmpParams, sizeof (TmpParams));
    TmpParams.rev_id         = AL_ETH_REV_ID_2;
    TmpParams.dev_id         = AL_ETH_DEV_ID_STANDARD;
    TmpParams.udma_id        = 0;
    TmpParams.enable_rx_parser = 0;
    TmpParams.udma_regs_base = (void *)(UINTN)Ctx->UdmaBase;
    TmpParams.ec_regs_base   = (void *)(UINTN)Ctx->EcBase;
    TmpParams.mac_regs_base  = (void *)(UINTN)Ctx->MacBase;

    /* adapter_init sets up register pointers and transitions UDMA to NORMAL */
    al_eth_adapter_init (&TmpAdapter, &TmpParams);

    /* Now perform halt sequence matching U-Boot al_eth_halt() */
    al_eth_mac_stop (&TmpAdapter);
    MicroSecondDelay (10);

    al_udma_q_handle_get (&TmpAdapter.tx_udma, 0, &TmpQ);
    al_udma_q_reset (TmpQ);
    al_udma_q_handle_get (&TmpAdapter.rx_udma, 0, &TmpQ);
    al_udma_q_reset (TmpQ);

    al_eth_adapter_stop (&TmpAdapter);
  }

  DEBUG ((DEBUG_INFO, "AlEthNext: [3b/15] Calling al_eth_adapter_init (rev_id=2, UDMA=0x%lx, EC=0x%lx, MAC=0x%lx)\n",
          Ctx->UdmaBase, Ctx->EcBase, Ctx->MacBase));

  /* 3. Fresh HAL adapter init on clean hardware */
  ZeroMem (&Ctx->HalAdapter, sizeof (Ctx->HalAdapter));
  ZeroMem (&AdapterParams, sizeof (AdapterParams));
  AdapterParams.rev_id           = AL_ETH_REV_ID_2;
  AdapterParams.dev_id           = AL_ETH_DEV_ID_STANDARD;
  AdapterParams.udma_id          = 0;
  AdapterParams.enable_rx_parser = 0;
  AdapterParams.udma_regs_base   = (void *)(UINTN)Ctx->UdmaBase;
  AdapterParams.ec_regs_base     = (void *)(UINTN)Ctx->EcBase;
  AdapterParams.mac_regs_base    = (void *)(UINTN)Ctx->MacBase;

  Err = al_eth_adapter_init (&Ctx->HalAdapter, &AdapterParams);
  if (Err) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: al_eth_adapter_init failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "AlEthNext: [3b/15] al_eth_adapter_init OK\n"));

  DEBUG ((DEBUG_INFO, "AlEthNext: [4/15] Configuring TX queue\n"));

  /* 4. Configure TX queue */
  ZeroMem (&TxQParams, sizeof (TxQParams));
  TxQParams.size           = AL_ETH_DESCS_PER_Q;
  TxQParams.desc_base      = (union al_udma_desc *)((UINTN)Ctx->DescRingBase + TX_SDESC_OFFSET);
  TxQParams.desc_phy_base  = Ctx->DescRingPhys + TX_SDESC_OFFSET;
  TxQParams.cdesc_base     = (uint8_t *)((UINTN)Ctx->DescRingBase + TX_CDESC_OFFSET);
  TxQParams.cdesc_phy_base = Ctx->DescRingPhys + TX_CDESC_OFFSET;
  TxQParams.cdesc_size     = AL_ETH_CDESC_SIZE;

  Err = al_eth_queue_config (&Ctx->HalAdapter, UDMA_TX, 0, &TxQParams);
  if (Err) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: TX queue_config failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  /* al_eth_queue_enable is a stub (-EPERM) in this HAL rev; U-Boot ignores it */
  al_eth_queue_enable (&Ctx->HalAdapter, UDMA_TX, 0);

  DEBUG ((DEBUG_INFO, "AlEthNext: [5/15] Configuring RX queue\n"));

  /* 5. Configure RX queue */
  ZeroMem (&RxQParams, sizeof (RxQParams));
  RxQParams.size           = AL_ETH_DESCS_PER_Q;
  RxQParams.desc_base      = (union al_udma_desc *)((UINTN)Ctx->DescRingBase + RX_SDESC_OFFSET);
  RxQParams.desc_phy_base  = Ctx->DescRingPhys + RX_SDESC_OFFSET;
  RxQParams.cdesc_base     = (uint8_t *)((UINTN)Ctx->DescRingBase + RX_CDESC_OFFSET);
  RxQParams.cdesc_phy_base = Ctx->DescRingPhys + RX_CDESC_OFFSET;
  RxQParams.cdesc_size     = AL_ETH_CDESC_SIZE;

  Err = al_eth_queue_config (&Ctx->HalAdapter, UDMA_RX, 0, &RxQParams);
  if (Err) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: RX queue_config failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  al_eth_queue_enable (&Ctx->HalAdapter, UDMA_RX, 0);

  DEBUG ((DEBUG_INFO, "AlEthNext: [6/15] Getting queue handles\n"));

  /* 6. Get queue handles */
  al_udma_q_handle_get (&Ctx->HalAdapter.tx_udma, 0, &Ctx->TxDmaQ);
  al_udma_q_handle_get (&Ctx->HalAdapter.rx_udma, 0, &Ctx->RxDmaQ);

  DEBUG ((DEBUG_INFO, "AlEthNext: [7/15] Configuring MAC (RGMII)\n"));

  /* 7. MAC configuration (RGMII for CCR2004's 1G port) */
  Err = al_eth_mac_config (&Ctx->HalAdapter, AL_ETH_MAC_MODE_RGMII);
  if (Err) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: MAC config failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  /* 8. RX packet limits */
  al_eth_rx_pkt_limit_config (&Ctx->HalAdapter, 30, AL_ETH_MAX_PKT_SIZE);

  DEBUG ((DEBUG_INFO, "AlEthNext: [9/15] Configuring MDIO (Clause 22, 1MHz MDC)\n"));

  /* 9. MDIO configuration — Clause 22, 500MHz ref, 1MHz MDC (per device tree) */
  Err = al_eth_mdio_config (&Ctx->HalAdapter, AL_ETH_MDIO_TYPE_CLAUSE_22,
                             AL_TRUE, AL_ETH_REF_FREQ_500_MHZ, 1000);
  if (Err) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: MDIO config failed: %d\n", Err));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "AlEthNext: [10/15] PHY init (MDIO scan, reset, autoneg)\n"));

  /* 10. PHY init: scan, reset, autoneg */
  Status = AlEthPhyInit (Ctx);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "AlEthNext: PHY init failed: %r (continuing without PHY)\n", Status));
  }

  DEBUG ((DEBUG_INFO, "AlEthNext: [11/15] Storing MAC address in EC\n"));

  /* 11. Store MAC address in EC */
  al_eth_mac_addr_store (
    (void *)(UINTN)Ctx->EcBase,
    0,
    (uint8_t *)&Ctx->SnpMode.CurrentAddress
    );

  /* EC forwarding tables are already configured by al_eth_adapter_init():
   * - EC enabled (gen.en, gen.fifo_en)
   * - RFW default set to forward to UDMA 0 (rfw_default[0].opt_1 = 1)
   * - MAC addr stored above via al_eth_mac_addr_store
   * Matches U-Boot which does not call fwd_mac_table_set or ctrl_table_def_set. */

  DEBUG ((DEBUG_INFO, "AlEthNext: [13/15] Filling RX ring (%u buffers)\n", AL_ETH_NUM_RX_DESC));

  /* 13. Fill RX ring */
  for (Idx = 0; Idx < AL_ETH_NUM_RX_DESC; Idx++) {
    struct al_buf  Buf;
    Buf.addr = (al_phys_addr_t)Ctx->RxBuffersPhys[Idx];
    Buf.len  = AL_ETH_RX_BUF_SIZE;

    Err = al_eth_rx_buffer_add (Ctx->RxDmaQ, &Buf, AL_ETH_RX_FLAGS_INT, NULL);
    if (Err) {
      DEBUG ((DEBUG_ERROR, "AlEthNext: rx_buffer_add[%u] failed: %d\n", Idx, Err));
      return EFI_DEVICE_ERROR;
    }
  }

  Ctx->RxBufTailIdx = 0;
  al_eth_rx_buffer_action (Ctx->RxDmaQ, AL_ETH_NUM_RX_DESC);

  DEBUG ((DEBUG_INFO, "AlEthNext: [14/15] Starting MAC\n"));

  /* 14. Start MAC */
  al_eth_mac_start (&Ctx->HalAdapter);

  /* 15. Check link — don't block, GetStatus() polls MediaPresent continuously */
  {
    UINT32  RgmiiStat = MacRead32 (Ctx, MAC_GEN_RGMII_STAT);
    Ctx->SnpMode.MediaPresent = ((RgmiiStat & RGMII_STAT_LINK) != 0);
  }

  Ctx->TxBufInFlight = NULL;

  DEBUG ((DEBUG_INFO, "AlEthNext: HW init complete, link %a\n",
          Ctx->SnpMode.MediaPresent ? "UP" : "DOWN"));

  return EFI_SUCCESS;
}

STATIC
VOID
AlEthHwShutdown (
  IN AL_ETH_NEXT_CONTEXT  *Ctx
  )
{
  UINT32  Idx;

  /* Shutdown sequence matching U-Boot al_eth_halt():
   * 1. Stop MAC
   * 2. Reset queues (pause + RST_Q)
   * 3. Stop adapter (UDMA disable) */
  al_eth_mac_stop (&Ctx->HalAdapter);
  MicroSecondDelay (10);

  if (Ctx->TxDmaQ != NULL) {
    al_udma_q_reset (Ctx->TxDmaQ);
  }
  if (Ctx->RxDmaQ != NULL) {
    al_udma_q_reset (Ctx->RxDmaQ);
  }

  al_eth_adapter_stop (&Ctx->HalAdapter);
  MicroSecondDelay (100);

  /* Free RX buffers */
  for (Idx = 0; Idx < AL_ETH_NUM_RX_DESC; Idx++) {
    if (Ctx->RxBuffers[Idx] != NULL) {
      gBS->FreePages (Ctx->RxBuffersPhys[Idx], EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE));
      Ctx->RxBuffers[Idx] = NULL;
    }
  }

  /* Free descriptor rings */
  if (Ctx->DescRingBase != NULL) {
    gBS->FreePages (Ctx->DescRingPhys, EFI_SIZE_TO_PAGES (TOTAL_DESC_SIZE));
    Ctx->DescRingBase = NULL;
  }
}

/* ---------- ExitBootServices callback ---------- */

STATIC
VOID
EFIAPI
AlEthNextExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = (AL_ETH_NEXT_CONTEXT *)Context;

  al_eth_mac_stop (&Ctx->HalAdapter);
  al_eth_adapter_stop (&Ctx->HalAdapter);
}

/* ---------- SNP Protocol Methods ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpStart (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);

  if (Ctx->SnpMode.State == EfiSimpleNetworkStarted ||
      Ctx->SnpMode.State == EfiSimpleNetworkInitialized) {
    return EFI_ALREADY_STARTED;
  }

  if (Ctx->SnpMode.State != EfiSimpleNetworkStopped) {
    return EFI_DEVICE_ERROR;
  }

  Ctx->SnpMode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpStop (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);

  if (Ctx->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  Ctx->SnpMode.State = EfiSimpleNetworkStopped;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpInitialize (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN UINTN                        ExtraRxBufSize  OPTIONAL,
  IN UINTN                        ExtraTxBufSize  OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);
  EFI_STATUS           Status;

  if (Ctx->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Ctx->SnpMode.State == EfiSimpleNetworkInitialized) {
    return EFI_SUCCESS;
  }

  Status = AlEthHwInitialize (Ctx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ctx->SnpMode.State = EfiSimpleNetworkInitialized;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpReset (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN BOOLEAN                      ExtendedVerification
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);
  EFI_STATUS           Status;

  if (Ctx->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  AlEthHwShutdown (Ctx);
  Status = AlEthHwInitialize (Ctx);
  if (EFI_ERROR (Status)) {
    Ctx->SnpMode.State = EfiSimpleNetworkStarted;
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpShutdown (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);

  if (Ctx->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (Ctx->SnpMode.State == EfiSimpleNetworkInitialized) {
    AlEthHwShutdown (Ctx);
  }

  Ctx->SnpMode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpReceiveFilters (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN  UINT32                       Enable,
  IN  UINT32                       Disable,
  IN  BOOLEAN                      ResetMCastFilter,
  IN  UINTN                        MCastFilterCnt   OPTIONAL,
  IN  EFI_MAC_ADDRESS              *MCastFilter     OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);
  UINT32               CmdCfg;

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ? EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  Ctx->SnpMode.ReceiveFilterSetting |= Enable;
  Ctx->SnpMode.ReceiveFilterSetting &= ~Disable;

  /* Toggle promiscuous mode in MAC */
  CmdCfg = MacRead32 (Ctx, MAC_1G_CMD_CFG);
  if (Ctx->SnpMode.ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) {
    CmdCfg |= MAC_1G_CMD_PROMIS_EN;
  } else {
    CmdCfg &= ~MAC_1G_CMD_PROMIS_EN;
  }
  MacWrite32 (Ctx, MAC_1G_CMD_CFG, CmdCfg);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpStationAddress (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN     BOOLEAN                      Reset,
  IN     EFI_MAC_ADDRESS              *New  OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ? EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Reset) {
    CopyMem (&Ctx->SnpMode.CurrentAddress, &Ctx->SnpMode.PermanentAddress,
             sizeof (EFI_MAC_ADDRESS));
  } else if (New != NULL) {
    CopyMem (&Ctx->SnpMode.CurrentAddress, New, sizeof (EFI_MAC_ADDRESS));
  }

  /* Update hardware */
  al_eth_mac_addr_store (
    (void *)(UINTN)Ctx->EcBase,
    0,
    (uint8_t *)&Ctx->SnpMode.CurrentAddress
    );

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpStatistics (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN     BOOLEAN                      Reset,
  IN OUT UINTN                        *StatisticsSize  OPTIONAL,
  OUT    EFI_NETWORK_STATISTICS       *StatisticsTable OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ? EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Reset) {
    ZeroMem (&Ctx->Stats, sizeof (Ctx->Stats));
  }

  if (StatisticsSize != NULL && StatisticsTable != NULL) {
    if (*StatisticsSize < sizeof (EFI_NETWORK_STATISTICS)) {
      *StatisticsSize = sizeof (EFI_NETWORK_STATISTICS);
      return EFI_BUFFER_TOO_SMALL;
    }

    *StatisticsSize = sizeof (EFI_NETWORK_STATISTICS);
    CopyMem (StatisticsTable, &Ctx->Stats, sizeof (EFI_NETWORK_STATISTICS));
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpMCastIpToMac (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN     BOOLEAN                      IPv6,
  IN     EFI_IP_ADDRESS               *IP,
  OUT    EFI_MAC_ADDRESS              *MAC
  )
{
  if (IPv6) {
    MAC->Addr[0] = 0x33;
    MAC->Addr[1] = 0x33;
    MAC->Addr[2] = IP->v6.Addr[12];
    MAC->Addr[3] = IP->v6.Addr[13];
    MAC->Addr[4] = IP->v6.Addr[14];
    MAC->Addr[5] = IP->v6.Addr[15];
  } else {
    MAC->Addr[0] = 0x01;
    MAC->Addr[1] = 0x00;
    MAC->Addr[2] = 0x5E;
    MAC->Addr[3] = IP->v4.Addr[1] & 0x7F;
    MAC->Addr[4] = IP->v4.Addr[2];
    MAC->Addr[5] = IP->v4.Addr[3];
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpNvData (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN     BOOLEAN                      ReadWrite,
  IN     UINTN                        Offset,
  IN     UINTN                        BufferSize,
  IN OUT VOID                         *Buffer
  )
{
  return EFI_UNSUPPORTED;
}

/* ---------- GetStatus ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpGetStatus (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  OUT    UINT32                       *InterruptStatus  OPTIONAL,
  OUT    VOID                         **TxBuf           OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);
  UINT32               RgmiiStat;

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ? EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (InterruptStatus != NULL) {
    *InterruptStatus = 0;
  }

  if (TxBuf != NULL) {
    *TxBuf = NULL;
    if (Ctx->TxBufInFlight != NULL) {
      int CompDescs = al_eth_comp_tx_get (Ctx->TxDmaQ);
      if (CompDescs > 0) {
        *TxBuf = Ctx->TxBufInFlight;
        Ctx->TxBufInFlight = NULL;
      }
    }
  }

  /* Update link status */
  RgmiiStat = MacRead32 (Ctx, MAC_GEN_RGMII_STAT);
  Ctx->SnpMode.MediaPresent = ((RgmiiStat & RGMII_STAT_LINK) != 0);

  return EFI_SUCCESS;
}

/* ---------- Transmit ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpTransmit (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  IN UINTN                        HeaderSize,
  IN UINTN                        BufferSize,
  IN VOID                         *Buffer,
  IN EFI_MAC_ADDRESS              *SrcAddr   OPTIONAL,
  IN EFI_MAC_ADDRESS              *DestAddr  OPTIONAL,
  IN UINT16                       *Protocol  OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);
  UINT8                *Pkt;
  int                  NumDescs;
  struct al_eth_pkt    TxPkt;

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ? EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize < Ctx->SnpMode.MediaHeaderSize) {
    return EFI_BUFFER_TOO_SMALL;
  }

  /* Previous TX still in flight? Try to reclaim it first. */
  if (Ctx->TxBufInFlight != NULL) {
    int Comp = al_eth_comp_tx_get (Ctx->TxDmaQ);
    if (Comp > 0) {
      Ctx->TxBufInFlight = NULL;
    } else {
      return EFI_NOT_READY;
    }
  }

  /* Build Ethernet header if requested */
  if (HeaderSize != 0) {
    if (HeaderSize != Ctx->SnpMode.MediaHeaderSize) {
      return EFI_INVALID_PARAMETER;
    }
    if (DestAddr == NULL || Protocol == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    Pkt = (UINT8 *)Buffer;
    CopyMem (Pkt, DestAddr, 6);
    CopyMem (Pkt + 6, (SrcAddr != NULL) ? SrcAddr : &Ctx->SnpMode.CurrentAddress, 6);
    Pkt[12] = (UINT8)(*Protocol >> 8);
    Pkt[13] = (UINT8)(*Protocol & 0xFF);
  }

  /* Flush packet data from cache to DRAM */
  WriteBackInvalidateDataCacheRange (Buffer, BufferSize);

  /* Prepare HAL TX packet */
  ZeroMem (&TxPkt, sizeof (TxPkt));
  TxPkt.num_of_bufs   = 1;
  TxPkt.bufs[0].addr  = (al_phys_addr_t)(UINTN)Buffer;
  TxPkt.bufs[0].len   = (uint32_t)BufferSize;

  NumDescs = al_eth_tx_pkt_prepare (Ctx->TxDmaQ, &TxPkt);
  if (NumDescs == 0) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: tx_pkt_prepare failed\n"));
    return EFI_DEVICE_ERROR;
  }

  /* Kick DMA — non-blocking, completion checked via GetStatus() */
  al_eth_tx_dma_action (Ctx->TxDmaQ, (uint32_t)NumDescs);

  Ctx->TxBufInFlight = Buffer;
  Ctx->Stats.TxGoodFrames++;

  return EFI_SUCCESS;
}

/* ---------- Receive ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextSnpReceive (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *Snp,
  OUT    UINTN                        *HeaderSize  OPTIONAL,
  IN OUT UINTN                        *BufferSize,
  OUT    VOID                         *Buffer,
  OUT    EFI_MAC_ADDRESS              *SrcAddr     OPTIONAL,
  OUT    EFI_MAC_ADDRESS              *DestAddr    OPTIONAL,
  OUT    UINT16                       *Protocol    OPTIONAL
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = AL_ETH_NEXT_FROM_SNP (Snp);
  struct al_eth_pkt    RxPkt;
  uint32_t             NumDescs;
  UINT32               PktLen;
  UINT8                *PktData;
  struct al_buf        ReBuf;
  int                  Err;

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ? EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Buffer == NULL || BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /* Poll for received packet */
  ZeroMem (&RxPkt, sizeof (RxPkt));
  NumDescs = al_eth_pkt_rx (Ctx->RxDmaQ, &RxPkt);
  if (NumDescs == 0) {
    return EFI_NOT_READY;
  }

  /* Check for errors */
  if (RxPkt.flags & (AL_ETH_RX_ERROR | AL_UDMA_CDESC_ERROR)) {
    DEBUG ((DEBUG_INFO, "AlEthNext: RX error flags=0x%x\n", RxPkt.flags));
    goto ReSubmit;
  }

  PktLen = RxPkt.bufs[0].len;
  if (PktLen < 14 || PktLen > AL_ETH_RX_BUF_SIZE) {
    DEBUG ((DEBUG_INFO, "AlEthNext: RX bad len=%u\n", PktLen));
    goto ReSubmit;
  }

  if (*BufferSize < PktLen) {
    *BufferSize = PktLen;
    /* Don't re-submit — caller will retry */
    goto ReSubmitSmall;
  }

  /* Copy packet data */
  PktData = (UINT8 *)Ctx->RxBuffers[Ctx->RxBufTailIdx];
  InvalidateDataCacheRange (PktData, PktLen);
  CopyMem (Buffer, PktData, PktLen);
  *BufferSize = PktLen;

  /* Extract header info */
  if (HeaderSize != NULL) {
    *HeaderSize = Ctx->SnpMode.MediaHeaderSize;
  }
  if (DestAddr != NULL) {
    CopyMem (DestAddr, PktData, 6);
  }
  if (SrcAddr != NULL) {
    CopyMem (SrcAddr, PktData + 6, 6);
  }
  if (Protocol != NULL) {
    *Protocol = (UINT16)((PktData[12] << 8) | PktData[13]);
  }

  Ctx->Stats.RxGoodFrames++;

ReSubmit:
  /* Re-submit RX buffer to ring */
  ReBuf.addr = (al_phys_addr_t)Ctx->RxBuffersPhys[Ctx->RxBufTailIdx];
  ReBuf.len  = AL_ETH_RX_BUF_SIZE;

  Err = al_eth_rx_buffer_add (Ctx->RxDmaQ, &ReBuf, AL_ETH_RX_FLAGS_INT, NULL);
  if (Err == 0) {
    al_eth_rx_buffer_action (Ctx->RxDmaQ, 1);
  }

  Ctx->RxBufTailIdx = (Ctx->RxBufTailIdx + 1) % AL_ETH_NUM_RX_DESC;

  if (NumDescs != 0 && !(RxPkt.flags & (AL_ETH_RX_ERROR | AL_UDMA_CDESC_ERROR))) {
    return EFI_SUCCESS;
  }
  return EFI_NOT_READY;

ReSubmitSmall:
  ReBuf.addr = (al_phys_addr_t)Ctx->RxBuffersPhys[Ctx->RxBufTailIdx];
  ReBuf.len  = AL_ETH_RX_BUF_SIZE;
  Err = al_eth_rx_buffer_add (Ctx->RxDmaQ, &ReBuf, AL_ETH_RX_FLAGS_INT, NULL);
  if (Err == 0) {
    al_eth_rx_buffer_action (Ctx->RxDmaQ, 1);
  }
  Ctx->RxBufTailIdx = (Ctx->RxBufTailIdx + 1) % AL_ETH_NUM_RX_DESC;
  return EFI_BUFFER_TOO_SMALL;
}

/* ---------- WaitForPacket event ---------- */

STATIC
VOID
EFIAPI
AlEthNextWaitForPacket (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  AL_ETH_NEXT_CONTEXT  *Ctx = (AL_ETH_NEXT_CONTEXT *)Context;
  struct al_eth_pkt    Pkt;
  uint32_t             Num;

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return;
  }

  /* Peek — if al_eth_pkt_rx would return >0, signal.
   * We can't actually peek without consuming, so we check the
   * completion descriptor directly via the UDMA.
   * The HAL's al_eth_pkt_rx checks cdesc — we just need to know
   * if there's a completion. A cheap check: read RCRHP vs our tail.
   * But the simplest approach matching the old driver is to just
   * check the raw completion descriptor word.
   */
  (void)Pkt;
  (void)Num;

  /* Check if completion ring has data.
   * The HAL stores the comp ring base in the queue structure.
   * For simplicity, we check by attempting a non-destructive peek.
   * Since we can't peek without consuming in the HAL, we'll just
   * look at the raw completion descriptor.
   */
  if (Ctx->RxDmaQ != NULL) {
    /* Read the completion descriptor at current consumer position.
     * Use the HAL's al_udma_new_cdesc to check for new completions. */
    volatile union al_udma_cdesc *CDesc;
    CDesc = al_udma_cdesc_idx_to_ptr (Ctx->RxDmaQ, Ctx->RxDmaQ->next_cdesc_idx);
    uint32_t Flags = MmioRead32 ((UINTN)CDesc);

    if (al_udma_new_cdesc (Ctx->RxDmaQ, Flags)) {
      gBS->SignalEvent (Event);
    }
  }
}

/* ---------- Driver Binding: Supported ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextSupported (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS            Status;
  EFI_PCI_IO_PROTOCOL   *PciIo;
  UINT16                VendorId;
  UINT16                DeviceId;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_VENDOR_ID_OFFSET, 1, &VendorId);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_DEVICE_ID_OFFSET, 1, &DeviceId);

  if (VendorId != AL_ETH_VENDOR_ID || DeviceId != AL_ETH_DEVICE_ID) {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid,
                       This->DriverBindingHandle, Controller);

  return Status;
}

/* ---------- Driver Binding: Start ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextStart (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  EFI_PCI_IO_PROTOCOL      *PciIo;
  AL_ETH_NEXT_CONTEXT      *Ctx;
  EFI_MAC_ADDRESS          MacAddr;
  UINT32                   BarLo;
  EFI_DEVICE_PATH_PROTOCOL *ParentDevicePath;
  EFI_DEVICE_PATH_PROTOCOL *FullPath;

  /* Open PciIo */
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Allocate context */
  Ctx = AllocateZeroPool (sizeof (AL_ETH_NEXT_CONTEXT));
  if (Ctx == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseProtocol;
  }

  Ctx->Signature        = AL_ETH_NEXT_SIGNATURE;
  Ctx->ControllerHandle = Controller;
  Ctx->PciIo            = PciIo;
  EfiInitializeLock (&Ctx->Lock, TPL_NOTIFY);

  /* Save and enable PCI attributes */
  PciIo->Attributes (PciIo, EfiPciIoAttributeOperationGet, 0, &Ctx->OriginalPciAttributes);
  PciIo->Attributes (PciIo, EfiPciIoAttributeOperationEnable,
                      EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);

  /* Read BAR addresses directly from PCI config */
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BASE_ADDRESSREG_OFFSET + AL_ETH_BAR_UDMA * 4, 1, &BarLo);
  Ctx->UdmaBase = (UINTN)(BarLo & 0xFFFFFFF0);

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BASE_ADDRESSREG_OFFSET + AL_ETH_BAR_MAC * 4, 1, &BarLo);
  Ctx->MacBase = (UINTN)(BarLo & 0xFFFFFFF0);

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BASE_ADDRESSREG_OFFSET + AL_ETH_BAR_EC * 4, 1, &BarLo);
  Ctx->EcBase = (BarLo != 0) ? (UINTN)(BarLo & 0xFFFFFFF0) : 0;

  DEBUG ((DEBUG_INFO, "AlEthNext: BARs: UDMA=0x%lx MAC=0x%lx EC=0x%lx\n",
          Ctx->UdmaBase, Ctx->MacBase, Ctx->EcBase));

  if (Ctx->EcBase == 0) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: EC BAR is 0, cannot proceed\n"));
    Status = EFI_DEVICE_ERROR;
    goto FreeCtx;
  }

  /* Try board info protocol for MAC address first */
  ZeroMem (&MacAddr, sizeof (MacAddr));
  {
    MIKROTIK_BOARD_INFO_PROTOCOL  *BoardInfo;
    EFI_STATUS                    BiStatus;

    BiStatus = gBS->LocateProtocol (&gMikroTikBoardInfoProtocolGuid, NULL, (VOID **)&BoardInfo);
    if (!EFI_ERROR (BiStatus)) {
      BoardInfo->GetMacAddress (BoardInfo, &MacAddr);
      DEBUG ((DEBUG_INFO, "AlEthNext: MAC from BoardInfo: %02x:%02x:%02x:%02x:%02x:%02x\n",
              MacAddr.Addr[0], MacAddr.Addr[1], MacAddr.Addr[2],
              MacAddr.Addr[3], MacAddr.Addr[4], MacAddr.Addr[5]));
    }
  }

  /* If BoardInfo MAC is all zeros, fall back to EC read */
  {
    BOOLEAN IsZero = TRUE;
    UINT32  i;

    for (i = 0; i < 6; i++) {
      if (MacAddr.Addr[i] != 0) {
        IsZero = FALSE;
        break;
      }
    }

    if (IsZero) {
      al_eth_mac_addr_read ((void *)(UINTN)Ctx->EcBase, 0, (uint8_t *)&MacAddr);

      /* Check again — generate random if still zero */
      IsZero = TRUE;
      for (i = 0; i < 6; i++) {
        if (MacAddr.Addr[i] != 0) {
          IsZero = FALSE;
          break;
        }
      }

      if (IsZero) {
        UINT64 Tsc = GetPerformanceCounter ();
        MacAddr.Addr[0] = 0x02;  /* locally administered */
        MacAddr.Addr[1] = (UINT8)(Tsc >> 8);
        MacAddr.Addr[2] = (UINT8)(Tsc >> 16);
        MacAddr.Addr[3] = (UINT8)(Tsc >> 24);
        MacAddr.Addr[4] = (UINT8)(Tsc >> 32);
        MacAddr.Addr[5] = (UINT8)(Tsc >> 40);
        DEBUG ((DEBUG_INFO, "AlEthNext: No MAC in HW or BoardInfo, generated random\n"));
      }
    }
  }

  DEBUG ((DEBUG_INFO, "AlEthNext: MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
          MacAddr.Addr[0], MacAddr.Addr[1], MacAddr.Addr[2],
          MacAddr.Addr[3], MacAddr.Addr[4], MacAddr.Addr[5]));

  /* Initialize SNP Mode */
  Ctx->SnpMode.State                 = EfiSimpleNetworkStopped;
  Ctx->SnpMode.HwAddressSize         = NET_ETHER_ADDR_LEN;
  Ctx->SnpMode.MediaHeaderSize        = 14;
  Ctx->SnpMode.MaxPacketSize          = 1500;
  Ctx->SnpMode.NvRamSize              = 0;
  Ctx->SnpMode.NvRamAccessSize        = 0;
  Ctx->SnpMode.ReceiveFilterMask      = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                                         EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |
                                         EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS;
  Ctx->SnpMode.ReceiveFilterSetting   = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                                         EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
  Ctx->SnpMode.IfType                 = NET_IFTYPE_ETHERNET;
  Ctx->SnpMode.MacAddressChangeable   = TRUE;
  Ctx->SnpMode.MultipleTxSupported    = FALSE;
  Ctx->SnpMode.MediaPresentSupported  = TRUE;
  Ctx->SnpMode.MediaPresent           = FALSE;
  CopyMem (&Ctx->SnpMode.CurrentAddress, &MacAddr, sizeof (EFI_MAC_ADDRESS));
  CopyMem (&Ctx->SnpMode.PermanentAddress, &MacAddr, sizeof (EFI_MAC_ADDRESS));
  SetMem (&Ctx->SnpMode.BroadcastAddress, sizeof (EFI_MAC_ADDRESS), 0xFF);

  /* Wire SNP protocol */
  Ctx->Snp.Revision       = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
  Ctx->Snp.Start          = AlEthNextSnpStart;
  Ctx->Snp.Stop           = AlEthNextSnpStop;
  Ctx->Snp.Initialize     = AlEthNextSnpInitialize;
  Ctx->Snp.Reset          = AlEthNextSnpReset;
  Ctx->Snp.Shutdown       = AlEthNextSnpShutdown;
  Ctx->Snp.ReceiveFilters = AlEthNextSnpReceiveFilters;
  Ctx->Snp.StationAddress = AlEthNextSnpStationAddress;
  Ctx->Snp.Statistics     = AlEthNextSnpStatistics;
  Ctx->Snp.MCastIpToMac   = AlEthNextSnpMCastIpToMac;
  Ctx->Snp.NvData         = AlEthNextSnpNvData;
  Ctx->Snp.GetStatus      = AlEthNextSnpGetStatus;
  Ctx->Snp.Transmit       = AlEthNextSnpTransmit;
  Ctx->Snp.Receive        = AlEthNextSnpReceive;
  Ctx->Snp.Mode           = &Ctx->SnpMode;

  /* Create WaitForPacket event */
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  AlEthNextWaitForPacket,
                  Ctx,
                  &Ctx->Snp.WaitForPacket
                  );
  if (EFI_ERROR (Status)) {
    goto FreeCtx;
  }

  /* Create ExitBootServices event */
  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_CALLBACK,
                  AlEthNextExitBootServices,
                  Ctx,
                  &Ctx->ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    goto CloseWfpEvent;
  }

  /* Build device path */
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&ParentDevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto CloseExitEvent;
  }

  Ctx->DevicePath = AllocateZeroPool (sizeof (AL_ETH_NEXT_DEVICE_PATH));
  if (Ctx->DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseExitEvent;
  }

  Ctx->DevicePath->MacAddrNode.Header.Type    = MESSAGING_DEVICE_PATH;
  Ctx->DevicePath->MacAddrNode.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength (&Ctx->DevicePath->MacAddrNode.Header, sizeof (MAC_ADDR_DEVICE_PATH));
  CopyMem (&Ctx->DevicePath->MacAddrNode.MacAddress, &MacAddr, sizeof (EFI_MAC_ADDRESS));
  Ctx->DevicePath->MacAddrNode.IfType = NET_IFTYPE_ETHERNET;
  SetDevicePathEndNode (&Ctx->DevicePath->End);

  FullPath = AppendDevicePathNode (
               ParentDevicePath,
               (EFI_DEVICE_PATH_PROTOCOL *)&Ctx->DevicePath->MacAddrNode
               );
  FreePool (Ctx->DevicePath);
  Ctx->DevicePath = (AL_ETH_NEXT_DEVICE_PATH *)FullPath;

  /* Install SNP + DevicePath on child handle */
  Ctx->ChildHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Ctx->ChildHandle,
                  &gEfiSimpleNetworkProtocolGuid, &Ctx->Snp,
                  &gEfiDevicePathProtocolGuid, Ctx->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto FreeDevicePath;
  }

  /* Open PciIo by child */
  gBS->OpenProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         (VOID **)&PciIo,
         This->DriverBindingHandle,
         Ctx->ChildHandle,
         EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
         );

  DEBUG ((DEBUG_INFO, "AlEthNext: Driver binding started, SNP installed\n"));
  return EFI_SUCCESS;

FreeDevicePath:
  FreePool (Ctx->DevicePath);
CloseExitEvent:
  gBS->CloseEvent (Ctx->ExitBootServicesEvent);
CloseWfpEvent:
  gBS->CloseEvent (Ctx->Snp.WaitForPacket);
FreeCtx:
  PciIo->Attributes (PciIo, EfiPciIoAttributeOperationSet, Ctx->OriginalPciAttributes, NULL);
  FreePool (Ctx);
CloseProtocol:
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid,
                       This->DriverBindingHandle, Controller);
  return Status;
}

/* ---------- Driver Binding: Stop ---------- */

STATIC
EFI_STATUS
EFIAPI
AlEthNextStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_NETWORK_PROTOCOL     *Snp;
  AL_ETH_NEXT_CONTEXT             *Ctx;

  if (NumberOfChildren == 0) {
    gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid,
                         This->DriverBindingHandle, Controller);
    return EFI_SUCCESS;
  }

  Status = gBS->OpenProtocol (
                  ChildHandleBuffer[0],
                  &gEfiSimpleNetworkProtocolGuid,
                  (VOID **)&Snp,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ctx = AL_ETH_NEXT_FROM_SNP (Snp);

  /* Uninstall protocols */
  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Ctx->ChildHandle,
                  &gEfiSimpleNetworkProtocolGuid, &Ctx->Snp,
                  &gEfiDevicePathProtocolGuid, Ctx->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Close PciIo opened by child */
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid,
                       This->DriverBindingHandle, Ctx->ChildHandle);

  /* Shutdown hardware if initialized */
  if (Ctx->SnpMode.State == EfiSimpleNetworkInitialized) {
    AlEthHwShutdown (Ctx);
  }

  /* Close events */
  gBS->CloseEvent (Ctx->ExitBootServicesEvent);
  gBS->CloseEvent (Ctx->Snp.WaitForPacket);

  /* Restore PCI attributes */
  Ctx->PciIo->Attributes (Ctx->PciIo, EfiPciIoAttributeOperationSet,
                            Ctx->OriginalPciAttributes, NULL);

  /* Free resources */
  FreePool (Ctx->DevicePath);
  FreePool (Ctx);

  return EFI_SUCCESS;
}

/* ---------- Entry Point ---------- */

/**
  Reset the RGMII PHY via GPIO40 (active-low reset line).

  Uses the EmbeddedGpio protocol (PL061 driver, gpio5 controller).
  GPIO40 = baseidx 0x28 + bit 0 in the PlatformGpioDxe table.
**/
#define ETH_PHY_RESET_GPIO  40

STATIC
VOID
EthPhyReset (
  VOID
  )
{
  EFI_STATUS     Status;
  EMBEDDED_GPIO  *Gpio;

  Status = gBS->LocateProtocol (&gEmbeddedGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AlEthNext: GPIO protocol not found, skipping PHY reset\n"));
    return;
  }

  //
  // Assert reset: drive GPIO40 high
  //
  Gpio->Set (Gpio, ETH_PHY_RESET_GPIO, GPIO_MODE_OUTPUT_1);
  gBS->Stall (100000);  // 100 ms

  //
  // Deassert reset: drive GPIO40 low
  //
  Gpio->Set (Gpio, ETH_PHY_RESET_GPIO, GPIO_MODE_OUTPUT_0);
  gBS->Stall (100000);  // 100 ms

  DEBUG ((DEBUG_INFO, "AlEthNext: PHY reset via GPIO40 complete\n"));
}

EFI_STATUS
EFIAPI
AlEthNextDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "AlEthNext: Driver loaded (HAL-based Alpine Ethernet)\n"));

  EthPhyReset ();

  /* Locate CPU Architecture Protocol (for SetMemoryAttributes) */
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AlEthNext: Failed to locate CPU Arch Protocol: %r\n", Status));
    return Status;
  }

  return EfiLibInstallDriverBinding (ImageHandle, SystemTable, &mDriverBinding, ImageHandle);
}
