#!/bin/bash
# scripts/update_disk.sh — Incrementally update files in an existing disk.img
#
# Use this instead of make_disk.sh when you want to refresh assets (fonts,
# icons, wallpaper, app manifests) WITHOUT recreating the image from scratch.
# Your runtime changes (config/, home/, var/, etc.) are preserved.
#
# Requires: mtools (brew install mtools)
# Usage:    scripts/update_disk.sh [--fonts] [--icons] [--wallpaper] [--apps] [--all]
#           (no flags = --all)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
DISK="${BUILD_DIR}/disk.img"

if [ ! -f "${DISK}" ]; then
    echo "[UPDATE] ERROR: ${DISK} does not exist. Run scripts/make_disk.sh first."
    exit 1
fi

# Parse flags
DO_FONTS=0; DO_ICONS=0; DO_WALLPAPER=0; DO_APPS=0
for arg in "$@"; do
    case "$arg" in
        --fonts)     DO_FONTS=1 ;;
        --icons)     DO_ICONS=1 ;;
        --wallpaper) DO_WALLPAPER=1 ;;
        --apps)      DO_APPS=1 ;;
        --all)       DO_FONTS=1; DO_ICONS=1; DO_WALLPAPER=1; DO_APPS=1 ;;
        *) echo "[UPDATE] Unknown flag: $arg"; exit 1 ;;
    esac
done

# Default: update everything
if [ $((DO_FONTS + DO_ICONS + DO_WALLPAPER + DO_APPS)) -eq 0 ]; then
    DO_FONTS=1; DO_ICONS=1; DO_WALLPAPER=1; DO_APPS=1
fi

mcopy_replace() {
    local src="$1" dst="$2"
    # -D o: overwrite existing file without prompting
    mcopy -D o -i "${DISK}" "${src}" "${dst}"
}

# ── Fonts ─────────────────────────────────────────────────────────────────────
if [ "${DO_FONTS}" -eq 1 ]; then
    echo "[UPDATE] Refreshing /fonts/ ..."
    FONTS_ASSET_DIR="${SCRIPT_DIR}/../assets/fonts"
    if [ -d "${FONTS_ASSET_DIR}" ]; then
        for font in "${FONTS_ASSET_DIR}"/*.ttf "${FONTS_ASSET_DIR}"/*.otf; do
            [ -f "$font" ] || continue
            fname="$(basename "${font}")"
            mcopy_replace "${font}" "::fonts/${fname}"
            echo "         /fonts/${fname}"
        done
        # Ensure the short-name aliases gfx.c expects are always present
        if [ -f "${FONTS_ASSET_DIR}/NotoSans-Regular.ttf" ]; then
            mcopy_replace "${FONTS_ASSET_DIR}/NotoSans-Regular.ttf" "::fonts/sans.ttf"
            echo "         /fonts/sans.ttf  (alias → NotoSans-Regular.ttf)"
        fi
        if [ -f "${FONTS_ASSET_DIR}/NotoSansMono-Regular.ttf" ]; then
            mcopy_replace "${FONTS_ASSET_DIR}/NotoSansMono-Regular.ttf" "::fonts/mono.ttf"
            echo "         /fonts/mono.ttf  (alias → NotoSansMono-Regular.ttf)"
        fi
    else
        echo "[UPDATE] Warning: assets/fonts/ not found"
    fi
fi

# ── Icons ──────────────────────────────────────────────────────────────────────
if [ "${DO_ICONS}" -eq 1 ]; then
    echo "[UPDATE] Refreshing /icons/ ..."
    ICON_DIR="${SCRIPT_DIR}/../assets/icons"
    if [ -d "${ICON_DIR}" ]; then
        count=0
        for bmp in "${ICON_DIR}"/*.bmp; do
            [ -f "$bmp" ] || continue
            fname="$(basename "${bmp}")"
            mcopy_replace "${bmp}" "::icons/${fname}"
            count=$((count + 1))
        done
        echo "         ${count} icon(s) updated"
    else
        echo "[UPDATE] Note: assets/icons/ not found — skipped"
    fi
fi

# ── Wallpaper ─────────────────────────────────────────────────────────────────
if [ "${DO_WALLPAPER}" -eq 1 ]; then
    BMP_SRC="${SCRIPT_DIR}/../assets/lumina_bg.bmp"
    if [ -f "${BMP_SRC}" ]; then
        echo "[UPDATE] Refreshing /lumina_bg.bmp ..."
        mcopy_replace "${BMP_SRC}" "::lumina_bg.bmp"
    else
        echo "[UPDATE] Note: assets/lumina_bg.bmp not found — skipped"
    fi
fi

# ── App manifests ─────────────────────────────────────────────────────────────
if [ "${DO_APPS}" -eq 1 ]; then
    echo "[UPDATE] Refreshing /apps/ manifests ..."
    update_app() {
        local name="$1" content="$2"
        printf "%s" "${content}" > /tmp/_aether_app_manifest.tmp
        mcopy_replace /tmp/_aether_app_manifest.tmp "::apps/${name}"
        rm -f /tmp/_aether_app_manifest.tmp
        echo "         /apps/${name}"
    }
    update_app "aether_term.app"    "name=Terminal\nicon=icon_term\nexec=/aether_term\ndescription=Terminal emulator\n"
    update_app "files.app"          "name=Files\nicon=icon_files\nexec=/files\ndescription=File browser\n"
    update_app "aether_editor.app"  "name=Aether IDE\nicon=icon_editor\nexec=/aether_editor\ndescription=Script editor\n"
    update_app "aether_browser.app" "name=Web Browser\nicon=icon_browser\nexec=/aether_browser\ndescription=Web browser (NetSurf)\n"
    update_app "sys_prefs.app"      "name=System Preferences\nicon=icon_hardware\nexec=/sys_prefs\ndescription=System settings\n"
fi

echo "[UPDATE] Done — ${DISK} updated in-place (runtime changes preserved)"
