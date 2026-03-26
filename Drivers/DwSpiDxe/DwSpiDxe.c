/** @file
  DesignWare SPI (SSI) controller DXE driver for MikroTik CCR2004.

  Drives the snps,designware-ssi SPI controller on the Alpine V2 SoC
  (SPI at 0xFD882000). Polling-based, no interrupts.

  Produces EFI_SPI_HC_PROTOCOL, EFI_SPI_CONFIGURATION_PROTOCOL, and
  EFI_DEVICE_PATH_PROTOCOL.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "DwSpiDxe.h"

//
// Device path template (GUID matches FILE_GUID from INF)
//
STATIC DW_SPI_DEVICE_PATH  mDwSpiDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (DW_SPI_DEVICE_PATH) - sizeof (EFI_DEVICE_PATH_PROTOCOL)),
        (UINT8)((sizeof (DW_SPI_DEVICE_PATH) - sizeof (EFI_DEVICE_PATH_PROTOCOL)) >> 8),
      },
    },
    // GUID matches FILE_GUID from INF
    { 0x7a2e3b4c, 0xe5f1, 0x4a06, { 0xb7, 0xd9, 0x2e, 0xab, 0x1f, 0x4d, 0x6e, 0x83 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

//
// SPI Configuration data (board-level: SPI NOR flash on CS0)
//

STATIC UINT32  mSpiFlashCsIndex = 0;  // CS0

STATIC EFI_SPI_PART  mSpiFlashPart = {
  L"Generic",                          // Vendor
  L"SPI-NOR",                          // PartNumber
  0,                                   // MinClockHz
  30000000,                            // MaxClockHz (30 MHz from DTS)
  FALSE                                // ChipSelectPolarity (active low)
};

STATIC EFI_SPI_BUS  mSpiBus;

STATIC EFI_SPI_PERIPHERAL  mSpiFlashPeripheral = {
  NULL,                                // NextSpiPeripheral
  L"SPI Flash",                        // FriendlyName
  NULL,                                // SpiPeripheralDriverGuid (set in init)
  &mSpiFlashPart,                      // SpiPart
  30000000,                            // MaxClockHz
  FALSE,                               // ClockPolarity (CPOL=0)
  FALSE,                               // ClockPhase (CPHA=0, SPI mode 0)
  0,                                   // Attributes
  NULL,                                // ConfigurationData
  &mSpiBus,                            // SpiBus
  NULL,                                // ChipSelect (use HC's)
  &mSpiFlashCsIndex                    // ChipSelectParameter
};

STATIC EFI_SPI_BUS  mSpiBus = {
  L"SPI0",                             // FriendlyName
  &mSpiFlashPeripheral,                // Peripherallist
  NULL,                                // ControllerPath (set in init)
  NULL,                                // Clock (use HC's)
  NULL                                 // ClockParameter
};

STATIC CONST EFI_SPI_BUS  *mSpiBusList[] = { &mSpiBus };

STATIC EFI_SPI_CONFIGURATION_PROTOCOL  mSpiConfig = {
  1,                                   // BusCount
  mSpiBusList                          // Buslist
};

extern EFI_GUID  gEdk2JedecSfdpSpiDxeDriverGuid;

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

#define DW_SPI_LOG_ENABLED  0

/**
  Write a formatted message directly to the serial port.
**/
STATIC
VOID
SpiLog (
  IN CONST CHAR8  *Fmt,
  ...
  )
{
#if DW_SPI_LOG_ENABLED
  VA_LIST  Args;
  CHAR8    Buf[128];
  UINTN    Len;

  VA_START (Args, Fmt);
  Len = AsciiVSPrint (Buf, sizeof (Buf), Fmt, Args);
  VA_END (Args);

  SerialPortWrite ((UINT8 *)Buf, Len);
#endif
}

STATIC
UINT16
DwSpiRead (
  IN DW_SPI_CONTEXT  *Ctx,
  IN UINTN           Offset
  )
{
  return MmioRead16 (Ctx->BaseAddress + Offset);
}

