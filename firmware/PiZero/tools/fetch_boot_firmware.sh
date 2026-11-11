#!/bin/sh
# Downloads the GPU boot files for the BCM2835 from raspberrypi/firmware at a pinned tag and
# stages them next to kernel.img in build/sdcard/. Usage: fetch_boot_firmware.sh [build-dir]
set -eu

TAG=1.20260915
FILES="bootcode.bin start.elf fixup.dat"
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build}
CACHE=$BUILD/boot-firmware/$TAG
DEST=$BUILD/sdcard

mkdir -p "$CACHE" "$DEST"
for f in $FILES; do
    if [ ! -s "$CACHE/$f" ]; then
        echo "fetching $f ($TAG)"
        curl -fsSL -o "$CACHE/$f.part" "https://raw.githubusercontent.com/raspberrypi/firmware/$TAG/boot/$f"
        mv "$CACHE/$f.part" "$CACHE/$f"
    fi
    cp "$CACHE/$f" "$DEST/$f"
done

ls -l "$DEST"
