#!/bin/sh
# AetherOS — build initrd.cpio from the userspace staging directory.
# Usage: make_initrd.sh <staging_dir> <output_cpio>
#
# Packages every file found directly in <staging_dir> into a CPIO newc
# archive. Files are stored without directory prefix (e.g. "init", not
# "./init") so the kernel CPIO reader can find them by bare name.
#
# Compatible with both GNU cpio (Linux) and BSD cpio (macOS).

set -e

STAGING="$1"
OUTPUT="$2"

if [ -z "$STAGING" ] || [ -z "$OUTPUT" ]; then
    echo "Usage: $0 <staging_dir> <output_cpio>" >&2
    exit 1
fi

if [ ! -d "$STAGING" ]; then
    echo "Error: staging directory '$STAGING' does not exist" >&2
    exit 1
fi

# List all files (including subdirectories like aeguitar/, apps/) and feed to cpio.
# Paths are stored relative to the staging root (e.g. "aeguitar/pedal_blue.bmp").
# The VFS accesses them via /initrd/<relpath> (e.g. /initrd/aeguitar/pedal_blue.bmp).
(cd "$STAGING" && find . -type f | sed 's|^\./||' \
    | cpio -o -H newc 2>/dev/null) > "$OUTPUT"

echo "initrd: $(wc -c < "$OUTPUT") bytes → $OUTPUT"
