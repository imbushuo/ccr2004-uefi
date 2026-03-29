// @file
//  NVRAM Setup formset GUID — shared between VFR and C code.
//
//  Copyright (c) 2024, MikroTik. All rights reserved.
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

#ifndef NVRAM_SETUP_GUID_H_
#define NVRAM_SETUP_GUID_H_

#define NVRAM_SETUP_FORMSET_GUID \
  { 0x8e1a3c4b, 0xd567, 0x4f92, { 0xa0, 0xb5, 0x7e, 0x2d, 0x8c, 0x3f, 0x1a, 0x97 } }

#ifndef EFI_HII_PLATFORM_SETUP_FORMSET_GUID
#define EFI_HII_PLATFORM_SETUP_FORMSET_GUID \
  { 0x93039971, 0x8545, 0x4b04, { 0xb4, 0x5e, 0x32, 0xeb, 0x83, 0x26, 0x04, 0x0e } }
#endif

// Action key for "Reset NVRAM" button
#define NVRAM_RESET_KEY  0x1001

#endif
