/** @file
  Board Info DXE driver for MikroTik CCR2004.

  Reads board-specific data (board name, serial number, MAC address) from the
  SPI NOR flash TLV block at offset 0xB0000 ("Hard" magic) and exposes it via
  the MIKROTIK_BOARD_INFO_PROTOCOL.

  After parsing, populates the Alpine V2 PBS SRAM general shared data struct
  with per-port MAC addresses derived from the board MAC.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SpiNorFlash.h>
#include <Protocol/BoardInfo.h>

#include "al_general_shared_data.h"

#define BOARD_INFO_FLASH_OFFSET  0xB0000
#define BOARD_INFO_READ_SIZE     4096
#define BOARD_INFO_MAGIC         0x64726148  /* "Hard" in little-endian */

#define RB_TAG_END          0x00
#define RB_TAG_BOARD_SERIAL 0x0B
#define RB_TAG_MAC          0x04
#define RB_TAG_BOARD_NAME   0x05

#define BOARD_NAME_MAX  64
#define SERIAL_MAX      32

/*
 * Alpine V2 PBS SRAM shared data address:
 *   AL_PBS_SRAM_BASE = 0xFD8A4000
 *   + SRAM_GENERAL_SHARED_DATA_OFFSET = 0x180
 */
#define SRAM_SHARED_DATA_ADDR  0xFD8A4180ULL

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
  BOOLEAN                       ChainBootFromRouterBoot;
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
  if (BoardName == NULL) return EFI_INVALID_PARAMETER;
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
  if (Serial == NULL) return EFI_INVALID_PARAMETER;
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
  if (MacAddress == NULL) return EFI_INVALID_PARAMETER;
  Instance = BASE_CR (This, BOARD_INFO_INSTANCE, Protocol);
  CopyMem (MacAddress, &Instance->MacAddress, sizeof (EFI_MAC_ADDRESS));
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
BoardInfoIsChainBoot (
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This
  )
{
  BOARD_INFO_INSTANCE  *Instance;

  Instance = BASE_CR (This, BOARD_INFO_INSTANCE, Protocol);
  return Instance->ChainBootFromRouterBoot;
}

STATIC
VOID
ParseTlvBlock (
  IN  UINT8               *FlashBuf,
  IN  UINTN               BufSize,
  OUT BOARD_INFO_INSTANCE  *Info
  )
{
  UINT8          *Pos, *End, *DataPtr;
  RB_TLV_HEADER  *Hdr;
  UINTN          CopyLen;

  if (BufSize < 4) return;
  if (*(UINT32 *)FlashBuf != BOARD_INFO_MAGIC) {
    DEBUG ((DEBUG_WARN, "[BoardInfo] Bad magic: 0x%08x\n", *(UINT32 *)FlashBuf));
    return;
  }

  Pos = FlashBuf + 4;
  End = FlashBuf + BufSize;

  while (Pos + sizeof (RB_TLV_HEADER) <= End) {
    Hdr = (RB_TLV_HEADER *)Pos;
    if (Hdr->Tag == RB_TAG_END) break;

    DataPtr = Pos + sizeof (RB_TLV_HEADER);
    if (DataPtr + Hdr->Length > End) break;

    switch (Hdr->Tag) {
      case RB_TAG_BOARD_NAME:
        CopyLen = Hdr->Length;
        if (CopyLen >= BOARD_NAME_MAX) CopyLen = BOARD_NAME_MAX - 1;
        CopyMem (Info->BoardName, DataPtr, CopyLen);
        Info->BoardName[CopyLen] = '\0';
        break;
      case RB_TAG_BOARD_SERIAL:
        CopyLen = Hdr->Length;
        if (CopyLen >= SERIAL_MAX) CopyLen = SERIAL_MAX - 1;
        CopyMem (Info->Serial, DataPtr, CopyLen);
        Info->Serial[CopyLen] = '\0';
        break;
      case RB_TAG_MAC:
        if (Hdr->Length >= 6) {
          CopyMem (&Info->MacAddress, DataPtr, 6);
        }
        break;
      default:
        break;
    }

    Pos = DataPtr + Hdr->Length;
  }
}

