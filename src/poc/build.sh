#!/bin/sh
# Build a proof-of-life render (assembly) linked at a fixed address. Runs the toolchain in Docker.
# Firmware symbols (fw 246): _pitch_buf = 0xFF801950 (per-voice int32[128] phase increments),
#   _iter_buf = 0x01DD4E18 (iter-private 256 B, zeroed when iter is selected).
# Canary regions (canary.S, 1 MB each, filled by ldr_patch.py --fill): _canary0..3.
#   src/poc/build.sh <name> <hex_addr> <outdir>   e.g. src/poc/build.sh silence 0x019B99D4 firmware/build
set -e
NAME=$1; ADDR=$2; OUT=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
docker run --rm --platform linux/amd64 -v "$HERE":/src:ro -v "$OUT":/out op1-bfin sh -c "
  set -e
  bfin-elf-as -I /src -o /out/$NAME.o /src/$NAME.S
  bfin-elf-ld -nostdlib -e _render -Ttext=$ADDR --defsym=_pitch_buf=0xFF801950 --defsym=_iter_buf=0x01DD4E18 --defsym=_canary0=0x00800000 --defsym=_canary1=0x01E10000 --defsym=_canary2=0x02C00000 --defsym=_canary3=0x03800000 -o /out/$NAME.elf /out/$NAME.o
  bfin-elf-objcopy -O binary -j .text /out/$NAME.elf /out/$NAME.bin
  bfin-elf-objdump -d /out/$NAME.elf
  bfin-elf-nm /out/$NAME.elf
"
