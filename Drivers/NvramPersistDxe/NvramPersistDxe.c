/** @file
  NVRAM persistence driver — serializes/deserializes UEFI variables
  to SPI NOR flash.

  On boot: reads the serialized variable store from SPI flash at offset
  0xF80000 and restores all variables via SetVariable.

  Before ExitBootServices: snapshots all current variables and writes
  them back to SPI flash.

  Runtime writes go to RAM only (via EmuVariable) and are discarded
  on next boot — only the ExitBootServices snapshot persists.

  The SPI region is also used by the NVRAM reset facility which simply
  erases the region, causing a fresh start on next boot.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/PlatformSpecificResetHandler.h>
#include <Protocol/SpiNorFlash.h>
#include "NvramSetupGuid.h"

//
// SPI flash layout for NVRAM
//
#define NVRAM_SPI_OFFSET     0x00F80000U
#define NVRAM_SPI_SIZE       0x00080000U  // 512 KB
#define NVRAM_SECTOR_SIZE    0x00001000U  // 4 KB

//
// Serialized store format
//
#define NVRAM_SIGNATURE      0x4D52564EU  // "NVRM"

#pragma pack(1)
typedef struct {
  UINT32  Signature;
  UINT32  TotalSize;   // header + all entries
  UINT32  EntryCount;
  UINT32  Crc32;       // CRC32 of data after this header
} NVRAM_STORE_HEADER;

typedef struct {
  UINT32    Attributes;
  EFI_GUID  VendorGuid;
  UINT32    NameSize;   // bytes, including null CHAR16 terminator
  UINT32    DataSize;   // bytes
  // Followed by: CHAR16 Name[NameSize/2], UINT8 Data[DataSize]
} NVRAM_VARIABLE_ENTRY;
#pragma pack()

STATIC EFI_SPI_NOR_FLASH_PROTOCOL  *mSpiNor;
STATIC EFI_EVENT                   mExitBootServicesEvent;

/**
  Compute CRC32 of a buffer.
**/
STATIC
UINT32
NvramCrc32 (
  IN  CONST VOID  *Data,
  IN  UINTN       Size
  )
{
  UINT32  Crc = 0;

  gBS->CalculateCrc32 ((VOID *)Data, Size, &Crc);
  return Crc;
}

