#include "spiloader.h"

/* ── PCIe EP hardware addresses (same as PcieEpTestApp) ───────────── */

#define PCIE_PORT_BASE      0xFD800000ULL
#define PBS_UNIT_REV_ADDR   0xFD8A815CULL

#define PBS_SERDES_RESET_NEW  (0xFD8A81B8ULL + 4124ULL * 4)
#define PBS_SERDES_CFG_NEW    (0xFD8A81B8ULL + 3784ULL * 4)
#define PBS_SERDES_CTL_NEW    (0xFD8A81B8ULL + 3788ULL * 4)

#define PBS_SERDES_RESET_LEG  (0xFD882000ULL + 155868ULL * 2)
#define PBS_SERDES_CFG_LEG    (0xFD882000ULL + 155732ULL * 2)
#define PBS_SERDES_CTL_LEG    (0xFD882000ULL + 155736ULL * 2)

#define DBI_PF0_BASE        (PCIE_PORT_BASE + 0x10000)
#define DBI_WR_EN_ADDR      (PCIE_PORT_BASE + 0x108BC)
#define DBI_CS2_OFFSET      0x4000

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

#define PCI_CFG_VID_DID     0x00
#define PCI_CFG_CMD_STS     0x04
#define PCI_CFG_REV_CLASS   0x08
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_BAR1        0x14
#define PCI_CFG_BAR2        0x18
#define PCI_CFG_BAR3        0x1C
#define PCI_CFG_BAR4        0x20
#define PCI_CFG_BAR5        0x24
#define PCI_CFG_SUBSYS      0x2C
#define PCI_CFG_INTLINE_PIN 0x3C
#define PCI_CFG_EXP_ROM     0x30

#define SHMEM_BASE          0x10000000ULL
#define SHMEM_SIZE          0x1000U

#define HELLO_VID           0x19AAU
#define HELLO_DID           0x961FU
#define HELLO_VID_DID       (((uint32_t)HELLO_DID << 16) | HELLO_VID)
#define HELLO_SUBSYS        HELLO_VID_DID
#define HELLO_CLASS_REV     0x02000000U
#define HELLO_BAR0_SIZE     0x1000U
#define HELLO_MAGIC         0xCAFEF00DU

/* ── Raw MMIO helpers ─────────────────────────────────────────────── */

static inline void
Wr32 (
  uint64_t Addr,
  uint32_t Val
  )
{
  *(volatile uint32_t *)(uintptr_t)Addr = Val;
}

static inline uint32_t
Rd32 (
  uint64_t Addr
  )
{
  return *(volatile uint32_t *)(uintptr_t)Addr;
}

static void
DbiWrite (
  uint32_t Off,
  uint32_t Val
  )
{
  Wr32 (DBI_WR_EN_ADDR, 1);
  Wr32 (DBI_PF0_BASE + Off, Val);
  Wr32 (DBI_WR_EN_ADDR, 0);
}

static uint32_t
DbiRead (
  uint32_t Off
  )
{
  return Rd32 (DBI_PF0_BASE + Off);
}

static void
DbiCs2Write (
  uint32_t Off,
  uint32_t Val
  )
{
  __asm__ volatile ("dsb sy" ::: "memory");
  Wr32 ((DBI_PF0_BASE + Off) | DBI_CS2_OFFSET, Val);
}

/* ── Rough delay (no timer needed, ~1 ms) ─────────────────────────── */

static void
BusyDelayUs (
  uint32_t Us
  )
{
  volatile uint32_t Count = Us * 500U;
  while (Count-- > 0U)
    ;
}

/* ── PCIe EP init steps ───────────────────────────────────────────── */

