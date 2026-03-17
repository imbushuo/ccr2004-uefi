/** @file
  Board Info DXE driver for MikroTik CCR2004.

  Reads board-specific data (board name, serial number, MAC address) from the
  SPI NOR flash TLV block at offset 0xB0000 ("Hard" magic) and exposes it via
  the MIKROTIK_BOARD_INFO_PROTOCOL.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SpiNorFlash.h>
#include <Protocol/BoardInfo.h>

#define BOARD_INFO_FLASH_OFFSET  0xB0000
#define BOARD_INFO_READ_SIZE     4096
#define BOARD_INFO_MAGIC         0x64726148  /* "Hard" in little-endian */

#define RB_TAG_END          0x00
#define RB_TAG_BOARD_SERIAL 0x0B
#define RB_TAG_MAC          0x04
#define RB_TAG_BOARD_NAME   0x05

#define BOARD_NAME_MAX  64
#define SERIAL_MAX      32

#pragma pack(1)
typedef struct {
  UINT16  Tag;
  UINT16  Length;
} RB_TLV_HEADER;
#pragma pack()

typedef struct {
  MIKROTIK_BOARD_INFO_PROTOCOL  Protocol;
  CHAR8                         BoardName[BOARD_NAME_MAX];
  CHAR8                         Serial[SERIAL_MAX];
  EFI_MAC_ADDRESS               MacAddress;
} BOARD_INFO_INSTANCE;

STATIC BOARD_INFO_INSTANCE  mBoardInfo;

