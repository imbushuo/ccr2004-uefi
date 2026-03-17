/** @file
  RouterBOOT trampoline application for MikroTik CCR2004.

  Reads the factory RouterBOOT image from SPI NOR flash, validates the
  Alpine HAL flash object header and footer checksums, exits UEFI boot
  services, and chain-loads RouterBOOT at its native load/exec addresses.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SpiNorFlash.h>

//
// RouterBOOT location in SPI flash
//
#define ROUTERBOOT_SPI_OFFSET  0x08a000
#define ROUTERBOOT_READ_SIZE   0x20000

//
// Alpine HAL flash object header
//
#define AL_FLASH_OBJ_MAGIC_NUM  0x000b9ec7

#pragma pack(1)
typedef struct {
  UINT32  MagicNum;
  UINT32  FormatRevId;
  UINT32  Id;
  UINT32  MajorVer;
  UINT32  MinorVer;
  UINT32  FixVer;
  UINT8   Desc[16];
  UINT32  Size;
  UINT32  LoadAddrHi;
  UINT32  LoadAddrLo;
  UINT32  ExecAddrHi;
  UINT32  ExecAddrLo;
  UINT32  Flags;
  UINT32  Reserved;
  UINT32  Checksum;
} AL_FLASH_OBJ_HDR;

typedef struct {
  UINT32  DataChecksum;
} AL_FLASH_OBJ_FOOTER;
#pragma pack()

/**
  Compute a 32-bit byte-sum checksum over a buffer.
**/
STATIC
UINT32
ByteSumChecksum (
  IN CONST UINT8  *Data,
  IN UINTN        Length
  )
{
  UINT32  Sum;
  UINTN   Index;

  Sum = 0;
  for (Index = 0; Index < Length; Index++) {
    Sum += Data[Index];
  }

  return Sum;
}

/**
  Write an ASCII string directly to the serial port.
  Works before and after ExitBootServices.
**/
STATIC
VOID
SerialLog (
  IN CONST CHAR8  *Msg
  )
{
  UINTN  Len;

  for (Len = 0; Msg[Len] != '\0'; Len++) {
  }

  SerialPortWrite ((UINT8 *)Msg, Len);
}

