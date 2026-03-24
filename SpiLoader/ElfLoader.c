#include "spiloader.h"

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6

#define SPILOADER_PHDR_LIMIT  32U

static ELF64_PHDR mProgramHeaders[SPILOADER_PHDR_LIMIT];

static int
RangeOverlaps (
  uint64_t StartA,
  uint64_t SizeA,
  uint64_t StartB,
  uint64_t SizeB
  )
{
  uint64_t EndA;
  uint64_t EndB;

  if ((SizeA == 0U) || (SizeB == 0U)) {
    return 0;
  }

  EndA = StartA + SizeA;
  EndB = StartB + SizeB;
  if ((EndA < StartA) || (EndB < StartB)) {
    return 1;
  }

  return !((EndA <= StartB) || (EndB <= StartA));
}

static int
ValidateElfHeader (
  const ELF64_EHDR *Header
  )
{
  if ((Header->e_ident[EI_MAG0] != ELFMAG0) ||
      (Header->e_ident[EI_MAG1] != ELFMAG1) ||
      (Header->e_ident[EI_MAG2] != ELFMAG2) ||
      (Header->e_ident[EI_MAG3] != ELFMAG3)) {
    return -1;
  }

  if ((Header->e_ident[EI_CLASS] != ELFCLASS64) ||
      (Header->e_ident[EI_DATA] != ELFDATA2LSB) ||
      (Header->e_ident[EI_VERSION] != EV_CURRENT)) {
    return -1;
  }

  if ((Header->e_machine != EM_AARCH64) ||
      (Header->e_version != EV_CURRENT) ||
      (Header->e_ehsize < sizeof (ELF64_EHDR)) ||
      (Header->e_phentsize != sizeof (ELF64_PHDR)) ||
      (Header->e_phnum == 0U) ||
      (Header->e_phnum > SPILOADER_PHDR_LIMIT)) {
    return -1;
  }

  if ((Header->e_phoff > SPILOADER_MAX_IMAGE_SIZE) ||
      ((Header->e_phoff + ((uint64_t)Header->e_phnum * sizeof (ELF64_PHDR))) > SPILOADER_MAX_IMAGE_SIZE)) {
    return -1;
  }

  return 0;
}

static int
ValidateProgramHeaders (
  const ELF64_EHDR *Header,
  uint64_t         ReservedBase,
  uint64_t         ReservedSize,
  uint64_t         *MaxFileExtent
  )
{
  uint64_t Highest;

  Highest = Header->e_phoff + ((uint64_t)Header->e_phnum * sizeof (ELF64_PHDR));
  for (uint16_t Index = 0; Index < Header->e_phnum; Index++) {
    const ELF64_PHDR *ProgramHeader;
    uint64_t         Target;
    uint64_t         SegmentEnd;
    uint64_t         FileEnd;

    ProgramHeader = &mProgramHeaders[Index];
    if (ProgramHeader->p_type != PT_LOAD) {
      continue;
    }

    if (ProgramHeader->p_memsz < ProgramHeader->p_filesz) {
      return -1;
    }

    Target = ProgramHeader->p_paddr;
    if (Target == 0U) {
      Target = ProgramHeader->p_vaddr;
    }

    SegmentEnd = Target + ProgramHeader->p_memsz;
    FileEnd = ProgramHeader->p_offset + ProgramHeader->p_filesz;
    if ((SegmentEnd < Target) || (FileEnd < ProgramHeader->p_offset)) {
      return -1;
    }

    if (FileEnd > SPILOADER_MAX_IMAGE_SIZE) {
      return -1;
    }

    if (RangeOverlaps (Target, ProgramHeader->p_memsz, ReservedBase, ReservedSize) != 0) {
      return -1;
    }

    if (FileEnd > Highest) {
      Highest = FileEnd;
    }
  }

  *MaxFileExtent = Highest;
  return 0;
}

static void
CleanInvalidateDataCacheRange (
  uint64_t Address,
  uint64_t Length
  )
{
  uint64_t CacheLineBytes;
  uint64_t Start;
  uint64_t End;
  uint64_t Ctr;

  if (Length == 0U) {
    return;
  }

  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (Ctr));
  CacheLineBytes = 4ULL << ((Ctr >> 16) & 0x0FU);
  Start = Address & ~(CacheLineBytes - 1U);
  End = (Address + Length + CacheLineBytes - 1U) & ~(CacheLineBytes - 1U);

  for (uint64_t Cursor = Start; Cursor < End; Cursor += CacheLineBytes) {
    __asm__ volatile ("dc civac, %0" :: "r" (Cursor) : "memory");
  }
}