STATIC
EFI_STATUS
EFIAPI
BoardInfoGetBoardName (
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This,
  OUT CONST CHAR8                         **BoardName
  )
{
  BOARD_INFO_INSTANCE  *Instance;

  if (BoardName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = BASE_CR (This, BOARD_INFO_INSTANCE, Protocol);
  *BoardName = Instance->BoardName;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BoardInfoGetSerial (
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This,
  OUT CONST CHAR8                         **Serial
  )
{
  BOARD_INFO_INSTANCE  *Instance;

  if (Serial == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = BASE_CR (This, BOARD_INFO_INSTANCE, Protocol);
  *Serial = Instance->Serial;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BoardInfoGetMacAddress (
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This,
  OUT EFI_MAC_ADDRESS                     *MacAddress
  )
{
  BOARD_INFO_INSTANCE  *Instance;

  if (MacAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = BASE_CR (This, BOARD_INFO_INSTANCE, Protocol);
  CopyMem (MacAddress, &Instance->MacAddress, sizeof (EFI_MAC_ADDRESS));
  return EFI_SUCCESS;
}

STATIC
VOID
ParseTlvBlock (
  IN  UINT8               *FlashBuf,
  IN  UINTN               BufSize,
  OUT BOARD_INFO_INSTANCE  *Info
  )
{
  UINT8           *Pos;
  UINT8           *End;
  RB_TLV_HEADER   *Hdr;
  UINT8           *DataPtr;
  UINTN           CopyLen;

  if (BufSize < 4) {
    return;
  }

  /* Validate magic */
  if (*(UINT32 *)FlashBuf != BOARD_INFO_MAGIC) {
    DEBUG ((DEBUG_WARN, "[BoardInfo] Bad magic: 0x%08x (expected 0x%08x)\n",
            *(UINT32 *)FlashBuf, BOARD_INFO_MAGIC));
    return;
  }

  Pos = FlashBuf + 4;
  End = FlashBuf + BufSize;

  while (Pos + sizeof (RB_TLV_HEADER) <= End) {
    Hdr = (RB_TLV_HEADER *)Pos;

    if (Hdr->Tag == RB_TAG_END) {
      break;
    }

    DataPtr = Pos + sizeof (RB_TLV_HEADER);
    if (DataPtr + Hdr->Length > End) {
      DEBUG ((DEBUG_WARN, "[BoardInfo] TLV overflow at tag 0x%04x\n", Hdr->Tag));
      break;
    }

    switch (Hdr->Tag) {
      case RB_TAG_BOARD_NAME:
        CopyLen = Hdr->Length;
        if (CopyLen >= BOARD_NAME_MAX) {
          CopyLen = BOARD_NAME_MAX - 1;
        }
        CopyMem (Info->BoardName, DataPtr, CopyLen);
        Info->BoardName[CopyLen] = '\0';
        DEBUG ((DEBUG_INFO, "[BoardInfo] Board name: %a\n", Info->BoardName));
        break;

      case RB_TAG_BOARD_SERIAL:
        CopyLen = Hdr->Length;
        if (CopyLen >= SERIAL_MAX) {
          CopyLen = SERIAL_MAX - 1;
        }
        CopyMem (Info->Serial, DataPtr, CopyLen);
        Info->Serial[CopyLen] = '\0';
        DEBUG ((DEBUG_INFO, "[BoardInfo] Serial: %a\n", Info->Serial));
        break;

      case RB_TAG_MAC:
        if (Hdr->Length >= 6) {
          CopyMem (&Info->MacAddress, DataPtr, 6);
          DEBUG ((DEBUG_INFO, "[BoardInfo] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                  Info->MacAddress.Addr[0], Info->MacAddress.Addr[1],
                  Info->MacAddress.Addr[2], Info->MacAddress.Addr[3],
                  Info->MacAddress.Addr[4], Info->MacAddress.Addr[5]));
        }
        break;

      default:
        break;
    }

    Pos = DataPtr + Hdr->Length;
  }
}

EFI_STATUS
EFIAPI
BoardInfoDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                   Status;
  EFI_SPI_NOR_FLASH_PROTOCOL   *SpiNor;
  UINT8                        *FlashBuf;
  EFI_HANDLE                   Handle;

  DEBUG ((DEBUG_INFO, "[BoardInfo] Driver loading\n"));

  /* Set defaults */
  ZeroMem (&mBoardInfo, sizeof (mBoardInfo));
  AsciiStrCpyS (mBoardInfo.BoardName, BOARD_NAME_MAX, "RouterBoard");
  AsciiStrCpyS (mBoardInfo.Serial, SERIAL_MAX, "Unknown");
  mBoardInfo.MacAddress.Addr[0] = 0x02;
  mBoardInfo.MacAddress.Addr[5] = 0x01;

  /* Locate SPI NOR flash protocol */
  Status = gBS->LocateProtocol (&gEfiSpiNorFlashProtocolGuid, NULL, (VOID **)&SpiNor);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[BoardInfo] SPI NOR flash not found: %r (using defaults)\n", Status));
    goto InstallProtocol;
  }

  /* Read TLV block from flash */
  FlashBuf = AllocatePool (BOARD_INFO_READ_SIZE);
  if (FlashBuf == NULL) {
    DEBUG ((DEBUG_WARN, "[BoardInfo] Failed to allocate read buffer (using defaults)\n"));
    goto InstallProtocol;
  }

  Status = SpiNor->ReadData (SpiNor, BOARD_INFO_FLASH_OFFSET, BOARD_INFO_READ_SIZE, FlashBuf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[BoardInfo] Flash read failed: %r (using defaults)\n", Status));
    FreePool (FlashBuf);
    goto InstallProtocol;
  }

  /* Parse TLV data */
  ParseTlvBlock (FlashBuf, BOARD_INFO_READ_SIZE, &mBoardInfo);
  FreePool (FlashBuf);

InstallProtocol:
  /* Wire protocol methods */
  mBoardInfo.Protocol.GetBoardName  = BoardInfoGetBoardName;
  mBoardInfo.Protocol.GetSerial     = BoardInfoGetSerial;
  mBoardInfo.Protocol.GetMacAddress = BoardInfoGetMacAddress;

  /* Install protocol */
  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gMikroTikBoardInfoProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mBoardInfo.Protocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[BoardInfo] Failed to install protocol: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "[BoardInfo] Protocol installed (board=%a serial=%a mac=%02x:%02x:%02x:%02x:%02x:%02x)\n",
          mBoardInfo.BoardName, mBoardInfo.Serial,
          mBoardInfo.MacAddress.Addr[0], mBoardInfo.MacAddress.Addr[1],
          mBoardInfo.MacAddress.Addr[2], mBoardInfo.MacAddress.Addr[3],
          mBoardInfo.MacAddress.Addr[4], mBoardInfo.MacAddress.Addr[5]));

  return EFI_SUCCESS;
}
