/** @file
  Platform GPIO controller description for MikroTik CCR2004 (Alpine V2 SoC).

  Installs PLATFORM_GPIO_CONTROLLER protocol describing the 6 PL061 GPIO
  controllers present on the Alpine V2 SoC, so that PL061GpioDxe can
  discover and manage all of them.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EmbeddedGpio.h>

//
// Alpine V2 has 6 PL061 GPIO controllers, each with 8 pins.
// Addresses and base indices from the device tree.
//
#define AL_GPIO_CONTROLLER_COUNT  6
#define AL_GPIO_PINS_PER_CTRL    8
#define AL_GPIO_TOTAL_PINS       (AL_GPIO_CONTROLLER_COUNT * AL_GPIO_PINS_PER_CTRL)

STATIC GPIO_CONTROLLER mGpioControllers[AL_GPIO_CONTROLLER_COUNT] = {
  { 0xFD887000, 0x00, AL_GPIO_PINS_PER_CTRL },  // gpio0
  { 0xFD888000, 0x08, AL_GPIO_PINS_PER_CTRL },  // gpio1
  { 0xFD889000, 0x10, AL_GPIO_PINS_PER_CTRL },  // gpio2
  { 0xFD88A000, 0x18, AL_GPIO_PINS_PER_CTRL },  // gpio3
  { 0xFD88B000, 0x20, AL_GPIO_PINS_PER_CTRL },  // gpio4
  { 0xFD897000, 0x28, AL_GPIO_PINS_PER_CTRL },  // gpio5
};

STATIC PLATFORM_GPIO_CONTROLLER mPlatformGpioController = {
  AL_GPIO_TOTAL_PINS,
  AL_GPIO_CONTROLLER_COUNT,
  mGpioControllers
};

EFI_STATUS
EFIAPI
PlatformGpioDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gPlatformGpioProtocolGuid,
                &mPlatformGpioController,
                NULL
                );
}
