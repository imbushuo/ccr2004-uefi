/** @file
  Static SMBIOS tables for MikroTik CCR2004-1G-2XS-PCIe.

  Annapurna Labs Alpine V2 (AL52400), 4x Cortex-A72 @ 1.5 GHz,
  4 GB DDR4 ECC, 1x 1GbE RJ45, 2x 25GbE SFP28, PCIe card form factor.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/SmBios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Smbios.h>
#include <Protocol/BoardInfo.h>

//
// Cross-reference handles
//
enum {
  SMBIOS_HANDLE_L1I = 0x1000,
  SMBIOS_HANDLE_L1D,
  SMBIOS_HANDLE_L2,
  SMBIOS_HANDLE_MOTHERBOARD,
  SMBIOS_HANDLE_CHASSIS,
  SMBIOS_HANDLE_PROCESSOR,
  SMBIOS_HANDLE_MEMORY,
  SMBIOS_HANDLE_DIMM
};

//
// String tables (each string is index 1, 2, 3 ... in order)
//

#define TYPE0_STRINGS                                    \
  "MikroTik\0"                    /* 1 Vendor */         \
  "0.1\0"                         /* 2 BiosVersion */    \
  __DATE__"\0"                    /* 3 BiosReleaseDate */

#define TYPE1_STRINGS                                    \
  "MikroTik\0"                    /* 1 Manufacturer */   \
  "CCR2004-1G-2XS-PCIe\0"        /* 2 ProductName */    \
  "1.0\0"                         /* 3 Version */        \
  "Not Set\0"                     /* 4 SerialNumber */   \
  "CCR2004-1G-2XS-PCIe\0"        /* 5 SKUNumber */      \
  "Cloud Core Router\0"           /* 6 Family */

#define TYPE2_STRINGS                                    \
  "MikroTik\0"                    /* 1 Manufacturer */   \
  "CCR2004-1G-2XS-PCIe\0"        /* 2 ProductName */    \
  "1.0\0"                         /* 3 Version */        \
  "Not Set\0"                     /* 4 SerialNumber */   \
  "Not Set\0"                     /* 5 AssetTag */       \
  "PCIe Slot\0"                   /* 6 ChassisLocation */

#define TYPE3_STRINGS                                    \
  "MikroTik\0"                    /* 1 Manufacturer */   \
  "1.0\0"                         /* 2 Version */        \
  "Not Set\0"                     /* 3 SerialNumber */   \
  "Not Set\0"                     /* 4 AssetTag */

#define TYPE4_STRINGS                                    \
  "SoC\0"                         /* 1 Socket */         \
  "Annapurna Labs\0"              /* 2 Manufacturer */   \
  "Alpine V2 AL52400\0"           /* 3 ProcessorVersion */ \
  "Not Set\0"                     /* 4 SerialNumber */   \
  "Not Set\0"                     /* 5 AssetTag */       \
  "AL52400\0"                     /* 6 PartNumber */

#define TYPE7_L1I_STRINGS   "L1 Instruction Cache\0"
#define TYPE7_L1D_STRINGS   "L1 Data Cache\0"
#define TYPE7_L2_STRINGS    "L2 Cache\0"

#define TYPE8_ETH_STRINGS                                \
  "Ethernet 1GbE\0"              /* 1 InternalRef */     \
  "1GbE RJ45\0"                  /* 2 ExternalRef */

#define TYPE8_SFP1_STRINGS                               \
  "SFP28 Port 1\0"               /* 1 InternalRef */     \
  "25GbE SFP28\0"                /* 2 ExternalRef */

#define TYPE8_SFP2_STRINGS                               \
  "SFP28 Port 2\0"               /* 1 InternalRef */     \
  "25GbE SFP28\0"                /* 2 ExternalRef */

#define TYPE16_STRINGS  "\0"
#define TYPE17_STRINGS                                   \
  "Onboard\0"                    /* 1 DeviceLocator */   \
  "Bank 0\0"                     /* 2 BankLocator */     \
  "Not Set\0"                    /* 3 Manufacturer */    \
  "Not Set\0"                    /* 4 SerialNumber */    \
  "Not Set\0"                    /* 5 AssetTag */        \
  "Not Set\0"                    /* 6 PartNumber */

