#ifndef SPILOADER_H_
#define SPILOADER_H_

#include <stddef.h>
#include <stdint.h>

#define SPILOADER_LOAD_ADDR        0x04000000ULL
#define SPILOADER_MAX_IMAGE_SIZE   (8U * 1024U * 1024U)
#define SPILOADER_MAX_EXEC_SIZE    65124U

#define CCR2004_UART0_BASE         0xFD883000ULL
#define CCR2004_UART_REG_SHIFT     2U
#define CCR2004_UART_CLOCK_HZ      500000000U
#define CCR2004_UART_BAUD          115200U

#define CCR2004_SPI_BASE           0xFD882000ULL
#define CCR2004_SPI_CLOCK_HZ       500000000U
#define CCR2004_SPI_MAX_HZ         30000000U
#define CCR2004_SPI_FLASH_CS       0U
#define CCR2004_SPI_FLASH_OFFSET   0x00800000U

#define SPI_NOR_CMD_READ_DATA      0x03U
#define SPI_NOR_CMD_READ_ID        0x9FU

#define CCR2004_GICD_BASE          0xF0200000ULL
#define CCR2004_GICR_BASE          0xF0280000ULL

#define ARRAY_SIZE(a)              (sizeof (a) / sizeof ((a)[0]))

typedef struct {
  uint64_t EntryPoint;
  uint64_t ReservedTop;
} SPILOADER_RESULT;

/*
 * Exception frame saved by Exception.S vector table.
 * Layout must match the assembly (288 bytes total).
 */
typedef struct {
  uint64_t X[31];     /* x0..x30, offsets 0..240   */
  uint64_t Sp;        /* pre-exception SP, off 248 */
  uint64_t Elr;       /* ELR_ELx,          off 256 */
  uint64_t Esr;       /* ESR_ELx,          off 264 */
  uint64_t Far;       /* FAR_ELx,          off 272 */
  uint64_t Spsr;      /* SPSR_ELx,         off 280 */
} EXCEPTION_FRAME;

typedef struct {
  unsigned char e_ident[16];
  uint16_t      e_type;
  uint16_t      e_machine;
  uint32_t      e_version;
  uint64_t      e_entry;
  uint64_t      e_phoff;
  uint64_t      e_shoff;
  uint32_t      e_flags;
  uint16_t      e_ehsize;
  uint16_t      e_phentsize;
  uint16_t      e_phnum;
  uint16_t      e_shentsize;
  uint16_t      e_shnum;
  uint16_t      e_shstrndx;
} ELF64_EHDR;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} ELF64_PHDR;

enum {
  PT_LOAD = 1,
};

enum {
  ELFCLASS64 = 2,
  ELFDATA2LSB = 1,
  EV_CURRENT = 1,
  EM_AARCH64 = 183,
};

void *SpiloaderMemCopy (void *Destination, const void *Source, size_t Length);
void *SpiloaderMemSet (void *Destination, int Value, size_t Length);
int SpiloaderMemCompare (const void *Left, const void *Right, size_t Length);

void UartInitialize (void);
void UartWrite (const char *String);
void UartWriteHex32 (uint32_t Value);
void UartWriteHex64 (uint64_t Value);

void SpiInitialize (void);
int SpiFlashRead (uint32_t FlashOffset, void *Buffer, size_t Length);
int SpiFlashReadJedecId (uint8_t JedecId[3]);

void WatchdogInit (void);
void WatchdogArm (uint32_t Seconds);
void WatchdogDisarm (void);
void DumpSpiControllerState (void);

int LoadImageFromFlash (SPILOADER_RESULT *Result);

void SpiloaderJumpToImage (uint64_t EntryPoint, uint64_t ReservedTop) __attribute__((noreturn));
void SpiloaderPanic (const char *Message) __attribute__((noreturn));
void SpiloaderExceptionHandler (uint64_t Type, EXCEPTION_FRAME *Frame);

extern char __image_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_top[];
extern char __stack_bottom[];

#endif
