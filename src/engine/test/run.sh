#!/bin/sh
# Simulator tests for the C engine + trampoline (bfin-elf-run in Docker).
#   ENGINE=params src/engine/test/run.sh > samples   stdout: samples from src/poc/test/harness.c driving _render
#                                      stderr: harness guard/phase line, then the ABI check result
# Regression: ENGINE=params reproduces src/poc/params.S, so its samples must equal
#   src/poc/test/run.sh params "--defsym _iter_buf=0"   (see notes/LOG.md, CENGINE entry).
set -e
ENGINE=${ENGINE:-params}
DIR=$(cd "$(dirname "$0")/.." && pwd)
POC=$(cd "$DIR/../poc" && pwd)
DATA=$(mktemp -d)
DATA_O=
ASM_O=
[ -f "$DIR/${ENGINE}_asm.S" ] && ASM_O=asm.o
if [ -f "$DIR/gen_${ENGINE}_data.py" ]; then     # engine data blob (see build.sh)
  python3 "$DIR/gen_${ENGINE}_data.py" "$DATA/engine_data.bin"
  printf '.section .rodata\n.align 4\n.global _engine_data\n_engine_data:\n.incbin "/data/engine_data.bin"\n' > "$DATA/data.S"
  DATA_O=data.o
fi
docker run --rm --platform linux/amd64 -v "$DIR":/src:ro -v "$POC/test":/poc:ro -v "$DATA":/data:ro op1-bfin sh -c "
  set -e
  cd /tmp
  if [ -n \"$DATA_O\" ]; then bfin-elf-as -o data.o /data/data.S; fi
  if [ -n \"$ASM_O\" ]; then bfin-elf-as -o asm.o /src/${ENGINE}_asm.S; fi
  bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror -c -o engine.o /src/$ENGINE.c
  bfin-elf-as -o tramp.o /src/tramp.S
  bfin-elf-gcc -std=gnu99 -msim -O1 -o h /poc/harness.c tramp.o engine.o $ASM_O $DATA_O
  bfin-elf-gcc -std=gnu99 -msim -O1 -o abi /src/test/abi_main.c /src/test/abi_check.S tramp.o engine.o $ASM_O $DATA_O
  bfin-elf-run h
  bfin-elf-run abi >&2"
