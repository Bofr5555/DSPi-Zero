#!/bin/sh
# Downloads the GPU boot files (the same for the BCM2835 and BCM2837) from raspberrypi/firmware at a pinned
# tag and stages them in <build-dir>/sdcard and <build-dir>/sdcard-loader. Usage: fetch_boot_firmware.sh [build-dir]
set -eu

TAG=1.20260915
FILES="bootcode.bin start.elf fixup.dat"
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build}
CACHE=$BUILD/boot-firmware/$TAG

mkdir -p "$CACHE" "$BUILD/sdcard" "$BUILD/sdcard-loader"
for f in $FILES; do
    if [ ! -s "$CACHE/$f" ]; then
        echo "fetching $f ($TAG)"
        curl -fsSL -o "$CACHE/$f.part" "https://raw.githubusercontent.com/raspberrypi/firmware/$TAG/boot/$f"
        mv "$CACHE/$f.part" "$CACHE/$f"
    fi
    cp "$CACHE/$f" "$BUILD/sdcard/$f"
    cp "$CACHE/$f" "$BUILD/sdcard-loader/$f"
done

ls -l "$BUILD/sdcard" "$BUILD/sdcard-loader"
