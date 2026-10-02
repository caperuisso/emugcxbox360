#!/bin/sh
# Builds tests/gc_c/*.c into DOLs with the libxenon cross compiler in 32-bit Gekko mode.
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
CC=${CC:-xenon-gcc}
OBJCOPY=${OBJCOPY:-xenon-objcopy}
HERE=$(dirname "$0")
OUT=${1:-build}
mkdir -p "$OUT"
FLAGS="-m32 -mcpu=750 -mno-altivec -O2 -ffreestanding -nostdlib -fno-pic -msdata=none -G 0 -fno-builtin"
for src in "$HERE"/*.c; do
  name=$(basename "$src" .c)
  $CC $FLAGS -T "$HERE/gc.ld" "$HERE/crt0.S" "$src" -o "$OUT/$name.elf"
  $OBJCOPY -O binary "$OUT/$name.elf" "$OUT/$name.bin"
  python3 "$HERE/../bin2dol.py" "$OUT/$name.bin" "$OUT/$name.dol" 0x80003100
done
