/** @file
  PlatformBootManagerLib for MikroTik CCR2004.

  Signals EndOfDxe, connects serial console, registers hotkeys,
  and launches the Setup UI (UiApp) when no boot option is available.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Guid/EventGroup.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/MikroTikNandFlash.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// Shell application FILE_GUID from ShellPkg/Application/Shell/Shell.inf
//
STATIC EFI_GUID mUefiShellFileGuid = {
  0x7C04A583, 0x9E3E, 0x4f1c,
  { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }
};

//
// RouterOSLoader FILE_GUID from Application/RouterOSLoader/RouterOSLoader.inf
//
STATIC EFI_GUID mRouterOSLoaderFileGuid = {
  0x3a8f7c2e, 0xd159, 0x4b6a,
  { 0x91, 0xe0, 0x7f, 0x4c, 0x5b, 0x2a, 0x8d, 0x63 }
};

/**
  Boot description handler: name NAND filesystem "Built-in NAND".
**/
STATIC
CHAR16 *
EFIAPI
PlatformNandBootDescription (
  IN EFI_HANDLE  Handle,
  IN CONST CHAR16  *DefaultDescription
  )
{
  EFI_STATUS  Status;
  VOID        *Dummy;

  Status = gBS->HandleProtocol (Handle, &gMikroTikNandFlashProtocolGuid, &Dummy);
  if (!EFI_ERROR (Status)) {
    return AllocateCopyPool (sizeof (L"Built-in NAND"), L"Built-in NAND");
  }

  return NULL;
}

/**
  Find all SimpleTextOut/SimpleTextIn handles and register them
  in the ConOut/ConIn/ErrOut console variables so BDS can use them.
**/
STATIC
VOID
PlatformRegisterConsoles (
  VOID
  )
{
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  EFI_HANDLE                *Handles;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     Index;

  //
  // Register all SimpleTextOut devices as ConOut and ErrOut.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleTextOutProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      DevicePath = DevicePathFromHandle (Handles[Index]);
      if (DevicePath != NULL) {
        EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
        EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
      }
    }
    FreePool (Handles);
  }

  //
  // Register all SimpleTextIn devices as ConIn.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleTextInProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      DevicePath = DevicePathFromHandle (Handles[Index]);
      if (DevicePath != NULL) {
        EfiBootManagerUpdateConsoleVariable (ConIn, DevicePath, NULL);
      }
    }
    FreePool (Handles);
  }
}

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  //
  // Signal EndOfDxe PI event.
  //
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  //
  // Dispatch deferred images.
  //
  EfiBootManagerDispatchDeferredImages ();

  //
  // Connect all devices so SerialDxe + TerminalDxe bind and produce
  // SimpleTextIn/SimpleTextOut on the serial port.
  //
  EfiBootManagerConnectAll ();

  //
  // Register discovered console devices in ConIn/ConOut/ErrOut variables.
  // Without this, BDS has no idea which devices are consoles (emulated
  // variables are empty on every boot).
  //
  PlatformRegisterConsoles ();
}