/**
  Restore variables from SPI flash into the variable store.
**/
STATIC
VOID
NvramRestore (
  VOID
  )
{
  EFI_STATUS          Status;
  UINT8               *StoreBuf;
  NVRAM_STORE_HEADER  *Hdr;
  UINT8               *Pos;
  UINT8               *End;
  UINT32              CalcCrc;
  UINTN               Restored;

  StoreBuf = AllocatePool (NVRAM_SPI_SIZE);
  if (StoreBuf == NULL) {
    DEBUG ((DEBUG_ERROR, "NvramPersist: out of memory for restore\n"));
    return;
  }

  Status = mSpiNor->ReadData (mSpiNor, NVRAM_SPI_OFFSET, NVRAM_SPI_SIZE, StoreBuf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "NvramPersist: SPI read failed: %r\n", Status));
    FreePool (StoreBuf);
    return;
  }

  Hdr = (NVRAM_STORE_HEADER *)StoreBuf;

  if (Hdr->Signature != NVRAM_SIGNATURE) {
    DEBUG ((DEBUG_WARN, "NvramPersist: no valid store (sig=0x%08x)\n", Hdr->Signature));
    FreePool (StoreBuf);
    return;
  }

  if (Hdr->TotalSize < sizeof (NVRAM_STORE_HEADER) || Hdr->TotalSize > NVRAM_SPI_SIZE) {
    DEBUG ((DEBUG_WARN, "NvramPersist: bad TotalSize 0x%x\n", Hdr->TotalSize));
    FreePool (StoreBuf);
    return;
  }

  //
  // Verify CRC over everything after the header
  //
  CalcCrc = NvramCrc32 (
              StoreBuf + sizeof (NVRAM_STORE_HEADER),
              Hdr->TotalSize - sizeof (NVRAM_STORE_HEADER)
              );
  if (CalcCrc != Hdr->Crc32) {
    DEBUG ((DEBUG_WARN, "NvramPersist: CRC mismatch (stored=0x%08x calc=0x%08x)\n",
            Hdr->Crc32, CalcCrc));
    FreePool (StoreBuf);
    return;
  }

  DEBUG ((DEBUG_WARN, "NvramPersist: restoring %u variables (%u bytes)\n",
          Hdr->EntryCount, Hdr->TotalSize));

  //
  // Iterate entries and call SetVariable for each
  //
  Pos = StoreBuf + sizeof (NVRAM_STORE_HEADER);
  End = StoreBuf + Hdr->TotalSize;
  Restored = 0;

  while (Pos + sizeof (NVRAM_VARIABLE_ENTRY) <= End) {
    NVRAM_VARIABLE_ENTRY  *Entry;
    CHAR16                *Name;
    VOID                  *Data;
    UINTN                 EntryTotalSize;

    Entry = (NVRAM_VARIABLE_ENTRY *)Pos;
    EntryTotalSize = sizeof (NVRAM_VARIABLE_ENTRY) + Entry->NameSize + Entry->DataSize;

    if (Pos + EntryTotalSize > End) {
      break;
    }

    Name = (CHAR16 *)(Pos + sizeof (NVRAM_VARIABLE_ENTRY));
    Data = (VOID *)(Pos + sizeof (NVRAM_VARIABLE_ENTRY) + Entry->NameSize);

    Status = gRT->SetVariable (
                    Name,
                    &Entry->VendorGuid,
                    Entry->Attributes,
                    Entry->DataSize,
                    Data
                    );
    if (!EFI_ERROR (Status)) {
      Restored++;
      DEBUG ((DEBUG_WARN, "NvramPersist:   [%u] %s (attr=0x%x size=%u)\n",
              Restored, Name, Entry->Attributes, Entry->DataSize));
    } else {
      DEBUG ((DEBUG_WARN, "NvramPersist:   FAIL %s: %r\n", Name, Status));
    }

    Pos += EntryTotalSize;
  }

  DEBUG ((DEBUG_WARN, "NvramPersist: %u variables restored\n", Restored));
  FreePool (StoreBuf);
}

