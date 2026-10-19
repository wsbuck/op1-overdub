#!/bin/sh
# Cycles of bench.c in the Blackfin simulator (the op1-bfin Docker image, as src/slot/test/fxsim.py uses).
set -e
cd "$(dirname "$0")"
docker run --rm --platform linux/amd64 -v "$PWD:/src:ro" op1-bfin sh -c \
  'cd /tmp && for d in -DNAIVE -DSTAGED; do bfin-elf-gcc -std=gnu99 -msim -mcpu=bf524 -O2 -Wall $d -o b /src/bench.c -lm && echo "$d:" && bfin-elf-run b; done'