#define TYPE19_STRINGS  "\0"
#define TYPE32_STRINGS  "\0"

//
// Packed structure types
//
#pragma pack(1)

typedef struct { SMBIOS_TABLE_TYPE0  Base; UINT8 Strings[sizeof (TYPE0_STRINGS)]; } CCR_TYPE0;
typedef struct { SMBIOS_TABLE_TYPE1  Base; UINT8 Strings[sizeof (TYPE1_STRINGS)]; } CCR_TYPE1;
typedef struct { SMBIOS_TABLE_TYPE2  Base; UINT8 Strings[sizeof (TYPE2_STRINGS)]; } CCR_TYPE2;
typedef struct { SMBIOS_TABLE_TYPE3  Base; UINT8 Strings[sizeof (TYPE3_STRINGS)]; } CCR_TYPE3;
typedef struct { SMBIOS_TABLE_TYPE4  Base; UINT8 Strings[sizeof (TYPE4_STRINGS)]; } CCR_TYPE4;
typedef struct { SMBIOS_TABLE_TYPE7  Base; UINT8 Strings[sizeof (TYPE7_L1I_STRINGS)]; } CCR_TYPE7_L1I;
typedef struct { SMBIOS_TABLE_TYPE7  Base; UINT8 Strings[sizeof (TYPE7_L1D_STRINGS)]; } CCR_TYPE7_L1D;
typedef struct { SMBIOS_TABLE_TYPE7  Base; UINT8 Strings[sizeof (TYPE7_L2_STRINGS)]; } CCR_TYPE7_L2;
typedef struct { SMBIOS_TABLE_TYPE8  Base; UINT8 Strings[sizeof (TYPE8_ETH_STRINGS)]; } CCR_TYPE8_ETH;
typedef struct { SMBIOS_TABLE_TYPE8  Base; UINT8 Strings[sizeof (TYPE8_SFP1_STRINGS)]; } CCR_TYPE8_SFP1;
typedef struct { SMBIOS_TABLE_TYPE8  Base; UINT8 Strings[sizeof (TYPE8_SFP2_STRINGS)]; } CCR_TYPE8_SFP2;
typedef struct { SMBIOS_TABLE_TYPE16 Base; UINT8 Strings[sizeof (TYPE16_STRINGS)]; } CCR_TYPE16;
typedef struct { SMBIOS_TABLE_TYPE17 Base; UINT8 Strings[sizeof (TYPE17_STRINGS)]; } CCR_TYPE17;
typedef struct { SMBIOS_TABLE_TYPE19 Base; UINT8 Strings[sizeof (TYPE19_STRINGS)]; } CCR_TYPE19;
typedef struct { SMBIOS_TABLE_TYPE32 Base; UINT8 Strings[sizeof (TYPE32_STRINGS)]; } CCR_TYPE32;

#pragma pack()

//
// BIOS Information (Type 0)
//
CCR_TYPE0 mType0 = {
  {
    {
      EFI_SMBIOS_TYPE_BIOS_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE0),
      SMBIOS_HANDLE_PI_RESERVED
    },
    1,                                           // Vendor
    2,                                           // BiosVersion
    0,                                           // BiosSegment (N/A for ARM)
    3,                                           // BiosReleaseDate
    0,                                           // BiosSize (N/A)
    {                                            // BiosCharacteristics
      0, 0, 0, 0, 0, 0, 0,
      1,                                         // PciIsSupported
      0, 0, 0,
      1,                                         // BiosIsUpgradable
      0, 0, 0, 0,
      1,                                         // SelectableBootIsSupported
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    { 0x01, 0x0C },                              // ACPI not supported, UEFI spec supported
    0,                                           // SystemBiosMajorRelease
    1,                                           // SystemBiosMinorRelease
    0xFF,                                        // EmbeddedControllerFirmwareMajorRelease
    0xFF                                         // EmbeddedControllerFirmwareMinorRelease
  },
  TYPE0_STRINGS
};

