#!/bin/bash
# Compile device tree source to DTB for inclusion in firmware volume.
# Called as PREBUILD from CCR2004.dsc.

set -e

DTS_DIR="edk2-platforms/Platform/MikroTik/CCR2004/Resources/DeviceTree"
DTS="${DTS_DIR}/CCR2004-1G-2XS-PCIe.dts"
DTB="${DTS_DIR}/CCR2004-1G-2XS-PCIe.dtb"

if [ ! -f "$DTS" ]; then
    echo "ERROR: $DTS not found" >&2
    exit 1
fi

# Only rebuild if DTS is newer than DTB
if [ ! -f "$DTB" ] || [ "$DTS" -nt "$DTB" ]; then
    echo "DTC: $DTS -> $DTB"
    dtc -I dts -O dtb -o "$DTB" "$DTS" 2>/dev/null
fi