STATIC
VOID
DwSpiWrite (
  IN DW_SPI_CONTEXT  *Ctx,
  IN UINTN           Offset,
  IN UINT16          Value
  )
{
  MmioWrite16 (Ctx->BaseAddress + Offset, Value);
}

STATIC
VOID
DwSpiEnable (
  IN DW_SPI_CONTEXT  *Ctx,
  IN UINT16          Enable
  )
{
  DwSpiWrite (Ctx, DW_SPI_SSIENR, Enable);
}

// ---------------------------------------------------------------------------
// Hardware initialization
// ---------------------------------------------------------------------------

/**
  Detect FIFO depth by probing TXFLTR register.
**/
STATIC
UINT32
DwSpiDetectFifoDepth (
  IN DW_SPI_CONTEXT  *Ctx
  )
{
  UINT32  FifoLen;
  UINT32  i;

  for (i = 1; i < 256; i++) {
    DwSpiWrite (Ctx, DW_SPI_TXFLTR, (UINT16)i);
    if (DwSpiRead (Ctx, DW_SPI_TXFLTR) != (UINT16)i) {
      break;
    }
  }

  FifoLen = (i == 1) ? 0 : (i - 1);
  DwSpiWrite (Ctx, DW_SPI_TXFLTR, 0);

  return FifoLen;
}

/**
  Initialize the DesignWare SSI controller hardware.
**/
STATIC
VOID
DwSpiHwInit (
  IN DW_SPI_CONTEXT  *Ctx
  )
{
  //
  // Disable controller
  //
  DwSpiEnable (Ctx, 0);

  //
  // Mask all interrupts
  //
  DwSpiWrite (Ctx, DW_SPI_IMR, 0);

  //
  // Detect FIFO depth
  //
  Ctx->FifoLen = DwSpiDetectFifoDepth (Ctx);
  if (Ctx->FifoLen == 0 || Ctx->FifoLen > 256) {
    Ctx->FifoLen = DW_SPI_FIFO_DEPTH;
  }

  SpiLog ("[DwSpi] HwInit: FIFO depth %u\r\n", Ctx->FifoLen);

  //
  // Clear any pending interrupts
  //
  DwSpiRead (Ctx, DW_SPI_ICR);
}

// ---------------------------------------------------------------------------
// Clock configuration
// ---------------------------------------------------------------------------