//
// System Information (Type 1)
//
CCR_TYPE1 mType1 = {
  {
    {
      EFI_SMBIOS_TYPE_SYSTEM_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE1),
      SMBIOS_HANDLE_PI_RESERVED
    },
    1,                                           // Manufacturer
    2,                                           // ProductName
    3,                                           // Version
    4,                                           // SerialNumber
    { 0xCC200400, 0x1A2B, 0x3C4D, { 0x4D, 0x69, 0x6B, 0x72, 0x6F, 0x54, 0x69, 0x6B } },
    SystemWakeupTypePowerSwitch,
    5,                                           // SKUNumber
    6                                            // Family
  },
  TYPE1_STRINGS
};

//
// Baseboard Information (Type 2)
//
CCR_TYPE2 mType2 = {
  {
    {
      EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE2),
      SMBIOS_HANDLE_MOTHERBOARD
    },
    1,                                           // Manufacturer
    2,                                           // ProductName
    3,                                           // Version
    4,                                           // SerialNumber
    5,                                           // AssetTag
    {                                            // FeatureFlag
      1,                                         // Motherboard
      0, 0, 0, 0, 0
    },
    6,                                           // LocationInChassis
    SMBIOS_HANDLE_CHASSIS,                       // ChassisHandle
    BaseBoardTypeProcessorMemoryModule,          // BoardType (add-in card)
    0,                                           // NumberOfContainedObjectHandles
    { 0 }
  },
  TYPE2_STRINGS
};

//
// System Enclosure (Type 3) - PCIe add-in card
//
CCR_TYPE3 mType3 = {
  {
    {
      EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE,
      sizeof (SMBIOS_TABLE_TYPE3),
      SMBIOS_HANDLE_CHASSIS
    },
    1,                                           // Manufacturer
    MiscChassisTypeOther,                        // PCIe card, no standard chassis type
    2,                                           // Version
    3,                                           // SerialNumber
    4,                                           // AssetTag
    ChassisStateSafe,                            // BootupState
    ChassisStateSafe,                            // PowerSupplyState
    ChassisStateSafe,                            // ThermalState
    ChassisSecurityStatusNone,                   // SecurityStatus
    { 0, 0, 0, 0 },                             // OemDefined
    0,                                           // Height (N/A)
    0,                                           // NumberofPowerCords
    0,                                           // ContainedElementCount
    0,                                           // ContainedElementRecordLength
    { { 0, 0, 0 } }
  },
  TYPE3_STRINGS
};

//
// Processor Information (Type 4) - 4x Cortex-A72 @ 1.5 GHz
//
CCR_TYPE4 mType4 = {
  {
    {
      EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE4),
      SMBIOS_HANDLE_PROCESSOR
    },
    1,                                           // Socket
    CentralProcessor,                            // ProcessorType
    ProcessorFamilyIndicatorFamily2,             // ProcessorFamily
    2,                                           // ProcessorManufacturer
    { 0, 0, 0, 0 },                               // ProcessorId
    3,                                           // ProcessorVersion
    { 0, 0, 0, 0, 0, 0 },                       // Voltage
    0,                                           // ExternalClock (unknown)
    1500,                                        // MaxSpeed (MHz)
    1500,                                        // CurrentSpeed (MHz)
    0x41,                                        // Status (populated, enabled)
    ProcessorUpgradeNone,                        // ProcessorUpgrade (soldered)
    SMBIOS_HANDLE_L1D,                           // L1CacheHandle
    SMBIOS_HANDLE_L1I,                           // L2CacheHandle (actually L1I, see note)
    SMBIOS_HANDLE_L2,                            // L3CacheHandle (actually L2)
    4,                                           // SerialNumber
    5,                                           // AssetTag
    6,                                           // PartNumber
    4,                                           // CoreCount
    4,                                           // EnabledCoreCount
    4,                                           // ThreadCount (no SMT on A72)
    0x00EC,                                      // ProcessorCharacteristics (64-bit, multi-core, ARM)
    ProcessorFamilyARMv8,                        // ProcessorFamily2
    4,                                           // CoreCount2
    4,                                           // EnabledCoreCount2
    4                                            // ThreadCount2
  },
  TYPE4_STRINGS
};

