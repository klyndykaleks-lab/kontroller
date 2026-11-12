#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  TOOLCHAIN=/Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin
  PATH="$TOOLCHAIN:$PATH"
  export PATH
fi
command -v arm-none-eabi-gcc >/dev/null
make -B -j4 -C "$ROOT/blackpill-f411-controller/Debug" all
