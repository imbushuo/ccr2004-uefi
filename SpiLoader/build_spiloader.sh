#!/bin/bash

set -euo pipefail

while [ $# -gt 0 ]; do
  case "$1" in
    -b) BUILD_TARGET="$2"; shift 2 ;;
    -t) TOOLCHAIN="$2"; shift 2 ;;
    -a) shift 2 ;;
    -p) shift 2 ;;
    *) shift ;;
  esac
done

: "${WORKSPACE:?WORKSPACE must be set by EDK2 build env}"

BUILD_TARGET="${BUILD_TARGET:-RELEASE}"
TOOLCHAIN="${TOOLCHAIN:-GCC}"
PREFIX="${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}"
CC="${PREFIX}gcc"
LD="${PREFIX}ld"
OBJCOPY="${PREFIX}objcopy"
OBJDUMP="${PREFIX}objdump"
SIZE="${PREFIX}size"

SRC_DIR="${WORKSPACE}/edk2-platforms/Platform/MikroTik/CCR2004/SpiLoader"
OUT_DIR="${WORKSPACE}/Build/CCR2004/${BUILD_TARGET}_${TOOLCHAIN}/FV/SpiLoader"
MAP_FILE="${OUT_DIR}/SpiLoader.map"
ELF_FILE="${OUT_DIR}/SpiLoader.elf"
BIN_FILE="${OUT_DIR}/SpiLoader.bin"

mkdir -p "${OUT_DIR}"

CFLAGS=(
  -c
  -Os
  -g0
  -ffreestanding
  -fno-builtin
  -fno-common
  -fno-stack-protector
  -fno-omit-frame-pointer
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -ffunction-sections
  -fdata-sections
  -fno-pic
  -fno-pie
  -Wall
  -Wextra
  -Werror
  -std=c11
  -mcpu=cortex-a72
  -mgeneral-regs-only
  -I"${SRC_DIR}"
  -I"${SRC_DIR}/hal/platform"
  -I"${SRC_DIR}/hal/include/common"
  -I"${SRC_DIR}/hal/include/pbs"
  -I"${SRC_DIR}/hal/include/pcie"
  -I"${SRC_DIR}/hal/include/iofic"
  -I"${SRC_DIR}/hal/include/sys_fabric"
  -I"${SRC_DIR}/hal/include/io_fabric"
  -I"${SRC_DIR}/hal/include/serdes"
  -I"${SRC_DIR}/hal/drivers/spi"
  -I"${SRC_DIR}/hal/drivers/pbs"
  -I"${SRC_DIR}/hal/drivers/pcie"
  -I"${SRC_DIR}/hal/drivers/sys_fabric"
  -I"${SRC_DIR}/hal/services/pcie"
  -include al_hal_plat_types.h
  -include al_hal_plat_services.h
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-sign-compare
  -Wno-unused-function
  -Wno-unused-variable
  -Wno-unused-but-set-variable
)

SFLAGS=(
  -c
  -Os
  -g0
  -ffreestanding
  -fno-pic
  -fno-pie
  -mcpu=cortex-a72
)

LDFLAGS=(
  -nostdlib
  -T "${SRC_DIR}/spiloader.ld"
  -Map "${MAP_FILE}"
  --gc-sections
  --build-id=none
)

SOURCES_C=(
  Main.c
  Support.c
  SpiFlash.c
  ElfLoader.c
  Exception.c
  Watchdog.c
  hal/drivers/spi/al_hal_spi.c
  hal/drivers/pbs/al_hal_pbs_stubs.c
  hal/drivers/pbs/al_hal_addr_map.c
  hal/drivers/pcie/al_hal_pcie.c
  hal/drivers/pcie/al_hal_pcie_interrupts.c
  hal/drivers/pcie/al_hal_pcie_reg_ptr_set_rev1.c
  hal/drivers/pcie/al_hal_pcie_reg_ptr_set_rev2.c
  hal/drivers/pcie/al_hal_pcie_reg_ptr_set_rev3.c
  hal/drivers/sys_fabric/al_hal_sys_fabric_utils.c
  hal/drivers/sys_fabric/al_hal_sys_fabric_utils_v1_v2.c
  hal/services/pcie/al_init_pcie.c
  hal/services/pcie/al_init_pcie_debug.c
)

SOURCES_S=(
  Entry.S
  ExceptionVector.S
)

OBJECTS=()

for SRC in "${SOURCES_C[@]}"; do
  OBJ_NAME="$(echo "${SRC}" | sed 's|/|_|g; s|\.c$|.o|')"
  OBJ="${OUT_DIR}/${OBJ_NAME}"
  "${CC}" "${CFLAGS[@]}" -o "${OBJ}" "${SRC_DIR}/${SRC}"
  OBJECTS+=("${OBJ}")
done

for SRC in "${SOURCES_S[@]}"; do
  OBJ="${OUT_DIR}/${SRC%.S}.o"
  "${CC}" "${SFLAGS[@]}" -o "${OBJ}" "${SRC_DIR}/${SRC}"
  OBJECTS+=("${OBJ}")
done

"${LD}" "${LDFLAGS[@]}" -o "${ELF_FILE}" "${OBJECTS[@]}"
"${OBJCOPY}" -O binary "${ELF_FILE}" "${BIN_FILE}"

BIN_SIZE=$(stat -c "%s" "${BIN_FILE}")
if [ "${BIN_SIZE}" -ge "${SPILOADER_MAX_EXEC_SIZE:-65124}" ]; then
  echo "ERROR: SpiLoader.bin is ${BIN_SIZE} bytes, limit is 65124 bytes" >&2
  exit 1
fi

"${SIZE}" "${ELF_FILE}"
"${OBJDUMP}" -h "${ELF_FILE}" > "${OUT_DIR}/SpiLoader.sections.txt"

cp "${ELF_FILE}" "${WORKSPACE}/Build/CCR2004/${BUILD_TARGET}_${TOOLCHAIN}/FV/SpiLoader.elf"
cp "${BIN_FILE}" "${WORKSPACE}/Build/CCR2004/${BUILD_TARGET}_${TOOLCHAIN}/FV/SpiLoader.bin"

echo "SpiLoader ELF: ${ELF_FILE}"
echo "SpiLoader BIN: ${BIN_FILE} (${BIN_SIZE} bytes)"
