#!/bin/bash
# scripts/fetch_fonts.sh — Download Noto Sans fonts for Phase 7.2
#
# Downloads NotoSans-Regular.ttf and NotoSansMono-Regular.ttf from the
# notofonts GitHub repository and places them in assets/fonts/.
# make_disk.sh then copies them to /fonts/ on the FAT32 disk image.
#
# Run once:  scripts/fetch_fonts.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FONTS_DIR="${SCRIPT_DIR}/../assets/fonts"

mkdir -p "${FONTS_DIR}"

BASE_URL="https://github.com/notofonts/noto-fonts/raw/main/hinted/ttf"

fetch_font() {
    local name="$1"
    local subdir="$2"
    local dest="${FONTS_DIR}/${name}"

    if [ -f "${dest}" ] && [ "$(wc -c < "${dest}")" -gt 50000 ]; then
        echo "[fetch_fonts] ${name} already present ($(wc -c < "${dest}") bytes)"
        return
    fi
    echo "[fetch_fonts] Downloading ${name}..."
    curl -fL -o "${dest}" "${BASE_URL}/${subdir}/${name}"
    echo "[fetch_fonts] ${name}: $(wc -c < "${dest}") bytes"
}

fetch_font "NotoSans-Regular.ttf"      "NotoSans"
fetch_font "NotoSansMono-Regular.ttf"  "NotoSansMono"

echo "[fetch_fonts] Fonts ready in assets/fonts/"
echo "[fetch_fonts] Run scripts/make_disk.sh to copy them to /fonts/ on disk."