/**
  Populate MAC addresses into PBS SRAM shared data using HAL API.

  The MAC from SPI flash TLV is the eth1 (port1, RGMII) address.
    eth0 (slot 0) = last byte - 1
    eth1 (slot 1) = as-is from flash
    eth2 (slot 2) = last byte + 1
    eth3 (slot 3) = last byte + 2
**/
STATIC
VOID
PopulateSramMacAddresses (
  IN EFI_MAC_ADDRESS  *Eth1Mac
  )
{
  struct al_general_shared_data  *Sd;
  UINT8   Mac[AL_GENERAL_SHARED_MAC_ADDR_LEN];
  INT8    Offsets[AL_GENERAL_SHARED_MAC_ADDR_NUM] = { -1, 0, 1, 2 };
  UINTN   Slot;

  Sd = (struct al_general_shared_data *)(UINTN)SRAM_SHARED_DATA_ADDR;

  /* Initialize the shared data struct (sets magic, zeros everything) */
  al_general_shared_data_init (Sd);

  for (Slot = 0; Slot < AL_GENERAL_SHARED_MAC_ADDR_NUM; Slot++) {
    CopyMem (Mac, Eth1Mac->Addr, AL_GENERAL_SHARED_MAC_ADDR_LEN);
    Mac[5] = (UINT8)((INT16)Mac[5] + Offsets[Slot]);

    al_general_shared_data_mac_addr_set (Sd, Mac, (unsigned int)Slot);

    DEBUG ((DEBUG_INFO, "[BoardInfo] SRAM MAC[%u]: %02x:%02x:%02x:%02x:%02x:%02x\n",
            Slot, Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]));
  }
}

EFI_STATUS
EFIAPI
BoardInfoDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                  Status;
  EFI_SPI_NOR_FLASH_PROTOCOL  *SpiNor;
  UINT8                       *FlashBuf;
  EFI_HANDLE                  Handle;

  DEBUG ((DEBUG_WARN, "[BoardInfo] Driver loading\n"));

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
    goto InstallProtocol;
  }

  Status = SpiNor->ReadData (SpiNor, BOARD_INFO_FLASH_OFFSET, BOARD_INFO_READ_SIZE, FlashBuf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[BoardInfo] Flash read failed: %r (using defaults)\n", Status));
    FreePool (FlashBuf);
    goto InstallProtocol;
  }

  ParseTlvBlock (FlashBuf, BOARD_INFO_READ_SIZE, &mBoardInfo);
  FreePool (FlashBuf);

  /* Populate MAC addresses into PBS SRAM shared data */
  PopulateSramMacAddresses (&mBoardInfo.MacAddress);

  //
  // Detect RouterBoot chainboot: check for the RouterBootChainBoot variable.
  // If present, set the flag and delete the variable (one-shot signal).
  //
  {
    UINT8     ChainBootVal;
    UINTN     ChainBootSize = sizeof (ChainBootVal);
    EFI_GUID  ChainBootGuid = ROUTERBOOT_CHAINBOOT_VARIABLE_GUID;

    Status = gRT->GetVariable (
                    ROUTERBOOT_CHAINBOOT_VARIABLE_NAME,
                    &ChainBootGuid,
                    NULL,
                    &ChainBootSize,
                    &ChainBootVal
                    );
    if (!EFI_ERROR (Status) && ChainBootVal != 0) {
      mBoardInfo.ChainBootFromRouterBoot = TRUE;
      DEBUG ((DEBUG_WARN, "[BoardInfo] RouterBoot chainboot detected (var value=%u)\n", ChainBootVal));
    } else {
      mBoardInfo.ChainBootFromRouterBoot = FALSE;
      DEBUG ((DEBUG_WARN, "[BoardInfo] Normal boot (no RouterBoot chainboot)\n"));
    }

    //
    // Always delete the variable to prevent stale chainboot signals.
    // Ignore errors if it didn't exist.
    //
    gRT->SetVariable (
           ROUTERBOOT_CHAINBOOT_VARIABLE_NAME,
           &ChainBootGuid,
           0,
           0,
           NULL
           );
  }

InstallProtocol:
  mBoardInfo.Protocol.GetBoardName            = BoardInfoGetBoardName;
  mBoardInfo.Protocol.GetSerial               = BoardInfoGetSerial;
  mBoardInfo.Protocol.GetMacAddress           = BoardInfoGetMacAddress;
  mBoardInfo.Protocol.IsChainBootFromRouterBoot = BoardInfoIsChainBoot;

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
