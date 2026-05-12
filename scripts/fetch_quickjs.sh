#!/bin/bash
# scripts/fetch_quickjs.sh — Download QuickJS 2021-03-27 into userspace/vendor/quickjs/
#
# QuickJS is a small, embeddable ES2020 JavaScript engine by Fabrice Bellard.
# Source: https://bellard.org/quickjs/
#
# Run once before building:
#   bash scripts/fetch_quickjs.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
VENDOR_DIR="$REPO_ROOT/userspace/vendor/quickjs"
QJS_VER="2021-03-27"
QJS_TGZ="/tmp/quickjs-${QJS_VER}.tar.xz"
QJS_URL="https://bellard.org/quickjs/quickjs-${QJS_VER}.tar.xz"

echo "[fetch_quickjs] Downloading QuickJS ${QJS_VER}..."
curl -L --progress-bar "$QJS_URL" -o "$QJS_TGZ"

echo "[fetch_quickjs] Extracting to $VENDOR_DIR ..."
rm -rf "$VENDOR_DIR"
mkdir -p "$VENDOR_DIR"
tar -xJf "$QJS_TGZ" -C "$VENDOR_DIR" --strip-components=1

echo "[fetch_quickjs] Removing host-only files (executables, generators)..."
rm -f "$VENDOR_DIR/qjs.c"
rm -f "$VENDOR_DIR/qjsc.c"
rm -f "$VENDOR_DIR/run-test262.c"
rm -f "$VENDOR_DIR/unicode_gen.c"
rm -f "$VENDOR_DIR/unicode_gen_def.h"
rm -f "$VENDOR_DIR/repl.c"       # generated REPL bytecode, used by qjs.c only

echo "[fetch_quickjs] Done. Source in $VENDOR_DIR"
echo "  Compiling: quickjs.c libregexp.c libunicode.c cutils.c libbf.c"
echo "  Excluded:  quickjs-libc.c (OS module — replaced by aether/ stubs)"
echo "  Next: cmake --build build/ (vendor_quickjs + user_js_test will now compile)"
