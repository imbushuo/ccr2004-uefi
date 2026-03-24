#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

bash "${SCRIPT_DIR}/fd2elf.sh" "$@"
bash "${SCRIPT_DIR}/SpiLoader/build_spiloader.sh" "$@"
