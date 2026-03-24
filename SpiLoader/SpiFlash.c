#include "spiloader.h"

enum {
  DW_SPI_CTRL0   = 0x00,
  DW_SPI_SSIENR  = 0x08,
  DW_SPI_SER     = 0x10,
  DW_SPI_BAUDR   = 0x14,
  DW_SPI_TXFLR   = 0x20,
  DW_SPI_RXFLR   = 0x24,
  DW_SPI_SR      = 0x28,
  DW_SPI_IMR     = 0x2C,
  DW_SPI_ICR     = 0x48,
  DW_SPI_DR      = 0x60,
  DW_SPI_SSI_OVR = 0xF4,
};

enum {
  DW_SPI_CTRL0_DFS_MASK   = 0x0F,
  DW_SPI_CTRL0_TMOD_SHIFT = 8,
  DW_SPI_TMOD_TR          = 0,
  DW_SPI_SR_BUSY          = 1U << 0,
  DW_SPI_SR_TFNF          = 1U << 1,
  DW_SPI_SSI_OVR_CS_ALL   = 0x0F,
  DW_SPI_FIFO_DEPTH       = 32U,
  SPI_FLASH_READ_CHUNK    = 4096U,
};

static void
MmioWrite16 (
  uint64_t Address,
  uint16_t Value
  )
{
  *(volatile uint16_t *)(uintptr_t)Address = Value;
}

static uint16_t
MmioRead16 (
  uint64_t Address
  )
{
  return *(volatile uint16_t *)(uintptr_t)Address;
}

static uint64_t
SpiReg (
  uint32_t Offset
  )
{
  return CCR2004_SPI_BASE + Offset;
}

static void
SpiControllerEnable (
  uint16_t Enable
  )
{
  MmioWrite16 (SpiReg (DW_SPI_SSIENR), Enable);
}

static void
SpiWaitIdle (
  void
  )
{
  while ((MmioRead16 (SpiReg (DW_SPI_SR)) & DW_SPI_SR_BUSY) != 0U) {
  }
}

void
SpiInitialize (
  void
  )
{
  uint32_t Divisor;

  Divisor = (CCR2004_SPI_CLOCK_HZ + CCR2004_SPI_MAX_HZ - 1U) / CCR2004_SPI_MAX_HZ;
  if (Divisor < 2U) {
    Divisor = 2U;
  }

  if ((Divisor & 1U) != 0U) {
    Divisor++;
  }

  SpiControllerEnable (0);
  MmioWrite16 (SpiReg (DW_SPI_IMR), 0);
  MmioWrite16 (SpiReg (DW_SPI_SER), 0);
  MmioWrite16 (SpiReg (DW_SPI_BAUDR), (uint16_t)Divisor);
  MmioRead16 (SpiReg (DW_SPI_ICR));
}

static int
SpiWriteThenRead (
  const uint8_t *Command,
  size_t        CommandLength,
  uint8_t       *ReadBuffer,
  size_t        ReadLength
  )
{
  uint16_t Ctrl0;
  size_t   CommandEcho;
  size_t   DummyRemaining;
  size_t   BytesRemaining;
  size_t   RxAvailable;
  size_t   TxUsed;

  Ctrl0 = (uint16_t)((8U - 1U) & DW_SPI_CTRL0_DFS_MASK);
  Ctrl0 |= (uint16_t)(DW_SPI_TMOD_TR << DW_SPI_CTRL0_TMOD_SHIFT);

  SpiControllerEnable (0);
  MmioWrite16 (SpiReg (DW_SPI_CTRL0), Ctrl0);
  MmioWrite16 (SpiReg (DW_SPI_SER), 0);
  MmioWrite16 (SpiReg (DW_SPI_SSI_OVR), DW_SPI_SSI_OVR_CS_ALL);
  SpiControllerEnable (1);

  for (size_t Index = 0; Index < CommandLength; Index++) {
    while ((MmioRead16 (SpiReg (DW_SPI_SR)) & DW_SPI_SR_TFNF) == 0U) {
    }

    MmioWrite16 (SpiReg (DW_SPI_DR), Command[Index]);
  }

  DummyRemaining = ReadLength;
  while ((DummyRemaining > 0U) &&
         ((MmioRead16 (SpiReg (DW_SPI_SR)) & DW_SPI_SR_TFNF) != 0U)) {
    MmioWrite16 (SpiReg (DW_SPI_DR), 0xFFU);
    DummyRemaining--;
  }

  MmioWrite16 (SpiReg (DW_SPI_SER), (uint16_t)(1U << CCR2004_SPI_FLASH_CS));

  CommandEcho = CommandLength;
  BytesRemaining = ReadLength;

  while (BytesRemaining > 0U) {
    TxUsed = MmioRead16 (SpiReg (DW_SPI_TXFLR));
    RxAvailable = MmioRead16 (SpiReg (DW_SPI_RXFLR));

    while ((DummyRemaining > 0U) &&
           (TxUsed < DW_SPI_FIFO_DEPTH) &&
           (RxAvailable < DW_SPI_FIFO_DEPTH)) {
      MmioWrite16 (SpiReg (DW_SPI_DR), 0xFFU);
      DummyRemaining--;
      TxUsed++;
      RxAvailable++;
    }

    RxAvailable = MmioRead16 (SpiReg (DW_SPI_RXFLR));
    while (RxAvailable > 0U) {
      uint8_t Byte;

      Byte = (uint8_t)MmioRead16 (SpiReg (DW_SPI_DR));
      if (CommandEcho > 0U) {
        CommandEcho--;
      } else {
        *ReadBuffer++ = Byte;
        BytesRemaining--;
        if (BytesRemaining == 0U) {
          break;
        }
      }

      RxAvailable--;
    }
  }

  SpiWaitIdle ();
  MmioWrite16 (SpiReg (DW_SPI_SER), 0);
  MmioWrite16 (SpiReg (DW_SPI_SSI_OVR), 0);
  SpiControllerEnable (0);
  MmioRead16 (SpiReg (DW_SPI_ICR));
  return 0;
}

int
SpiFlashReadJedecId (
  uint8_t JedecId[3]
  )
{
  const uint8_t Command = SPI_NOR_CMD_READ_ID;

  return SpiWriteThenRead (&Command, 1, JedecId, 3);
}

static int
SpiFlashReadChunk (
  uint32_t FlashOffset,
  uint8_t  *Buffer,
  size_t   Length
  )
{
  uint8_t Command[4];

  Command[0] = SPI_NOR_CMD_READ_DATA;
  Command[1] = (uint8_t)(FlashOffset >> 16);
  Command[2] = (uint8_t)(FlashOffset >> 8);
  Command[3] = (uint8_t)(FlashOffset >> 0);

  return SpiWriteThenRead (Command, sizeof (Command), Buffer, Length);
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
    size_t Chunk;

    Chunk = (Length > SPI_FLASH_READ_CHUNK) ? SPI_FLASH_READ_CHUNK : Length;
    if (SpiFlashReadChunk (FlashOffset, Bytes, Chunk) != 0) {
      return -1;
    }

    FlashOffset += (uint32_t)Chunk;
    Bytes += Chunk;
    Length -= Chunk;
  }

  return 0;
}
