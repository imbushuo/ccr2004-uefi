/** @file
  SEC phase for MikroTik CCR2004.

  Forked from ArmPlatformPkg/PeilessSec. Performs PEI initialization
  (serial, HOBs, MMU) then decompresses and loads DXE Core.

  Copyright (c) 2011-2017, ARM Limited. All rights reserved.
  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Sec.h"

typedef
VOID
(EFIAPI *DXE_CORE_ENTRY_POINT)(
  IN  VOID *HobStart
  );

#define IS_XIP()  (((UINT64)FixedPcdGet64 (PcdFdBaseAddress) > mSystemMemoryEnd) ||\
                  ((FixedPcdGet64 (PcdFdBaseAddress) + FixedPcdGet32 (PcdFdSize)) <= FixedPcdGet64 (PcdSystemMemoryBase)))

UINT64  mSystemMemoryEnd = FixedPcdGet64 (PcdSystemMemoryBase) +
                           FixedPcdGet64 (PcdSystemMemorySize) - 1;

/**
  SEC main routine.

  @param[in]  UefiMemoryBase  Start of the PI/UEFI memory region
  @param[in]  StackBase       Start of the stack
  @param[in]  StartTimeStamp  Timer value at start of execution
**/
STATIC
VOID
SecMain (
  IN  UINTN   UefiMemoryBase,
  IN  UINTN   StackBase,
  IN  UINT64  StartTimeStamp
  )
{
  EFI_HOB_HANDOFF_INFO_TABLE  *HobList;
  EFI_STATUS                  Status;
  CHAR8                       Buffer[100];
  UINTN                       CharCount;
  UINTN                       StacksSize;
  FIRMWARE_SEC_PERFORMANCE    Performance;

  // Ensure the FD is either part of the System Memory or totally outside of the System Memory (XIP)
  ASSERT (
    IS_XIP () ||
    ((FixedPcdGet64 (PcdFdBaseAddress) >= FixedPcdGet64 (PcdSystemMemoryBase)) &&
     ((UINT64)(FixedPcdGet64 (PcdFdBaseAddress) + FixedPcdGet32 (PcdFdSize)) <= (UINT64)mSystemMemoryEnd))
    );

  // Initialize the architecture specific bits
  ArchInitialize ();

  // Initialize the Serial Port
  SerialPortInitialize ();
  CharCount = AsciiSPrint (
                Buffer,
                sizeof (Buffer),
                "UEFI firmware (version %s built at %a on %a)\n\r",
                (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString),
                __TIME__,
                __DATE__
                );
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // Initialize the Debug Agent for Source Level Debugging
  InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  SaveAndSetDebugTimerInterrupt (TRUE);

  // Declare the PI/UEFI memory region
  SerialPortWrite ((UINT8 *)"SEC: HOB init\n\r", 15);
  HobList = HobConstructor (
              (VOID *)UefiMemoryBase,
              FixedPcdGet32 (PcdSystemMemoryUefiRegionSize),
              (VOID *)UefiMemoryBase,
              (VOID *)StackBase // The top of the UEFI Memory is reserved for the stack
              );
  PrePeiSetHobList (HobList);

  // Initialize MMU and Memory HOBs (Resource Descriptor HOBs)
  SerialPortWrite ((UINT8 *)"SEC: MMU init\n\r", 15);
  Status = MemoryPeim (UefiMemoryBase, FixedPcdGet32 (PcdSystemMemoryUefiRegionSize));
  ASSERT_EFI_ERROR (Status);

  // Create the Stacks HOB
  StacksSize = PcdGet32 (PcdCPUCorePrimaryStackSize);
  BuildStackHob (StackBase, StacksSize);

  // Build CPU HOB
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Store timer value logged at the beginning of firmware image execution
  Performance.ResetEnd = GetTimeInNanoSecond (StartTimeStamp);

  // Build SEC Performance Data Hob
  BuildGuidDataHob (&gEfiFirmwarePerformanceGuid, &Performance, sizeof (Performance));

  // Set the Boot Mode
  SetBootMode (ArmPlatformGetBootMode ());

  // Initialize Platform HOBs (CpuHob and FvHob)
  SerialPortWrite ((UINT8 *)"SEC: Platform HOBs\n\r", 20);
  Status = PlatformPeim ();
  ASSERT_EFI_ERROR (Status);

  // Now, the HOB List has been initialized, we can register performance information
  PERF_START (NULL, "PEI", NULL, StartTimeStamp);

  // SEC phase needs to run library constructors by hand.
  ProcessLibraryConstructorList ();

  // Decompress the main firmware volume
  Status = DecompressFirstFv ();
  CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "SEC: DecompressFirstFv = %r\n\r", Status);
  SerialPortWrite ((UINT8 *)Buffer, CharCount);
  ASSERT_EFI_ERROR (Status);

  // Dump FV HOBs after decompression
  {
    EFI_PEI_HOB_POINTERS  DebugHob;
    UINTN                 FvCount = 0;
    DebugHob.Raw = GetHobList ();
    while ((DebugHob.Raw = GetNextHob (EFI_HOB_TYPE_FV, DebugHob.Raw)) != NULL) {
      CharCount = AsciiSPrint (
                    Buffer, sizeof (Buffer),
                    "SEC: FV[%d] Base=0x%lx Len=0x%lx\n\r",
                    FvCount++,
                    DebugHob.FirmwareVolume->BaseAddress,
                    DebugHob.FirmwareVolume->Length
                    );
      SerialPortWrite ((UINT8 *)Buffer, CharCount);
      DebugHob.Raw = GET_NEXT_HOB (DebugHob);
    }
  }

  // Find and load DXE Core manually with debug output
  {
    EFI_PEI_FV_HANDLE     DxeVol;
    EFI_PEI_FILE_HANDLE   DxeFile = NULL;
    VOID                  *PeCoffImage;
    EFI_PHYSICAL_ADDRESS  ImageAddress;
    UINT64                ImageSize;
    EFI_PHYSICAL_ADDRESS  EntryPoint;
    EFI_FV_FILE_INFO      FvFileInfo;

    Status = FfsAnyFvFindFirstFile (EFI_FV_FILETYPE_DXE_CORE, &DxeVol, &DxeFile);
    CharCount = AsciiSPrint (
                  Buffer, sizeof (Buffer),
                  "SEC: Find DXE_CORE = %r File=0x%p\n\r",
                  Status, DxeFile
                  );
    SerialPortWrite ((UINT8 *)Buffer, CharCount);

    if (!EFI_ERROR (Status)) {
      Status = FfsFindSectionData (EFI_SECTION_PE32, DxeFile, &PeCoffImage);
      CharCount = AsciiSPrint (
                    Buffer, sizeof (Buffer),
                    "SEC: Find PE32 = %r Image=0x%p\n\r",
                    Status, PeCoffImage
                    );
      SerialPortWrite ((UINT8 *)Buffer, CharCount);
    }

    if (!EFI_ERROR (Status)) {
      SerialPortWrite ((UINT8 *)"SEC: LoadPeCoffImage\n\r", 21);
      Status = LoadPeCoffImage (PeCoffImage, &ImageAddress, &ImageSize, &EntryPoint);
      CharCount = AsciiSPrint (
                    Buffer, sizeof (Buffer),
                    "SEC: LoadPeCoff = %r Addr=0x%lx Size=0x%lx EP=0x%lx\n\r",
                    Status, ImageAddress, ImageSize, EntryPoint
                    );
      SerialPortWrite ((UINT8 *)Buffer, CharCount);
    }

    if (!EFI_ERROR (Status)) {
      // Build the Module HOB for DxeCore — DxeMain searches for this at startup
      Status = FfsGetFileInfo (DxeFile, &FvFileInfo);
      ASSERT_EFI_ERROR (Status);
      BuildModuleHob (
        &FvFileInfo.FileName,
        ImageAddress,
        EFI_SIZE_TO_PAGES ((UINT32)ImageSize) * EFI_PAGE_SIZE,
        EntryPoint
        );
      CharCount = AsciiSPrint (
                    Buffer, sizeof (Buffer),
                    "SEC: Built ModuleHob for DxeCore\n\r"
                    );
      SerialPortWrite ((UINT8 *)Buffer, CharCount);

      VOID *Hob = GetHobList ();
      CharCount = AsciiSPrint (
                    Buffer, sizeof (Buffer),
                    "SEC: Jumping to DXE Core at 0x%lx HobList=0x%p\n\r",
                    EntryPoint, Hob
                    );
      SerialPortWrite ((UINT8 *)Buffer, CharCount);
      ((DXE_CORE_ENTRY_POINT)(UINTN)EntryPoint)(Hob);
    }
  }

  // Should not reach here
  Status = EFI_DEVICE_ERROR;

  CharCount = AsciiSPrint (
                Buffer, sizeof (Buffer),
                "SEC: LoadDxeCoreFromFv = %r (should not reach here)\n\r",
                Status
                );
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // Should never reach here
  CpuDeadLoop ();
}

/**
  C entrypoint into the SEC driver.

  @param[in]  UefiMemoryBase  Start of the PI/UEFI memory region
  @param[in]  StackBase       Start of the stack
**/
VOID
CEntryPoint (
  IN  UINTN  UefiMemoryBase,
  IN  UINTN  StackBase
  )
{
  UINT64  StartTimeStamp;

  // Initialize the platform specific controllers
  ArmPlatformInitialize (ArmReadMpidr ());

  if (PerformanceMeasurementEnabled ()) {
    // We cannot call yet the PerformanceLib because the HOB List has not been initialized
    StartTimeStamp = GetPerformanceCounter ();
  } else {
    StartTimeStamp = 0;
  }

  // Data Cache enabled on Primary core when MMU is enabled.
  ArmDisableDataCache ();
  // Invalidate instruction cache
  ArmInvalidateInstructionCache ();
  // Enable Instruction Caches on all cores.
  ArmEnableInstructionCache ();

  InvalidateDataCacheRange (
    (VOID *)UefiMemoryBase,
    FixedPcdGet32 (PcdSystemMemoryUefiRegionSize)
    );

  SecMain (UefiMemoryBase, StackBase, StartTimeStamp);

  // Should never return
  ASSERT (FALSE);
}