/**
  Set SPI clock divisor. Controller must be disabled before calling.
**/
STATIC
EFI_STATUS
DwSpiSetClock (
  IN  DW_SPI_CONTEXT  *Ctx,
  IN  UINT32          RequestedHz,
  OUT UINT32          *ActualHz
  )
{
  UINT32  ClkDiv;

  if (RequestedHz == 0) {
    *ActualHz = 0;
    return EFI_SUCCESS;
  }

  //
  // Compute divisor: round up, then round up to even (hardware requirement)
  // Minimum divisor is 2
  //
  ClkDiv = (DW_SPI_REF_CLOCK_HZ + RequestedHz - 1) / RequestedHz;
  if (ClkDiv < 2) {
    ClkDiv = 2;
  }

  if ((ClkDiv & 1) != 0) {
    ClkDiv++;
  }

  DwSpiWrite (Ctx, DW_SPI_BAUDR, (UINT16)ClkDiv);
  *ActualHz = DW_SPI_REF_CLOCK_HZ / ClkDiv;

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Polling helpers
// ---------------------------------------------------------------------------

/**
  Poll SR for BUSY=0, with timeout.
**/
STATIC
EFI_STATUS
DwSpiPollBusy (
  IN DW_SPI_CONTEXT  *Ctx
  )
{
  UINTN  Elapsed;

  for (Elapsed = 0; Elapsed < DW_SPI_POLL_TIMEOUT_US; Elapsed += DW_SPI_POLL_INTERVAL_US) {
    if ((DwSpiRead (Ctx, DW_SPI_SR) & DW_SPI_SR_BUSY) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (DW_SPI_POLL_INTERVAL_US);
  }

  DEBUG ((DEBUG_ERROR, "DwSpiDxe: Busy timeout\n"));
  return EFI_TIMEOUT;
}

// ---------------------------------------------------------------------------
// EFI_SPI_HC_PROTOCOL implementation
// ---------------------------------------------------------------------------

/**
  Assert or deassert the SPI chip select.
**/
STATIC
EFI_STATUS
EFIAPI
DwSpiChipSelect (
  IN CONST EFI_SPI_HC_PROTOCOL  *This,
  IN CONST EFI_SPI_PERIPHERAL   *SpiPeripheral,
  IN BOOLEAN                    PinValue
  )
{
  DW_SPI_CONTEXT  *Ctx;
  UINT32          CsIndex;

  if ((This == NULL) || (SpiPeripheral == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (SpiPeripheral->ChipSelectParameter == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = DW_SPI_FROM_HC (This);
  CsIndex = *(UINT32 *)SpiPeripheral->ChipSelectParameter;

  if (CsIndex >= DW_SPI_NUM_CS) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // If PinValue matches ChipSelectPolarity, CS is being asserted
  //
  if (PinValue == SpiPeripheral->SpiPart->ChipSelectPolarity) {
    Ctx->ActiveCs = CsIndex;
  } else {
    Ctx->ActiveCs = 0xFF;
  }

  return EFI_SUCCESS;
}

/**
  Set up the clock for a SPI chip.
**/
STATIC
EFI_STATUS
EFIAPI
DwSpiClock (
  IN CONST EFI_SPI_HC_PROTOCOL  *This,
  IN CONST EFI_SPI_PERIPHERAL   *SpiPeripheral,
  IN UINT32                     *ClockHz
  )
{
  DW_SPI_CONTEXT  *Ctx;
  EFI_STATUS      Status;
  UINT32          Actual;

  if ((This == NULL) || (ClockHz == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = DW_SPI_FROM_HC (This);
  EfiAcquireLock (&Ctx->Lock);

  DwSpiEnable (Ctx, 0);
  Status = DwSpiSetClock (Ctx, *ClockHz, &Actual);
  *ClockHz = Actual;
  DwSpiEnable (Ctx, 1);

  EfiReleaseLock (&Ctx->Lock);
  return Status;
}

/**
  Perform a WRITE_THEN_READ transaction using TX+RX mode.

  Follows the Alpine HAL al_spi_read() algorithm:
  1. Disable SPI, set TMOD=TX+RX, SER=0, ssi_ovr=0x0F, enable SPI
  2. Push command bytes into TX FIFO (no CS yet — nothing clocks out)
  3. Pre-fill TX FIFO with dummy 0xFF bytes
  4. Assert CS via SER — clocking starts
  5. Main loop: feed dummy TX, drain RX (discard cmd echoes, store data)
  6. Wait idle, deassert CS, clear ssi_ovr, disable SPI
**/
STATIC
EFI_STATUS
DwSpiWriteThenRead (
  IN  DW_SPI_CONTEXT  *Ctx,
  IN  UINT16          Ctrl0Base,
  IN  UINT8           *WriteBuffer,
  IN  UINT32          WriteBytes,
  OUT UINT8           *ReadBuffer,
  IN  UINT32          ReadBytes
  )
{
  UINT32  DummyTx;
  UINT32  CmdEcho;
  UINT32  ByteLen;
  UINT32  TxUsed;
  UINT32  RxUsed;
  UINT32  RxAvail;
  UINT32  Watchdog;
  UINT32  CmdIdx;
  UINT16  Ctrl0;

  DummyTx = ReadBytes;
  CmdEcho = WriteBytes;
  ByteLen = ReadBytes;

  SpiLog ("[DwSpi] WriteThenRead: cmd %u bytes, read %u bytes\r\n",
          WriteBytes, ReadBytes);

  //
  // 1. Disable SPI, configure TMOD=TX+RX
  //
  DwSpiEnable (Ctx, 0);

  Ctrl0 = Ctrl0Base | (DW_SPI_TMOD_TR << DW_SPI_CTRL0_TMOD_SHIFT);
  DwSpiWrite (Ctx, DW_SPI_CTRL0, Ctrl0);

  //
  // 2. SER=0 (no CS yet), ssi_ovr=0x0F (hold CS when SER is set)
  //
  DwSpiWrite (Ctx, DW_SPI_SER, 0);
  DwSpiWrite (Ctx, DW_SPI_SSI_OVR, DW_SPI_SSI_OVR_CS_ALL);

  //
  // 3. Enable SPI (no CS asserted, so nothing clocks)
  //
  DwSpiEnable (Ctx, 1);

  //
  // 4. Push command bytes into TX FIFO
  //
  for (CmdIdx = 0; CmdIdx < WriteBytes; CmdIdx++) {
    while ((DwSpiRead (Ctx, DW_SPI_SR) & DW_SPI_SR_TFNF) == 0) {
      MicroSecondDelay (DW_SPI_POLL_INTERVAL_US);
    }
    DwSpiWrite (Ctx, DW_SPI_DR, (UINT16)WriteBuffer[CmdIdx]);
  }

  //
  // 5. Pre-fill TX FIFO with dummy bytes before asserting CS
  //
  while (DummyTx > 0) {
    if ((DwSpiRead (Ctx, DW_SPI_SR) & DW_SPI_SR_TFNF) == 0) {
      break;
    }
    DwSpiWrite (Ctx, DW_SPI_DR, 0xFF);
    DummyTx--;
  }

  //
  // 6. Assert CS — clocking begins
  //
  DwSpiWrite (Ctx, DW_SPI_SER, (UINT16)(BIT0 << Ctx->ActiveCs));

  SpiLog ("[DwSpi] WriteThenRead: CS asserted, dummy remaining %u\r\n", DummyTx);

  //
  // 7. Main loop: interleave dummy TX writes and RX FIFO draining
  //
  Watchdog = 0;
  while (ByteLen > 0) {
    TxUsed  = DwSpiRead (Ctx, DW_SPI_TXFLR);
    RxUsed  = RxAvail = DwSpiRead (Ctx, DW_SPI_RXFLR);

    if (RxAvail > 0) {
      Watchdog = 0;
    } else {
      Watchdog++;
      if ((Watchdog % 100) == 0) {
        MicroSecondDelay (1);
      }
    }

    if (Watchdog >= 100000) {
      SpiLog ("[DwSpi] WriteThenRead: TIMEOUT bytelen=%u\r\n", ByteLen);
      goto Cleanup;
    }

    //
    // Feed more dummy bytes while FIFO has space.
    // RxUsed tracks total RX occupancy (for back-pressure).
    // RxAvail stays at the register snapshot (for drain loop).
    //
    while (DummyTx > 0 && TxUsed < Ctx->FifoLen && RxUsed < Ctx->FifoLen) {
      DwSpiWrite (Ctx, DW_SPI_DR, 0xFF);
      DummyTx--;
      TxUsed++;
      RxUsed++;
    }

    //
    // Drain RX FIFO — only read entries that were present at loop top
    //
    while (RxAvail > 0) {
      UINT8  Byte;

      Byte = (UINT8)DwSpiRead (Ctx, DW_SPI_DR);

      if (CmdEcho > 0) {
        //
        // Discard echoed command byte (TX+RX mode echoes TX data)
        //
        CmdEcho--;
      } else {
        *ReadBuffer++ = Byte;
        ByteLen--;
      }

      RxAvail--;
    }
  }

  //
  // 8. Wait for controller to go idle
  //
  DwSpiPollBusy (Ctx);

Cleanup:
  //
  // 9. Deassert CS, clear override, disable
  //
  DwSpiWrite (Ctx, DW_SPI_SER, 0);
  DwSpiWrite (Ctx, DW_SPI_SSI_OVR, 0);
  DwSpiEnable (Ctx, 0);
  DwSpiRead (Ctx, DW_SPI_ICR);

  if (ByteLen > 0) {
    SpiLog ("[DwSpi] WriteThenRead: FAIL, %u bytes unread\r\n", ByteLen);
    return EFI_TIMEOUT;
  }

  SpiLog ("[DwSpi] WriteThenRead: done\r\n");
  return EFI_SUCCESS;
}

/**
  Perform a SPI transaction.
**/
STATIC
EFI_STATUS
EFIAPI
DwSpiTransaction (
  IN CONST EFI_SPI_HC_PROTOCOL  *This,
  IN EFI_SPI_BUS_TRANSACTION    *BusTransaction
  )
{
  DW_SPI_CONTEXT  *Ctx;
  EFI_STATUS      Status;
  UINT16          Ctrl0;
  UINT32          Tmod;
  UINT32          FrameSize;
  UINT32          WriteBytes;
  UINT32          ReadBytes;
  UINT8           *WriteBuffer;
  UINT8           *ReadBuffer;
  UINT32          Written;
  UINT32          Read;
  UINT16          Sr;

  if ((This == NULL) || (BusTransaction == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = DW_SPI_FROM_HC (This);
  EfiAcquireLock (&Ctx->Lock);

  //
  // Validate CS is selected
  //
  if (Ctx->ActiveCs == 0xFF) {
    DEBUG ((DEBUG_ERROR, "DwSpiDxe: No CS selected\n"));
    Status = EFI_NOT_READY;
    goto Exit;
  }

  WriteBytes  = BusTransaction->WriteBytes;
  ReadBytes   = BusTransaction->ReadBytes;
  WriteBuffer = BusTransaction->WriteBuffer;
  ReadBuffer  = BusTransaction->ReadBuffer;
  FrameSize   = BusTransaction->FrameSize;

  SpiLog ("[DwSpi] Transaction: type=%u write=%u read=%u\r\n",
          BusTransaction->TransactionType, WriteBytes, ReadBytes);

  //
  // Determine TMOD from TransactionType
  //
  switch (BusTransaction->TransactionType) {
    case SPI_TRANSACTION_FULL_DUPLEX:
      Tmod = DW_SPI_TMOD_TR;
      if ((WriteBytes == 0) || (ReadBytes == 0) || (WriteBuffer == NULL) || (ReadBuffer == NULL)) {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
      }
      break;
    case SPI_TRANSACTION_WRITE_ONLY:
      Tmod = DW_SPI_TMOD_TO;
      if ((WriteBytes == 0) || (WriteBuffer == NULL)) {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
      }
      break;
    case SPI_TRANSACTION_READ_ONLY:
      Tmod = DW_SPI_TMOD_RO;
      if ((ReadBytes == 0) || (ReadBuffer == NULL)) {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
      }
      break;
    case SPI_TRANSACTION_WRITE_THEN_READ:
      //
      // Uses TX+RX mode with deferred CS — handled by dedicated helper
      //
      if ((WriteBytes == 0) || (WriteBuffer == NULL) || (ReadBytes == 0) || (ReadBuffer == NULL)) {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
      }
      {
        UINT16  Ctrl0Base;

        Ctrl0Base = (UINT16)((FrameSize - 1) & DW_SPI_CTRL0_DFS_MASK);
        if (BusTransaction->SpiPeripheral != NULL) {
          if (BusTransaction->SpiPeripheral->ClockPhase) {
            Ctrl0Base |= DW_SPI_CTRL0_SCPH;
          }
          if (BusTransaction->SpiPeripheral->ClockPolarity) {
            Ctrl0Base |= DW_SPI_CTRL0_SCOL;
          }
        }

        Status = DwSpiWriteThenRead (
                   Ctx,
                   Ctrl0Base,
                   WriteBuffer,
                   WriteBytes,
                   ReadBuffer,
                   ReadBytes
                   );
      }
      goto Exit;
    default:
      Status = EFI_UNSUPPORTED;
      goto Exit;
  }

  //
  // Disable controller for configuration
  //
  DwSpiEnable (Ctx, 0);

  //
  // Configure CTRL0
  //
  Ctrl0 = (UINT16)((FrameSize - 1) & DW_SPI_CTRL0_DFS_MASK);
  if (BusTransaction->SpiPeripheral != NULL) {
    if (BusTransaction->SpiPeripheral->ClockPhase) {
      Ctrl0 |= DW_SPI_CTRL0_SCPH;
    }
    if (BusTransaction->SpiPeripheral->ClockPolarity) {
      Ctrl0 |= DW_SPI_CTRL0_SCOL;
    }
  }
  Ctrl0 |= (UINT16)(Tmod << DW_SPI_CTRL0_TMOD_SHIFT);

  DwSpiWrite (Ctx, DW_SPI_CTRL0, Ctrl0);

  //
  // For RO: set number of frames to receive
  //
  if (Tmod == DW_SPI_TMOD_RO) {
    DwSpiWrite (Ctx, DW_SPI_CTRL1, (UINT16)(ReadBytes - 1));
  }

  //
  // For write-only (TMOD_TO): use SSI_OVR to control CS timing.
  // Pre-fill FIFO before asserting CS to prevent premature CS deassert
  // if the FIFO drains between software-paced byte pushes.
  //
  if (Tmod == DW_SPI_TMOD_TO) {
    DwSpiWrite (Ctx, DW_SPI_SER, 0);
    DwSpiWrite (Ctx, DW_SPI_SSI_OVR, DW_SPI_SSI_OVR_CS_ALL);
    DwSpiEnable (Ctx, 1);

    //
    // Pre-fill TX FIFO (up to FIFO depth) before asserting CS
    //
    for (Written = 0; Written < WriteBytes && Written < Ctx->FifoLen; Written++) {
      DwSpiWrite (Ctx, DW_SPI_DR, (UINT16)WriteBuffer[Written]);
    }

    //
    // Assert CS — clocking begins with FIFO already primed
    //
    DwSpiWrite (Ctx, DW_SPI_SER, (UINT16)(BIT0 << Ctx->ActiveCs));

    //
    // Push remaining bytes
    //
    for (; Written < WriteBytes; Written++) {
      while ((DwSpiRead (Ctx, DW_SPI_SR) & DW_SPI_SR_TFNF) == 0) {
        MicroSecondDelay (DW_SPI_POLL_INTERVAL_US);
      }
      DwSpiWrite (Ctx, DW_SPI_DR, (UINT16)WriteBuffer[Written]);
    }

    //
    // Wait for TX FIFO empty and not busy
    //
    while ((DwSpiRead (Ctx, DW_SPI_SR) & DW_SPI_SR_TFE) == 0) {
      MicroSecondDelay (DW_SPI_POLL_INTERVAL_US);
    }
    Status = DwSpiPollBusy (Ctx);

    //
    // Deassert CS, clear override
    //
    DwSpiWrite (Ctx, DW_SPI_SER, 0);
    DwSpiWrite (Ctx, DW_SPI_SSI_OVR, 0);
    DwSpiEnable (Ctx, 0);
    DwSpiRead (Ctx, DW_SPI_ICR);
    goto Done;
  }

  //
  // For non-TMOD_TO: original CS handling
  //
  DwSpiWrite (Ctx, DW_SPI_SER, (UINT16)(BIT0 << Ctx->ActiveCs));

  //
  // Enable controller
  //
  DwSpiEnable (Ctx, 1);

  //
  // Execute transfer based on TMOD
  //
  switch (Tmod) {
    case DW_SPI_TMOD_TO:
      //
      // (handled above — should not reach here)
      //
      Status = EFI_DEVICE_ERROR;
      break;

    case DW_SPI_TMOD_RO:
      //
      // Read Only: write a single dummy byte to start clocking
      //
      DwSpiWrite (Ctx, DW_SPI_DR, 0x00);

      for (Read = 0; Read < ReadBytes; Read++) {
        while ((DwSpiRead (Ctx, DW_SPI_SR) & DW_SPI_SR_RFNE) == 0) {
          MicroSecondDelay (DW_SPI_POLL_INTERVAL_US);
        }

        ReadBuffer[Read] = (UINT8)DwSpiRead (Ctx, DW_SPI_DR);
      }

      Status = DwSpiPollBusy (Ctx);
      break;

    case DW_SPI_TMOD_TR:
      //
      // Full Duplex: interleave writes and reads
      //
      Written = 0;
      Read = 0;
      while ((Written < WriteBytes) || (Read < ReadBytes)) {
        Sr = DwSpiRead (Ctx, DW_SPI_SR);

        if ((Written < WriteBytes) && ((Sr & DW_SPI_SR_TFNF) != 0)) {
          DwSpiWrite (Ctx, DW_SPI_DR, (UINT16)WriteBuffer[Written]);
          Written++;
        }

        if ((Read < ReadBytes) && ((Sr & DW_SPI_SR_RFNE) != 0)) {
          ReadBuffer[Read] = (UINT8)DwSpiRead (Ctx, DW_SPI_DR);
          Read++;
        }

        if (((Written >= WriteBytes) || ((Sr & DW_SPI_SR_TFNF) == 0)) &&
            ((Read >= ReadBytes) || ((Sr & DW_SPI_SR_RFNE) == 0)))
        {
          MicroSecondDelay (DW_SPI_POLL_INTERVAL_US);
        }
      }

      Status = DwSpiPollBusy (Ctx);
      break;

    default:
      Status = EFI_UNSUPPORTED;
      break;
  }

  //
  // Disable controller (auto-deasserts CS)
  //
  DwSpiEnable (Ctx, 0);

  //
  // Clear any pending interrupts
  //
  DwSpiRead (Ctx, DW_SPI_ICR);

Done:
Exit:
  EfiReleaseLock (&Ctx->Lock);
  return Status;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
DwSpiDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS          Status;
  DW_SPI_CONTEXT      *Ctx;
  DW_SPI_DEVICE_PATH  *DevicePath;
  EFI_HANDLE          ConfigHandle;

  //
  // Allocate context
  //
  Ctx = AllocateZeroPool (sizeof (DW_SPI_CONTEXT));
  if (Ctx == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ctx->Signature   = DW_SPI_SIGNATURE;
  Ctx->Handle      = NULL;
  Ctx->BaseAddress = DW_SPI_BASE;
  Ctx->ActiveCs    = 0xFF;
  EfiInitializeLock (&Ctx->Lock, TPL_NOTIFY);

  //
  // Initialize hardware (detect FIFO depth, mask interrupts)
  //
  DwSpiHwInit (Ctx);

  DEBUG ((DEBUG_INFO, "DwSpiDxe: DW SSI at 0x%lx, FIFO depth %u\n",
    (UINT64)Ctx->BaseAddress, Ctx->FifoLen));

  //
  // Wire up HC protocol functions
  //
  Ctx->HcProtocol.Attributes = HC_SUPPORTS_WRITE_ONLY_OPERATIONS |
                                HC_SUPPORTS_READ_ONLY_OPERATIONS |
                                HC_SUPPORTS_WRITE_THEN_READ_OPERATIONS;
  Ctx->HcProtocol.FrameSizeSupportMask = BIT7;         // 8-bit frames
  Ctx->HcProtocol.MaximumTransferBytes = 0xFFFFFFFF;   // No hardware limit on PIO
  Ctx->HcProtocol.ChipSelect  = DwSpiChipSelect;
  Ctx->HcProtocol.Clock       = DwSpiClock;
  Ctx->HcProtocol.Transaction = DwSpiTransaction;

  //
  // Allocate device path from template
  //
  DevicePath = AllocateCopyPool (sizeof (mDwSpiDevicePathTemplate),
                                 &mDwSpiDevicePathTemplate);
  if (DevicePath == NULL) {
    FreePool (Ctx);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Set SPI bus controller path to match HC device path
  //
  mSpiBus.ControllerPath = (EFI_DEVICE_PATH_PROTOCOL *)DevicePath;

  //
  // Set peripheral driver GUID
  //
  mSpiFlashPeripheral.SpiPeripheralDriverGuid = &gEdk2JedecSfdpSpiDxeDriverGuid;

  //
  // Install SPI HC + Device Path on context handle
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Ctx->Handle,
                  &gEfiSpiHcProtocolGuid,
                  &Ctx->HcProtocol,
                  &gEfiDevicePathProtocolGuid,
                  (EFI_DEVICE_PATH_PROTOCOL *)DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DwSpiDxe: HC protocol install failed: %r\n", Status));
    FreePool (DevicePath);
    FreePool (Ctx);
    return Status;
  }

  //
  // Install SPI Configuration on a separate handle
  //
  ConfigHandle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &ConfigHandle,
                  &gEfiSpiConfigurationProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mSpiConfig
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DwSpiDxe: SPI config protocol install failed: %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}
