#include "spiloader.h"

static void
LogBanner (
  void
  )
{
  UartWrite ("SpiLoader: start\r\n");
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

void
SpiloaderMain (
  void
  )
{
  SPILOADER_RESULT Result;

  UartInitialize ();
  LogBanner ();

  SpiInitialize ();
  LogJedecId ();

  if (LoadImageFromFlash (&Result) != 0) {
    SpiloaderPanic ("ELF load failed");
  }

  UartWrite ("SpiLoader: jump 0x");
  UartWriteHex64 (Result.EntryPoint);
  UartWrite ("\r\n");

  SpiloaderJumpToImage (Result.EntryPoint, Result.ReservedTop);
}
