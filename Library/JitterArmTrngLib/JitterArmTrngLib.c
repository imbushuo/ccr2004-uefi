/** @file
  Jitter-based ArmTrngLib implementation.

  Generates entropy by measuring timing jitter in the ARM architecture
  timer (CNTPCT_EL0).  Repeated tight-loop memory and ALU operations
  experience variable latency from cache, DRAM refresh, branch prediction,
  and interrupt delivery.  The low-order bits of the delta between timer
  samples carry physical entropy.

  The output is mixed through a simple hash (rotate-xor accumulator)
  to distribute entropy across all bits.

  This is NOT a certified NIST SP 800-90B source, but provides practical
  entropy on SoCs that lack a hardware TRNG block.

  Copyright (c) 2024, MikroTik. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>

//
// Number of jitter samples to collect per output byte.
// More samples = better entropy quality at the cost of latency.
//
#define SAMPLES_PER_BYTE  64

//
// Scratch buffer used to introduce memory-subsystem jitter.
// Sized to span multiple cache lines.
//
#define JITTER_BUF_SIZE   512

STATIC volatile UINT8 mJitterBuf[JITTER_BUF_SIZE];

/**
  Perform a timing-variable workload to generate jitter.
  Touches memory across cache lines and does data-dependent ALU work.
**/
STATIC
UINT64
JitterRound (
  IN  UINT64  Seed
  )
{
  UINTN   i;
  UINT64  Acc;

  Acc = Seed;
  for (i = 0; i < JITTER_BUF_SIZE; i += 64) {
    mJitterBuf[i] = (UINT8)(Acc ^ i);
    Acc += mJitterBuf[i];
    Acc = (Acc << 7) | (Acc >> 57);
    Acc ^= mJitterBuf[(i + 33) % JITTER_BUF_SIZE];
  }

  return Acc;
}

/**
  Collect one byte of entropy from timer jitter.
**/
STATIC
UINT8
CollectJitterByte (
  VOID
  )
{
  UINT64  Acc;
  UINT64  T0;
  UINT64  T1;
  UINT64  Delta;
  UINTN   i;

  Acc = GetPerformanceCounter ();

  for (i = 0; i < SAMPLES_PER_BYTE; i++) {
    T0  = GetPerformanceCounter ();
    Acc = JitterRound (Acc);
    T1  = GetPerformanceCounter ();

    Delta = T1 - T0;

    //
    // Mix the low bits of the timing delta into the accumulator.
    // The LSBs carry the most jitter from cache/DRAM/pipeline variance.
    //
    Acc ^= Delta;
    Acc  = (Acc << 3) | (Acc >> 61);
    Acc += Delta & 0xFF;
  }

  //
  // Final fold: XOR all 8 bytes of the accumulator into one byte.
  //
  return (UINT8)((Acc) ^ (Acc >> 8) ^ (Acc >> 16) ^ (Acc >> 24) ^
                 (Acc >> 32) ^ (Acc >> 40) ^ (Acc >> 48) ^ (Acc >> 56));
}

// ---- ArmTrngLib interface ----

RETURN_STATUS
EFIAPI
GetArmTrngVersion (
  OUT UINT16  *MajorRevision,
  OUT UINT16  *MinorRevision
  )
{
  if (MajorRevision == NULL || MinorRevision == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *MajorRevision = 1;
  *MinorRevision = 0;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
GetArmTrngUuid (
  OUT GUID  *Guid
  )
{
  //
  // Optional — no UUID for a software jitter source.
  //
  return RETURN_UNSUPPORTED;
}

UINTN
EFIAPI
GetArmTrngMaxSupportedEntropyBits (
  VOID
  )
{
  //
  // We can fill arbitrary buffers, but cap at 256 bits (32 bytes)
  // per call to keep latency reasonable.
  //
  return 256;
}

RETURN_STATUS
EFIAPI
GetArmTrngEntropy (
  IN  UINTN  EntropyBits,
  IN  UINTN  BufferSize,
  OUT UINT8  *Buffer
  )
{
  UINTN  BytesNeeded;
  UINTN  i;

  if (Buffer == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  BytesNeeded = (EntropyBits + 7) / 8;

  if (BufferSize < BytesNeeded) {
    return RETURN_BAD_BUFFER_SIZE;
  }

  if (EntropyBits > GetArmTrngMaxSupportedEntropyBits ()) {
    return RETURN_UNSUPPORTED;
  }

  ZeroMem (Buffer, BufferSize);

  for (i = 0; i < BytesNeeded; i++) {
    Buffer[i] = CollectJitterByte ();
  }

  return RETURN_SUCCESS;
}
