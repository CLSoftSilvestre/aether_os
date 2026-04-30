#!/bin/bash
# scripts/fetch_lua.sh — Download Lua 5.4 source into userspace/vendor/lua54/
#
# Run once before building:
#   bash scripts/fetch_lua.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
VENDOR_DIR="$REPO_ROOT/userspace/vendor/lua54"
LUA_VER="5.4.7"
LUA_TGZ="/tmp/lua-${LUA_VER}.tar.gz"
LUA_URL="https://www.lua.org/ftp/lua-${LUA_VER}.tar.gz"

echo "[fetch_lua] Downloading Lua ${LUA_VER}..."
curl -L --progress-bar "$LUA_URL" -o "$LUA_TGZ"

echo "[fetch_lua] Extracting src/ to $VENDOR_DIR ..."
mkdir -p "$VENDOR_DIR"
# Extract only the src/ subdirectory, stripping the top-level "lua-X.Y.Z/src/" prefix
tar -xzf "$LUA_TGZ" -C "$VENDOR_DIR" \
    --strip-components=2 \
    "lua-${LUA_VER}/src"

echo "[fetch_lua] Removing lua.c and luac.c (not used in embedded build)..."
rm -f "$VENDOR_DIR/lua.c"
rm -f "$VENDOR_DIR/luac.c"

echo "[fetch_lua] Done. Source in $VENDOR_DIR"
echo "  Next: cmake --build build/ (aether_interp will now compile)"
