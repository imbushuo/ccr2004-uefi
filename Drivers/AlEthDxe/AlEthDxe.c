/** @file
  Annapurna Labs Ethernet DXE driver for MikroTik CCR2004.

  Drives PCI 1c36:0001 (Alpine V2 standard v2 NIC) connected via RGMII
  to an Atheros AR8035 PHY. Implements EFI_SIMPLE_NETWORK_PROTOCOL.

  Rewritten to follow the kernel HAL init sequence:
    1. UDMA defaults (FIFO depth, AXI timeout, comp ack)
    2. UDMA → NORMAL (before queue config — kernel pattern)
    3. Queue init (ring pointers, comp cfg, rate limiter, enable)
    4. EC/MAC/MDIO/PHY init
    5. RX buffer fill
    6. MAC start

  Uses software ring_id-based completion tracking (kernel pattern)
  instead of reading RCRHP/TCRHP hardware registers.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AlEthDxe.h"

//
// Forward declarations
//
EFI_STATUS EFIAPI AlEthSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS EFIAPI AlEthDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS EFIAPI AlEthDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  );

STATIC EFI_DRIVER_BINDING_PROTOCOL  mAlEthDriverBinding = {
  AlEthSupported,
  AlEthDriverStart,
  AlEthDriverStop,
  0x10,
  NULL,
  NULL
};

// ============================================================================
// Register access helpers — direct MMIO to work around PciBusDxe BAR mismatch
// on Alpine V2 SoC-integrated PCI devices
// ============================================================================

#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Protocol/Cpu.h>

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;

#define MacRead32(Ctx, Off)        MmioRead32 ((Ctx)->MacBase + (Off))
#define MacWrite32(Ctx, Off, Val)  MmioWrite32 ((Ctx)->MacBase + (Off), (Val))

#define EcRead32(Ctx, Off)         MmioRead32 ((Ctx)->EcBase + (Off))
#define EcWrite32(Ctx, Off, Val)   MmioWrite32 ((Ctx)->EcBase + (Off), (Val))

#define UdmaRead32(Ctx, Off)       MmioRead32 ((Ctx)->UdmaBase + (Off))
#define UdmaWrite32(Ctx, Off, Val) MmioWrite32 ((Ctx)->UdmaBase + (Off), (Val))

// ============================================================================
// MDIO (Clause 22 via 10G MAC)
// ============================================================================

STATIC
EFI_STATUS
AlEthMdioConfig (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINT32  Val;

  //
  // Release shared MDIO bus lock in case a previous firmware stage left it held
  //
  MacWrite32 (Ctx, MAC_GEN_MDIO_CTRL1, 0);
  MicroSecondDelay (100);

  //
  // Force use of 10G MAC MDIO interface
  //
  Val = MacRead32 (Ctx, MAC_GEN_CFG);
  Val |= MAC_GEN_CFG_MDIO_1_10;
  MacWrite32 (Ctx, MAC_GEN_CFG, Val);

  //
  // Configure MDIO: Clause 22, preserve existing clock divider from bootloader
  //
  Val = MacRead32 (Ctx, MAC_10G_MDIO_CFG_STATUS);
  DEBUG ((DEBUG_INFO, "AlEthDxe: MDIO cfg_status before=0x%08x\n", Val));

  Val &= ~MDIO_CFG_CLAUSE45;
  Val &= ~MDIO_CFG_HOLD_MASK;
  Val |= (3 << MDIO_CFG_HOLD_SHIFT);

  if ((Val & MDIO_CFG_CLK_MASK) == 0) {
    Val &= ~MDIO_CFG_CLK_MASK;
    Val |= (250 << MDIO_CFG_CLK_SHIFT);
  }

  MacWrite32 (Ctx, MAC_10G_MDIO_CFG_STATUS, Val);

  Val = MacRead32 (Ctx, MAC_10G_MDIO_CFG_STATUS);
  DEBUG ((DEBUG_INFO, "AlEthDxe: MDIO cfg_status after=0x%08x\n", Val));

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
AlEthMdioWaitBusy (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINTN   Elapsed;
  UINT32  Val;

  for (Elapsed = 0; Elapsed < AL_ETH_MDIO_TIMEOUT_US; Elapsed += AL_ETH_MDIO_POLL_US) {
    Val = MacRead32 (Ctx, MAC_10G_MDIO_CFG_STATUS);
    if ((Val & MDIO_CFG_BUSY) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (AL_ETH_MDIO_POLL_US);
  }

  DEBUG ((DEBUG_ERROR, "AlEthDxe: MDIO busy timeout\n"));
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
AlEthMdioRead (
  IN  AL_ETH_CONTEXT  *Ctx,
  IN  UINT8           PhyAddr,
  IN  UINT8           Reg,
  OUT UINT16          *Value
  )
{
  EFI_STATUS  Status;
  UINT16      Cmd;
  UINT32      CfgStatus;

  Status = AlEthMdioWaitBusy (Ctx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd = (UINT16)((Reg & 0x1F) | ((PhyAddr & 0x1F) << 5) | MDIO_CMD_READ);
  MacWrite32 (Ctx, MAC_10G_MDIO_CMD, (UINT32)Cmd);

  Status = AlEthMdioWaitBusy (Ctx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CfgStatus = MacRead32 (Ctx, MAC_10G_MDIO_CFG_STATUS);
  if (CfgStatus & MDIO_CFG_ERROR) {
    DEBUG ((DEBUG_ERROR, "AlEthDxe: MDIO read error phy=%d reg=%d cfg=0x%08x\n",
            PhyAddr, Reg, CfgStatus));
    return EFI_DEVICE_ERROR;
  }

  *Value = (UINT16)(MacRead32 (Ctx, MAC_10G_MDIO_DATA) & 0xFFFF);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
AlEthMdioWrite (
  IN AL_ETH_CONTEXT  *Ctx,
  IN UINT8           PhyAddr,
  IN UINT8           Reg,
  IN UINT16          Value
  )
{
  EFI_STATUS  Status;
  UINT16      Cmd;
  UINT32      CfgStatus;

  Status = AlEthMdioWaitBusy (Ctx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd = (UINT16)((Reg & 0x1F) | ((PhyAddr & 0x1F) << 5));
  MacWrite32 (Ctx, MAC_10G_MDIO_CMD, (UINT32)Cmd);
  MacWrite32 (Ctx, MAC_10G_MDIO_DATA, (UINT32)Value);

  Status = AlEthMdioWaitBusy (Ctx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CfgStatus = MacRead32 (Ctx, MAC_10G_MDIO_CFG_STATUS);
  if (CfgStatus & MDIO_CFG_ERROR) {
    DEBUG ((DEBUG_ERROR, "AlEthDxe: MDIO write error phy=%d reg=%d cfg=0x%08x\n",
            PhyAddr, Reg, CfgStatus));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

// ============================================================================
// MAC RGMII Configuration
// ============================================================================

STATIC
VOID
AlEthMacConfigRgmii (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINT32  Val;

  MacWrite32 (Ctx, MAC_GEN_CLK_CFG, 0x40003210);

  MacWrite32 (Ctx, MAC_1G_CMD_CFG,
              MAC_1G_CMD_CFG_NO_LGTH_CHECK |
              MAC_1G_CMD_CFG_CNTL_FRM_ENA |
              MAC_1G_CMD_CFG_PROMIS_EN);

  MacWrite32 (Ctx, MAC_1G_RX_SECTION_EMPTY, 0x00000000);
  MacWrite32 (Ctx, MAC_1G_RX_SECTION_FULL,  0x0000000C);
  MacWrite32 (Ctx, MAC_1G_RX_ALMOST_EMPTY,  0x00000008);
  MacWrite32 (Ctx, MAC_1G_RX_ALMOST_FULL,   0x00000008);
  MacWrite32 (Ctx, MAC_1G_TX_SECTION_EMPTY, 0x00000008);
  MacWrite32 (Ctx, MAC_1G_TX_SECTION_FULL,  0x0000000C);
  MacWrite32 (Ctx, MAC_1G_TX_ALMOST_EMPTY,  0x00000008);
  MacWrite32 (Ctx, MAC_1G_TX_ALMOST_FULL,   0x00000008);

  MacWrite32 (Ctx, MAC_GEN_CFG, 0x00000000);

  Val = MacRead32 (Ctx, MAC_GEN_CFG);
  Val |= MAC_GEN_CFG_MDIO_1_10;
  MacWrite32 (Ctx, MAC_GEN_CFG, Val);

  MacWrite32 (Ctx, MAC_GEN_MAC_1G_CFG, 0x00000002);

  Val = MacRead32 (Ctx, MAC_GEN_MUX_SEL);
  Val &= ~0x7;
  MacWrite32 (Ctx, MAC_GEN_MUX_SEL, Val);

  MacWrite32 (Ctx, MAC_GEN_RGMII_SEL, 0x0F);
  MacWrite32 (Ctx, MAC_1G_FRM_LEN, AL_ETH_MAX_PKT_SIZE);
}

STATIC
VOID
AlEthMacStart (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINT32  Val;

  Val = MacRead32 (Ctx, MAC_1G_CMD_CFG);
  Val |= (MAC_1G_CMD_CFG_TX_ENA | MAC_1G_CMD_CFG_RX_ENA);
  MacWrite32 (Ctx, MAC_1G_CMD_CFG, Val);
}

STATIC
VOID
AlEthMacStop (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINT32  Val;

  Val = MacRead32 (Ctx, MAC_1G_CMD_CFG);
  Val &= ~(MAC_1G_CMD_CFG_TX_ENA | MAC_1G_CMD_CFG_RX_ENA);
  MacWrite32 (Ctx, MAC_1G_CMD_CFG, Val);
}

// ============================================================================
// MAC Address
// ============================================================================

STATIC
VOID
AlEthSetMacAddr (
  IN AL_ETH_CONTEXT     *Ctx,
  IN EFI_MAC_ADDRESS    *Addr
  )
{
  UINT32  Lo;
  UINT32  Hi;

  Lo = Addr->Addr[0] |
       ((UINT32)Addr->Addr[1] << 8) |
       ((UINT32)Addr->Addr[2] << 16) |
       ((UINT32)Addr->Addr[3] << 24);
  Hi = Addr->Addr[4] |
       ((UINT32)Addr->Addr[5] << 8);

  MacWrite32 (Ctx, MAC_1G_MAC_0, Lo);
  MacWrite32 (Ctx, MAC_1G_MAC_1, Hi);
}

STATIC
VOID
AlEthGetMacAddr (
  IN  AL_ETH_CONTEXT   *Ctx,
  OUT EFI_MAC_ADDRESS   *Addr
  )
{
  UINT32  Lo;
  UINT32  Hi;

  Lo = MacRead32 (Ctx, MAC_1G_MAC_0);
  Hi = MacRead32 (Ctx, MAC_1G_MAC_1);

  ZeroMem (Addr, sizeof (*Addr));
  Addr->Addr[0] = (UINT8)(Lo & 0xFF);
  Addr->Addr[1] = (UINT8)((Lo >> 8) & 0xFF);
  Addr->Addr[2] = (UINT8)((Lo >> 16) & 0xFF);
  Addr->Addr[3] = (UINT8)((Lo >> 24) & 0xFF);
  Addr->Addr[4] = (UINT8)(Hi & 0xFF);
  Addr->Addr[5] = (UINT8)((Hi >> 8) & 0xFF);
}

// ============================================================================
// EC Configuration
// ============================================================================

STATIC
VOID
AlEthEcInit (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  EcWrite32 (Ctx, EC_GEN_EN, 0xFFFFFFFF);
  EcWrite32 (Ctx, EC_GEN_FIFO_EN, 0xFFFFFFFF);
  EcWrite32 (Ctx, EC_MAC_MIN_PKT, 30);
  EcWrite32 (Ctx, EC_MAC_MAX_PKT, AL_ETH_MAX_PKT_SIZE);
}

// ============================================================================
// UDMA Init — follows kernel al_mod_udma_init + al_mod_udma_set_defaults
//
// Critical difference from previous code:
//   Kernel transitions UDMA to NORMAL *before* queue config.
//   The UDMA idles in NORMAL with no queues enabled until queues are set up.
// ============================================================================

STATIC
VOID
AlEthUdmaInit (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINT32  Val;

  //
  // Step 1: Disable queues — Boot ROM may have left EN_PREF set with
  // stale ring addresses, causing AXI errors when UDMA transitions.
  //
  Val = UdmaRead32 (Ctx, M2S_Q0_CFG);
  Val &= ~(UDMA_Q_CFG_EN_PREF | UDMA_Q_CFG_EN_SCHEDULING);
  UdmaWrite32 (Ctx, M2S_Q0_CFG, Val);

  Val = UdmaRead32 (Ctx, S2M_Q0_CFG);
  Val &= ~(UDMA_Q_CFG_EN_PREF | UDMA_Q_CFG_EN_SCHEDULING);
  UdmaWrite32 (Ctx, S2M_Q0_CFG, Val);

  //
  // Step 2: Queue reset. HP registers are NOT reset by this — they retain
  // stale values from RouterBOOT. We must read and adapt to them.
  //
  UdmaWrite32 (Ctx, M2S_Q0_SW_CTRL, UDMA_SW_CTRL_RST_Q);
  UdmaWrite32 (Ctx, S2M_Q0_SW_CTRL, UDMA_SW_CTRL_RST_Q);
  MicroSecondDelay (10);

  //
  // Step 3: UDMA defaults — prefetch, FIFO, AXI timeout, comp ack
  //
  UdmaWrite32 (Ctx, M2S_DESC_PREF_CFG_3, 0x1081);
  UdmaWrite32 (Ctx, S2M_DESC_PREF_CFG_3, 0x1081);

  Val = UdmaRead32 (Ctx, M2S_RD_DATA_CFG);
  Val &= ~0xFFF;
  Val |= 256;
  UdmaWrite32 (Ctx, M2S_RD_DATA_CFG, Val);

  UdmaWrite32 (Ctx, GEN_AXI_CFG_1, 5000000);
  UdmaWrite32 (Ctx, M2S_COMP_ACK, 0);
  UdmaWrite32 (Ctx, S2M_COMP_ACK, 0);

  Val = UdmaRead32 (Ctx, S2M_Q0_COMP_CFG);
  Val &= ~0x6;
  UdmaWrite32 (Ctx, S2M_Q0_COMP_CFG, Val);

  //
  // Step 4: Transition to NORMAL
  //
  UdmaWrite32 (Ctx, M2S_CHANGE_STATE, UDMA_CHANGE_NORMAL);
  UdmaWrite32 (Ctx, S2M_CHANGE_STATE, UDMA_CHANGE_NORMAL);
  MicroSecondDelay (1000);

  //
  // HP values are read AFTER the second SW_CTRL_RST_Q in AlEthQueueConfig,
  // not here — that reset changes HP state.
  //
}

// ============================================================================
// Queue Configuration — follows kernel al_mod_udma_q_init sequence
// ============================================================================

STATIC
VOID
AlEthQueueConfig (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINT32  Val;

  //
  // Queue reset in NORMAL state — required for the DMA to properly accept
  // new ring base pointers.
  //
  UdmaWrite32 (Ctx, M2S_Q0_SW_CTRL, UDMA_SW_CTRL_RST_Q);
  UdmaWrite32 (Ctx, S2M_Q0_SW_CTRL, UDMA_SW_CTRL_RST_Q);
  MicroSecondDelay (10);

  //
  // Read HP values NOW — after the final queue reset. These are the actual
  // values the DMA will use. Reading before this reset gave wrong values.
  //
  {
    UINT32  Rdrhp = UdmaRead32 (Ctx, S2M_Q0_RDRHP);
    UINT32  Rcrhp = UdmaRead32 (Ctx, S2M_Q0_RCRHP);
    UINT32  DescIdx = Rdrhp & 0x1F;
    UINT32  CompIdx = Rcrhp & 0x1F;

    Ctx->RxDescRingId = (Rdrhp >> 30) & AL_UDMA_RING_ID_MASK;
    Ctx->RxConsIdx = CompIdx;
    Ctx->RxProdIdx = 0;
    Ctx->RxCompDescOffset = (CompIdx - DescIdx + AL_ETH_NUM_RX_DESC) % AL_ETH_NUM_RX_DESC;

    DEBUG ((DEBUG_INFO, "AlEthDxe: Post-reset HP: RDRHP=0x%08x RCRHP=0x%08x → descRid=%d consIdx=%d offset=%d\n",
            Rdrhp, Rcrhp, Ctx->RxDescRingId, Ctx->RxConsIdx, Ctx->RxCompDescOffset));
  }

  //
  // TX Queue 0: al_mod_udma_q_config → clear INTERNAL_PAUSE_DMB in rate limiter
  //
  Val = UdmaRead32 (Ctx, M2S_Q0_RLIMIT_MASK);
  Val &= ~UDMA_RLIMIT_MASK_INTERNAL_PAUSE_DMB;
  UdmaWrite32 (Ctx, M2S_Q0_RLIMIT_MASK, Val);

  //
  // TX Queue 0: al_mod_udma_q_set_pointers → ring base, length, comp ring
  //
  UdmaWrite32 (Ctx, M2S_Q0_TDRBP_LOW,  (UINT32)(Ctx->TxDescRingPhys & 0xFFFFFFFF));
  UdmaWrite32 (Ctx, M2S_Q0_TDRBP_HIGH, (UINT32)(Ctx->TxDescRingPhys >> 32));
  UdmaWrite32 (Ctx, M2S_Q0_TDRL,        AL_ETH_NUM_TX_DESC);
  UdmaWrite32 (Ctx, M2S_Q0_TCRBP_LOW,  (UINT32)(Ctx->TxCompRingPhys & 0xFFFFFFFF));
  UdmaWrite32 (Ctx, M2S_Q0_TCRBP_HIGH, (UINT32)(Ctx->TxCompRingPhys >> 32));

  //
  // TX Queue 0: al_mod_udma_q_config_compl → EN_COMP_RING_UPDATE | DIS_COMP_COAL
  //
  Val = UdmaRead32 (Ctx, M2S_Q0_COMP_CFG);
  Val |= UDMA_COMP_CFG_EN_RING_UPDATE | UDMA_COMP_CFG_DIS_COAL;
  UdmaWrite32 (Ctx, M2S_Q0_COMP_CFG, Val);

  //
  // TX Queue 0: al_mod_udma_q_enable → EN_PREF | EN_SCHEDULING
  //
  Val = UdmaRead32 (Ctx, M2S_Q0_CFG);
  Val |= UDMA_Q_CFG_EN_PREF | UDMA_Q_CFG_EN_SCHEDULING;
  UdmaWrite32 (Ctx, M2S_Q0_CFG, Val);

  //
  // RX Queue 0: al_mod_udma_q_set_pointers → ring base, length, comp ring
  //
  UdmaWrite32 (Ctx, S2M_Q0_RDRBP_LOW,  (UINT32)(Ctx->RxDescRingPhys & 0xFFFFFFFF));
  UdmaWrite32 (Ctx, S2M_Q0_RDRBP_HIGH, (UINT32)(Ctx->RxDescRingPhys >> 32));
  UdmaWrite32 (Ctx, S2M_Q0_RDRL,        AL_ETH_NUM_RX_DESC);
  UdmaWrite32 (Ctx, S2M_Q0_RCRBP_LOW,  (UINT32)(Ctx->RxCompRingPhys & 0xFFFFFFFF));
  UdmaWrite32 (Ctx, S2M_Q0_RCRBP_HIGH, (UINT32)(Ctx->RxCompRingPhys >> 32));

  //
  // RX Queue 0: al_mod_udma_q_config_compl → EN_COMP_RING_UPDATE | DIS_COMP_COAL
  //
  Val = UdmaRead32 (Ctx, S2M_Q0_COMP_CFG);
  Val |= UDMA_COMP_CFG_EN_RING_UPDATE | UDMA_COMP_CFG_DIS_COAL;
  UdmaWrite32 (Ctx, S2M_Q0_COMP_CFG, Val);

  //
  // RX Queue 0: set completion descriptor size in s2m_comp.cfg_1c
  // (kernel: al_mod_udma_q_config_compl, cdesc_size >> 2 in words)
  // Our cdesc is 16 bytes = 4 words.
  //
  Val = UdmaRead32 (Ctx, S2M_COMP_CFG_1C_REG);
  Val &= ~S2M_COMP_CFG_1C_DESC_SIZE_MASK;
  Val |= (sizeof (AL_ETH_CDESC) >> 2) & S2M_COMP_CFG_1C_DESC_SIZE_MASK;
  UdmaWrite32 (Ctx, S2M_COMP_CFG_1C_REG, Val);

  //
  // RX Queue 0: al_mod_udma_q_enable → EN_PREF | EN_SCHEDULING
  //
  Val = UdmaRead32 (Ctx, S2M_Q0_CFG);
  Val |= UDMA_Q_CFG_EN_PREF | UDMA_Q_CFG_EN_SCHEDULING;
  UdmaWrite32 (Ctx, S2M_Q0_CFG, Val);

  DEBUG ((DEBUG_INFO, "AlEthDxe: M2S_Q0_CFG=0x%x S2M_Q0_CFG=0x%x\n",
          UdmaRead32 (Ctx, M2S_Q0_CFG), UdmaRead32 (Ctx, S2M_Q0_CFG)));
  DEBUG ((DEBUG_INFO, "AlEthDxe: M2S_Q0_COMP_CFG=0x%x S2M_Q0_COMP_CFG=0x%x\n",
          UdmaRead32 (Ctx, M2S_Q0_COMP_CFG), UdmaRead32 (Ctx, S2M_Q0_COMP_CFG)));
  DEBUG ((DEBUG_INFO, "AlEthDxe: S2M_COMP_CFG_1C=0x%x\n",
          UdmaRead32 (Ctx, S2M_COMP_CFG_1C_REG)));

  //
  // Verify ring base registers match our allocations
  //
  DEBUG ((DEBUG_INFO, "AlEthDxe: TDRBP readback=0x%08x_%08x (expect 0x%lx)\n",
          UdmaRead32 (Ctx, M2S_Q0_TDRBP_HIGH), UdmaRead32 (Ctx, M2S_Q0_TDRBP_LOW),
          Ctx->TxDescRingPhys));
  DEBUG ((DEBUG_INFO, "AlEthDxe: TCRBP readback=0x%08x_%08x (expect 0x%lx)\n",
          UdmaRead32 (Ctx, M2S_Q0_TCRBP_HIGH), UdmaRead32 (Ctx, M2S_Q0_TCRBP_LOW),
          Ctx->TxCompRingPhys));
  DEBUG ((DEBUG_INFO, "AlEthDxe: TDRL readback=%d (expect %d)\n",
          UdmaRead32 (Ctx, M2S_Q0_TDRL), AL_ETH_NUM_TX_DESC));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RDRBP readback=0x%08x_%08x (expect 0x%lx)\n",
          UdmaRead32 (Ctx, S2M_Q0_RDRBP_HIGH), UdmaRead32 (Ctx, S2M_Q0_RDRBP_LOW),
          Ctx->RxDescRingPhys));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RCRBP readback=0x%08x_%08x (expect 0x%lx)\n",
          UdmaRead32 (Ctx, S2M_Q0_RCRBP_HIGH), UdmaRead32 (Ctx, S2M_Q0_RCRBP_LOW),
          Ctx->RxCompRingPhys));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RDRL readback=%d (expect %d)\n",
          UdmaRead32 (Ctx, S2M_Q0_RDRL), AL_ETH_NUM_RX_DESC));
}

// ============================================================================
// RX Buffer Population
// ============================================================================

STATIC
EFI_STATUS
AlEthRxFill (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  EFI_STATUS          Status;
  UINTN               Idx;
  AL_ETH_DESC         *Desc;
  EFI_PHYSICAL_ADDRESS PhysAddr;

  //
  // RxDescRingId already set by AlEthUdmaInit from stale RDRHP ring_id
  //

  for (Idx = 0; Idx < AL_ETH_NUM_RX_DESC; Idx++) {
    EFI_PHYSICAL_ADDRESS  BufPages;

    BufPages = 0xFFFFFFFF;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE),
                    &BufPages
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "AlEthDxe: Failed to allocate RX buffer %u\n", Idx));
      return Status;
    }

    //
    // Flush any stale cache lines before switching to UC
    //
    WriteBackInvalidateDataCacheRange (
      (VOID *)(UINTN)BufPages,
      EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE))
      );

    if (mCpu != NULL) {
      Status = mCpu->SetMemoryAttributes (
                       mCpu,
                       BufPages,
                       EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE)),
                       EFI_MEMORY_UC
                       );
      if (EFI_ERROR (Status) && Idx == 0) {
        DEBUG ((DEBUG_WARN, "AlEthDxe: RX buf UC failed: %r\n", Status));
      }
    }

    Ctx->RxBuffers[Idx] = (VOID *)(UINTN)BufPages;
    Ctx->RxBufferMappings[Idx] = NULL;
    PhysAddr = BufPages;
    Ctx->RxBuffersPhys[Idx] = PhysAddr;

    Desc = (AL_ETH_DESC *)Ctx->RxDescRing + Idx;
    {
      UINTN  Da = (UINTN)Desc;
      MmioWrite32 (Da + 0, (AL_ETH_RX_BUF_SIZE & AL_S2M_DESC_LEN_MASK) |
                            AL_S2M_DESC_RING_ID(Ctx->RxDescRingId));
      MmioWrite32 (Da + 4, 0);
      MmioWrite32 (Da + 8, (UINT32)(PhysAddr & 0xFFFFFFFF));
      MmioWrite32 (Da + 12, (UINT32)(PhysAddr >> 32));
    }
  }

  MemoryFence ();
  UdmaWrite32 (Ctx, S2M_Q0_RDRTP_INC, AL_ETH_NUM_RX_DESC);

  DEBUG ((DEBUG_INFO, "AlEthDxe: RxFill: posted %d descs, consIdx=%d compRid=%d nextDescRid=%d\n",
          AL_ETH_NUM_RX_DESC, Ctx->RxConsIdx, Ctx->RxCompRingId, Ctx->RxDescRingId));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RxFill: S2M_STATE=0x%08x RDRHP=0x%08x RCRHP=0x%08x\n",
          UdmaRead32 (Ctx, S2M_STATE),
          UdmaRead32 (Ctx, S2M_Q0_RDRHP),
          UdmaRead32 (Ctx, S2M_Q0_RCRHP)));

  //
  // After filling all 32 descriptors (indices 0..31), the producer index
  // wraps back to 0. Ring_id increments on wrap.
  // RxConsIdx and RxCompRingId were set by AlEthUdmaInit from stale HP state.
  //
  Ctx->RxProdIdx = 0;
  Ctx->RxDescRingId = (Ctx->RxDescRingId + 1) & AL_UDMA_RING_ID_MASK;

  //
  // Drain stale completions: the DMA starts processing immediately after
  // RDRTP_INC, so by the time we get here, some completions may already
  // exist from background traffic or stale RouterBOOT descriptor processing.
  // Flush them all so the ring is clean when the network stack starts polling.
  //
  {
    UINTN   DrainCount = 0;
    UINT32  DmaHead;

    MicroSecondDelay (1000);  // give DMA time to process initial descriptors

    DmaHead = UdmaRead32 (Ctx, S2M_Q0_RCRHP) & (AL_ETH_NUM_RX_DESC - 1);
    while (DmaHead != Ctx->RxConsIdx && DrainCount < AL_ETH_NUM_RX_DESC) {
      // Re-submit the consumed descriptor
      AL_ETH_DESC  *DDesc = (AL_ETH_DESC *)Ctx->RxDescRing + Ctx->RxProdIdx;
      UINTN  DDa = (UINTN)DDesc;
      MmioWrite32 (DDa + 0, (AL_ETH_RX_BUF_SIZE & AL_S2M_DESC_LEN_MASK) |
                             AL_S2M_DESC_RING_ID(Ctx->RxDescRingId));
      MmioWrite32 (DDa + 4, 0);
      MmioWrite32 (DDa + 8, (UINT32)(Ctx->RxBuffersPhys[Ctx->RxProdIdx] & 0xFFFFFFFF));
      MmioWrite32 (DDa + 12, (UINT32)(Ctx->RxBuffersPhys[Ctx->RxProdIdx] >> 32));
      MemoryFence ();
      UdmaWrite32 (Ctx, S2M_Q0_RDRTP_INC, 1);

      Ctx->RxConsIdx = (Ctx->RxConsIdx + 1) % AL_ETH_NUM_RX_DESC;
      Ctx->RxProdIdx = (Ctx->RxProdIdx + 1) % AL_ETH_NUM_RX_DESC;
      if (Ctx->RxProdIdx == 0) {
        Ctx->RxDescRingId = (Ctx->RxDescRingId + 1) & AL_UDMA_RING_ID_MASK;
      }
      DrainCount++;
    }

    DEBUG ((DEBUG_INFO, "AlEthDxe: RxFill: drained %u stale completions, consIdx=%d\n",
            DrainCount, Ctx->RxConsIdx));
  }

  return EFI_SUCCESS;
}

// ============================================================================
// PHY Init
// ============================================================================

STATIC
EFI_STATUS
AlEthPhyInit (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  EFI_STATUS  Status;
  UINT16      PhyId1;
  UINT16      PhyId2;
  UINT16      Bmcr;
  UINTN       Timeout;
  UINT8       PhyAddr;
  UINT8       Addr;
  BOOLEAN     Found;

  Found = FALSE;
  PhyAddr = AL_ETH_PHY_ADDR;

  for (Addr = 0; Addr < 32; Addr++) {
    UINT8  Try = (Addr == 0) ? AL_ETH_PHY_ADDR : ((Addr <= AL_ETH_PHY_ADDR) ? Addr - 1 : Addr);

    Status = AlEthMdioRead (Ctx, Try, 2, &PhyId1);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = AlEthMdioRead (Ctx, Try, 3, &PhyId2);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((PhyId1 == 0xFFFF && PhyId2 == 0xFFFF) ||
        (PhyId1 == 0x0000 && PhyId2 == 0x0000))
    {
      continue;
    }

    PhyAddr = Try;
    Found = TRUE;
    DEBUG ((DEBUG_INFO, "AlEthDxe: Found PHY at MDIO addr %d, ID = 0x%04X:0x%04X\n",
            PhyAddr, PhyId1, PhyId2));
    break;
  }

  if (!Found) {
    DEBUG ((DEBUG_ERROR, "AlEthDxe: No PHY found on MDIO bus\n"));
    return EFI_NOT_FOUND;
  }

  // Software reset
  Status = AlEthMdioWrite (Ctx, PhyAddr, 0, BIT15);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Timeout = 0; Timeout < 1000; Timeout++) {
    MicroSecondDelay (1000);
    Status = AlEthMdioRead (Ctx, PhyAddr, 0, &Bmcr);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((Bmcr & BIT15) == 0) {
      break;
    }
  }

  if (Bmcr & BIT15) {
    DEBUG ((DEBUG_ERROR, "AlEthDxe: PHY reset timeout\n"));
    return EFI_TIMEOUT;
  }

  // Advertise 10/100 full/half + 802.3 selector
  Status = AlEthMdioWrite (Ctx, PhyAddr, 4, 0x01E1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // 1000BASE-T control: advertise 1000BASE-T full duplex
  Status = AlEthMdioWrite (Ctx, PhyAddr, 9, 0x0200);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Enable auto-negotiation and restart
  Status = AlEthMdioWrite (Ctx, PhyAddr, 0, 0x1200);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "AlEthDxe: PHY auto-negotiation started\n"));
  return EFI_SUCCESS;
}

// ============================================================================
// DMA ring allocation helpers
// ============================================================================

STATIC
EFI_STATUS
AlEthAllocateRing (
  IN  AL_ETH_CONTEXT       *Ctx,
  IN  UINTN                Size,
  OUT VOID                 **VirtAddr,
  OUT EFI_PHYSICAL_ADDRESS *PhysAddr,
  OUT VOID                 **Mapping
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Pages;

  Pages = 0xFFFFFFFF;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  EFI_SIZE_TO_PAGES (Size),
                  &Pages
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *VirtAddr = (VOID *)(UINTN)Pages;
  *PhysAddr = Pages;
  *Mapping = NULL;

  //
  // Zero while still cacheable (fast), then flush to RAM and invalidate
  // all cache lines BEFORE switching to UC.  On ARM64, changing PTE from
  // WB→UC without cleaning the cache leaves stale lines that respond to
  // reads, hiding DMA-written data.
  //
  ZeroMem (*VirtAddr, Size);
  WriteBackInvalidateDataCacheRange (*VirtAddr, Size);

  //
  // Map as uncacheable so DMA engine sees writes immediately.
  // This is what dma_alloc_coherent() does in Linux.
  //
  if (mCpu == NULL) {
    gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  }

  if (mCpu != NULL) {
    Status = mCpu->SetMemoryAttributes (
                     mCpu,
                     Pages,
                     EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (Size)),
                     EFI_MEMORY_UC
                     );
    DEBUG ((DEBUG_INFO, "AlEthDxe: SetMemoryAttributes UC @ 0x%lx: %r\n", Pages, Status));
  } else {
    DEBUG ((DEBUG_ERROR, "AlEthDxe: CPU protocol not found, cannot set UC!\n"));
  }

  return EFI_SUCCESS;
}

STATIC
VOID
AlEthFreeRing (
  IN AL_ETH_CONTEXT  *Ctx,
  IN UINTN           Size,
  IN VOID            *VirtAddr,
  IN VOID            *Mapping
  )
{
  if (VirtAddr != NULL) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)VirtAddr, EFI_SIZE_TO_PAGES (Size));
  }
}

STATIC
VOID
AlEthFreeRxBuffers (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINTN  Idx;

  for (Idx = 0; Idx < AL_ETH_NUM_RX_DESC; Idx++) {
    Ctx->RxBufferMappings[Idx] = NULL;
    if (Ctx->RxBuffers[Idx] != NULL) {
      gBS->FreePages (
             (EFI_PHYSICAL_ADDRESS)(UINTN)Ctx->RxBuffers[Idx],
             EFI_SIZE_TO_PAGES (AL_ETH_RX_BUF_SIZE)
             );
      Ctx->RxBuffers[Idx] = NULL;
    }
  }
}

// ============================================================================
// HW Initialize / Shutdown — kernel init order
// ============================================================================

STATIC
EFI_STATUS
AlEthHwInitialize (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  EFI_STATUS  Status;
  UINTN       DescRingSize;
  UINTN       CompRingSize;

  //
  // Allocate TX descriptor and completion rings
  //
  DescRingSize = AL_ETH_NUM_TX_DESC * sizeof (AL_ETH_DESC);
  CompRingSize = AL_ETH_NUM_TX_DESC * sizeof (AL_ETH_CDESC);

  Status = AlEthAllocateRing (
             Ctx, DescRingSize,
             &Ctx->TxDescRing, &Ctx->TxDescRingPhys, &Ctx->TxDescRingMapping
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AlEthAllocateRing (
             Ctx, CompRingSize,
             &Ctx->TxCompRing, &Ctx->TxCompRingPhys, &Ctx->TxCompRingMapping
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Allocate RX descriptor and completion rings
  //
  DescRingSize = AL_ETH_NUM_RX_DESC * sizeof (AL_ETH_DESC);
  CompRingSize = AL_ETH_NUM_RX_DESC * sizeof (AL_ETH_CDESC);

  Status = AlEthAllocateRing (
             Ctx, DescRingSize,
             &Ctx->RxDescRing, &Ctx->RxDescRingPhys, &Ctx->RxDescRingMapping
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AlEthAllocateRing (
             Ctx, CompRingSize,
             &Ctx->RxCompRing, &Ctx->RxCompRingPhys, &Ctx->RxCompRingMapping
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Initialize UDMA — sets defaults and transitions to NORMAL (kernel order)
  //
  DEBUG ((DEBUG_INFO, "AlEthDxe: UDMA init (kernel HAL sequence)...\n"));
  AlEthUdmaInit (Ctx);

  //
  // Configure queues — after UDMA is already NORMAL (kernel order)
  //
  DEBUG ((DEBUG_INFO, "AlEthDxe: Queue config...\n"));
  AlEthQueueConfig (Ctx);

  //
  // EC init
  //
  if (Ctx->EcBase != 0) {
    DEBUG ((DEBUG_INFO, "AlEthDxe: EC init (base=0x%lx)...\n", (UINT64)Ctx->EcBase));
    AlEthEcInit (Ctx);
  } else {
    DEBUG ((DEBUG_INFO, "AlEthDxe: EC BAR not present, skipping EC init\n"));
  }

  //
  // MAC RGMII config
  //
  DEBUG ((DEBUG_INFO, "AlEthDxe: MAC RGMII config...\n"));
  AlEthMacConfigRgmii (Ctx);

  //
  // MDIO config
  //
  DEBUG ((DEBUG_INFO, "AlEthDxe: MDIO config...\n"));
  AlEthMdioConfig (Ctx);

  //
  // PHY init
  //
  Status = AlEthPhyInit (Ctx);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AlEthDxe: PHY init failed: %r (continuing)\n", Status));
  }

  //
  // Set MAC address
  //
  AlEthSetMacAddr (Ctx, &Ctx->SnpMode.CurrentAddress);

  //
  // Fill RX buffers
  //
  Status = AlEthRxFill (Ctx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // MAC start — TX and RX enabled
  //
  AlEthMacStart (Ctx);

  //
  // Wait for PHY link to come up. AR8035 auto-negotiation typically takes
  // 2-5 seconds. Without this wait, MnpDxe sees MediaPresent=FALSE during
  // early DHCP and prints "No network cable detected", then gives up before
  // the link is established.
  //
  {
    UINTN   LinkWait;
    UINT32  RgmiiStat;

    DEBUG ((DEBUG_INFO, "AlEthDxe: Waiting for link...\n"));
    for (LinkWait = 0; LinkWait < 5000; LinkWait += 100) {
      RgmiiStat = MacRead32 (Ctx, MAC_GEN_RGMII_STAT);
      if (RgmiiStat & RGMII_STAT_LINK) {
        DEBUG ((DEBUG_INFO, "AlEthDxe: Link up after %u ms (RGMII_STAT=0x%x)\n",
                LinkWait, RgmiiStat));
        Ctx->SnpMode.MediaPresent = TRUE;
        break;
      }
      MicroSecondDelay (100000);  // 100ms
    }

    if (LinkWait >= 5000) {
      DEBUG ((DEBUG_WARN, "AlEthDxe: Link not up after 5s (RGMII_STAT=0x%x), continuing\n",
              MacRead32 (Ctx, MAC_GEN_RGMII_STAT)));
    }
  }

  //
  // Initialize TX ring tracking
  //
  Ctx->TxProdIdx = 0;
  Ctx->TxConsIdx = 0;
  Ctx->TxBufInFlight = NULL;
  Ctx->TxDescRingId = AL_UDMA_INITIAL_RING_ID;
  Ctx->TxCompRingId = AL_UDMA_INITIAL_RING_ID;

  //
  // Dump DMA state for debug
  //
  DEBUG ((DEBUG_INFO, "AlEthDxe: TX desc ring phys=0x%lx virt=%p\n",
          Ctx->TxDescRingPhys, Ctx->TxDescRing));
  DEBUG ((DEBUG_INFO, "AlEthDxe: TX comp ring phys=0x%lx\n", Ctx->TxCompRingPhys));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RX desc ring phys=0x%lx virt=%p\n",
          Ctx->RxDescRingPhys, Ctx->RxDescRing));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RX comp ring phys=0x%lx\n", Ctx->RxCompRingPhys));
  DEBUG ((DEBUG_INFO, "AlEthDxe: RX buf[0] virt=%p phys=0x%lx\n",
          Ctx->RxBuffers[0], Ctx->RxBuffersPhys[0]));
  DEBUG ((DEBUG_INFO, "AlEthDxe: M2S_STATE=0x%x S2M_STATE=0x%x\n",
          UdmaRead32 (Ctx, M2S_STATE), UdmaRead32 (Ctx, S2M_STATE)));
  DEBUG ((DEBUG_INFO, "AlEthDxe: M2S_Q0_CFG=0x%x S2M_Q0_CFG=0x%x\n",
          UdmaRead32 (Ctx, M2S_Q0_CFG), UdmaRead32 (Ctx, S2M_Q0_CFG)));
  DEBUG ((DEBUG_INFO, "AlEthDxe: MAC_1G_CMD_CFG=0x%x\n",
          MacRead32 (Ctx, MAC_1G_CMD_CFG)));

  return EFI_SUCCESS;
}

STATIC
VOID
AlEthHwShutdown (
  IN AL_ETH_CONTEXT  *Ctx
  )
{
  UINTN  DescRingSize;
  UINTN  CompRingSize;

  AlEthMacStop (Ctx);
  MicroSecondDelay (10);

  UdmaWrite32 (Ctx, M2S_CHANGE_STATE, UDMA_CHANGE_DIS);
  UdmaWrite32 (Ctx, S2M_CHANGE_STATE, UDMA_CHANGE_DIS);
  MicroSecondDelay (100);

  AlEthFreeRxBuffers (Ctx);

  DescRingSize = AL_ETH_NUM_TX_DESC * sizeof (AL_ETH_DESC);
  CompRingSize = AL_ETH_NUM_TX_DESC * sizeof (AL_ETH_CDESC);
  AlEthFreeRing (Ctx, DescRingSize, Ctx->TxDescRing, Ctx->TxDescRingMapping);
  AlEthFreeRing (Ctx, CompRingSize, Ctx->TxCompRing, Ctx->TxCompRingMapping);
  Ctx->TxDescRing = NULL;
  Ctx->TxCompRing = NULL;
  Ctx->TxDescRingMapping = NULL;
  Ctx->TxCompRingMapping = NULL;

  DescRingSize = AL_ETH_NUM_RX_DESC * sizeof (AL_ETH_DESC);
  CompRingSize = AL_ETH_NUM_RX_DESC * sizeof (AL_ETH_CDESC);
  AlEthFreeRing (Ctx, DescRingSize, Ctx->RxDescRing, Ctx->RxDescRingMapping);
  AlEthFreeRing (Ctx, CompRingSize, Ctx->RxCompRing, Ctx->RxCompRingMapping);
  Ctx->RxDescRing = NULL;
  Ctx->RxCompRing = NULL;
  Ctx->RxDescRingMapping = NULL;
  Ctx->RxCompRingMapping = NULL;
}

// ============================================================================
// ExitBootServices callback
// ============================================================================

STATIC
VOID
EFIAPI
AlEthExitBootServicesCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = (AL_ETH_CONTEXT *)Context;
  AlEthMacStop (Ctx);
  UdmaWrite32 (Ctx, M2S_CHANGE_STATE, UDMA_CHANGE_DIS);
  UdmaWrite32 (Ctx, S2M_CHANGE_STATE, UDMA_CHANGE_DIS);
}

// ============================================================================
// SNP Protocol Implementation
// ============================================================================

STATIC
EFI_STATUS
EFIAPI
AlEthSnpStart (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State == EfiSimpleNetworkStarted ||
      Ctx->SnpMode.State == EfiSimpleNetworkInitialized)
  {
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
AlEthSnpStop (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  Ctx->SnpMode.State = EfiSimpleNetworkStopped;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthSnpInitialize (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN UINTN                        ExtraRxBufferSize  OPTIONAL,
  IN UINTN                        ExtraTxBufferSize  OPTIONAL
  )
{
  AL_ETH_CONTEXT  *Ctx;
  EFI_STATUS      Status;

  Ctx = AL_ETH_FROM_SNP (This);

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
AlEthSnpReset (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN BOOLEAN                      ExtendedVerification
  )
{
  AL_ETH_CONTEXT  *Ctx;
  EFI_STATUS      Status;

  Ctx = AL_ETH_FROM_SNP (This);

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
AlEthSnpShutdown (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = AL_ETH_FROM_SNP (This);

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
AlEthSnpReceiveFilters (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN UINT32                       Enable,
  IN UINT32                       Disable,
  IN BOOLEAN                      ResetMCastFilter,
  IN UINTN                        MCastFilterCnt    OPTIONAL,
  IN EFI_MAC_ADDRESS              *MCastFilter      OPTIONAL
  )
{
  AL_ETH_CONTEXT  *Ctx;
  UINT32          Val;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ?
           EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  Ctx->SnpMode.ReceiveFilterSetting |= Enable;
  Ctx->SnpMode.ReceiveFilterSetting &= ~Disable;

  Val = MacRead32 (Ctx, MAC_1G_CMD_CFG);
  if (Ctx->SnpMode.ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) {
    Val |= MAC_1G_CMD_CFG_PROMIS_EN;
  } else {
    Val &= ~MAC_1G_CMD_CFG_PROMIS_EN;
  }

  MacWrite32 (Ctx, MAC_1G_CMD_CFG, Val);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthSnpStationAddress (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN BOOLEAN                      Reset,
  IN EFI_MAC_ADDRESS              *New  OPTIONAL
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ?
           EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Reset) {
    CopyMem (&Ctx->SnpMode.CurrentAddress, &Ctx->SnpMode.PermanentAddress, sizeof (EFI_MAC_ADDRESS));
  } else if (New != NULL) {
    CopyMem (&Ctx->SnpMode.CurrentAddress, New, sizeof (EFI_MAC_ADDRESS));
  } else {
    return EFI_INVALID_PARAMETER;
  }

  AlEthSetMacAddr (Ctx, &Ctx->SnpMode.CurrentAddress);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
AlEthSnpStatistics (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN     BOOLEAN                      Reset,
  IN OUT UINTN                        *StatisticsSize  OPTIONAL,
  OUT    EFI_NETWORK_STATISTICS       *StatisticsTable OPTIONAL
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ?
           EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Reset) {
    ZeroMem (&Ctx->Stats, sizeof (Ctx->Stats));
  }

  Ctx->Stats.TxGoodFrames = MacRead32 (Ctx, MAC_1G_STAT_FRAMES_TXED_OK);
  Ctx->Stats.RxGoodFrames = MacRead32 (Ctx, MAC_1G_STAT_FRAMES_RXED_OK);
  Ctx->Stats.RxCrcErrorFrames = MacRead32 (Ctx, MAC_1G_STAT_FCS_ERRORS);

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
AlEthSnpMCastIpToMac (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN  BOOLEAN                      IPv6,
  IN  EFI_IP_ADDRESS               *IP,
  OUT EFI_MAC_ADDRESS              *MAC
  )
{
  if (IP == NULL || MAC == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (MAC, sizeof (*MAC));

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
AlEthSnpNvData (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN     BOOLEAN                      ReadWrite,
  IN     UINTN                        Offset,
  IN     UINTN                        BufferSize,
  IN OUT VOID                         *Buffer
  )
{
  return EFI_UNSUPPORTED;
}

// ============================================================================
// GetStatus — TX completion via software ring_id matching (kernel pattern)
// ============================================================================

STATIC
EFI_STATUS
EFIAPI
AlEthSnpGetStatus (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  OUT UINT32                       *InterruptStatus  OPTIONAL,
  OUT VOID                         **TxBuf           OPTIONAL
  )
{
  AL_ETH_CONTEXT  *Ctx;
  UINT32          RgmiiStat;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ?
           EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (InterruptStatus != NULL) {
    *InterruptStatus = 0;
  }

  //
  // Check TX completions using ring_id matching (kernel pattern)
  //
  if (TxBuf != NULL) {
    *TxBuf = NULL;
    if (Ctx->TxBufInFlight != NULL) {
      {
        UINT32  DmaTxHead = UdmaRead32 (Ctx, M2S_Q0_TCRHP) & (AL_ETH_NUM_TX_DESC - 1);
        if (DmaTxHead != Ctx->TxConsIdx) {
          *TxBuf = Ctx->TxBufInFlight;
          Ctx->TxBufInFlight = NULL;
          Ctx->TxConsIdx = (Ctx->TxConsIdx + 1) % AL_ETH_NUM_TX_DESC;
        }
      }
    }
  }

  //
  // Check link status
  //
  RgmiiStat = MacRead32 (Ctx, MAC_GEN_RGMII_STAT);
  Ctx->SnpMode.MediaPresent = ((RgmiiStat & RGMII_STAT_LINK) != 0);

  return EFI_SUCCESS;
}

// ============================================================================
// Transmit — kernel tx_pkt_prepare pattern with ring_id, CONCAT, barrier
// ============================================================================

STATIC
EFI_STATUS
EFIAPI
AlEthSnpTransmit (
  IN EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  IN UINTN                        HeaderSize,
  IN UINTN                        BufferSize,
  IN VOID                         *Buffer,
  IN EFI_MAC_ADDRESS              *SrcAddr   OPTIONAL,
  IN EFI_MAC_ADDRESS              *DestAddr  OPTIONAL,
  IN UINT16                       *Protocol  OPTIONAL
  )
{
  AL_ETH_CONTEXT       *Ctx;
  UINT8                *Pkt;
  AL_ETH_DESC          *Desc;
  EFI_PHYSICAL_ADDRESS PhysAddr;
  UINT32               LenCtrl;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ?
           EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize > AL_ETH_MAX_PKT_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Fill Ethernet header if requested
  //
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

  PhysAddr = (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;

  //
  // Build TX descriptor (kernel al_mod_eth_tx_pkt_prepare pattern)
  //   len_ctrl: length | ring_id | FIRST | LAST | CONCAT
  //   meta_ctrl: 0
  //   buf_ptr: physical address
  //
  LenCtrl = (UINT32)(BufferSize & AL_M2S_DESC_LEN_MASK) |
            AL_M2S_DESC_FIRST | AL_M2S_DESC_LAST | AL_M2S_DESC_CONCAT |
            AL_M2S_DESC_RING_ID(Ctx->TxDescRingId);

  Desc = (AL_ETH_DESC *)Ctx->TxDescRing + Ctx->TxProdIdx;
  {
    UINTN  DescAddr = (UINTN)Desc;

    MmioWrite32 (DescAddr + 0, LenCtrl);
    MmioWrite32 (DescAddr + 4, 0);  // MetaCtrl
    MmioWrite32 (DescAddr + 8, (UINT32)(PhysAddr & 0xFFFFFFFF));
    MmioWrite32 (DescAddr + 12, (UINT32)(PhysAddr >> 32));
  }

  //
  // Flush TX packet data from CPU cache to RAM (caller's buffer is cacheable).
  // Use WriteBackInvalidate — plain WriteBack may not reach RAM on this platform.
  //
  WriteBackInvalidateDataCacheRange (Buffer, BufferSize);

  //
  // Data memory barrier before tail pointer write (kernel pattern)
  //
  MemoryFence ();

  //
  // Advance producer index and update ring_id on wrap
  //
  Ctx->TxProdIdx = (Ctx->TxProdIdx + 1) % AL_ETH_NUM_TX_DESC;
  if (Ctx->TxProdIdx == 0) {
    Ctx->TxDescRingId = (Ctx->TxDescRingId + 1) & AL_UDMA_RING_ID_MASK;
  }

  //
  // Kick TX DMA: write TDRTP_INC = 1
  //
  UdmaWrite32 (Ctx, M2S_Q0_TDRTP_INC, 1);

  //
  // Track for GetStatus recycling
  //
  Ctx->TxBufInFlight = Buffer;

  DEBUG ((DEBUG_VERBOSE, "AlEthDxe: TX[%d] len=%u phys=0x%lx ctrl=0x%08x\n",
          (Ctx->TxProdIdx == 0 ? AL_ETH_NUM_TX_DESC - 1 : Ctx->TxProdIdx - 1),
          BufferSize, PhysAddr, LenCtrl));

  return EFI_SUCCESS;
}

// ============================================================================
// Receive — software ring_id-based completion tracking (kernel pattern)
// ============================================================================

STATIC
EFI_STATUS
EFIAPI
AlEthSnpReceive (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  OUT    UINTN                        *HeaderSize   OPTIONAL,
  IN OUT UINTN                        *BufferSize,
  OUT    VOID                         *Buffer,
  OUT    EFI_MAC_ADDRESS              *SrcAddr      OPTIONAL,
  OUT    EFI_MAC_ADDRESS              *DestAddr     OPTIONAL,
  OUT    UINT16                       *Protocol     OPTIONAL
  )
{
  AL_ETH_CONTEXT  *Ctx;
  AL_ETH_CDESC    *CDesc;
  UINT32          CdescWord0;
  UINT32          PktLen;
  UINT8           *PktData;
  AL_ETH_DESC     *Desc;

  Ctx = AL_ETH_FROM_SNP (This);

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return (Ctx->SnpMode.State == EfiSimpleNetworkStopped) ?
           EFI_NOT_STARTED : EFI_DEVICE_ERROR;
  }

  if (Buffer == NULL || BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Check for RX completions using hardware RCRHP register.
  // RCRHP[4:0] is the DMA's completion write pointer — if it differs
  // from our consumer index, there are completions to process.
  //
  {
    UINT32  DmaCompHead = UdmaRead32 (Ctx, S2M_Q0_RCRHP) & (AL_ETH_NUM_RX_DESC - 1);
    if (DmaCompHead == Ctx->RxConsIdx) {
      return EFI_NOT_READY;
    }
  }

  CDesc = (AL_ETH_CDESC *)Ctx->RxCompRing + Ctx->RxConsIdx;
  CdescWord0 = MmioRead32 ((UINTN)CDesc);

  //
  // Valid completion — check for errors
  //
  if (CdescWord0 & AL_UDMA_CDESC_ERROR) {
    DEBUG ((DEBUG_WARN, "AlEthDxe: RX error at idx %u cdesc=0x%08x\n",
            Ctx->RxConsIdx, CdescWord0));
    goto DropAndReSubmit;
  }

  //
  // Extract packet length from completion word 1 (kernel rx completion format)
  //
  PktLen = MmioRead32 ((UINTN)CDesc + 4) & 0xFFFF;
  if (PktLen < 14 || PktLen > AL_ETH_RX_BUF_SIZE) {
    DEBUG ((DEBUG_WARN, "AlEthDxe: RX bad length %u at idx %u\n",
            PktLen, Ctx->RxConsIdx));
    goto DropAndReSubmit;
  }

  if (*BufferSize < PktLen) {
    *BufferSize = PktLen;
    return EFI_BUFFER_TOO_SMALL;
  }

  //
  // Copy packet data
  //
  {
    UINT32 BufIdx = (Ctx->RxConsIdx + AL_ETH_NUM_RX_DESC - Ctx->RxCompDescOffset) % AL_ETH_NUM_RX_DESC;
    PktData = (UINT8 *)Ctx->RxBuffers[BufIdx];
  }
  CopyMem (Buffer, PktData, PktLen);
  *BufferSize = PktLen;

  {
    UINT32 Ci = Ctx->RxConsIdx;
    UINT8  *B0 = (UINT8 *)Ctx->RxBuffers[Ci];
    UINT8  *Bm1 = (UINT8 *)Ctx->RxBuffers[(Ci + AL_ETH_NUM_RX_DESC - 1) % AL_ETH_NUM_RX_DESC];
    UINT8  *Bp1 = (UINT8 *)Ctx->RxBuffers[(Ci + 1) % AL_ETH_NUM_RX_DESC];
    DEBUG ((DEBUG_INFO, "AlEthDxe: RX ci=%d len=%u used=buf[%d] proto=%02x%02x | buf[ci]=%02x%02x buf[ci-1]=%02x%02x buf[ci+1]=%02x%02x\n",
            Ci, PktLen,
            (Ci + AL_ETH_NUM_RX_DESC - Ctx->RxCompDescOffset) % AL_ETH_NUM_RX_DESC,
            PktData[12], PktData[13],
            B0[12], B0[13],
            Bm1[12], Bm1[13],
            Bp1[12], Bp1[13]));
  }

  if (HeaderSize != NULL) {
    *HeaderSize = Ctx->SnpMode.MediaHeaderSize;
  }

  if (DestAddr != NULL) {
    ZeroMem (DestAddr, sizeof (*DestAddr));
    CopyMem (DestAddr, PktData, 6);
  }

  if (SrcAddr != NULL) {
    ZeroMem (SrcAddr, sizeof (*SrcAddr));
    CopyMem (SrcAddr, PktData + 6, 6);
  }

  if (Protocol != NULL) {
    *Protocol = (UINT16)((PktData[12] << 8) | PktData[13]);
  }

  //
  // Re-submit RX buffer: desc[ProdIdx] → buffer[ProdIdx] maintains invariant
  //
  Desc = (AL_ETH_DESC *)Ctx->RxDescRing + Ctx->RxProdIdx;
  {
    UINTN  Da = (UINTN)Desc;
    MmioWrite32 (Da + 0, (AL_ETH_RX_BUF_SIZE & AL_S2M_DESC_LEN_MASK) |
                          AL_S2M_DESC_RING_ID(Ctx->RxDescRingId));
    MmioWrite32 (Da + 4, 0);
    MmioWrite32 (Da + 8, (UINT32)(Ctx->RxBuffersPhys[Ctx->RxProdIdx] & 0xFFFFFFFF));
    MmioWrite32 (Da + 12, (UINT32)(Ctx->RxBuffersPhys[Ctx->RxProdIdx] >> 32));
  }

  MemoryFence ();
  UdmaWrite32 (Ctx, S2M_Q0_RDRTP_INC, 1);

  Ctx->RxConsIdx = (Ctx->RxConsIdx + 1) % AL_ETH_NUM_RX_DESC;

  Ctx->RxProdIdx = (Ctx->RxProdIdx + 1) % AL_ETH_NUM_RX_DESC;
  if (Ctx->RxProdIdx == 0) {
    Ctx->RxDescRingId = (Ctx->RxDescRingId + 1) & AL_UDMA_RING_ID_MASK;
  }

  DEBUG ((DEBUG_VERBOSE, "AlEthDxe: RX ok ci=%d pi=%d RDRHP=0x%x RCRHP=0x%x\n",
          Ctx->RxConsIdx, Ctx->RxProdIdx,
          UdmaRead32 (Ctx, S2M_Q0_RDRHP),
          UdmaRead32 (Ctx, S2M_Q0_RCRHP)));

  return EFI_SUCCESS;

DropAndReSubmit:
  Desc = (AL_ETH_DESC *)Ctx->RxDescRing + Ctx->RxProdIdx;
  {
    UINTN  Da = (UINTN)Desc;
    MmioWrite32 (Da + 0, (AL_ETH_RX_BUF_SIZE & AL_S2M_DESC_LEN_MASK) |
                          AL_S2M_DESC_RING_ID(Ctx->RxDescRingId));
    MmioWrite32 (Da + 4, 0);
    MmioWrite32 (Da + 8, (UINT32)(Ctx->RxBuffersPhys[Ctx->RxProdIdx] & 0xFFFFFFFF));
    MmioWrite32 (Da + 12, (UINT32)(Ctx->RxBuffersPhys[Ctx->RxProdIdx] >> 32));
  }

  MemoryFence ();
  UdmaWrite32 (Ctx, S2M_Q0_RDRTP_INC, 1);

  Ctx->RxConsIdx = (Ctx->RxConsIdx + 1) % AL_ETH_NUM_RX_DESC;

  Ctx->RxProdIdx = (Ctx->RxProdIdx + 1) % AL_ETH_NUM_RX_DESC;
  if (Ctx->RxProdIdx == 0) {
    Ctx->RxDescRingId = (Ctx->RxDescRingId + 1) & AL_UDMA_RING_ID_MASK;
  }

  return EFI_NOT_READY;
}

// ============================================================================
// WaitForPacket event — ring_id matching
// ============================================================================

STATIC
VOID
EFIAPI
AlEthWaitForPacketNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  AL_ETH_CONTEXT  *Ctx;

  Ctx = (AL_ETH_CONTEXT *)Context;

  if (Ctx->SnpMode.State != EfiSimpleNetworkInitialized) {
    return;
  }

  {
    UINT32  DmaCompHead = UdmaRead32 (Ctx, S2M_Q0_RCRHP) & (AL_ETH_NUM_RX_DESC - 1);
    if (DmaCompHead != Ctx->RxConsIdx) {
      gBS->SignalEvent (Event);
    }
  }
}

// ============================================================================
// Driver Binding Protocol
// ============================================================================

EFI_STATUS
EFIAPI
AlEthSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT16               VendorId;
  UINT16               DeviceId;

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

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_VENDOR_ID_OFFSET,
                        1,
                        &VendorId
                        );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_DEVICE_ID_OFFSET,
                        1,
                        &DeviceId
                        );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (VendorId != AL_ETH_PCI_VENDOR_ID || DeviceId != AL_ETH_PCI_DEVICE_ID) {
    Status = EFI_UNSUPPORTED;
  }

Done:
  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

EFI_STATUS
EFIAPI
AlEthDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                Status;
  EFI_PCI_IO_PROTOCOL       *PciIo;
  AL_ETH_CONTEXT            *Ctx;
  UINT64                    Supports;
  EFI_MAC_ADDRESS           MacAddr;
  EFI_DEVICE_PATH_PROTOCOL  *ParentDevicePath;
  BOOLEAN                   IsAllZero;
  UINTN                     Idx;

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

  Ctx = AllocateZeroPool (sizeof (AL_ETH_CONTEXT));
  if (Ctx == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseProtocol;
  }

  Ctx->Signature = AL_ETH_CONTEXT_SIGNATURE;
  Ctx->ControllerHandle = Controller;
  Ctx->PciIo = PciIo;
  EfiInitializeLock (&Ctx->Lock, TPL_NOTIFY);

  //
  // Save and enable PCI attributes
  //
  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationGet,
                    0,
                    &Ctx->OriginalPciAttributes
                    );
  if (EFI_ERROR (Status)) {
    goto FreeCtx;
  }

  Supports = EFI_PCI_IO_ATTRIBUTE_MEMORY |
             EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;

  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationEnable,
                    Supports,
                    NULL
                    );
  if (EFI_ERROR (Status)) {
    goto FreeCtx;
  }

  //
  // Read actual BAR addresses from PCI config space
  //
  {
    UINT32  BarLo;

    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BASE_ADDRESSREG_OFFSET + 0 * 4, 1, &BarLo);
    Ctx->UdmaBase = (UINTN)(BarLo & 0xFFFFFFF0);

    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BASE_ADDRESSREG_OFFSET + 2 * 4, 1, &BarLo);
    Ctx->MacBase = (UINTN)(BarLo & 0xFFFFFFF0);

    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, PCI_BASE_ADDRESSREG_OFFSET + 4 * 4, 1, &BarLo);
    Ctx->EcBase = (BarLo != 0) ? (UINTN)(BarLo & 0xFFFFFFF0) : 0;

    DEBUG ((DEBUG_INFO, "AlEthDxe: UDMA=0x%lx MAC=0x%lx EC=0x%lx\n",
            (UINT64)Ctx->UdmaBase, (UINT64)Ctx->MacBase, (UINT64)Ctx->EcBase));
  }

  if (Ctx->MacBase == 0 || Ctx->UdmaBase == 0) {
    DEBUG ((DEBUG_ERROR, "AlEthDxe: BAR addresses invalid\n"));
    Status = EFI_DEVICE_ERROR;
    goto FreeCtx;
  }

  //
  // Read MAC address from hardware
  //
  AlEthGetMacAddr (Ctx, &MacAddr);

  IsAllZero = TRUE;
  for (Idx = 0; Idx < 6; Idx++) {
    if (MacAddr.Addr[Idx] != 0) {
      IsAllZero = FALSE;
      break;
    }
  }

  if (IsAllZero) {
    UINT64  Tsc;

    Tsc = GetPerformanceCounter ();
    MacAddr.Addr[0] = 0x02;
    MacAddr.Addr[1] = (UINT8)(Tsc >> 8);
    MacAddr.Addr[2] = (UINT8)(Tsc >> 16);
    MacAddr.Addr[3] = (UINT8)(Tsc >> 24);
    MacAddr.Addr[4] = (UINT8)(Tsc >> 32);
    MacAddr.Addr[5] = (UINT8)(Tsc >> 40);
    DEBUG ((DEBUG_INFO, "AlEthDxe: Generated random MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            MacAddr.Addr[0], MacAddr.Addr[1], MacAddr.Addr[2],
            MacAddr.Addr[3], MacAddr.Addr[4], MacAddr.Addr[5]));
  }

  //
  // Initialize SNP Mode
  //
  Ctx->SnpMode.State = EfiSimpleNetworkStopped;
  Ctx->SnpMode.HwAddressSize = NET_ETHER_ADDR_LEN;
  Ctx->SnpMode.MediaHeaderSize = 14;
  Ctx->SnpMode.MaxPacketSize = 1500;
  Ctx->SnpMode.NvRamSize = 0;
  Ctx->SnpMode.NvRamAccessSize = 0;
  Ctx->SnpMode.ReceiveFilterMask =
    EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
    EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |
    EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS;
  Ctx->SnpMode.ReceiveFilterSetting =
    EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
    EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
  Ctx->SnpMode.MaxMCastFilterCount = 0;
  Ctx->SnpMode.MCastFilterCount = 0;
  Ctx->SnpMode.IfType = NET_IFTYPE_ETHERNET;
  Ctx->SnpMode.MacAddressChangeable = TRUE;
  Ctx->SnpMode.MultipleTxSupported = FALSE;
  Ctx->SnpMode.MediaPresentSupported = TRUE;
  Ctx->SnpMode.MediaPresent = FALSE;

  CopyMem (&Ctx->SnpMode.CurrentAddress, &MacAddr, sizeof (EFI_MAC_ADDRESS));
  CopyMem (&Ctx->SnpMode.PermanentAddress, &MacAddr, sizeof (EFI_MAC_ADDRESS));
  SetMem (&Ctx->SnpMode.BroadcastAddress, sizeof (EFI_MAC_ADDRESS), 0xFF);

  //
  // Wire SNP protocol
  //
  Ctx->Snp.Revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
  Ctx->Snp.Start = AlEthSnpStart;
  Ctx->Snp.Stop = AlEthSnpStop;
  Ctx->Snp.Initialize = AlEthSnpInitialize;
  Ctx->Snp.Reset = AlEthSnpReset;
  Ctx->Snp.Shutdown = AlEthSnpShutdown;
  Ctx->Snp.ReceiveFilters = AlEthSnpReceiveFilters;
  Ctx->Snp.StationAddress = AlEthSnpStationAddress;
  Ctx->Snp.Statistics = AlEthSnpStatistics;
  Ctx->Snp.MCastIpToMac = AlEthSnpMCastIpToMac;
  Ctx->Snp.NvData = AlEthSnpNvData;
  Ctx->Snp.GetStatus = AlEthSnpGetStatus;
  Ctx->Snp.Transmit = AlEthSnpTransmit;
  Ctx->Snp.Receive = AlEthSnpReceive;
  Ctx->Snp.WaitForPacket = NULL;
  Ctx->Snp.Mode = &Ctx->SnpMode;

  //
  // Create WaitForPacket event
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  AlEthWaitForPacketNotify,
                  Ctx,
                  &Ctx->Snp.WaitForPacket
                  );
  if (EFI_ERROR (Status)) {
    goto RestoreAttrs;
  }

  //
  // Create ExitBootServices event
  //
  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_CALLBACK,
                  AlEthExitBootServicesCallback,
                  Ctx,
                  &Ctx->ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    goto CloseWaitEvent;
  }

  //
  // Build MAC device path
  //
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

  Ctx->DevicePath = AllocateZeroPool (sizeof (AL_ETH_DEVICE_PATH));
  if (Ctx->DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseExitEvent;
  }

  Ctx->DevicePath->MacAddrNode.Header.Type = MESSAGING_DEVICE_PATH;
  Ctx->DevicePath->MacAddrNode.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength (&Ctx->DevicePath->MacAddrNode.Header, sizeof (MAC_ADDR_DEVICE_PATH));
  CopyMem (&Ctx->DevicePath->MacAddrNode.MacAddress, &MacAddr, sizeof (EFI_MAC_ADDRESS));
  Ctx->DevicePath->MacAddrNode.IfType = NET_IFTYPE_ETHERNET;
  SetDevicePathEndNode (&Ctx->DevicePath->End);

  {
    EFI_DEVICE_PATH_PROTOCOL  *FullPath;

    FullPath = AppendDevicePathNode (
                 ParentDevicePath,
                 (EFI_DEVICE_PATH_PROTOCOL *)&Ctx->DevicePath->MacAddrNode
                 );
    if (FullPath == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto FreeDevPath;
    }

    FreePool (Ctx->DevicePath);
    Ctx->DevicePath = (AL_ETH_DEVICE_PATH *)FullPath;
  }

  //
  // Install SNP + DevicePath on a new child handle
  //
  Ctx->ChildHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Ctx->ChildHandle,
                  &gEfiSimpleNetworkProtocolGuid,
                  &Ctx->Snp,
                  &gEfiDevicePathProtocolGuid,
                  Ctx->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto FreeDevPath;
  }

  //
  // Open PciIo BY_CHILD_CONTROLLER
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Ctx->ChildHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (
           Ctx->ChildHandle,
           &gEfiSimpleNetworkProtocolGuid,
           &Ctx->Snp,
           &gEfiDevicePathProtocolGuid,
           Ctx->DevicePath,
           NULL
           );
    goto FreeDevPath;
  }

  DEBUG ((DEBUG_INFO, "AlEthDxe: Installed SNP on PCI 1c36:0001, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
          MacAddr.Addr[0], MacAddr.Addr[1], MacAddr.Addr[2],
          MacAddr.Addr[3], MacAddr.Addr[4], MacAddr.Addr[5]));

  return EFI_SUCCESS;

FreeDevPath:
  if (Ctx->DevicePath != NULL) {
    FreePool (Ctx->DevicePath);
  }

CloseExitEvent:
  gBS->CloseEvent (Ctx->ExitBootServicesEvent);

CloseWaitEvent:
  gBS->CloseEvent (Ctx->Snp.WaitForPacket);

RestoreAttrs:
  PciIo->Attributes (
           PciIo,
           EfiPciIoAttributeOperationSet,
           Ctx->OriginalPciAttributes,
           NULL
           );

FreeCtx:
  FreePool (Ctx);

CloseProtocol:
  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

EFI_STATUS
EFIAPI
AlEthDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                     Status;
  EFI_SIMPLE_NETWORK_PROTOCOL    *Snp;
  AL_ETH_CONTEXT                 *Ctx;

  if (NumberOfChildren == 0) {
    gBS->CloseProtocol (
           Controller,
           &gEfiPciIoProtocolGuid,
           This->DriverBindingHandle,
           Controller
           );
    return EFI_SUCCESS;
  }

  ASSERT (NumberOfChildren == 1);

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

  Ctx = AL_ETH_FROM_SNP (Snp);

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Ctx->ChildHandle,
                  &gEfiSimpleNetworkProtocolGuid,
                  &Ctx->Snp,
                  &gEfiDevicePathProtocolGuid,
                  Ctx->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Ctx->ChildHandle
         );

  if (Ctx->SnpMode.State == EfiSimpleNetworkInitialized) {
    AlEthHwShutdown (Ctx);
  }

  gBS->CloseEvent (Ctx->ExitBootServicesEvent);
  gBS->CloseEvent (Ctx->Snp.WaitForPacket);

  Ctx->PciIo->Attributes (
                Ctx->PciIo,
                EfiPciIoAttributeOperationSet,
                Ctx->OriginalPciAttributes,
                NULL
                );

  if (Ctx->DevicePath != NULL) {
    FreePool (Ctx->DevicePath);
  }

  FreePool (Ctx);

  return EFI_SUCCESS;
}

// ============================================================================
// Entry Point
// ============================================================================

EFI_STATUS
EFIAPI
AlEthDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBinding (
           ImageHandle,
           SystemTable,
           &mAlEthDriverBinding,
           ImageHandle
           );
}
