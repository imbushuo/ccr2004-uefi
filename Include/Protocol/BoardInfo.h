/** @file
  MikroTik Board Info Protocol definition.

  Provides board-specific data (board name, serial number, MAC address)
  parsed from the SPI flash TLV block at offset 0xB0000.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MIKROTIK_BOARD_INFO_PROTOCOL_H_
#define MIKROTIK_BOARD_INFO_PROTOCOL_H_

#include <Uefi.h>
#include <Protocol/SimpleNetwork.h>

#define MIKROTIK_BOARD_INFO_PROTOCOL_GUID \
  { 0xb3a1d5e7, 0x4c2f, 0x4a89, { 0x91, 0x3e, 0x7d, 0x5b, 0x8f, 0x2c, 0x6a, 0x01 } }

typedef struct _MIKROTIK_BOARD_INFO_PROTOCOL MIKROTIK_BOARD_INFO_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *MIKROTIK_BOARD_INFO_GET_BOARD_NAME)(
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This,
  OUT CONST CHAR8                         **BoardName
  );

typedef
EFI_STATUS
(EFIAPI *MIKROTIK_BOARD_INFO_GET_SERIAL)(
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This,
  OUT CONST CHAR8                         **Serial
  );

typedef
EFI_STATUS
(EFIAPI *MIKROTIK_BOARD_INFO_GET_MAC_ADDRESS)(
  IN  CONST MIKROTIK_BOARD_INFO_PROTOCOL  *This,
  OUT EFI_MAC_ADDRESS                     *MacAddress
  );

struct _MIKROTIK_BOARD_INFO_PROTOCOL {
  MIKROTIK_BOARD_INFO_GET_BOARD_NAME   GetBoardName;
  MIKROTIK_BOARD_INFO_GET_SERIAL       GetSerial;
  MIKROTIK_BOARD_INFO_GET_MAC_ADDRESS  GetMacAddress;
};

extern EFI_GUID  gMikroTikBoardInfoProtocolGuid;

#endif /* MIKROTIK_BOARD_INFO_PROTOCOL_H_ */