//
// Cache Information (Type 7) - L1 Instruction, 48 KB per core, 3-way
//
CCR_TYPE7_L1I mType7L1I = {
  {
    {
      EFI_SMBIOS_TYPE_CACHE_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE7),
      SMBIOS_HANDLE_L1I
    },
    1,                                           // SocketDesignation
    0x0180,                                      // CacheConfiguration: enabled, write-back, L1, internal
    { 48, 0 },                                   // MaximumCacheSize (48 KB)
    { 48, 0 },                                   // InstalledSize (48 KB)
    { 0, 0, 0, 0, 0, 1, 0, 0 },                 // SupportedSRAMType (synchronous)
    { 0, 0, 0, 0, 0, 1, 0, 0 },                 // CurrentSRAMType (synchronous)
    0,                                           // CacheSpeed
    CacheErrorParity,                            // ErrorCorrectionType
    CacheTypeInstruction,                        // SystemCacheType
    CacheAssociativity4Way                       // Associativity (A72 L1I is 3-way, closest is 4)
  },
  TYPE7_L1I_STRINGS
};

//
// Cache Information (Type 7) - L1 Data, 32 KB per core, 2-way
//
CCR_TYPE7_L1D mType7L1D = {
  {
    {
      EFI_SMBIOS_TYPE_CACHE_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE7),
      SMBIOS_HANDLE_L1D
    },
    1,                                           // SocketDesignation
    0x0180,                                      // CacheConfiguration: enabled, write-back, L1, internal
    { 32, 0 },                                   // MaximumCacheSize (32 KB)
    { 32, 0 },                                   // InstalledSize (32 KB)
    { 0, 0, 0, 0, 0, 1, 0, 0 },                 // SupportedSRAMType (synchronous)
    { 0, 0, 0, 0, 0, 1, 0, 0 },                 // CurrentSRAMType (synchronous)
    0,                                           // CacheSpeed
    CacheErrorSingleBit,                         // ErrorCorrectionType
    CacheTypeData,                               // SystemCacheType
    CacheAssociativity2Way                       // Associativity
  },
  TYPE7_L1D_STRINGS
};

//
// Cache Information (Type 7) - L2 Unified, 1 MB shared
//
CCR_TYPE7_L2 mType7L2 = {
  {
    {
      EFI_SMBIOS_TYPE_CACHE_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE7),
      SMBIOS_HANDLE_L2
    },
    1,                                           // SocketDesignation
    0x0281,                                      // CacheConfiguration: enabled, write-back, L2, internal
    { 1024, 0 },                                 // MaximumCacheSize (1024 KB = 1 MB)
    { 1024, 0 },                                 // InstalledSize (1024 KB)
    { 0, 0, 0, 0, 0, 1, 0, 0 },                 // SupportedSRAMType (synchronous)
    { 0, 0, 0, 0, 0, 1, 0, 0 },                 // CurrentSRAMType (synchronous)
    0,                                           // CacheSpeed
    CacheErrorSingleBit,                         // ErrorCorrectionType
    CacheTypeUnified,                            // SystemCacheType
    CacheAssociativity16Way                      // Associativity
  },
  TYPE7_L2_STRINGS
};

//
// Port Connector Information (Type 8) - 1GbE RJ45
//
CCR_TYPE8_ETH mType8Eth = {
  {
    {
      EFI_SMBIOS_TYPE_PORT_CONNECTOR_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE8),
      SMBIOS_HANDLE_PI_RESERVED
    },
    1,                                           // InternalReferenceDesignator
    PortConnectorTypeNone,                       // InternalConnectorType
    2,                                           // ExternalReferenceDesignator
    PortConnectorTypeRJ45,                       // ExternalConnectorType
    PortTypeNetworkPort                          // PortType
  },
  TYPE8_ETH_STRINGS
};

//
// Port Connector Information (Type 8) - SFP28 Port 1
//
CCR_TYPE8_SFP1 mType8Sfp1 = {
  {
    {
      EFI_SMBIOS_TYPE_PORT_CONNECTOR_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE8),
      SMBIOS_HANDLE_PI_RESERVED
    },
    1,                                           // InternalReferenceDesignator
    PortConnectorTypeNone,                       // InternalConnectorType
    2,                                           // ExternalReferenceDesignator
    PortConnectorTypeOther,                      // ExternalConnectorType (SFP28)
    PortTypeNetworkPort                          // PortType
  },
  TYPE8_SFP1_STRINGS
};

