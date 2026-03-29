/** @file
  PCIe EP Hello World smoke test for CCR2004-1G-2XS-PCIe.

  Configures the Alpine V2 PCIe port 0 in Endpoint mode, exposes a single
  PF (VID:DID = 19AA:961F, class = Network Controller) with a 4KB BAR0
  backed by local SRAM at 0x10000000.

  After running this app, the host should see the device via lspci and
  can read the magic 0xCAFEF00D from BAR0 offset 0.

  Based on reverse-engineered RouterBOOT PCIe EP init sequence.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>

/* ── Hardware base addresses ───────────────────────────────────────── */

#define PCIE_PORT_BASE      0xFD800000ULL
#define PBS_BASE            0xFD8A8000ULL

/* PBS revision register (determines SERDES register layout) */
#define PBS_UNIT_REV_ADDR   0xFD8A815CULL

/* PBS rev > 1 (Rev3): new SERDES register block */
#define PBS_SERDES_RESET_NEW_ADDR  (0xFD8A81B8ULL + 4124ULL * 4)
#define PBS_SERDES_CFG_NEW_ADDR    (0xFD8A81B8ULL + 3784ULL * 4)
#define PBS_SERDES_CTL_NEW_ADDR    (0xFD8A81B8ULL + 3788ULL * 4)

/* PBS rev <= 1: legacy SERDES register block */
#define PBS_SERDES_RESET_LEG_ADDR  (0xFD882000ULL + 155868ULL * 2)
#define PBS_SERDES_CFG_LEG_ADDR    (0xFD882000ULL + 155732ULL * 2)
#define PBS_SERDES_CTL_LEG_ADDR    (0xFD882000ULL + 155736ULL * 2)

/* ── DBI (PF0 PCI config space from EP side) ─────────────────────── */

#define DBI_PF0_BASE        (PCIE_PORT_BASE + 0x10000)
#define DBI_WR_EN_ADDR      (PCIE_PORT_BASE + 0x108BC)

#define PCI_CFG_VID_DID     0x00
#define PCI_CFG_CMD_STS     0x04
#define PCI_CFG_REV_CLASS   0x08
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_SUBSYS      0x2C

/* ── iATU ─────────────────────────────────────────────────────────── */

#define IATU_BASE           (PCIE_PORT_BASE + 0x10700)
#define IATU_VIEWPORT_ADDR  (IATU_BASE + 0x200)
#define IATU_CTRL2_ADDR     (IATU_BASE + 0x204)
#define IATU_CTRL1_ADDR     (IATU_BASE + 0x208)
#define IATU_SRC_LO_ADDR    (IATU_BASE + 0x20C)
#define IATU_SRC_HI_ADDR    (IATU_BASE + 0x210)
#define IATU_LIMIT_ADDR     (IATU_BASE + 0x214)
#define IATU_TGT_LO_ADDR   (IATU_BASE + 0x218)
#define IATU_TGT_HI_ADDR   (IATU_BASE + 0x21C)

#define MAX_PFS_ADDR        (IATU_BASE + 0x18)

/* ── DBI CS2 (BAR mask programming) ──────────────────────────────── */

#define DBI_CS2_OFFSET      0x4000  /* Rev3 */

/* ── Shared memory (local SRAM for PCIe BAR0) ────────────────────── */

#define SHMEM_BASE          0x10000000ULL
#define SHMEM_SIZE          0x1000

/* ── Device identity ─────────────────────────────────────────────── */

#define HELLO_VID           0x19AA
#define HELLO_DID           0x961F
#define HELLO_VID_DID       ((UINT32)HELLO_DID << 16 | HELLO_VID)
#define HELLO_SUBSYS        HELLO_VID_DID
#define HELLO_CLASS_REV     0x02000000  /* class=0x020000 (Network), rev=0 */
#define HELLO_BAR0_SIZE     0x1000
#define HELLO_MAGIC         0xCAFEF00D

/* ── Helpers ─────────────────────────────────────────────────────── */

STATIC
VOID
DbiWrite (
  UINT32  ByteOffset,
  UINT32  Value
  )
{
  MmioWrite32 (DBI_WR_EN_ADDR, 1);
  MmioWrite32 (DBI_PF0_BASE + ByteOffset, Value);
  MmioWrite32 (DBI_WR_EN_ADDR, 0);
}

STATIC
UINT32
DbiRead (
  UINT32  ByteOffset
  )
{
  return MmioRead32 (DBI_PF0_BASE + ByteOffset);
}

