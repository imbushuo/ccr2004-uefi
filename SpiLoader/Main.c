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

/*
 * Special boot mode via PBS SRAM shared data.
 *
 * The magic word is stored in al_general_shared_data.reserved2[0..3]
 * interpreted as a little-endian uint32_t.
 *
 * Address: AL_PBS_SRAM_BASE + SHARED_DATA_OFFSET + reserved2 offset
 *   AL_PBS_SRAM_BASE = 0xFD8A4000
 *   SHARED_DATA_OFFSET = 0x180
 *   reserved2 offset within struct = 0x1A
 *   => 0xFD8A419A
 *
 * Note: this is at a 2-byte aligned (not 4-byte) address, so we must
 * use byte access to read/write the uint32_t to avoid alignment faults
 * on device-adjacent SRAM.
 */
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

  /* Clear magic to prevent boot loop */
  WriteSramMagic (0x00000000U);

  /* Load payload from SPI flash */
  UartWrite ("SpiLoader: loading 0x");
  UartWriteHex32 (SPECIAL_SPI_SIZE);
  UartWrite (" bytes from SPI 0x");
  UartWriteHex32 (SPECIAL_SPI_OFFSET);
  UartWrite (" to 0x");
  UartWriteHex64 (SPECIAL_LOAD_ADDR);
  UartWrite ("\r\n");

  if (SpiFlashRead (SPECIAL_SPI_OFFSET, (void *)(uintptr_t)SPECIAL_LOAD_ADDR,
                    SPECIAL_SPI_SIZE) != 0) {
    SpiloaderPanic ("special mode: SPI read failed");
  }

  UartWrite ("SpiLoader: jumping to 0x");
  UartWriteHex64 (SPECIAL_LOAD_ADDR);
  UartWrite ("\r\n");

  SpiloaderJumpToImage (SPECIAL_LOAD_ADDR, 0);
  /* Does not return */
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

  CheckSpecialMode ();

  LogJedecId ();

  if (LoadImageFromFlash (&Result) != 0) {
    SpiloaderPanic ("ELF load failed");
  }

  UartWrite ("SpiLoader: jump 0x");
  UartWriteHex64 (Result.EntryPoint);
  UartWrite ("\r\n");

  SpiloaderJumpToImage (Result.EntryPoint, Result.ReservedTop);
}
