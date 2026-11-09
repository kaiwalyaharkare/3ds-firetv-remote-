#!/usr/bin/env bash
# Package the app ELF as an installable .cia (shows up on the HOME Menu).
# Usage: make_cia.sh <app.elf> <out.cia>
# Needs `makerom` (github.com/3DSGuy/Project_CTR) and `bannertool`
# (github.com/diasurgical/bannertool) on PATH, or set MAKEROM / BANNERTOOL.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ELF="$1"; OUT="$2"
MAKEROM="${MAKEROM:-makerom}"
BANNERTOOL="${BANNERTOOL:-bannertool}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

VERSION="${APP_VERSION:-1.0.0}"
IFS=. read -r MAJOR MINOR MICRO <<<"$VERSION"

"$BANNERTOOL" makebanner -i "$HERE/banner.png" -a "$HERE/banner.wav" -o "$TMP/banner.bnr" >/dev/null
"$BANNERTOOL" makesmdh -s "FireTV Remote" -l "Control a Fire TV over Wi-Fi (ADB)" -p "Kai" \
    -i "$HERE/../icon.png" -o "$TMP/icon.icn" >/dev/null

"$MAKEROM" -f cia -o "$OUT" -elf "$ELF" -rsf "$HERE/app.rsf" -target t -exefslogo \
    -icon "$TMP/icon.icn" -banner "$TMP/banner.bnr" \
    -major "$MAJOR" -minor "${MINOR:-0}" -micro "${MICRO:-0}" \
    -DAPP_TITLE="FireTV Remote" -DAPP_PRODUCT_CODE="CTR-H-FTVR" -DAPP_UNIQUE_ID=0xF7A3E \
    -DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=Legacy -DAPP_CATEGORY=Application \
    -DAPP_USE_ON_SD=true -DAPP_MEMORY_TYPE=Application -DAPP_CPU_SPEED=804MHz \
    -DAPP_ENABLE_L2_CACHE=true -DAPP_VERSION_MAJOR="$MAJOR" -DAPP_ENCRYPTED=false
echo "Built $OUT"