/**
  Snapshot all current variables and write to SPI flash.
**/
STATIC
VOID
NvramSave (
  VOID
  )
{
  EFI_STATUS          Status;
  UINT8               *StoreBuf;
  UINTN               StorePos;
  NVRAM_STORE_HEADER  *Hdr;
  CHAR16              VarName[256];
  UINTN               VarNameSize;
  EFI_GUID            VendorGuid;
  UINT32              Attributes;
  UINTN               DataSize;
  UINT8               *DataBuf;
  UINT32              EntryCount;
  UINT32              SectorsToErase;
  UINT32              WriteSize;

  StoreBuf = AllocateZeroPool (NVRAM_SPI_SIZE);
  if (StoreBuf == NULL) {
    DEBUG ((DEBUG_ERROR, "NvramPersist: out of memory for save\n"));
    return;
  }

  DataBuf = AllocatePool (0x10000);
  if (DataBuf == NULL) {
    FreePool (StoreBuf);
    return;
  }

  //
  // Reserve space for header
  //
  StorePos = sizeof (NVRAM_STORE_HEADER);
  EntryCount = 0;

  //
  // Enumerate all variables
  //
  VarName[0] = L'\0';
  VarNameSize = sizeof (VarName);

  while (TRUE) {
    VarNameSize = sizeof (VarName);
    Status = gRT->GetNextVariableName (&VarNameSize, VarName, &VendorGuid);
    if (Status == EFI_NOT_FOUND) {
      break;
    }

    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Read variable data
    //
    DataSize = 0x10000;
    Status = gRT->GetVariable (VarName, &VendorGuid, &Attributes, &DataSize, DataBuf);
    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Skip volatile variables (they're not meant to persist)
    //
    if ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0) {
      continue;
    }

    //
    // Compute entry size and check if it fits
    //
    {
      UINTN              NameBytes;
      UINTN              EntrySize;
      NVRAM_VARIABLE_ENTRY *Entry;

      NameBytes = VarNameSize;  // already includes null terminator in bytes
      EntrySize = sizeof (NVRAM_VARIABLE_ENTRY) + NameBytes + DataSize;

      if (StorePos + EntrySize > NVRAM_SPI_SIZE) {
        DEBUG ((DEBUG_WARN, "NvramPersist: store full, skipping remaining vars\n"));
        break;
      }

      Entry = (NVRAM_VARIABLE_ENTRY *)(StoreBuf + StorePos);
      Entry->Attributes = Attributes;
      CopyMem (&Entry->VendorGuid, &VendorGuid, sizeof (EFI_GUID));
      Entry->NameSize = (UINT32)NameBytes;
      Entry->DataSize = (UINT32)DataSize;
      CopyMem (StoreBuf + StorePos + sizeof (NVRAM_VARIABLE_ENTRY), VarName, NameBytes);
      CopyMem (StoreBuf + StorePos + sizeof (NVRAM_VARIABLE_ENTRY) + NameBytes, DataBuf, DataSize);

      StorePos += EntrySize;
      EntryCount++;
    }
  }

  FreePool (DataBuf);

  //
  // Fill header
  //
  Hdr = (NVRAM_STORE_HEADER *)StoreBuf;
  Hdr->Signature  = NVRAM_SIGNATURE;
  Hdr->TotalSize  = (UINT32)StorePos;
  Hdr->EntryCount = EntryCount;
  Hdr->Crc32      = NvramCrc32 (
                       StoreBuf + sizeof (NVRAM_STORE_HEADER),
                       StorePos - sizeof (NVRAM_STORE_HEADER)
                       );

  DEBUG ((DEBUG_WARN, "NvramPersist: saving %u variables (%u bytes)\n",
          EntryCount, StorePos));

  //
  // Erase sectors covering the data
  //
  WriteSize = (UINT32)((StorePos + NVRAM_SECTOR_SIZE - 1) & ~(NVRAM_SECTOR_SIZE - 1));
  SectorsToErase = WriteSize / NVRAM_SECTOR_SIZE;

  Status = mSpiNor->Erase (mSpiNor, NVRAM_SPI_OFFSET, SectorsToErase);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NvramPersist: erase failed: %r\n", Status));
    FreePool (StoreBuf);
    return;
  }

  //
  // Program
  //
  Status = mSpiNor->WriteData (mSpiNor, NVRAM_SPI_OFFSET, WriteSize, StoreBuf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NvramPersist: write failed: %r\n", Status));
  } else {
    DEBUG ((DEBUG_WARN, "NvramPersist: saved to SPI flash\n"));
  }

  FreePool (StoreBuf);
}

/**
  Erase the entire NVRAM SPI region.
**/
STATIC
EFI_STATUS
NvramEraseAll (
  VOID
  )
{
  UINT32  Sectors = NVRAM_SPI_SIZE / NVRAM_SECTOR_SIZE;

  DEBUG ((DEBUG_WARN, "NvramPersist: erasing NVRAM region (%u sectors)\n", Sectors));
  return mSpiNor->Erase (mSpiNor, NVRAM_SPI_OFFSET, Sectors);
}

// ---- HII Setup Page (Device Manager integration) ----

extern UINT8  NvramSetupBin[];
extern UINT8  NvramPersistDxeStrings[];

STATIC EFI_GUID  mNvramSetupFormsetGuid = NVRAM_SETUP_FORMSET_GUID;

STATIC EFI_HII_HANDLE  mHiiHandle;

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;
#pragma pack()

STATIC HII_VENDOR_DEVICE_PATH  mHiiVendorDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH) } },
    NVRAM_SETUP_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL) } }
};

STATIC EFI_HANDLE  mHiiDriverHandle;