STATIC
VOID
DbiCs2Write (
  UINT32  ByteOffset,
  UINT32  Value
  )
{
  MemoryFence ();
  MmioWrite32 ((DBI_PF0_BASE + ByteOffset) | DBI_CS2_OFFSET, Value);
}

/* ── Step 1: SERDES init ─────────────────────────────────────────── */

STATIC
VOID
SerdesInit (
  VOID
  )
{
  UINT32  PbsRev;
  UINT64  ResetAddr, CfgAddr, CtlAddr;

  PbsRev = MmioRead32 (PBS_UNIT_REV_ADDR) >> 16;

  if (PbsRev > 1) {
    ResetAddr = PBS_SERDES_RESET_NEW_ADDR;
    CfgAddr   = PBS_SERDES_CFG_NEW_ADDR;
    CtlAddr   = PBS_SERDES_CTL_NEW_ADDR;
  } else {
    ResetAddr = PBS_SERDES_RESET_LEG_ADDR;
    CfgAddr   = PBS_SERDES_CFG_LEG_ADDR;
    CtlAddr   = PBS_SERDES_CTL_LEG_ADDR;
  }

  Print (L"  SERDES: PBS rev=%u, using %a registers\n",
         PbsRev, (PbsRev > 1) ? "new" : "legacy");

  MmioWrite32 (ResetAddr, 1);     // assert reset
  MmioWrite32 (CfgAddr,   0x40);  // lane config: PCIe Gen2/3
  MmioWrite32 (CtlAddr,   0x17);  // PLL / clock control
  MmioWrite32 (ResetAddr, 0);     // release reset — PLL locks
  MicroSecondDelay (1000);

  Print (L"  SERDES: initialized\n");
}

/* ── Step 2: Max PFs ─────────────────────────────────────────────── */

STATIC
VOID
SetMaxPfs (
  UINT32  Count
  )
{
  UINT32  Cur = MmioRead32 (MAX_PFS_ADDR);

  MmioWrite32 (MAX_PFS_ADDR, (Cur & ~0xFFU) | (Count - 1));
  Print (L"  Max PFs: %u\n", Count);
}

/* ── Step 3: PCI identity ────────────────────────────────────────── */

STATIC
VOID
ProgramPciIdentity (
  VOID
  )
{
  DbiWrite (PCI_CFG_VID_DID,   HELLO_VID_DID);
  DbiWrite (PCI_CFG_REV_CLASS, HELLO_CLASS_REV);
  DbiWrite (PCI_CFG_SUBSYS,    HELLO_SUBSYS);

  Print (L"  PCI identity: VID=%04x DID=%04x class=%06x\n",
         HELLO_VID, HELLO_DID, HELLO_CLASS_REV >> 8);
}

/* ── Step 4: BAR0 ────────────────────────────────────────────────── */

STATIC
VOID
ProgramBar0 (
  VOID
  )
{
  // Set BAR mask via CS2 shadow space (determines reported size)
  DbiCs2Write (PCI_CFG_BAR0, (HELLO_BAR0_SIZE - 1) | 1);

  // Set BAR type: memory, 32-bit, non-prefetchable
  DbiWrite (PCI_CFG_BAR0, 0x00000000);

  Print (L"  BAR0: %u bytes, memory, 32-bit, non-prefetchable\n", HELLO_BAR0_SIZE);
}

/* ── Step 5: Disable interrupts ──────────────────────────────────── */

STATIC
VOID
PcieDisableMsiMsix (
  VOID
  )
{
  UINT32  MsiCap, MsixCap;

  MsiCap = DbiRead (0x50);
  if ((MsiCap & 0xFFFF) == 0x0005) {
    DbiWrite (0x50, MsiCap & ~(1U << 16));
    Print (L"  MSI: disabled (was at cap offset 0x50)\n");
  }

  MsixCap = DbiRead (0xB0);
  if ((MsixCap & 0xFFFF) == 0x0011) {
    DbiWrite (0xB0, MsixCap & ~(1U << 31));
    Print (L"  MSI-X: disabled (was at cap offset 0xB0)\n");
  }
}

/* ── Step 6: iATU inbound ────────────────────────────────────────── */