//
// Port Connector Information (Type 8) - SFP28 Port 2
//
CCR_TYPE8_SFP2 mType8Sfp2 = {
  {
    {
      EFI_SMBIOS_TYPE_PORT_CONNECTOR_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE8),
      SMBIOS_HANDLE_PI_RESERVED
    },
    1,                                           // InternalReferenceDesignator
    PortConnectorTypeNone,                       // InternalConnectorType
    2,                                           // ExternalReferenceDesignator
    PortConnectorTypeOther,                      // ExternalConnectorType (SFP28)
    PortTypeNetworkPort                          // PortType
  },
  TYPE8_SFP2_STRINGS
};

//
// Physical Memory Array (Type 16) - 4 GB DDR4 ECC onboard
//
CCR_TYPE16 mType16 = {
  {
    {
      EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY,
      sizeof (SMBIOS_TABLE_TYPE16),
      SMBIOS_HANDLE_MEMORY
    },
    MemoryArrayLocationSystemBoard,              // Location
    MemoryArrayUseSystemMemory,                  // Use
    MemoryErrorCorrectionSingleBitEcc,           // ECC enabled
    0x400000,                                    // MaximumCapacity (4 GB in KB)
    0xFFFE,                                      // MemoryErrorInformationHandle
    1                                            // NumberOfMemoryDevices
  },
  TYPE16_STRINGS
};

//
// Memory Device (Type 17) - 4 GB DDR4 ECC onboard
//
CCR_TYPE17 mType17 = {
  {
    {
      EFI_SMBIOS_TYPE_MEMORY_DEVICE,
      sizeof (SMBIOS_TABLE_TYPE17),
      SMBIOS_HANDLE_DIMM
    },
    SMBIOS_HANDLE_MEMORY,                        // MemoryArrayHandle
    0xFFFE,                                      // MemoryErrorInformationHandle
    72,                                          // TotalWidth (64 data + 8 ECC)
    64,                                          // DataWidth
    0x1000,                                      // Size (4096 MB = 4 GB)
    MemoryFormFactorChip,                        // FormFactor (soldered on board)
    0,                                           // DeviceSet
    1,                                           // DeviceLocator
    2,                                           // BankLocator
    MemoryTypeDdr4,                              // MemoryType
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // TypeDetail
    2400,                                        // Speed (MHz)
    3,                                           // Manufacturer
    4,                                           // SerialNumber
    5,                                           // AssetTag
    6,                                           // PartNumber
    0,                                           // Attributes
    0x1000,                                      // ExtendedSize (4 GB)
    2400,                                        // ConfiguredMemoryClockSpeed
    1200,                                        // MinimumVoltage (mV)
    1200,                                        // MaximumVoltage (mV)
    1200                                         // ConfiguredVoltage (mV)
  },
  TYPE17_STRINGS
};

//
// Memory Array Mapped Address (Type 19) - 0x00000000 to 0xFFFFFFFF (4 GB)
//
CCR_TYPE19 mType19 = {
  {
    {
      EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS,
      sizeof (SMBIOS_TABLE_TYPE19),
      SMBIOS_HANDLE_PI_RESERVED
    },
    0x00000000,                                  // StartingAddress (KB)
    0x003FFFFF,                                  // EndingAddress (4 GB - 1 in KB)
    SMBIOS_HANDLE_MEMORY,                        // MemoryArrayHandle
    1,                                           // PartitionWidth
    0,                                           // ExtendedStartingAddress
    0                                            // ExtendedEndingAddress
  },
  TYPE19_STRINGS
};

//
// System Boot Information (Type 32)
//
CCR_TYPE32 mType32 = {
  {
    {
      EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION,
      sizeof (SMBIOS_TABLE_TYPE32),
      SMBIOS_HANDLE_PI_RESERVED
    },
    { 0, 0, 0, 0, 0, 0 },                       // Reserved
    BootInformationStatusNoError
  },
  TYPE32_STRINGS
};

