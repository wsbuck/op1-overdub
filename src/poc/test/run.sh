#!/bin/sh
# Run a proof-of-life render in the GNU Blackfin simulator.
#   Usage: src/poc/test/run.sh <name> [as --defsym ...] [gcc -D ...]  (> samples)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --platform linux/amd64 -v "$HERE":/src:ro op1-bfin sh -c "
  cp /src/$1.S /tmp/r.S
  bfin-elf-as -I /src $2 -o /tmp/r.o /tmp/r.S
  bfin-elf-gcc -std=gnu99 -msim -O1 $3 -o /tmp/h /src/test/harness.c /tmp/r.o && bfin-elf-run /tmp/h"