static void
LoadSegment (
  const ELF64_PHDR *ProgramHeader
  )
{
  uint64_t Target;

  Target = (ProgramHeader->p_paddr != 0U) ? ProgramHeader->p_paddr : ProgramHeader->p_vaddr;

  if (ProgramHeader->p_filesz > 0U) {
    if (SpiFlashRead (CCR2004_SPI_FLASH_OFFSET + (uint32_t)ProgramHeader->p_offset,
                      (void *)(uintptr_t)Target,
                      (size_t)ProgramHeader->p_filesz) != 0) {
      SpiloaderPanic ("segment read");
    }
  }

  if (ProgramHeader->p_memsz > ProgramHeader->p_filesz) {
    SpiloaderMemSet ((void *)(uintptr_t)(Target + ProgramHeader->p_filesz),
                     0,
                     (size_t)(ProgramHeader->p_memsz - ProgramHeader->p_filesz));
  }

  CleanInvalidateDataCacheRange (Target, ProgramHeader->p_memsz);
}

int
LoadImageFromFlash (
  SPILOADER_RESULT *Result
  )
{
  ELF64_EHDR Header;
  uint64_t   ReservedBase;
  uint64_t   ReservedTop;
  uint64_t   ReservedSize;
  uint64_t   MaxFileExtent;

  ReservedBase = SPILOADER_LOAD_ADDR;
  ReservedTop = (uint64_t)(uintptr_t)__stack_top;
  ReservedSize = ReservedTop - ReservedBase;

  if (SpiFlashRead (CCR2004_SPI_FLASH_OFFSET, &Header, sizeof (Header)) != 0) {
    return -1;
  }

  if (ValidateElfHeader (&Header) != 0) {
    UartWrite ("SpiLoader: bad ELF header\r\n");
    return -1;
  }

  if (SpiFlashRead (CCR2004_SPI_FLASH_OFFSET + (uint32_t)Header.e_phoff,
                    mProgramHeaders,
                    (size_t)Header.e_phnum * sizeof (ELF64_PHDR)) != 0) {
    UartWrite ("SpiLoader: PHDR read failed\r\n");
    return -1;
  }

  if (ValidateProgramHeaders (&Header, ReservedBase, ReservedSize, &MaxFileExtent) != 0) {
    UartWrite ("SpiLoader: bad PHDRs\r\n");
    return -1;
  }

  UartWrite ("SpiLoader: ELF bytes <= 0x");
  UartWriteHex64 (MaxFileExtent);
  UartWrite ("\r\n");

  for (uint16_t Index = 0; Index < Header.e_phnum; Index++) {
    if (mProgramHeaders[Index].p_type != PT_LOAD) {
      continue;
    }

    UartWrite ("SpiLoader: load seg @ 0x");
    UartWriteHex64 ((mProgramHeaders[Index].p_paddr != 0U) ? mProgramHeaders[Index].p_paddr : mProgramHeaders[Index].p_vaddr);
    UartWrite (" size 0x");
    UartWriteHex64 (mProgramHeaders[Index].p_memsz);
    UartWrite ("\r\n");

    LoadSegment (&mProgramHeaders[Index]);
  }

  Result->EntryPoint = Header.e_entry;
  Result->ReservedTop = ReservedTop;
  return 0;
}

static inline uint64_t
ReadCurrentEl (
  void
  )
{
  uint64_t CurrentEl;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  return CurrentEl >> 2;
}

void
SpiloaderJumpToImage (
  uint64_t EntryPoint,
  uint64_t ReservedTop
  )
{
  uint64_t Sctlr;

  CleanInvalidateDataCacheRange (SPILOADER_LOAD_ADDR, ReservedTop - SPILOADER_LOAD_ADDR);
  __asm__ volatile ("dsb sy" ::: "memory");
  __asm__ volatile ("ic iallu" ::: "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
  __asm__ volatile ("isb" ::: "memory");
  __asm__ volatile ("msr daifset, #0xf" ::: "memory");

  switch (ReadCurrentEl ()) {
    case 2:
      __asm__ volatile ("mrs %0, sctlr_el2" : "=r" (Sctlr));
      Sctlr &= ~((uint64_t)1U << 0);
      Sctlr &= ~((uint64_t)1U << 2);
      Sctlr &= ~((uint64_t)1U << 12);
      __asm__ volatile ("msr sctlr_el2, %0" :: "r" (Sctlr) : "memory");
      break;

    default:
      __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (Sctlr));
      Sctlr &= ~((uint64_t)1U << 0);
      Sctlr &= ~((uint64_t)1U << 2);
      Sctlr &= ~((uint64_t)1U << 12);
      __asm__ volatile ("msr sctlr_el1, %0" :: "r" (Sctlr) : "memory");
      break;
  }

  __asm__ volatile ("dsb sy" ::: "memory");
  __asm__ volatile ("isb" ::: "memory");
  __asm__ volatile (
    "mov x0, xzr\n"
    "mov x1, xzr\n"
    "mov x2, xzr\n"
    "mov x3, xzr\n"
    "br  %0\n"
    :
    : "r" (EntryPoint)
    : "x0", "x1", "x2", "x3", "memory"
  );

  __builtin_unreachable ();
}
