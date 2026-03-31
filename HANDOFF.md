# Work-in-Progress Handoff — MikroTik CCR2004 UEFI Firmware

**Date:** 2026-03-31
**Build status:** Both UEFI FD and SpiLoader compile cleanly (`-b DEBUG -t GCC`)

## Build Command

```bash
source setup.sh && source edk2/edksetup.sh
build -a AARCH64 -t GCC -p Platform/MikroTik/CCR2004/CCR2004.dsc -b DEBUG
```

- Toolchain is `GCC` (not `GCC5` — renamed in this EDK2 version).
- `setup.sh` sets `WORKSPACE`, `PACKAGES_PATH`, and `GCC_AARCH64_PREFIX`.
- SpiLoader is built as a postbuild step via `build_spiloader.sh`.

## Build Artifacts

| Artifact | Path | Size |
|---|---|---|
| UEFI FD | `Build/CCR2004/DEBUG_GCC/FV/CCR2004.fd` | 3.5 MB |
| UEFI ELF | `Build/CCR2004/DEBUG_GCC/FV/CCR2004.elf` | (for symbol debug) |
| SpiLoader binary | `Build/CCR2004/DEBUG_GCC/FV/SpiLoader.bin` | 12,476 bytes (limit: 65,124) |
| SpiLoader ELF | `Build/CCR2004/DEBUG_GCC/FV/SpiLoader.elf` | (for symbol debug) |

## What This Session Did

### 1. 4GB DRAM Memory Map (PlatformLibMem.c)

Added the 4th GB of DRAM at `0x200000000` (1 GB). The DTS defines three memory regions:
- `0x0_00000000` — 2 GB
- `0x0_80000000` — 1 GB
- `0x2_00000000` — 1 GB (newly added)

The 32-bit range `0xC0000000–0xFFFFFFFF` is SoC peripheral MMIO, so the last GB goes above the 4 GB mark. `MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS` bumped from 9 to 10.

### 2. UEFI Debug Level Change (CCR2004.dsc)

`PcdDebugPrintErrorLevel` changed from `0x80000002` (ERROR+WARN) to `0x80000042` (ERROR+WARN+INFO).

### 3. SpiLoader Exception Handling

New files: `Exception.c`, `ExceptionVector.S`

- AArch64 exception vector table (2 KB aligned, all 16 vectors)
- Catches all exceptions: Sync, IRQ, FIQ, SError at all EL combinations
- Detects EL at runtime (EL1/EL2/EL3) for correct system register access
- Prints: ESR (decoded exception class name), ELR, FAR, SPSR, full X0–X30 dump, SP
- Frame-pointer backtrace (enabled via `-fno-omit-frame-pointer`)
- VBAR installed in `Entry.S` before calling `SpiloaderMain`
- SError unmasked (`daifclr, #4`) so async bus faults are caught

### 4. SpiLoader Watchdog Timer (Watchdog.c)

Minimal GICv3 + ARM EL2 hypervisor physical timer (CNTHP, INTID 26):
- Wakes GIC redistributor, configures PPI 26 as Group 1 NS (IRQ delivery)
- Arms 5-second countdown before ELF load
- If timer fires (bus-level hang), IRQ vector catches it, dumps SPI controller registers
- `WatchdogDisarm()` masks all DAIF, disables timer, disables GIC Group 1, clears pending PPI

**Note:** The watchdog timer IRQ did NOT fire during testing because the bus-level hang prevents the CPU from retiring instructions to take the interrupt. The stall is confirmed at ~576 KB into the SPI flash read (CPU freezes on an MMIO or DRAM bus transaction).

### 5. SpiLoader PCIe EP Init (Main.c)

Hand-written bare-metal PCIe EP initialization, ported from `PcieEpTestApp.c`:
- SERDES init (PBS rev detection, lane config, PLL)
- 1 PF, VID:DID = 19AA:961F, class = Network Controller
- BAR0: 4 KB MMIO, 32-bit, non-prefetchable; BAR1–5 + Expansion ROM disabled
- Command register: Memory Space=1, I/O Space=0, Bus Master=0, Interrupt Disable=1
- Interrupt Pin cleared, MSI/MSI-X disabled
- iATU inbound: BAR0 → SHMEM at `0x10000000`
- SHMEM initialized: magic `0xCAFEF00D`, VID:DID, BAR size, firmware version

Boot sequence: UART → exception vectors → SError unmask → **PCIe EP init** → SPI init → JEDEC → watchdog arm → ELF load → watchdog disarm → jump to UEFI.

### 6. Alpine HAL PCIe Port (al_init_pcie.c)

Ported `services/pcie/al_init_pcie.c` from `~/sources/unvr/alpine_hal` to **both**:

