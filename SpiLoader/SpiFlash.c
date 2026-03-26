#include "spiloader.h"
#include "hal/al_hal_spi.h"

static struct al_spi_interface gSpi;

void
SpiInitialize (
  void
  )
{
  al_spi_init (&gSpi, (void *)(uintptr_t)CCR2004_SPI_BASE, CCR2004_SPI_CLOCK_HZ);
  al_spi_claim_bus (&gSpi, CCR2004_SPI_MAX_HZ,
                    AL_SPI_PHASE_SLAVE_SELECT, AL_SPI_POLARITY_INACTIVE_LOW,
                    CCR2004_SPI_FLASH_CS);
}

int
SpiFlashReadJedecId (
  uint8_t JedecId[3]
  )
{
  const uint8_t Cmd = SPI_NOR_CMD_READ_ID;
  return al_spi_read (&gSpi, &Cmd, 1, JedecId, 3,
                       CCR2004_SPI_FLASH_CS, 1000000);
}

int
SpiFlashRead (
  uint32_t FlashOffset,
  void     *Buffer,
  size_t   Length
  )
{
  uint8_t *Bytes;

  if (((uint64_t)FlashOffset + Length) > 0x01000000ULL) {
    return -1;
  }

  Bytes = (uint8_t *)Buffer;
  while (Length > 0U) {
    size_t  Chunk;
    uint8_t Cmd[4];

    Chunk = (Length > 4096U) ? 4096U : Length;

    Cmd[0] = SPI_NOR_CMD_READ_DATA;
    Cmd[1] = (uint8_t)(FlashOffset >> 16);
    Cmd[2] = (uint8_t)(FlashOffset >> 8);
    Cmd[3] = (uint8_t)(FlashOffset >> 0);

    if (al_spi_read (&gSpi, Cmd, 4, Bytes, (uint32_t)Chunk,
                      CCR2004_SPI_FLASH_CS, 1000000) != 0) {
      return -1;
    }

    FlashOffset += (uint32_t)Chunk;
    Bytes += Chunk;
    Length -= Chunk;
  }

  return 0;
}
