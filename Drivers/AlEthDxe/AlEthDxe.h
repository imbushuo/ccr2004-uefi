/** @file
  Annapurna Labs Ethernet DXE driver header for MikroTik CCR2004.

  Rewritten to follow the kernel HAL init sequence from
  udm-kernel/drivers/net/ethernet/al/ for correct UDMA operation.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef AL_ETH_DXE_H_
#define AL_ETH_DXE_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleNetwork.h>
#include <IndustryStandard/Pci.h>

//
// PCI identification
//
#define AL_ETH_PCI_VENDOR_ID  0x1C36
#define AL_ETH_PCI_DEVICE_ID  0x0001

//
// PCI BAR indices: UDMA=BAR0, MAC=BAR2, EC=BAR4 (BAR4 absent on 1c36:0001)
//
#define AL_ETH_PCI_BAR_UDMA  0
#define AL_ETH_PCI_BAR_MAC   2
#define AL_ETH_PCI_BAR_EC    4

// ============================================================================
// 1G MAC register offsets (from MAC BAR base)
// ============================================================================

#define MAC_1G_CMD_CFG           0x008
#define MAC_1G_MAC_0             0x00C
#define MAC_1G_MAC_1             0x010
#define MAC_1G_FRM_LEN           0x014
#define MAC_1G_RX_SECTION_EMPTY  0x01C
#define MAC_1G_RX_SECTION_FULL   0x020
#define MAC_1G_TX_SECTION_EMPTY  0x024
#define MAC_1G_TX_SECTION_FULL   0x028
#define MAC_1G_RX_ALMOST_EMPTY   0x02C
#define MAC_1G_RX_ALMOST_FULL    0x030
#define MAC_1G_TX_ALMOST_EMPTY   0x034
#define MAC_1G_TX_ALMOST_FULL    0x038

#define MAC_1G_CMD_CFG_TX_ENA         BIT0
#define MAC_1G_CMD_CFG_RX_ENA         BIT1
#define MAC_1G_CMD_CFG_PROMIS_EN      BIT4
#define MAC_1G_CMD_CFG_HD_EN          BIT10
#define MAC_1G_CMD_CFG_CNTL_FRM_ENA   BIT23
#define MAC_1G_CMD_CFG_NO_LGTH_CHECK  BIT24

#define MAC_1G_STAT_FRAMES_TXED_OK  0x068
#define MAC_1G_STAT_FRAMES_RXED_OK  0x06C
#define MAC_1G_STAT_FCS_ERRORS      0x070
#define MAC_1G_STAT_ALIGNMENT_ERR   0x074
#define MAC_1G_STAT_OCTETS_TXED_OK  0x078
#define MAC_1G_STAT_OCTETS_RXED_OK  0x07C

// ============================================================================
// 10G MAC MDIO registers (from MAC BAR base)
// ============================================================================

#define MAC_10G_MDIO_CFG_STATUS  0x430
#define MAC_10G_MDIO_CMD         0x434
#define MAC_10G_MDIO_DATA        0x438
#define MAC_10G_MDIO_REGADDR     0x43C

#define MDIO_CFG_BUSY        BIT0
#define MDIO_CFG_ERROR       BIT1
#define MDIO_CFG_CLAUSE45    BIT6
#define MDIO_CFG_HOLD_MASK   0x0000001C
#define MDIO_CFG_HOLD_SHIFT  2
#define MDIO_CFG_CLK_MASK    0x0000FF80
#define MDIO_CFG_CLK_SHIFT   7

#define MDIO_CMD_READ  BIT15

// ============================================================================
// MAC Gen registers (from MAC BAR base)
// ============================================================================

#define MAC_GEN_VERSION     0x900
#define MAC_GEN_CFG         0x90C
#define MAC_GEN_MAC_1G_CFG  0x910
#define MAC_GEN_RGMII_CFG   0x918
#define MAC_GEN_RGMII_STAT  0x91C
#define MAC_GEN_MDIO_CTRL1  0x93C
#define MAC_GEN_MUX_SEL     0x964
#define MAC_GEN_CLK_CFG     0x968
#define MAC_GEN_RGMII_SEL   0x974

#define MAC_GEN_CFG_MDIO_1_10  BIT10
#define RGMII_STAT_LINK        BIT4

// ============================================================================
// EC register offsets (from EC BAR base, BAR4 — absent on CCR2004 PCI device,
// but pre-configured by boot ROM)
// ============================================================================

#define EC_GEN_EN       0x84
#define EC_GEN_FIFO_EN  0x88
#define EC_MAC_MIN_PKT  0xC4
#define EC_MAC_MAX_PKT  0xC8

// ============================================================================
// UDMA register offsets (all from BAR0 base)
//
// Memory map (from kernel struct unit_regs_v3):
//   0x00000 - M2S (TX) regs
//   0x10000 - S2M (RX) regs
//   0x1C000 - Gen regs (AXI, interrupts, etc.)
// ============================================================================

//
// M2S (TX) unit registers
//
#define M2S_STATE            0x200
#define M2S_CHANGE_STATE     0x204

//
// M2S read/data config (struct udma_m2s_rd at +0x300)
//
#define M2S_DESC_PREF_CFG_3  0x308
#define M2S_RD_DATA_CFG      0x310

//
// M2S completion controller (struct udma_m2s_comp at +0x400)
//
#define M2S_COMP_ACK         0x408

//
// M2S Queue 0 registers (struct udma_m2s_q at +0x1000)
//
#define M2S_Q0_CFG        0x1020
#define M2S_Q0_TDRBP_LOW  0x1028
#define M2S_Q0_TDRBP_HIGH 0x102C
#define M2S_Q0_TDRL       0x1030
#define M2S_Q0_TDRHP      0x1034
#define M2S_Q0_TDRTP_INC  0x1038
#define M2S_Q0_TDRTP      0x103C
#define M2S_Q0_TCRBP_LOW  0x1044
#define M2S_Q0_TCRBP_HIGH 0x1048
#define M2S_Q0_TCRHP      0x104C
#define M2S_Q0_RLIMIT_MASK 0x1074
#define M2S_Q0_COMP_CFG   0x10A0
#define M2S_Q0_SW_CTRL    0x10B0

//
// S2M (RX) unit registers (at +0x10000)
//
#define S2M_STATE            0x10200
#define S2M_CHANGE_STATE     0x10204
#define S2M_CLEAR_CTRL       0x10244
#define S2M_FIFO_EN          0x1024C
#define S2M_STREAM_CFG       0x10250

//
// S2M read config (struct udma_s2m_rd at +0x10300)
//
#define S2M_DESC_PREF_CFG_3  0x10308

//
// S2M completion controller (struct udma_s2m_comp at +0x10380)
//
#define S2M_COMP_CFG_1C_REG  0x10380
#define S2M_COMP_ACK         0x1038C

//
// S2M Queue 0 registers (struct udma_s2m_q at +0x11000)
//
#define S2M_Q0_CFG        0x11020
#define S2M_Q0_RDRBP_LOW  0x11028
#define S2M_Q0_RDRBP_HIGH 0x1102C
#define S2M_Q0_RDRL       0x11030
#define S2M_Q0_RDRHP      0x11034
#define S2M_Q0_RDRTP_INC  0x11038
#define S2M_Q0_RDRTP      0x1103C
#define S2M_Q0_RCRBP_LOW  0x11044
#define S2M_Q0_RCRBP_HIGH 0x11048
#define S2M_Q0_RCRHP      0x1104C
#define S2M_Q0_COMP_CFG   0x11054
#define S2M_Q0_COMP_CFG_2 0x11058
#define S2M_Q0_SW_CTRL    0x110B0

//
// Gen AXI config (gen at +0x1C000, axi at +0x2280 within gen)
//
#define GEN_AXI_CFG_1     0x1E280

// ============================================================================
// UDMA state machine
// ============================================================================

//
// STATE register: 2-bit sub-unit fields at [1:0], [5:4], [9:8], [13:12].
// Values observed on Alpine V2 hardware:
//   0x2222 = all sub-units NORMAL after CHANGE_NORMAL write.
// Note: kernel HAL defines IDLE=0, NORMAL=1, ABORT=2, but Alpine V2 hardware
// uses IDLE=1, NORMAL=2 (confirmed by hardware observation).
//
#define UDMA_STATE_DISABLE 0
#define UDMA_STATE_IDLE    1
#define UDMA_STATE_NORMAL  2
#define UDMA_STATE_ABORT   3

//
// CHANGE_STATE register: one-hot bits to request state transitions.
//
#define UDMA_CHANGE_NORMAL  BIT0
#define UDMA_CHANGE_DIS     BIT1
#define UDMA_CHANGE_ABORT   BIT2

//
// S2M stream config bits (for stream flush during RX NORMAL transition)
//
#define S2M_STREAM_DISABLE       BIT0
#define S2M_STREAM_FLUSH         BIT4
#define S2M_STREAM_STOP_PREFETCH BIT8

//
// Queue CFG register bits (same positions for M2S and S2M)
//
#define UDMA_Q_CFG_EN_PREF        BIT16
#define UDMA_Q_CFG_EN_SCHEDULING  BIT17

//
// Completion CFG register bits
//
#define UDMA_COMP_CFG_EN_RING_UPDATE  BIT0
#define UDMA_COMP_CFG_DIS_COAL        BIT1

//
// SW_CTRL register
//
#define UDMA_SW_CTRL_RST_Q  BIT8

//
// Rate limiter mask: clear this bit to enable DMB pause
//
#define UDMA_RLIMIT_MASK_INTERNAL_PAUSE_DMB  BIT2

//
// S2M completion config (cfg_1c) — cdesc size field
//
#define S2M_COMP_CFG_1C_DESC_SIZE_MASK  0x0F

//
// M2S data read config — FIFO depth field
//
#define M2S_RD_DATA_CFG_FIFO_DEPTH_MASK   0x00000FFF
#define M2S_RD_DATA_CFG_FIFO_DEPTH_SHIFT  0

// ============================================================================
// Descriptor flags
// ============================================================================

#define AL_M2S_DESC_RING_ID_SHIFT  24
#define AL_M2S_DESC_RING_ID_MASK   (0x3U << AL_M2S_DESC_RING_ID_SHIFT)
#define AL_M2S_DESC_RING_ID(id)    ((UINT32)((id) & 0x3) << AL_M2S_DESC_RING_ID_SHIFT)
#define AL_M2S_DESC_FIRST          BIT26
#define AL_M2S_DESC_LAST           BIT27
#define AL_M2S_DESC_INT_EN         BIT28
#define AL_M2S_DESC_CONCAT         BIT31
#define AL_M2S_DESC_LEN_MASK       0xFFFFF

#define AL_S2M_DESC_RING_ID_SHIFT  24
#define AL_S2M_DESC_RING_ID_MASK   (0x3U << AL_S2M_DESC_RING_ID_SHIFT)
#define AL_S2M_DESC_RING_ID(id)    ((UINT32)((id) & 0x3) << AL_S2M_DESC_RING_ID_SHIFT)
#define AL_S2M_DESC_INT_EN         BIT28
#define AL_S2M_DESC_LEN_MASK       0xFFFF

#define AL_UDMA_INITIAL_RING_ID    1
#define AL_UDMA_RING_ID_MASK       0x3

//
// Completion descriptor flags (word 0)
//
#define AL_UDMA_CDESC_RING_ID_SHIFT  24
#define AL_UDMA_CDESC_RING_ID_MASK   (0x3U << AL_UDMA_CDESC_RING_ID_SHIFT)
#define AL_UDMA_CDESC_RING_ID_GET(w) (((w) >> AL_UDMA_CDESC_RING_ID_SHIFT) & 0x3)
#define AL_UDMA_CDESC_FIRST   BIT26
#define AL_UDMA_CDESC_LAST    BIT27
#define AL_UDMA_CDESC_ERROR   BIT31

// ============================================================================
// Descriptor structures (16 bytes each, matching hardware format)
// ============================================================================

#pragma pack(1)

//
// Submission descriptor: used for both TX and RX
//   TX: LenCtrl = length | ring_id | FIRST | LAST | CONCAT
//       MetaCtrl = 0
//       BufPtr = packet physical address
//   RX: LenCtrl = buf_size | ring_id
//       MetaCtrl = 0 (buf2_ptr_lo)
//       BufPtr = buffer physical address (buf1_ptr)
//
typedef struct {
  UINT32  LenCtrl;
  UINT32  MetaCtrl;
  UINT64  BufPtr;
} AL_ETH_DESC;

//
// Completion descriptor (16 bytes)
//   Word 0: ctrl_meta — ring_id[25:24] | FIRST[26] | LAST[27] | ERROR[31]
//   Word 1: buf1_length[15:0] (RX packet length)
//   Word 2: buf2 info
//   Word 3: extended flags
//
typedef struct {
  UINT32  CtrlMeta;
  UINT32  Len;
  UINT32  Buf2Info;
  UINT32  Flags;
} AL_ETH_CDESC;

#pragma pack()

// ============================================================================
// Driver constants
// ============================================================================

#define AL_ETH_NUM_TX_DESC   32
#define AL_ETH_NUM_RX_DESC   32
#define AL_ETH_RX_BUF_SIZE   2048
#define AL_ETH_MAX_PKT_SIZE  1518
#define AL_ETH_PHY_ADDR      0

#define AL_ETH_MDIO_TIMEOUT_US  10000
#define AL_ETH_MDIO_POLL_US     10

#define AL_ETH_CONTEXT_SIGNATURE  SIGNATURE_32('A','L','E','T')

// ============================================================================
// Device path and context
// ============================================================================

typedef struct {
  MAC_ADDR_DEVICE_PATH     MacAddrNode;
  EFI_DEVICE_PATH_PROTOCOL End;
} AL_ETH_DEVICE_PATH;

typedef struct {
  UINT32                          Signature;
  EFI_HANDLE                      ControllerHandle;
  EFI_HANDLE                      ChildHandle;
  EFI_LOCK                        Lock;
  EFI_PCI_IO_PROTOCOL             *PciIo;
  UINT64                          OriginalPciAttributes;

  // Direct MMIO base addresses (read from PCI config space)
  UINTN                           UdmaBase;  // BAR0
  UINTN                           MacBase;   // BAR2
  UINTN                           EcBase;    // BAR4 (0 if absent)

  EFI_SIMPLE_NETWORK_PROTOCOL     Snp;
  EFI_SIMPLE_NETWORK_MODE         SnpMode;
  EFI_EVENT                       ExitBootServicesEvent;

  // DMA descriptor rings
  VOID                            *TxDescRing;
  EFI_PHYSICAL_ADDRESS            TxDescRingPhys;
  VOID                            *TxDescRingMapping;
  VOID                            *TxCompRing;
  EFI_PHYSICAL_ADDRESS            TxCompRingPhys;
  VOID                            *TxCompRingMapping;
  UINT32                          TxProdIdx;
  UINT32                          TxConsIdx;

  VOID                            *RxDescRing;
  EFI_PHYSICAL_ADDRESS            RxDescRingPhys;
  VOID                            *RxDescRingMapping;
  VOID                            *RxCompRing;
  EFI_PHYSICAL_ADDRESS            RxCompRingPhys;
  VOID                            *RxCompRingMapping;
  UINT32                          RxProdIdx;
  UINT32                          RxConsIdx;       // completion ring consumer
  UINT32                          RxCompDescOffset; // offset: comp[N] → desc[N - offset]

  // RX packet buffers
  VOID                            *RxBuffers[AL_ETH_NUM_RX_DESC];
  EFI_PHYSICAL_ADDRESS            RxBuffersPhys[AL_ETH_NUM_RX_DESC];
  VOID                            *RxBufferMappings[AL_ETH_NUM_RX_DESC];

  // TX buffer tracking
  VOID                            *TxBufInFlight;

  //
  // Ring ID tracking (kernel pattern: al_mod_udma_q.desc_ring_id / comp_ring_id)
  // Starts at AL_UDMA_INITIAL_RING_ID (1).
  // desc_ring_id increments when desc index wraps (0→1→2→3→0).
  // comp_ring_id increments when comp index wraps.
  // HW copies ring_id from submission desc to completion desc.
  // Software detects new completions by checking cdesc ring_id == comp_ring_id.
  //
  UINT32                          TxDescRingId;
  UINT32                          TxCompRingId;
  UINT32                          RxDescRingId;
  UINT32                          RxCompRingId;

  EFI_NETWORK_STATISTICS          Stats;

  // Debug counters
  UINT32                          DbgRxPollCount;
  UINT32                          DbgWfpCount;

  // Device path
  AL_ETH_DEVICE_PATH              *DevicePath;
} AL_ETH_CONTEXT;

#define AL_ETH_FROM_SNP(a) \
  CR(a, AL_ETH_CONTEXT, Snp, AL_ETH_CONTEXT_SIGNATURE)

#endif // AL_ETH_DXE_H_
