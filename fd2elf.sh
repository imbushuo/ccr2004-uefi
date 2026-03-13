#!/bin/bash
# Wrap CCR2004.fd into a single-segment AArch64 ELF with entry at 0x01100000.
# Called automatically as a POSTBUILD step from the DSC.
# EDK2 passes: -b <target> -t <toolchain> -a <arch> -p <dsc>

set -e

# Parse EDK2 postbuild arguments
while [ $# -gt 0 ]; do
  case "$1" in
    -b) BUILD_TARGET="$2"; shift 2 ;;
    -t) TOOLCHAIN="$2"; shift 2 ;;
    -a) shift 2 ;;
    -p) shift 2 ;;
    *)  shift ;;
  esac
done

LOAD_ADDR=0x01100000
FD="${WORKSPACE}/Build/CCR2004/${BUILD_TARGET}_${TOOLCHAIN}/FV/CCR2004.fd"
ELF="${WORKSPACE}/Build/CCR2004/${BUILD_TARGET}_${TOOLCHAIN}/FV/CCR2004.elf"
PREFIX="${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}"

if [ ! -f "$FD" ]; then
  echo "ERROR: $FD not found" >&2
  exit 1
fi

# Create a minimal linker script that places the FD blob at LOAD_ADDR
LDSCRIPT=$(mktemp /tmp/ccr2004-ld.XXXXXX)
trap "rm -f '${LDSCRIPT}' '${FD}.o'" EXIT

cat > "$LDSCRIPT" <<EOF
ENTRY(_start)
SECTIONS
{
  . = ${LOAD_ADDR};
  .data : { *(.data) }
}
EOF

# Wrap the raw FD binary into an object file
${PREFIX}objcopy \
  -I binary \
  -O elf64-littleaarch64 \
  -B aarch64 \
  --rename-section .data=.data,alloc,load,contents \
  --set-start ${LOAD_ADDR} \
  "$FD" "${FD}.o"

# Link into an ELF with _start at the load address
${PREFIX}ld \
  -T "$LDSCRIPT" \
  -o "$ELF" \
  --defsym=_start=${LOAD_ADDR} \
  "${FD}.o"

echo "ELF image: $ELF (load/entry @ ${LOAD_ADDR})"