**AlpineHalLib** (UEFI library):
- Added to `AlpineHalLib.inf`: PCIe drivers, sys_fabric drivers, PBS utils, addr_map, PCIe init service
- New include paths: `include/pcie/`, `include/sys_fabric/`, `include/serdes/`, `drivers/pcie/`, `drivers/sys_fabric/`, `services/pcie/`
- `AL_DEV_ID=AL_DEV_ID_ALPINE_V2` defined in `al_hal_plat_types.h`
- Real `al_hal_pbs_utils.c` included (reads device ID from hardware)

**SpiLoader HAL** (reorganized from flat `hal/` into folders):
```
hal/
  platform/          plat_types.h, plat_services.h, al_hal_iomap.h
  include/
    common/          al_hal_common.h, types.h, reg_utils.h
    pbs/             pbs_utils.h, pbs_regs.h, addr_map.h, sbus.h
    pcie/            al_hal_pcie.h, al_hal_pcie_interrupts.h
    iofic/           al_hal_iofic.h, al_hal_iofic_regs.h
    sys_fabric/      sys_fabric_utils.h, nb_regs.h, nb_regs_v1_v2.h, anpa_regs.h, pasw.h
    io_fabric/       unit_adapter_regs.h
    serdes/          all serdes interface/init headers
  drivers/
    spi/             al_hal_spi.c/.h
    pbs/             al_hal_pbs_stubs.c, al_hal_addr_map.c
    pcie/            al_hal_pcie.c, interrupts, reg_ptr_set_rev1/2/3, regs headers
    sys_fabric/      sys_fabric_utils.c, _v1_v2.c
  services/
    pcie/            al_init_pcie.c/.h, debug, params
```

Platform shims added: `al_phys_addr_t`, `al_memset`, `al_memcpy`, `al_memcmp`, `al_strcmp`. Proprietary `al_init_pcie_debug_ex.h` stubbed as empty header.

The HAL PCIe code **compiles and links** but is not yet wired into `Main.c` — the hand-written EP init is still used. The HAL's `al_init_pcie()` is available for the next step of replacing the manual init.

## Active Investigation: PCIe EP Bus Hang

**Problem:** When the CCR2004 board has its PCIe EP port connected to a host, the SpiLoader stalls during the SPI flash ELF load. The stall is deterministic at ~576 KB into the read.

**Findings:**
- The last UART output is a **partial hex character** (`0x0▒`), proving the CPU is frozen mid-instruction on a bus transaction
- Timer interrupt (GICv3 CNTHP) does NOT preempt — confirms a true bus-level hang where the CPU pipeline stalls on a load/store that never completes
- SError does not fire — not an async external abort
- The hand-written PCIe EP init was added to SpiLoader to properly configure the EP before the host starts sending TLPs, to see if that prevents the hang
- **This has not been tested yet** with the combined EP init + SPI load

**Next steps for this investigation:**
1. Test current build (EP init before SPI load) to see if properly configured EP prevents the hang
2. If still hanging: use the HAL `al_init_pcie()` for more robust EP initialization (link training, etc.)
3. Consider: the hang may require disabling inbound TLPs during SPI load, or configuring the NB fabric to prioritize SPI bus access

## Key Files Modified This Session

| File | Change |
|---|---|
| `Library/PlatformLib/PlatformLibMem.c` | Added 4th GB DRAM at 0x200000000 |
| `CCR2004.dsc` | Debug level → 0x80000042 (INFO) |
| `SpiLoader/Entry.S` | VBAR install |
| `SpiLoader/Main.c` | PCIe EP init, checkpoints, watchdog, SError unmask |
| `SpiLoader/Exception.c` | Exception handler with ESR decode + backtrace |
| `SpiLoader/ExceptionVector.S` | AArch64 vector table |
| `SpiLoader/Watchdog.c` | GICv3 + CNTHP timer watchdog + SPI register dump |
| `SpiLoader/SpiFlash.c` | Reverted to clean (no debug progress) |
| `SpiLoader/ElfLoader.c` | Reverted to clean (no debug progress) |
| `SpiLoader/spiloader.h` | EXCEPTION_FRAME, watchdog/dump declarations, GIC addresses |
| `SpiLoader/spiloader.ld` | .text.exceptions section with 2 KB alignment |
| `SpiLoader/build_spiloader.sh` | Reorganized HAL paths, new source files, `-fno-omit-frame-pointer` |
| `SpiLoader/hal/platform/al_hal_plat_types.h` | AL_DEV_ID, al_phys_addr_t, extra errno values |
| `SpiLoader/hal/platform/al_hal_plat_services.h` | al_memset, al_memcpy, al_memcmp, al_strcmp |
| `Library/AlpineHalLib/AlpineHalLib.inf` | New PCIe/sys_fabric/pbs sources + include paths |
| `Library/AlpineHalLib/al_hal_plat_types.h` | AL_DEV_ID defines |

## Upstream HAL Source

PCIe HAL files are copied from: `~/sources/unvr/alpine_hal/` (services/pcie/, drivers/pcie/, drivers/pbs/, drivers/sys_fabric/, and corresponding include/ directories).