STATIC
EFI_STATUS
EFIAPI
NvramExtractConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Request,
  OUT EFI_STRING                            *Progress,
  OUT EFI_STRING                            *Results
  )
{
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
NvramRouteConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                            *Progress
  )
{
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
NvramCallback (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  EFI_BROWSER_ACTION                    Action,
  IN  EFI_QUESTION_ID                       QuestionId,
  IN  UINT8                                 Type,
  IN  EFI_IFR_TYPE_VALUE                    *Value,
  OUT EFI_BROWSER_ACTION_REQUEST            *ActionRequest
  )
{
  if (Action != EFI_BROWSER_ACTION_CHANGED) {
    return EFI_UNSUPPORTED;
  }

  if (QuestionId == NVRAM_RESET_KEY) {
    EFI_STATUS  Status;

    Status = NvramEraseAll ();
    if (!EFI_ERROR (Status)) {
      //
      // Show "done" string via the form browser
      //
      *ActionRequest = EFI_BROWSER_ACTION_REQUEST_EXIT;
      DEBUG ((DEBUG_WARN, "NvramPersist: NVRAM erased via Setup UI\n"));
    }

    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mConfigAccess = {
  NvramExtractConfig,
  NvramRouteConfig,
  NvramCallback
};

STATIC
VOID
NvramInstallSetupPage (
  VOID
  )
{
  EFI_STATUS  Status;

  mHiiDriverHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHiiDriverHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mHiiVendorDevicePath,
                  &gEfiHiiConfigAccessProtocolGuid,
                  &mConfigAccess,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "NvramPersist: failed to install HII driver handle: %r\n", Status));
    return;
  }

  mHiiHandle = HiiAddPackages (
                 &mNvramSetupFormsetGuid,
                 mHiiDriverHandle,
                 NvramPersistDxeStrings,
                 NvramSetupBin,
                 NULL
                 );
  if (mHiiHandle == NULL) {
    DEBUG ((DEBUG_WARN, "NvramPersist: HiiAddPackages failed\n"));
  }
}

/**
  ReadyToBoot / ExitBootServices callback — save variables to SPI.
**/
STATIC
VOID
EFIAPI
NvramEventCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  NvramSave ();
}

/**
  ResetSystem callback — save variables to SPI before reset.
**/
STATIC
VOID
EFIAPI
NvramResetCallback (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData  OPTIONAL
  )
{
  NvramSave ();
}

/**
  Entry point.
**/
EFI_STATUS
EFIAPI
NvramPersistDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  //
  // Locate SPI NOR flash
  //
  Status = gBS->LocateProtocol (&gEfiSpiNorFlashProtocolGuid, NULL, (VOID **)&mSpiNor);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NvramPersist: SPI NOR not found: %r\n", Status));
    return Status;
  }

  //
  // Restore saved variables from SPI flash
  //
  NvramRestore ();

  //
  // Signal that NVRAM restore is complete by installing a protocol.
  // Drivers that depend on persisted variables should include
  // gMikroTikNvramReadyProtocolGuid in their Depex.
  //
  {
    EFI_HANDLE  NvramReadyHandle = NULL;

    gBS->InstallProtocolInterface (
           &NvramReadyHandle,
           &gMikroTikNvramReadyProtocolGuid,
           EFI_NATIVE_INTERFACE,
           NULL
           );
  }

  //
  // Register ExitBootServices callback to save before OS boot
  //
  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_CALLBACK,
                  NvramEventCallback,
                  NULL,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NvramPersist: failed to create EBS event: %r\n", Status));
  }

  //
  // Also register a ReadyToBoot event to save periodically (in case EBS
  // callback has limited time for SPI flash operations)
  //
  {
    EFI_EVENT  ReadyToBootEvent;

    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    NvramEventCallback,
                    NULL,
                    &gEfiEventReadyToBootGuid,
                    &ReadyToBootEvent
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "NvramPersist: failed to create ReadyToBoot event: %r\n", Status));
    }
  }

  //
  // Register a reset handler so NVRAM is saved before any ResetSystem
  // call during boot services (e.g. shell "reset" command, setup changes).
  //
  {
    EDKII_PLATFORM_SPECIFIC_RESET_HANDLER_PROTOCOL  *ResetHandler;

    Status = gBS->LocateProtocol (
                    &gEdkiiPlatformSpecificResetHandlerProtocolGuid,
                    NULL,
                    (VOID **)&ResetHandler
                    );
    if (!EFI_ERROR (Status)) {
      ResetHandler->RegisterResetNotify (
                      ResetHandler,
                      NvramResetCallback
                      );
    } else {
      DEBUG ((DEBUG_WARN, "NvramPersist: ResetHandler protocol not found, "
              "NVRAM may not persist on reset\n"));
    }
  }

  //
  // Install HII setup page for "Reset NVRAM" in Device Manager
  //
  NvramInstallSetupPage ();

  DEBUG ((DEBUG_WARN, "NvramPersist: driver initialized\n"));
  return EFI_SUCCESS;
}
