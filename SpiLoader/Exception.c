#include "spiloader.h"

static const char *gVectorNames[] = {
  "Sync (SP_EL0)",   "IRQ (SP_EL0)",   "FIQ (SP_EL0)",   "SError (SP_EL0)",
  "Sync (SP_ELx)",   "IRQ (SP_ELx)",   "FIQ (SP_ELx)",   "SError (SP_ELx)",
  "Sync (Lower64)",  "IRQ (Lower64)",  "FIQ (Lower64)",  "SError (Lower64)",
  "Sync (Lower32)",  "IRQ (Lower32)",  "FIQ (Lower32)",  "SError (Lower32)",
};

static const char *
EsrClassName (
  uint32_t Ec
  )
{
  switch (Ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "Trapped WFI/WFE";
    case 0x0E: return "Illegal execution state";
    case 0x15: return "SVC (AArch64)";
    case 0x20: return "Instruction abort (lower EL)";
    case 0x21: return "Instruction abort (same EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data abort (lower EL)";
    case 0x25: return "Data abort (same EL)";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "Floating point exception";
    case 0x2F: return "SError interrupt";
    case 0x30: return "Breakpoint (lower EL)";
    case 0x31: return "Breakpoint (same EL)";
    case 0x32: return "Software step (lower EL)";
    case 0x33: return "Software step (same EL)";
    case 0x34: return "Watchpoint (lower EL)";
    case 0x35: return "Watchpoint (same EL)";
    case 0x38: return "BKPT (AArch32)";
    case 0x3C: return "BRK (AArch64)";
    default:   return "Reserved/Other";
  }
}

static void
PrintRegName (
  unsigned int Reg
  )
{
  char Buf[4];

  Buf[0] = 'X';
  Buf[1] = (char)('0' + (Reg / 10));
  Buf[2] = (char)('0' + (Reg % 10));
  Buf[3] = '\0';
  UartWrite (Buf);
}

void
SpiloaderExceptionHandler (
  uint64_t        Type,
  EXCEPTION_FRAME *Frame
  )
{
  uint32_t     Ec;
  unsigned int Index;
  uint64_t     Fp;
  uint64_t     Pc;
  uint64_t     StackLow;
  uint64_t     StackHigh;

  /*
   * Vector index 5 = IRQ at current EL with SP_ELx.
   * This is our watchdog timer interrupt — dump SPI state.
   */
  if (Type == 5U) {
    UartWrite ("\r\n\r\n*** WATCHDOG TIMEOUT (SPI stall detected) ***\r\n");
    UartWrite ("CPU was stuck at ELR: 0x");
    UartWriteHex64 (Frame->Elr);
    UartWrite ("\r\n");
    DumpSpiControllerState ();
    UartWrite ("\r\n");
  }

  UartWrite ("\r\n*** EXCEPTION: ");
  if (Type < ARRAY_SIZE (gVectorNames)) {
    UartWrite (gVectorNames[Type]);
  } else {
    UartWrite ("Unknown");
  }
  UartWrite (" ***\r\n");

  /* Decode ESR exception class */
  Ec = (uint32_t)(Frame->Esr >> 26) & 0x3FU;
  UartWrite ("ESR:  0x");
  UartWriteHex64 (Frame->Esr);
  UartWrite ("  (EC=0x");
  UartWriteHex32 (Ec);
  UartWrite (" ");
  UartWrite (EsrClassName (Ec));
  UartWrite (")\r\n");

  UartWrite ("ELR:  0x");
  UartWriteHex64 (Frame->Elr);
  UartWrite ("\r\n");

  UartWrite ("FAR:  0x");
  UartWriteHex64 (Frame->Far);
  UartWrite ("\r\n");

  UartWrite ("SPSR: 0x");
  UartWriteHex64 (Frame->Spsr);
  UartWrite ("\r\n\r\n");

  /* Register dump — 4 per line */
  for (Index = 0; Index < 31U; Index++) {
    PrintRegName (Index);
    UartWrite ("=");
    UartWriteHex64 (Frame->X[Index]);
    if ((Index & 3U) == 3U) {
      UartWrite ("\r\n");
    } else {
      UartWrite ("  ");
    }
  }

  UartWrite ("\r\nSP =");
  UartWriteHex64 (Frame->Sp);
  UartWrite ("\r\n");

  /* Backtrace via frame-pointer chain (x29) */
  UartWrite ("\r\nBacktrace:\r\n");
  Fp = Frame->X[29];
  Pc = Frame->Elr;
  StackLow  = (uint64_t)(uintptr_t)__stack_bottom;
  StackHigh = (uint64_t)(uintptr_t)__stack_top;

  for (Index = 0; Index < 16U; Index++) {
    UartWrite ("  [");
    UartWriteHex32 (Index);
    UartWrite ("] ");
    UartWriteHex64 (Pc);
    UartWrite ("\r\n");

    if ((Fp == 0U) ||
        (Fp < StackLow) ||
        (Fp >= StackHigh) ||
        ((Fp & 0xFU) != 0U)) {
      break;
    }

    Pc = *(volatile uint64_t *)(uintptr_t)(Fp + 8U);
    Fp = *(volatile uint64_t *)(uintptr_t)Fp;
  }

  UartWrite ("\r\n*** HALTED ***\r\n");

  for (;;) {
    __asm__ volatile ("wfi");
  }
}
