#include "spiloader.h"

/*
 * Minimal GICv3 + ARM generic timer watchdog for SpiLoader.
 *
 * Arms the EL2 hypervisor physical timer (CNTHP, INTID 26) that
 * fires an IRQ after N seconds.  The exception vector table catches
 * the IRQ and calls SpiloaderExceptionHandler, which dumps SPI
 * controller state and full register context.
 *
 * We configure just enough of the GIC to route that single PPI.
 */

/* ---- GICv3 Distributor (GICD) ---- */

#define GICD_CTLR              (CCR2004_GICD_BASE + 0x0000U)
#define GICD_CTLR_EN_GRP0     (1U << 0)
#define GICD_CTLR_EN_GRP1_NS  (1U << 1)

/* ---- GICv3 Redistributor (GICR) ---- */

/* RD_base frame */
#define GICR_WAKER             (CCR2004_GICR_BASE + 0x0014U)
#define GICR_WAKER_PS          (1U << 1)   /* ProcessorSleep */
#define GICR_WAKER_CA          (1U << 2)   /* ChildrenAsleep */

/* SGI_base frame (64 KB offset from RD_base) */
#define GICR_SGI_BASE          (CCR2004_GICR_BASE + 0x10000U)
#define GICR_IGROUPR0          (GICR_SGI_BASE + 0x0080U)
#define GICR_ISENABLER0        (GICR_SGI_BASE + 0x0100U)
#define GICR_ICENABLER0        (GICR_SGI_BASE + 0x0180U)
#define GICR_IPRIORITYR(n)     (GICR_SGI_BASE + 0x0400U + (n))
#define GICR_ICPENDR0          (GICR_SGI_BASE + 0x0280U)
#define GICR_IGRPMODR0         (GICR_SGI_BASE + 0x0D00U)

/*
 * EL2 physical timer (CNTHP) PPI = INTID 26.
 * This is the correct timer for code running at EL2.
 * (INTID 30 is the EL1 physical timer which may be trapped.)
 */
#define TIMER_PPI_INTID  26U

/* ---- MMIO helpers ---- */

static inline void
Mmio32Write (
  uint64_t Address,
  uint32_t Value
  )
{
  *(volatile uint32_t *)(uintptr_t)Address = Value;
}

static inline uint32_t
Mmio32Read (
  uint64_t Address
  )
{
  return *(volatile uint32_t *)(uintptr_t)Address;
}

static inline uint16_t
Mmio16Read (
  uint64_t Address
  )
{
  return *(volatile uint16_t *)(uintptr_t)Address;
}

/* ---- GICv3 system register access ---- */

static inline void
WriteIccSreEl2 (
  uint64_t Value
  )
{
  __asm__ volatile ("msr S3_4_C12_C9_5, %0" :: "r" (Value));
  __asm__ volatile ("isb");
}

static inline uint64_t
ReadIccSreEl2 (
  void
  )
{
  uint64_t Value;
  __asm__ volatile ("mrs %0, S3_4_C12_C9_5" : "=r" (Value));
  return Value;
}

static inline void
WriteIccPmr (
  uint64_t Value
  )
{
  __asm__ volatile ("msr S3_0_C4_C6_0, %0" :: "r" (Value));
}

static inline void
WriteIccIgrpen1El1 (
  uint64_t Value
  )
{
  __asm__ volatile ("msr S3_0_C12_C12_7, %0" :: "r" (Value));
  __asm__ volatile ("isb");
}

/* ---- EL2 hypervisor physical timer (CNTHP) ---- */

static inline uint64_t
ReadCntfrq (
  void
  )
{
  uint64_t Value;
  __asm__ volatile ("mrs %0, CNTFRQ_EL0" : "=r" (Value));
  return Value;
}

static inline void
WriteCnthpTval (
  uint64_t Value
  )
{
  __asm__ volatile ("msr CNTHP_TVAL_EL2, %0" :: "r" (Value));
}

static inline void
WriteCnthpCtl (
  uint64_t Value
  )
{
  __asm__ volatile ("msr CNTHP_CTL_EL2, %0" :: "r" (Value));
  __asm__ volatile ("isb");
}

/* ---- SPI register dump ---- */

