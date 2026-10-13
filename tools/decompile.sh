#!/bin/sh
# Decompile functions from the local Ghidra project.  Usage: tools/decompile.sh <program> <addr>...
#   e.g. tools/decompile.sh te-boot_230.elf 0x1068
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)
GHIDRA=${GHIDRA:-$HOME/tools/ghidra_12.1.3_PUBLIC}
PROG=$1; shift
"$GHIDRA/support/analyzeHeadless" "$ROOT/firmware/ghidra" op1 -process "$PROG" -noanalysis -readOnly \
  -scriptPath "$HERE/ghidra_scripts" -postScript Decompile.java "$@" 2>&1 \
  | awk '/^INFO .*====|^====/{p=1} p' | grep -v '^INFO \|^ (HeadlessAnalyzer)\|^WARN'