/**
  Register an application from the firmware volume as a boot option,
  if not already present.
**/
STATIC
VOID
PlatformRegisterFvBootOption (
  IN  EFI_GUID   *FileGuid,
  IN  CHAR16     *Description,
  IN  UINT32     Attributes
  )
{
  EFI_STATUS                          Status;
  UINTN                               HandleCount;
  EFI_HANDLE                          *HandleBuffer;
  UINTN                               Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL       *Fv;
  EFI_FV_FILETYPE                     FoundType;
  UINT32                              FvStatus;
  EFI_FV_FILE_ATTRIBUTES              FileAttributes;
  UINTN                               Size;
  EFI_DEVICE_PATH_PROTOCOL            *DevicePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH   FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION        NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION        *BootOptions;
  UINTN                               BootOptionCount;
  BOOLEAN                             Found;

  //
  // Locate the FV containing the file.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    FoundType = EFI_FV_FILETYPE_APPLICATION;
    Status = Fv->ReadFile (
                   Fv,
                   FileGuid,
                   NULL,
                   &Size,
                   &FoundType,
                   &FileAttributes,
                   &FvStatus
                   );
    if (!EFI_ERROR (Status)) {
      break;
    }
  }

  if (Index >= HandleCount) {
    FreePool (HandleBuffer);
    return;
  }

  //
  // Build device path: FV device path + FV file node.
  //
  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle (HandleBuffer[Index]);
  ASSERT (DevicePath != NULL);
  DevicePath = AppendDevicePathNode (
                 DevicePath,
                 (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                 );
  FreePool (HandleBuffer);

  //
  // Check if this boot option already exists.
  //
  BootOptions = EfiBootManagerGetLoadOptions (
                  &BootOptionCount,
                  LoadOptionTypeBoot
                  );

  Found = FALSE;
  for (Index = 0; Index < BootOptionCount; Index++) {
    if ((BootOptions[Index].FilePath != NULL) &&
        (CompareMem (
           BootOptions[Index].FilePath,
           DevicePath,
           GetDevicePathSize (DevicePath)
           ) == 0))
    {
      Found = TRUE;
      break;
    }
  }

  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

  if (!Found) {
    Status = EfiBootManagerInitializeLoadOption (
               &NewOption,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               Attributes,
               Description,
               DevicePath,
               NULL,
               0
               );
    if (!EFI_ERROR (Status)) {
      EfiBootManagerAddLoadOptionVariable (&NewOption, MAX_UINTN);
      EfiBootManagerFreeLoadOption (&NewOption);
    }
  }

  FreePool (DevicePath);
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_INPUT_KEY                 F2;
  EFI_INPUT_KEY                 Esc;
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;

  //
  // Register F2 and Esc as hotkeys to enter Setup menu.
  //
  F2.ScanCode     = SCAN_F2;
  F2.UnicodeChar  = CHAR_NULL;
  Esc.ScanCode    = SCAN_ESC;
  Esc.UnicodeChar = CHAR_NULL;

  Status = EfiBootManagerGetBootManagerMenu (&BootOption);
  if (!EFI_ERROR (Status)) {
    EfiBootManagerAddKeyOptionVariable (
      NULL, (UINT16)BootOption.OptionNumber, 0, &F2, NULL
      );
    EfiBootManagerAddKeyOptionVariable (
      NULL, (UINT16)BootOption.OptionNumber, 0, &Esc, NULL
      );
    EfiBootManagerFreeLoadOption (&BootOption);
  }

  //
  // Register boot description handler so NAND shows as "Built-in NAND".
  //
  EfiBootManagerRegisterBootDescriptionHandler (PlatformNandBootDescription);

  //
  // Register RouterOS NPK Loader as the primary boot option.
  // It auto-boots after the 3-second timeout (PcdPlatformBootTimeOut).
  //
  PlatformRegisterFvBootOption (
    &mRouterOSLoaderFileGuid,
    L"RouterOS",
    LOAD_OPTION_ACTIVE
    );

  //
  // Register UEFI Shell as a secondary boot option.
  //
  PlatformRegisterFvBootOption (
    &mUefiShellFileGuid,
    L"UEFI Shell",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP
    );

  Print (L"Press ESC or F2 for Setup. Booting RouterOS in 3 seconds...\n");
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  BootManagerMenu;

  DEBUG ((DEBUG_INFO, "[CCR2004] PlatformBootManagerUnableToBoot: ENTER\n"));

  Status = EfiBootManagerGetBootManagerMenu (&BootManagerMenu);
  DEBUG ((DEBUG_INFO, "[CCR2004] EfiBootManagerGetBootManagerMenu: %r\n", Status));
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Reconnect all in case new devices appeared.
  //
  EfiBootManagerConnectAll ();

  //
  // Enumerate all discoverable boot options (PXE, HTTP boot, etc.)
  // so they appear in the Boot Manager menu inside UiApp.
  // This is intentionally NOT done in AfterConsole to avoid
  // automatic PXE/network boot attempts during normal startup.
  //
  EfiBootManagerRefreshAllBootOption ();

  //
  // Remove "UEFI Non-Block Boot Device" entries — FV-based images
  // auto-discovered by BDS that clutter the boot menu.
  // The NAND filesystem gets a proper name from our boot description handler.
  //
  {
    EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
    UINTN                         Count;
    UINTN                         i;

    BootOptions = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
    for (i = 0; i < Count; i++) {
      if ((BootOptions[i].Description != NULL) &&
          (StrnCmp (BootOptions[i].Description, L"UEFI Non-Block Boot Device", 26) == 0))
      {
        EfiBootManagerDeleteLoadOptionVariable (
          BootOptions[i].OptionNumber, LoadOptionTypeBoot);
      }
    }
    EfiBootManagerFreeLoadOptions (BootOptions, Count);
  }

  DEBUG ((DEBUG_INFO, "[CCR2004] Launching UiApp (Boot Manager Menu)...\n"));
  EfiBootManagerBoot (&BootManagerMenu);
  DEBUG ((DEBUG_INFO, "[CCR2004] UiApp returned: %r\n", BootManagerMenu.Status));
  EfiBootManagerFreeLoadOption (&BootManagerMenu);
}
