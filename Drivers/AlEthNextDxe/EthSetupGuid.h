#ifndef ETH_SETUP_GUID_H_
#define ETH_SETUP_GUID_H_

#define ETH_SETUP_FORMSET_GUID \
  { 0x2c7e4a1b, 0xf389, 0x4d56, { 0xa0, 0xb2, 0x3e, 0x8c, 0x7d, 0x1f, 0x5a, 0x93 } }

#ifndef EFI_HII_PLATFORM_SETUP_FORMSET_GUID
#define EFI_HII_PLATFORM_SETUP_FORMSET_GUID \
  { 0x93039971, 0x8545, 0x4b04, { 0xb4, 0x5e, 0x32, 0xeb, 0x83, 0x26, 0x04, 0x0e } }
#endif

#define ETH_SETUP_VARSTORE_ID  0x0100
#define ETH_ENABLE_KEY         0x2001

#pragma pack(1)
typedef struct {
  UINT8  EnableNetworking;  // 0 = disabled, 1 = enabled (default)
} ETH_SETUP_CONFIG;
#pragma pack()

#define ETH_SETUP_VARIABLE_NAME  L"EthSetup"

#endif