static void
PcieEpSerdesInit (
  void
  )
{
  uint32_t PbsRev;
  uint64_t ResetAddr, CfgAddr, CtlAddr;

  PbsRev = Rd32 (PBS_UNIT_REV_ADDR) >> 16;

  UartWrite ("SpiLoader: PCIe EP SERDES init (PBS rev=");
  UartWriteHex32 (PbsRev);

  if (PbsRev > 1U) {
    ResetAddr = PBS_SERDES_RESET_NEW;
    CfgAddr   = PBS_SERDES_CFG_NEW;
    CtlAddr   = PBS_SERDES_CTL_NEW;
    UartWrite (" new)\r\n");
  } else {
    ResetAddr = PBS_SERDES_RESET_LEG;
    CfgAddr   = PBS_SERDES_CFG_LEG;
    CtlAddr   = PBS_SERDES_CTL_LEG;
    UartWrite (" legacy)\r\n");
  }

  Wr32 (ResetAddr, 1);      /* assert reset */
  Wr32 (CfgAddr,   0x40);   /* lane config: PCIe Gen2/3 */
  Wr32 (CtlAddr,   0x17);   /* PLL / clock control */
  Wr32 (ResetAddr, 0);      /* release reset — PLL locks */
  BusyDelayUs (1000);

  UartWrite ("SpiLoader: PCIe EP SERDES done\r\n");
}

static void
PcieEpSetMaxPfs (
  void
  )
{
  uint32_t Cur = Rd32 (MAX_PFS_ADDR);
  Wr32 (MAX_PFS_ADDR, (Cur & ~0xFFU) | 0U);  /* 1 PF: value = count-1 = 0 */
  UartWrite ("SpiLoader: PCIe EP max PFs = 1\r\n");
}

static void
PcieEpProgramIdentity (
  void
  )
{
  DbiWrite (PCI_CFG_VID_DID,   HELLO_VID_DID);
  DbiWrite (PCI_CFG_REV_CLASS, HELLO_CLASS_REV);
  DbiWrite (PCI_CFG_SUBSYS,    HELLO_SUBSYS);

  UartWrite ("SpiLoader: PCIe EP identity VID:DID=");
  UartWriteHex32 (HELLO_VID);
  UartWrite (":");
  UartWriteHex32 (HELLO_DID);
  UartWrite ("\r\n");
}

static void
PcieEpProgramBars (
  void
  )
{
  /* Enable BAR0: 4KB, memory, 32-bit, non-prefetchable */
  DbiCs2Write (PCI_CFG_BAR0, (HELLO_BAR0_SIZE - 1U) | 1U);
  DbiWrite (PCI_CFG_BAR0, 0x00000000U);

  /* Disable BAR1–5: clear enable bit in CS2 mask */
  DbiCs2Write (PCI_CFG_BAR1, 0x00000000U);
  DbiCs2Write (PCI_CFG_BAR2, 0x00000000U);
  DbiCs2Write (PCI_CFG_BAR3, 0x00000000U);
  DbiCs2Write (PCI_CFG_BAR4, 0x00000000U);
  DbiCs2Write (PCI_CFG_BAR5, 0x00000000U);

  /* Disable Expansion ROM BAR */
  DbiCs2Write (PCI_CFG_EXP_ROM, 0x00000000U);
  DbiWrite (PCI_CFG_EXP_ROM, 0x00000000U);

  UartWrite ("SpiLoader: PCIe EP BAR0 4KB mem32, BAR1-5+ROM disabled\r\n");
}

static void
PcieEpSetCommandAndInterrupt (
  void
  )
{
  uint32_t IntReg;

  /*
   * Command register (offset 0x04):
   *   bit 0  I/O Space Enable    = 0 (no I/O BARs)
   *   bit 1  Memory Space Enable = 1
   *   bit 2  Bus Master Enable   = 0 (EP doesn't initiate)
   *   bit 10 Interrupt Disable   = 1 (no legacy INTx)
   */
  DbiWrite (PCI_CFG_CMD_STS, (1U << 10) | (1U << 1));

  /*
   * Interrupt Pin register (offset 0x3D, byte 1 of dword 0x3C):
   *   0 = no legacy interrupt pin
   */
  IntReg = DbiRead (PCI_CFG_INTLINE_PIN);
  IntReg &= 0xFFFF00FFU;  /* clear Interrupt Pin (byte 1) */
  DbiWrite (PCI_CFG_INTLINE_PIN, IntReg);

  UartWrite ("SpiLoader: PCIe EP cmd=0x");
  UartWriteHex32 (DbiRead (PCI_CFG_CMD_STS) & 0xFFFFU);
  UartWrite (" intpin=0\r\n");
}

