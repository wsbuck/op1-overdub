#!/bin/sh
# Build the C engine + trampoline as one flat block linked at a fixed address. Runs the toolchain in Docker.
#   [ENGINE=va] src/engine/build.sh <hex_addr> <outdir> [max_bytes]   (ENGINE = src/engine/<name>.c)
#   e.g. src/engine/build.sh 0x019B99D4 firmware/build          (in place in iter's render, 438 B max)
#        src/engine/build.sh 0x01E10000 firmware/build 0x100000 (free SDRAM region 1, see ldr_patch.py FREE)
# Output: <outdir>/engine.bin (raw block for tools/ldr_patch.py), engine.elf, disassembly on stdout.
# -nostdlib makes any libgcc/libc call an undefined-symbol link error; engine.ld rejects .data/.bss and
# oversize code. If src/engine/gen_<ENGINE>_data.py exists, its output is linked in as const _engine_data
# (e.g. wavetables); it is generated at build time, not committed.
set -e
ADDR=$1; OUT=$(cd "$2" && pwd); MAX=${3:-438}; ENGINE=${ENGINE:-va}
HERE=$(cd "$(dirname "$0")" && pwd)
rm -f "$OUT/engine_data.bin" "$OUT/data.o" "$OUT/asm.o"
DATA_O=
ASM_O=
[ -f "$HERE/${ENGINE}_asm.S" ] && ASM_O=/out/asm.o     # optional hand-written assembly for the engine
if [ -f "$HERE/gen_${ENGINE}_data.py" ]; then
  python3 "$HERE/gen_${ENGINE}_data.py" "$OUT/engine_data.bin"
  DATA_O=/out/data.o
fi
docker run --rm --platform linux/amd64 -v "$HERE":/src:ro -v "$OUT":/out op1-bfin sh -c "
  set -e
  bfin-elf-gcc -std=gnu99 -mcpu=bf524 -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -c -o /out/engine.o /src/$ENGINE.c
  bfin-elf-as -o /out/tramp.o /src/tramp.S
  if [ -n \"$ASM_O\" ]; then bfin-elf-as -o /out/asm.o /src/${ENGINE}_asm.S; fi
  if [ -n \"$DATA_O\" ]; then
    printf '.section .rodata\\n.align 4\\n.global _engine_data\\n_engine_data:\\n.incbin \"/out/engine_data.bin\"\\n' > /out/data.S
    bfin-elf-as -o /out/data.o /out/data.S
  fi
  bfin-elf-ld -nostdlib -T /src/engine.ld -Ttext=$ADDR --defsym=SLOT_SIZE=$MAX -e _render --defsym=_pitch_buf=0xFF801950 --defsym=_prof_stats=0x00800000 --defsym=_voice_ram=0x00810000 --defsym=_te_engine_ids=0xFF8016E4 --defsym=_te_ssamp=0x01DF6F78 --defsym=_te_knobs=0xFF8013C4 --defsym=_te_synth_slot=0x01AAAC0C $ENGINE_DEFSYMS \
    -o /out/engine.elf /out/tramp.o /out/engine.o $ASM_O $DATA_O
  bfin-elf-objcopy -O binary -j .text /out/engine.elf /out/engine.bin
  bfin-elf-objdump -d /out/engine.elf
  bfin-elf-nm -n /out/engine.elf
  bfin-elf-size -A /out/engine.elf
"
