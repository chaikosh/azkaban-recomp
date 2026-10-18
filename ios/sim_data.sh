#!/bin/sh
# Put the game data into the simulator app's Documents folder, as Finder file sharing does on a
# device.  Files are APFS clones (cp -c): no extra disk space.  The app must be installed.
#   ios/sim_data.sh [booted|<udid>] [--disc] [--memcard <file>]
#     --disc      also the extracted disc/ files (only read with HP_IOP=hle)
#     --memcard   start from a copy of this memory card image
# HP_ISO overrides the ISO (default: iso/Harry Potter and the Prisoner of Azkaban (USA).iso).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SIM=${1:-booted}
[ $# -gt 0 ] && shift
ISO=${HP_ISO:-$ROOT/iso/Harry Potter and the Prisoner of Azkaban (USA).iso}
[ -f "$ISO" ] || { echo "no ISO at $ISO" >&2; exit 1; }
DOCS="$(xcrun simctl get_app_container "$SIM" com.chaikosh.hp3 data)/Documents"
mkdir -p "$DOCS"
NAME=$(basename "$ISO")
[ -f "$DOCS/$NAME" ] || cp -c "$ISO" "$DOCS/$NAME"
while [ $# -gt 0 ]; do
    case "$1" in
    --disc) [ -d "$DOCS/disc" ] || cp -c -R "$ROOT/disc/" "$DOCS/disc" ;;
    --memcard) shift; cp "$1" "$DOCS/memcard0.ps2" ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift
done
ls -la "$DOCS"