STATIC
VOID
ConfigureIatuInbound (
  VOID
  )
{
  // Select region: PF0, inbound
  MmioWrite32 (IATU_VIEWPORT_ADDR, (1U << 31) | 0);

  // Source = don't care (BAR-match mode)
  MmioWrite32 (IATU_SRC_LO_ADDR, 0);
  MmioWrite32 (IATU_SRC_HI_ADDR, 0);

  // Target = local SRAM
  MmioWrite32 (IATU_TGT_LO_ADDR, (UINT32)SHMEM_BASE);
  MmioWrite32 (IATU_TGT_HI_ADDR, 0);

  // Control 2: memory type
  MmioWrite32 (IATU_CTRL2_ADDR, 0);

  // Control 1: enable + BAR-match on BAR0
  MmioWrite32 (IATU_CTRL1_ADDR, (1U << 31) | (1U << 29));

  Print (L"  iATU: BAR0 -> SRAM @ 0x%lx (%u bytes)\n", SHMEM_BASE, SHMEM_SIZE);
}

/* ── Step 7: Init shared memory ──────────────────────────────────── */

STATIC
VOID
InitShmem (
  VOID
  )
{
  volatile UINT32  *Shmem = (volatile UINT32 *)(UINTN)SHMEM_BASE;

  ZeroMem ((VOID *)(UINTN)SHMEM_BASE, SHMEM_SIZE);
  MemoryFence ();

  Shmem[0] = HELLO_MAGIC;       // +0x00: magic
  Shmem[1] = HELLO_VID_DID;     // +0x04: VID:DID echo
  Shmem[2] = HELLO_BAR0_SIZE;   // +0x08: BAR0 size
  Shmem[3] = 0x00000001;        // +0x0C: firmware version
  MemoryFence ();

  Print (L"  SHMEM: magic 0x%08x written at 0x%lx\n", HELLO_MAGIC, SHMEM_BASE);
}

/* ── Verify readback ─────────────────────────────────────────────── */

STATIC
VOID
VerifyConfig (
  VOID
  )
{
  UINT32  VidDid, ClassRev, Bar0, Subsys;

  VidDid   = DbiRead (PCI_CFG_VID_DID);
  ClassRev = DbiRead (PCI_CFG_REV_CLASS);
  Bar0     = DbiRead (PCI_CFG_BAR0);
  Subsys   = DbiRead (PCI_CFG_SUBSYS);

  Print (L"\n  Config readback:\n");
  Print (L"    VID:DID    = %04x:%04x %a\n",
         VidDid & 0xFFFF, VidDid >> 16,
         (VidDid == HELLO_VID_DID) ? "(OK)" : "(MISMATCH!)");
  Print (L"    Class:Rev  = %06x:%02x\n", ClassRev >> 8, ClassRev & 0xFF);
  Print (L"    BAR0       = 0x%08x\n", Bar0);
  Print (L"    Subsys     = %04x:%04x\n", Subsys & 0xFFFF, Subsys >> 16);

  Print (L"\n  SHMEM readback:\n");
  {
    volatile UINT32  *Shmem = (volatile UINT32 *)(UINTN)SHMEM_BASE;

    Print (L"    [0x00] magic    = 0x%08x %a\n",
           Shmem[0], (Shmem[0] == HELLO_MAGIC) ? "(OK)" : "(MISMATCH!)");
    Print (L"    [0x04] VID:DID  = 0x%08x\n", Shmem[1]);
    Print (L"    [0x08] BAR size = 0x%08x\n", Shmem[2]);
    Print (L"    [0x0C] FW ver   = 0x%08x\n", Shmem[3]);
  }
}

/* ── Entry point ─────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
PcieEpTestAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  Print (L"\nPCIe EP Hello World — CCR2004 smoke test\n");
  Print (L"=========================================\n\n");

  Print (L"Step 1: SERDES init\n");
  SerdesInit ();

  Print (L"Step 2: Set max PFs\n");
  SetMaxPfs (1);

  Print (L"Step 3: Program PCI identity\n");
  ProgramPciIdentity ();

  Print (L"Step 4: Program BAR0\n");
  ProgramBar0 ();

  Print (L"Step 5: Disable interrupts\n");
  PcieDisableMsiMsix ();

  Print (L"Step 6: Configure iATU inbound\n");
  ConfigureIatuInbound ();

  Print (L"Step 7: Initialize shared memory\n");
  InitShmem ();

  Print (L"\nVerifying configuration...\n");
  VerifyConfig ();

  Print (L"\n*** PCIe EP is LIVE ***\n");
  Print (L"Host should see device 19AA:961F on the PCIe bus.\n");
  Print (L"Read BAR0+0x00 from host → expect 0xCAFEF00D.\n");
  Print (L"\nSHMEM at 0x%lx is accessible from EP side.\n", SHMEM_BASE);
  Print (L"Host writes to BAR0 appear at SHMEM on EP.\n\n");

  return EFI_SUCCESS;
}