static void
PcieEpDisableMsiMsix (
  void
  )
{
  uint32_t Cap;

  Cap = DbiRead (0x50);
  if ((Cap & 0xFFFFU) == 0x0005U) {
    DbiWrite (0x50, Cap & ~(1U << 16));
  }
  Cap = DbiRead (0xB0);
  if ((Cap & 0xFFFFU) == 0x0011U) {
    DbiWrite (0xB0, Cap & ~(1U << 31));
  }
  UartWrite ("SpiLoader: PCIe EP MSI/MSI-X disabled\r\n");
}

static void
PcieEpConfigureIatu (
  void
  )
{
  /* Select region: PF0, inbound */
  Wr32 (IATU_VIEWPORT_ADDR, (1U << 31) | 0U);
  Wr32 (IATU_SRC_LO_ADDR, 0);
  Wr32 (IATU_SRC_HI_ADDR, 0);
  Wr32 (IATU_TGT_LO_ADDR, (uint32_t)SHMEM_BASE);
  Wr32 (IATU_TGT_HI_ADDR, 0);
  Wr32 (IATU_CTRL2_ADDR, 0);
  Wr32 (IATU_CTRL1_ADDR, (1U << 31) | (1U << 29));   /* enable + BAR-match BAR0 */

  UartWrite ("SpiLoader: PCIe EP iATU BAR0 -> SHMEM 0x");
  UartWriteHex64 (SHMEM_BASE);
  UartWrite ("\r\n");
}

static void
PcieEpInitShmem (
  void
  )
{
  volatile uint32_t *Shmem = (volatile uint32_t *)(uintptr_t)SHMEM_BASE;
  uint32_t i;

  for (i = 0; i < SHMEM_SIZE / 4U; i++) {
    Shmem[i] = 0;
  }
  __asm__ volatile ("dsb sy" ::: "memory");

  Shmem[0] = HELLO_MAGIC;       /* +0x00: magic */
  Shmem[1] = HELLO_VID_DID;     /* +0x04: VID:DID echo */
  Shmem[2] = HELLO_BAR0_SIZE;   /* +0x08: BAR0 size */
  Shmem[3] = 0x00000001U;       /* +0x0C: firmware version */
  __asm__ volatile ("dsb sy" ::: "memory");

  UartWrite ("SpiLoader: PCIe EP SHMEM magic=0x");
  UartWriteHex32 (Shmem[0]);
  UartWrite ("\r\n");
}

static void
PcieEpInit (
  void
  )
{
  UartWrite ("SpiLoader: PCIe EP init start\r\n");

  PcieEpSerdesInit ();
  PcieEpSetMaxPfs ();
  PcieEpProgramIdentity ();
  PcieEpProgramBars ();
  PcieEpSetCommandAndInterrupt ();
  PcieEpDisableMsiMsix ();
  PcieEpConfigureIatu ();
  PcieEpInitShmem ();

  /* Readback verification */
  UartWrite ("SpiLoader: PCIe EP readback VID:DID=");
  UartWriteHex32 (DbiRead (PCI_CFG_VID_DID));
  UartWrite (" BAR0=");
  UartWriteHex32 (DbiRead (PCI_CFG_BAR0));
  UartWrite ("\r\n");

  UartWrite ("SpiLoader: PCIe EP init done — device LIVE\r\n");
}

/* ── Banner / panic / JEDEC / special mode (unchanged) ────────────── */

static void
LogBanner (
  void
  )
{
  uint64_t CurrentEl;
  char     ElChar[2];

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  CurrentEl >>= 2;
  ElChar[0] = (char)('0' + CurrentEl);
  ElChar[1] = '\0';

  UartWrite ("SpiLoader: start (EL");
  UartWrite (ElChar);
  UartWrite (")\r\n");
}

void
SpiloaderPanic (
  const char *Message
  )
{
  UartWrite ("SpiLoader: panic: ");
  UartWrite (Message);
  UartWrite ("\r\n");

  for (;;) {
    __asm__ volatile ("wfi");
  }
}