void
DumpSpiControllerState (
  void
  )
{
  UartWrite ("\r\nSPI controller dump (base 0xFD882000):\r\n");

  UartWrite ("  CTRLR0=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x00U));
  UartWrite ("  CTRLR1=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x04U));
  UartWrite ("  SSIENR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x08U));
  UartWrite ("\r\n");

  UartWrite ("  SER=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x10U));
  UartWrite ("  BAUDR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x14U));
  UartWrite ("\r\n");

  UartWrite ("  TXFLR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x20U));
  UartWrite ("  RXFLR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x24U));
  UartWrite ("\r\n");

  UartWrite ("  SR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x28U));
  UartWrite ("  (BUSY|TFNF|TFE|RFNE|RFF|TXE|DCOL)\r\n");

  UartWrite ("  IMR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x2CU));
  UartWrite ("  ISR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x30U));
  UartWrite ("  RISR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0x34U));
  UartWrite ("\r\n");

  UartWrite ("  SSI_OVR=");
  UartWriteHex32 ((uint32_t)Mmio16Read (CCR2004_SPI_BASE + 0xF4U));
  UartWrite ("\r\n");
}

/* ---- GICv3 + timer init ---- */

void
WatchdogInit (
  void
  )
{
  uint32_t Waker;
  uint32_t Val;

  /*
   * Step 1: Enable ICC system register access at EL2.
   */
  WriteIccSreEl2 (ReadIccSreEl2 () | 0x1U);

  /*
   * Step 2: Enable both Group 0 and Group 1 NS in the distributor.
   */
  Mmio32Write (GICD_CTLR,
               Mmio32Read (GICD_CTLR) | GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1_NS);

  /*
   * Step 3: Wake up the redistributor.
   */
  Waker = Mmio32Read (GICR_WAKER);
  Waker &= ~GICR_WAKER_PS;
  Mmio32Write (GICR_WAKER, Waker);
  while ((Mmio32Read (GICR_WAKER) & GICR_WAKER_CA) != 0U) {
    /* spin until ChildrenAsleep clears */
  }

  /*
   * Step 4: Configure the timer PPI (INTID 26) as Group 1 NS.
   *
   * Group 1 NS = IGROUPR0[n]=1, IGRPMODR0[n]=0.
   * Group 0 → FIQ (often trapped to EL3), so we must use Group 1 NS → IRQ.
   */
  Val = Mmio32Read (GICR_IGROUPR0);
  Val |= (1U << TIMER_PPI_INTID);
  Mmio32Write (GICR_IGROUPR0, Val);

  Val = Mmio32Read (GICR_IGRPMODR0);
  Val &= ~(1U << TIMER_PPI_INTID);
  Mmio32Write (GICR_IGRPMODR0, Val);

  /*
   * Step 5: Set priority for the timer PPI and enable it.
   */
  {
    volatile uint8_t *PriorityReg =
      (volatile uint8_t *)(uintptr_t)GICR_IPRIORITYR (TIMER_PPI_INTID);
    *PriorityReg = 0xA0U;
  }
  Mmio32Write (GICR_ISENABLER0, 1U << TIMER_PPI_INTID);

  /*
   * Step 6: Set priority mask to allow all priorities, enable Group 1.
   */
  WriteIccPmr (0xFFU);
  WriteIccIgrpen1El1 (0x1U);

  UartWrite ("SpiLoader: watchdog initialized (GICv3 + CNTHP, INTID ");
  UartWriteHex32 (TIMER_PPI_INTID);
  UartWrite (")\r\n");
}

void
WatchdogArm (
  uint32_t Seconds
  )
{
  uint64_t Freq;

  Freq = ReadCntfrq ();

  UartWrite ("SpiLoader: arming ");
  UartWriteHex32 (Seconds);
  UartWrite ("s watchdog (freq=");
  UartWriteHex64 (Freq);
  UartWrite (")\r\n");

  /* Set countdown and enable EL2 hypervisor physical timer */
  WriteCnthpTval (Freq * Seconds);
  WriteCnthpCtl (0x1U);  /* ENABLE=1, IMASK=0 */

  /* Unmask IRQ and FIQ at CPU */
  __asm__ volatile ("msr daifclr, #3");
}

void
WatchdogDisarm (
  void
  )
{
  /* Mask all exceptions at CPU first */
  __asm__ volatile ("msr daifset, #0xf");

  /* Disable the EL2 hypervisor physical timer */
  WriteCnthpCtl (0x0U);

  /* Disable Group 1 interrupt delivery */
  WriteIccIgrpen1El1 (0x0U);

  /* Disable the PPI in the redistributor and clear any pending */
  Mmio32Write (GICR_ICENABLER0, 1U << TIMER_PPI_INTID);
  Mmio32Write (GICR_ICPENDR0, 1U << TIMER_PPI_INTID);
}
