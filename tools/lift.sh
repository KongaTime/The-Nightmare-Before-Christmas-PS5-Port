#!/usr/bin/env bash
# Generate the recompiled game C from the user's own disc. WSL Ubuntu only.
# Usage: tools/lift.sh [workdir]   (defaults to ~/oogie-work)
set -euo pipefail
WORK="${1:-$HOME/oogie-work}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XR="$WORK/xboxrecomp"
XBE="$WORK/game/default.xbe"
SEEDS="$ROOT/upstream/scripts/icall_seeds.json"
MANUAL="$ROOT/upstream/port/src/recomp_manual.c"

[ -f "$XBE" ] || { echo "missing $XBE (extract default.xbe from your disc first)"; exit 1; }
[ -d "$XR" ] || { echo "missing $XR (clone sp00nznet/xboxrecomp at eb0aa1d)"; exit 1; }

mkdir -p "$WORK/build_out" "$WORK/logs" "$WORK/gen"
cd "$XR"
python3 -m tools.xbe_parser "$XBE" --json "$WORK/game/default_analysis.json"
python3 -m tools.disasm "$XBE" --analysis-json "$WORK/game/default_analysis.json" \
  -o "$WORK/build_out/disasm" --force --seed-functions "$SEEDS"
python3 -m tools.func_id "$XBE" --functions "$WORK/build_out/disasm/functions.json" \
  --strings "$WORK/build_out/disasm/strings.json" --xrefs "$WORK/build_out/disasm/xrefs.json" \
  -o "$WORK/build_out/func_id"
python3 -m tools.abi_analysis "$XBE" --disasm-dir "$WORK/build_out/disasm" \
  --func-id-dir "$WORK/build_out/func_id" --output-dir "$WORK/build_out/abi"
python3 "$ROOT/upstream/scripts/oogie_recomp.py" "$XBE" --all --split 1000 \
  --game-name OogiesRevenge --disasm-dir "$WORK/build_out/disasm" \
  --func-id-dir "$WORK/build_out/func_id" --abi-dir "$WORK/build_out/abi" \
  --exclude-manual "$MANUAL" --gen-dir "$WORK/gen" -o "$WORK/build_out/recomp"
echo "lift done: $(ls "$WORK/gen" | wc -l) generated files in $WORK/gen"