static void
LogJedecId (
  void
  )
{
  uint8_t JedecId[3];

  if (SpiFlashReadJedecId (JedecId) != 0) {
    UartWrite ("SpiLoader: JEDEC read failed\r\n");
    return;
  }

  UartWrite ("SpiLoader: JEDEC ");
  UartWriteHex32 ((uint32_t)JedecId[0]);
  UartWrite (" ");
  UartWriteHex32 ((uint32_t)JedecId[1]);
  UartWrite (" ");
  UartWriteHex32 ((uint32_t)JedecId[2]);
  UartWrite ("\r\n");
}

#define SRAM_MAGIC_ADDR      0xFD8A419AULL
#define SPECIAL_MAGIC_VALUE  0xFDFFB001U
#define SPECIAL_SPI_OFFSET   0x007F0000U
#define SPECIAL_SPI_SIZE     0x00010000U
#define SPECIAL_LOAD_ADDR    0x00110000ULL

static uint32_t
ReadSramMagic (
  void
  )
{
  volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)SRAM_MAGIC_ADDR;
  return (uint32_t)p[0]
       | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16)
       | ((uint32_t)p[3] << 24);
}

static void
WriteSramMagic (
  uint32_t Value
  )
{
  volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)SRAM_MAGIC_ADDR;
  p[0] = (uint8_t)(Value);
  p[1] = (uint8_t)(Value >> 8);
  p[2] = (uint8_t)(Value >> 16);
  p[3] = (uint8_t)(Value >> 24);
}

static void
CheckSpecialMode (
  void
  )
{
  uint32_t MagicVal = ReadSramMagic ();

  UartWrite ("SpiLoader: SRAM magic = 0x");
  UartWriteHex32 (MagicVal);
  UartWrite ("\r\n");

  if (MagicVal != SPECIAL_MAGIC_VALUE) {
    return;
  }

  UartWrite ("SpiLoader: special mode triggered!\r\n");
  WriteSramMagic (0x00000000U);

  if (SpiFlashRead (SPECIAL_SPI_OFFSET, (void *)(uintptr_t)SPECIAL_LOAD_ADDR,
                    SPECIAL_SPI_SIZE) != 0) {
    SpiloaderPanic ("special mode: SPI read failed");
  }

  UartWrite ("SpiLoader: jumping to 0x");
  UartWriteHex64 (SPECIAL_LOAD_ADDR);
  UartWrite ("\r\n");

  SpiloaderJumpToImage (SPECIAL_LOAD_ADDR, 0);
}

/* ── Main entry ───────────────────────────────────────────────────── */

void
SpiloaderMain (
  void
  )
{
  SPILOADER_RESULT Result;

  UartInitialize ();
  LogBanner ();
  UartWrite ("SpiLoader: exception vectors installed\r\n");

  /* Unmask SError so async bus faults are caught immediately */
  __asm__ volatile ("msr daifclr, #4");
  UartWrite ("SpiLoader: SError unmasked\r\n");

  /* PCIe EP init — bring up the link before SPI flash access */
  PcieEpInit ();

  UartWrite ("SpiLoader: SPI init\r\n");
  SpiInitialize ();
  UartWrite ("SpiLoader: SPI init done\r\n");

  CheckSpecialMode ();
  UartWrite ("SpiLoader: normal boot path\r\n");

  LogJedecId ();

  /* Arm watchdog for ELF load */
  WatchdogInit ();
  WatchdogArm (5);

  UartWrite ("SpiLoader: loading ELF from SPI offset 0x");
  UartWriteHex32 (CCR2004_SPI_FLASH_OFFSET);
  UartWrite ("\r\n");

  if (LoadImageFromFlash (&Result) != 0) {
    SpiloaderPanic ("ELF load failed");
  }

  WatchdogDisarm ();

  UartWrite ("SpiLoader: transferring control to UEFI @ 0x");
  UartWriteHex64 (Result.EntryPoint);
  UartWrite ("\r\n");

  SpiloaderJumpToImage (Result.EntryPoint, Result.ReservedTop);
}
