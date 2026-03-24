#include "spiloader.h"

void *
SpiloaderMemCopy (
  void       *Destination,
  const void *Source,
  size_t     Length
  )
{
  uint8_t       *Dst;
  const uint8_t *Src;
  size_t        Index;

  Dst = (uint8_t *)Destination;
  Src = (const uint8_t *)Source;

  for (Index = 0; Index < Length; Index++) {
    Dst[Index] = Src[Index];
  }

  return Destination;
}

void *
SpiloaderMemSet (
  void   *Destination,
  int    Value,
  size_t Length
  )
{
  uint8_t *Dst;
  size_t  Index;

  Dst = (uint8_t *)Destination;
  for (Index = 0; Index < Length; Index++) {
    Dst[Index] = (uint8_t)Value;
  }

  return Destination;
}

int
SpiloaderMemCompare (
  const void *Left,
  const void *Right,
  size_t     Length
  )
{
  const uint8_t *A;
  const uint8_t *B;
  size_t        Index;

  A = (const uint8_t *)Left;
  B = (const uint8_t *)Right;

  for (Index = 0; Index < Length; Index++) {
    if (A[Index] != B[Index]) {
      return (int)A[Index] - (int)B[Index];
    }
  }

  return 0;
}

static void
MmioWrite32 (
  uint64_t Address,
  uint32_t Value
  )
{
  *(volatile uint32_t *)(uintptr_t)Address = Value;
}

static uint32_t
MmioRead32 (
  uint64_t Address
  )
{
  return *(volatile uint32_t *)(uintptr_t)Address;
}

enum {
  UART_RBR = 0,
  UART_THR = 0,
  UART_DLL = 0,
  UART_IER = 1,
  UART_DLM = 1,
  UART_FCR = 2,
  UART_LCR = 3,
  UART_MCR = 4,
  UART_LSR = 5,
};

enum {
  UART_LCR_DLAB = 0x80,
  UART_LCR_8N1 = 0x03,
  UART_FCR_ENABLE = 0x01,
  UART_FCR_CLEAR_RX = 0x02,
  UART_FCR_CLEAR_TX = 0x04,
  UART_MCR_DTR = 0x01,
  UART_MCR_RTS = 0x02,
  UART_LSR_THRE = 0x20,
};

static uint64_t
UartReg (
  uint32_t Index
  )
{
  return CCR2004_UART0_BASE + ((uint64_t)Index << CCR2004_UART_REG_SHIFT);
}

void
UartInitialize (
  void
  )
{
  uint32_t Divisor;

  Divisor = CCR2004_UART_CLOCK_HZ / (16U * CCR2004_UART_BAUD);
  if (Divisor == 0U) {
    Divisor = 1U;
  }

  MmioWrite32 (UartReg (UART_IER), 0);
  MmioWrite32 (UartReg (UART_LCR), UART_LCR_DLAB);
  MmioWrite32 (UartReg (UART_DLL), Divisor & 0xFFU);
  MmioWrite32 (UartReg (UART_DLM), (Divisor >> 8) & 0xFFU);
  MmioWrite32 (UartReg (UART_LCR), UART_LCR_8N1);
  MmioWrite32 (UartReg (UART_FCR), UART_FCR_ENABLE | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX);
  MmioWrite32 (UartReg (UART_MCR), UART_MCR_DTR | UART_MCR_RTS);
}

static void
UartPutChar (
  char Character
  )
{
  while ((MmioRead32 (UartReg (UART_LSR)) & UART_LSR_THRE) == 0U) {
  }

  MmioWrite32 (UartReg (UART_THR), (uint32_t)(uint8_t)Character);
}

void
UartWrite (
  const char *String
  )
{
  while (*String != '\0') {
    UartPutChar (*String++);
  }
}

static char
HexNibble (
  uint8_t Value
  )
{
  Value &= 0x0FU;
  if (Value < 10U) {
    return (char)('0' + Value);
  }

  return (char)('a' + (Value - 10U));
}

void
UartWriteHex32 (
  uint32_t Value
  )
{
  int Shift;

  for (Shift = 28; Shift >= 0; Shift -= 4) {
    UartPutChar (HexNibble ((uint8_t)(Value >> Shift)));
  }
}

void
UartWriteHex64 (
  uint64_t Value
  )
{
  int Shift;

  for (Shift = 60; Shift >= 0; Shift -= 4) {
    UartPutChar (HexNibble ((uint8_t)(Value >> Shift)));
  }
}
