#!/usr/bin/env bash
# Extract the player's own disc image into a folder install.py can upload.
#
#   tools/extract-disc.sh "/mnt/c/.../Oogie's Revenge (USA).iso" ~/oogie-work/dump
#
# Uses upstream's own read-only XDVDFS reader (upstream/tools/xiso_tool.py), so
# no extra tooling is needed. Never writes to the image. The result is the Xbox
# disc's own tree: default.xbe at the root and the game's data/ media/ sound/
# folders beside it, which is what the guest sees as D:\.
set -euo pipefail

ISO=${1:-}
OUT=${2:-$HOME/oogie-work/dump}

if [ -z "$ISO" ]; then
    echo "usage: $0 <disc.iso> [out-dir]" >&2
    exit 2
fi
[ -f "$ISO" ] || { echo "no such image: $ISO" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$HERE/upstream/tools/xiso_tool.py"
[ -f "$TOOL" ] || { echo "missing $TOOL" >&2; exit 2; }

# The dump is most of the disc, so say what it will cost before starting.
need=$(stat -c %s "$ISO")
avail=$(df --output=avail -B1 "$(dirname "$OUT")" 2>/dev/null | tail -1 || echo 0)
echo "image $(numfmt --to=iec "$need")  free $(numfmt --to=iec "$avail" 2>/dev/null || echo '?')"
if [ "$avail" -lt "$need" ] 2>/dev/null; then
    echo "not enough free space for the dump" >&2
    exit 1
fi

mkdir -p "$OUT"
python3 "$TOOL" extractall "$ISO" "$OUT"

# The title looks for default.xbe first, so its absence means the extraction
# did not land the way the port expects.
[ -f "$OUT/default.xbe" ] || { echo "no default.xbe in $OUT" >&2; exit 1; }
echo "==> $OUT: $(du -sh "$OUT" | cut -f1), $(find "$OUT" -type f | wc -l) files"
