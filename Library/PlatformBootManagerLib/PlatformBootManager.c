/** @file
  PlatformBootManagerLib for MikroTik CCR2004.

  Signals EndOfDxe, connects serial console, registers hotkeys,
  and launches the Setup UI (UiApp) when no boot option is available.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Guid/EventGroup.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

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

  Print (L"Press ESC or F2 for Setup.\n");
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

  DEBUG ((DEBUG_INFO, "[CCR2004] Launching UiApp (Boot Manager Menu)...\n"));
  EfiBootManagerBoot (&BootManagerMenu);
  DEBUG ((DEBUG_INFO, "[CCR2004] UiApp returned: %r\n", BootManagerMenu.Status));
  EfiBootManagerFreeLoadOption (&BootManagerMenu);
}