/**
  Entry point for the RouterBOOT trampoline application.

  @param[in] ImageHandle  The image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS           Should not return on success (jumps to RouterBOOT).
  @retval EFI_NOT_FOUND         SPI NOR flash protocol not found.
  @retval EFI_DEVICE_ERROR      SPI read failed.
  @retval EFI_COMPROMISED_DATA  Header or payload checksum mismatch.
**/
EFI_STATUS
EFIAPI
RouterBootAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                  Status;
  EFI_SPI_NOR_FLASH_PROTOCOL  *SpiNor;
  UINT8                       *FlashBuf;
  AL_FLASH_OBJ_HDR            *Hdr;
  AL_FLASH_OBJ_FOOTER         *Footer;
  UINT32                      HdrChecksum;
  UINT32                      DataChecksum;
  UINT64                      LoadAddr;
  UINT64                      ExecAddr;
  UINT32                      PayloadSize;
  UINT8                       *PayloadSrc;
  UINTN                       MemoryMapSize;
  EFI_MEMORY_DESCRIPTOR       *MemoryMap;
  UINTN                       MapKey;
  UINTN                       DescriptorSize;
  UINT32                      DescriptorVersion;
  UINTN                       Index;
  typedef VOID (*ROUTERBOOT_ENTRY) (VOID);
  ROUTERBOOT_ENTRY            EntryPoint;

  SerialLog ("[RB] RouterBOOT Trampoline\r\n");
  Print (L"RouterBOOT Trampoline\n\n");

  //
  // Step 1: Locate SPI NOR flash protocol and read RouterBOOT region.
  //
  SerialLog ("[RB] Step 1: Locating SPI NOR flash protocol\r\n");
  Status = gBS->LocateProtocol (
                  &gEfiSpiNorFlashProtocolGuid,
                  NULL,
                  (VOID **)&SpiNor
                  );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: SPI NOR flash protocol not found: %r\n", Status);
    return Status;
  }

  //
  // Step 1a: Read just the header first as a small test read.
  //
  SerialLog ("[RB] Step 1: Reading header from SPI offset 0x08a000\r\n");
  FlashBuf = AllocatePool (ROUTERBOOT_READ_SIZE);
  if (FlashBuf == NULL) {
    Print (L"ERROR: Failed to allocate read buffer\n");
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SpiNor->ReadData (
                     SpiNor,
                     ROUTERBOOT_SPI_OFFSET,
                     sizeof (AL_FLASH_OBJ_HDR),
                     FlashBuf
                     );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: SPI header read failed: %r\n", Status);
    FreePool (FlashBuf);
    return Status;
  }

  SerialLog ("[RB] Step 1: Header read complete\r\n");

  //
  // Dump raw header bytes for debugging.
  //
  Print (L"  Raw header (%u bytes):\n", (UINT32)sizeof (AL_FLASH_OBJ_HDR));
  for (Index = 0; Index < sizeof (AL_FLASH_OBJ_HDR); Index++) {
    if ((Index % 16) == 0) {
      Print (L"  %04x:", (UINT32)Index);
    }

    Print (L" %02x", FlashBuf[Index]);
    if ((Index % 16) == 15 || Index == sizeof (AL_FLASH_OBJ_HDR) - 1) {
      Print (L"\n");
    }
  }

  //
  // Step 2: Validate al_flash_obj_hdr.
  //
  SerialLog ("[RB] Step 2: Validating flash object header\r\n");
  Hdr = (AL_FLASH_OBJ_HDR *)FlashBuf;

  if (Hdr->MagicNum != AL_FLASH_OBJ_MAGIC_NUM) {
    Print (
      L"ERROR: Bad magic number: 0x%08x (expected 0x%08x)\n",
      Hdr->MagicNum,
      AL_FLASH_OBJ_MAGIC_NUM
      );
    FreePool (FlashBuf);
    return EFI_COMPROMISED_DATA;
  }

  //
  // Header checksum covers all fields except the Checksum field itself.
  //
  HdrChecksum = ByteSumChecksum (FlashBuf, sizeof (AL_FLASH_OBJ_HDR) - sizeof (UINT32));
  if (HdrChecksum != Hdr->Checksum) {
    Print (
      L"ERROR: Header checksum mismatch: computed 0x%08x, expected 0x%08x\n",
      HdrChecksum,
      Hdr->Checksum
      );
    FreePool (FlashBuf);
    return EFI_COMPROMISED_DATA;
  }

  PayloadSize = Hdr->Size;
  LoadAddr    = LShiftU64 (Hdr->LoadAddrHi, 32) | Hdr->LoadAddrLo;
  ExecAddr    = LShiftU64 (Hdr->ExecAddrHi, 32) | Hdr->ExecAddrLo;

  //
  // Bounds check: header + payload + footer must fit in what we read.
  //
  if (sizeof (AL_FLASH_OBJ_HDR) + PayloadSize + sizeof (AL_FLASH_OBJ_FOOTER) > ROUTERBOOT_READ_SIZE) {
    Print (
      L"ERROR: Payload too large: %u bytes (max %u)\n",
      PayloadSize,
      (UINT32)(ROUTERBOOT_READ_SIZE - sizeof (AL_FLASH_OBJ_HDR) - sizeof (AL_FLASH_OBJ_FOOTER))
      );
    FreePool (FlashBuf);
    return EFI_COMPROMISED_DATA;
  }

  SerialLog ("[RB] Step 2: Header valid\r\n");

  //
  // Step 4: Print info early (before full payload read).
  //
  Print (L"  Description : %.16a\n", Hdr->Desc);
  Print (L"  Version     : %u.%u.%u\n", Hdr->MajorVer, Hdr->MinorVer, Hdr->FixVer);
  Print (L"  Load address: 0x%lx\n", LoadAddr);
  Print (L"  Exec address: 0x%lx\n", ExecAddr);
  Print (L"  Payload size: %u bytes\n", PayloadSize);

  //
  // Step 1b: Now read the full region (header + payload + footer).
  //
  SerialLog ("[RB] Step 1b: Reading full payload from SPI\r\n");
  Status = SpiNor->ReadData (
                     SpiNor,
                     ROUTERBOOT_SPI_OFFSET,
                     sizeof (AL_FLASH_OBJ_HDR) + PayloadSize + sizeof (AL_FLASH_OBJ_FOOTER),
                     FlashBuf
                     );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: SPI payload read failed: %r\n", Status);
    FreePool (FlashBuf);
    return Status;
  }

  SerialLog ("[RB] Step 1b: Full payload read complete\r\n");

  //
  // Re-point after full read (buffer was overwritten from offset 0).
  //
  Hdr = (AL_FLASH_OBJ_HDR *)FlashBuf;

  //
  // Step 3: Validate payload data checksum.
  //
  SerialLog ("[RB] Step 3: Validating payload data checksum\r\n");
  PayloadSrc = FlashBuf + sizeof (AL_FLASH_OBJ_HDR);
  Footer     = (AL_FLASH_OBJ_FOOTER *)(PayloadSrc + PayloadSize);

  DataChecksum = ByteSumChecksum (PayloadSrc, PayloadSize);
  if (DataChecksum != Footer->DataChecksum) {
    Print (
      L"ERROR: Data checksum mismatch: computed 0x%08x, expected 0x%08x\n",
      DataChecksum,
      Footer->DataChecksum
      );
    FreePool (FlashBuf);
    return EFI_COMPROMISED_DATA;
  }

  SerialLog ("[RB] Step 3: Payload checksum valid\r\n");
  Print (L"\nBooting Factory RouterBOOT...\n");

  //
  // Step 5: Save relocation parameters (all in local variables on stack).
  // PayloadSrc, PayloadSize, LoadAddr, ExecAddr are already set above.
  //

  //
  // Step 6: ExitBootServices.
  // Standard GetMemoryMap -> ExitBootServices pattern with one retry.
  //
  SerialLog ("[RB] Step 6: Calling ExitBootServices\r\n");
  MemoryMapSize = 0;
  MemoryMap     = NULL;

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  ASSERT (Status == EFI_BUFFER_TOO_SMALL);

  //
  // Allocate with extra space for the allocation itself.
  //
  MemoryMapSize += 2 * DescriptorSize;
  MemoryMap = AllocatePool (MemoryMapSize);
  if (MemoryMap == NULL) {
    Print (L"ERROR: Failed to allocate memory map buffer\n");
    FreePool (FlashBuf);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->GetMemoryMap (
                  &MemoryMapSize,
                  MemoryMap,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: GetMemoryMap failed: %r\n", Status);
    FreePool (MemoryMap);
    FreePool (FlashBuf);
    return Status;
  }

  Status = gBS->ExitBootServices (ImageHandle, MapKey);
  if (EFI_ERROR (Status)) {
    //
    // MapKey was stale; retry once with a fresh map.
    //
    MemoryMapSize = 0;
    Status = gBS->GetMemoryMap (
                    &MemoryMapSize,
                    NULL,
                    &MapKey,
                    &DescriptorSize,
                    &DescriptorVersion
                    );
    // Re-use the existing buffer if large enough, otherwise we cannot allocate
    // (ExitBootServices already partially tore down).  The buffer from above
    // should be large enough.
    Status = gBS->GetMemoryMap (
                    &MemoryMapSize,
                    MemoryMap,
                    &MapKey,
                    &DescriptorSize,
                    &DescriptorVersion
                    );
    if (!EFI_ERROR (Status)) {
      Status = gBS->ExitBootServices (ImageHandle, MapKey);
    }

    if (EFI_ERROR (Status)) {
      //
      // Cannot recover - UEFI is in an undefined state.
      //
      CpuDeadLoop ();
    }
  }

  //
  // === No UEFI Boot Services available beyond this point ===
  //

  //
  // Step 7: Post-ExitBootServices — prepare platform hardware and trampoline.
  //
  SerialLog ("[RB] Step 7: Preparing platform hardware\r\n");

  //
  // 7a: Disable all exceptions and interrupts so no stray handler fires
  //     after UEFI runtime is torn down.
  //
  ArmDisableInterrupts ();
  ArmDisableAsynchronousAbort ();
  ArmDisableBranchPrediction ();

  //
  // 7b: Flush the payload source buffer from D-cache to DRAM, then clean
  //     the entire data cache so stack, locals, and any other dirty lines
  //     are written back before caches are turned off.
  //
  WriteBackInvalidateDataCacheRange (
    FlashBuf,
    ROUTERBOOT_READ_SIZE
    );
  WriteBackInvalidateDataCacheRange (
    (VOID *)(UINTN)LoadAddr,
    PayloadSize
    );
  WriteBackInvalidateDataCacheRange (
    MemoryMap,
    MemoryMapSize
    );

  //
  // 7c: Disable caches, MMU, and invalidate TLB.
  //     Order matters: I-cache invalidate, then disable D$, I$, MMU, TLB.
  //
  ArmInvalidateInstructionCache ();
  ArmDisableDataCache ();
  ArmDisableInstructionCache ();
  ArmDisableMmu ();
  ArmInvalidateTlb ();

  //
  // 7d: Copy payload to load address.
  //     MMU is off — all addresses are flat physical.
  //
  SerialLog ("[RB] Step 7: Copying payload to load address\r\n");
  {
    volatile UINT8  *Dst = (volatile UINT8 *)(UINTN)LoadAddr;
    UINT8           *Src = PayloadSrc;
    UINT32          Len  = PayloadSize;

    for (Index = 0; Index < Len; Index++) {
      Dst[Index] = Src[Index];
    }
  }

  //
  // 7e: Invalidate I-cache so CPU fetches fresh code from the copy.
  //
  ArmInvalidateInstructionCache ();

  //
  // 7f: Jump to RouterBOOT entry point — never returns.
  //
  SerialLog ("[RB] Step 7: Jumping to RouterBOOT\r\n");
  EntryPoint = (ROUTERBOOT_ENTRY)(UINTN)ExecAddr;
  EntryPoint ();

  //
  // Should never reach here.
  //
  CpuDeadLoop ();
  return EFI_SUCCESS;
}