//
// Table list
//
STATIC VOID *mSmbiosTables[] = {
  &mType0,
  &mType1,
  &mType2,
  &mType3,
  &mType4,
  &mType7L1I,
  &mType7L1D,
  &mType7L2,
  &mType8Eth,
  &mType8Sfp1,
  &mType8Sfp2,
  &mType16,
  &mType17,
  &mType19,
  &mType32,
  NULL
};

EFI_STATUS
EFIAPI
SmbiosPlatformDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                      Status;
  EFI_SMBIOS_PROTOCOL             *Smbios;
  EFI_SMBIOS_HANDLE               SmbiosHandle;
  UINTN                           Index;
  EFI_SMBIOS_HANDLE               Type1Handle;
  EFI_SMBIOS_HANDLE               Type2Handle;
  MIKROTIK_BOARD_INFO_PROTOCOL    *BoardInfo;
  CONST CHAR8                     *Name;
  CONST CHAR8                     *Serial;
  UINTN                           StringNum;
  CHAR8                           NameBuf[64];
  CHAR8                           SerialBuf[32];

  Type1Handle = SMBIOS_HANDLE_PI_RESERVED;
  Type2Handle = SMBIOS_HANDLE_PI_RESERVED;

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; mSmbiosTables[Index] != NULL; Index++) {
    SmbiosHandle = ((EFI_SMBIOS_TABLE_HEADER *)mSmbiosTables[Index])->Handle;
    Status = Smbios->Add (Smbios, NULL, &SmbiosHandle, (EFI_SMBIOS_TABLE_HEADER *)mSmbiosTables[Index]);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SMBIOS: Failed to add table index %d: %r\n", Index, Status));
      break;
    }

    /* Save handles for Type1 and Type2 to update strings later */
    if (((EFI_SMBIOS_TABLE_HEADER *)mSmbiosTables[Index])->Type == EFI_SMBIOS_TYPE_SYSTEM_INFORMATION) {
      Type1Handle = SmbiosHandle;
    } else if (((EFI_SMBIOS_TABLE_HEADER *)mSmbiosTables[Index])->Type == EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION) {
      Type2Handle = SmbiosHandle;
    }
  }

  /* Update SMBIOS strings from board info */
  Status = gBS->LocateProtocol (&gMikroTikBoardInfoProtocolGuid, NULL, (VOID **)&BoardInfo);
  if (!EFI_ERROR (Status)) {
    BoardInfo->GetBoardName (BoardInfo, &Name);
    BoardInfo->GetSerial (BoardInfo, &Serial);
    AsciiStrCpyS (NameBuf, sizeof (NameBuf), Name);
    AsciiStrCpyS (SerialBuf, sizeof (SerialBuf), Serial);

    /* Type 1 (System Information):
     *   String 2 = ProductName
     *   String 4 = SerialNumber
     *   String 5 = SKUNumber */
    if (Type1Handle != SMBIOS_HANDLE_PI_RESERVED) {
      StringNum = 2;
      Smbios->UpdateString (Smbios, &Type1Handle, &StringNum, NameBuf);
      StringNum = 4;
      Smbios->UpdateString (Smbios, &Type1Handle, &StringNum, SerialBuf);
      StringNum = 5;
      Smbios->UpdateString (Smbios, &Type1Handle, &StringNum, NameBuf);
    }

    /* Type 2 (Baseboard Information):
     *   String 2 = ProductName
     *   String 4 = SerialNumber */
    if (Type2Handle != SMBIOS_HANDLE_PI_RESERVED) {
      StringNum = 2;
      Smbios->UpdateString (Smbios, &Type2Handle, &StringNum, NameBuf);
      StringNum = 4;
      Smbios->UpdateString (Smbios, &Type2Handle, &StringNum, SerialBuf);
    }

    DEBUG ((DEBUG_INFO, "SMBIOS: Updated strings from BoardInfo (name=%a serial=%a)\n", NameBuf, SerialBuf));
  }

  return EFI_SUCCESS;
}
