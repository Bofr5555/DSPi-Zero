#!/usr/bin/env python3
"""Send an image to the Pi Zero USB loader and start it, so no SD card swap is needed.

Usage: python3.14 firmware/PiZero/tools/pizero_load.py IMAGE.img [--wait SECONDS] [--no-reboot]

If the loader is not on USB, any running platform image (product name containing "Zero" or "PiZero")
is asked to reboot to it first. Needs pyusb with libusb. Protocol: firmware/PiZero/loader/loader_proto.h.
"""
import argparse
import struct
import sys
import time
import zlib

import usb.core
import usb.util

LOADER_VID, LOADER_PID = 0xCAFE, 0x5A4C
REQ_LOAD_BEGIN, REQ_STATUS, REQ_GO = 0x01, 0x02, 0x03
APP_REQ_REBOOT_TO_LOADER = 0x40
STATES = {0: "idle", 1: "receiving", 2: "complete", 3: "CRC mismatch", 4: "too big", 5: "starting"}
OUT_VENDOR, IN_VENDOR = 0x40, 0xC0
EP_OUT = 0x01


def find_loader():
    return usb.core.find(idVendor=LOADER_VID, idProduct=LOADER_PID)


def platform_images():
    """Running images built on the platform. The real DSPi shares 2E8B:FEAA, so match by name too."""
    found = []
    for dev in usb.core.find(find_all=True):
        if (dev.idVendor, dev.idProduct) == (LOADER_VID, LOADER_PID):
            continue
        try:
            product = dev.product or ""
        except (usb.core.USBError, ValueError):
            continue
        if "Zero" in product:
            found.append((dev, product))
    return found


def reboot_to_loader():
    images = platform_images()
    for dev, product in images:
        print(f"asking {product!r} to reboot to the loader")
        try:
            dev.ctrl_transfer(OUT_VENDOR, APP_REQ_REBOOT_TO_LOADER, 0, 0, None, timeout=1000)
        except usb.core.USBError as e:
            print(f"  request failed ({e}); the image may not support it, or the board is hung")
    return bool(images)


def status(dev):
    state, size, received, max_size = struct.unpack("<IIII", bytes(dev.ctrl_transfer(IN_VENDOR, REQ_STATUS, 0, 0, 16)))
    return state, size, received, max_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--wait", type=float, default=30.0, help="seconds to wait for the loader")
    ap.add_argument("--no-reboot", action="store_true", help="do not ask a running image to reboot")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF

    dev = find_loader()
    if dev is None and not args.no_reboot:
        reboot_to_loader()
    t0 = time.monotonic()
    while dev is None and time.monotonic() - t0 < args.wait:
        time.sleep(0.25)
        dev = find_loader()
    if dev is None:
        print(f"FAIL: loader {LOADER_VID:04x}:{LOADER_PID:04x} not found after {args.wait:.0f} s. "
              "If the board is hung, power-cycle it.")
        return 1
    if time.monotonic() - t0 > 0.5:
        print(f"loader up after {time.monotonic() - t0:.1f} s")

    _, _, _, max_size = status(dev)
    if len(data) > max_size:
        print(f"FAIL: image is {len(data)} bytes, the loader takes at most {max_size}")
        return 1

    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass
    usb.util.claim_interface(dev, 0)
    start = time.monotonic()
    dev.ctrl_transfer(OUT_VENDOR, REQ_LOAD_BEGIN, 0, 0, struct.pack("<II", len(data), crc))
    dev.write(EP_OUT, data, timeout=10000)

    while True:
        state, size, received, _ = status(dev)
        if state != 1 or received == size:
            break
        if time.monotonic() - start > 10:
            print(f"FAIL: stalled at {received} of {size} bytes")
            return 1
        time.sleep(0.01)
    if state != 2:
        print(f"FAIL: loader state {STATES.get(state, state)}, {received} of {size} bytes")
        return 1
    elapsed = time.monotonic() - start

    try:
        dev.ctrl_transfer(OUT_VENDOR, REQ_GO, 0, 0, None)
    except usb.core.USBError as e:
        state = status(dev)[0]
        print(f"FAIL: GO refused ({e}), loader state {STATES.get(state, state)}")
        return 1
    usb.util.release_interface(dev, 0)
    usb.util.dispose_resources(dev)
    print(f"sent {args.image}: {len(data)} bytes, CRC-32 {crc:08x}, in {elapsed:.2f} s; started at 0x8000")
    return 0


if __name__ == "__main__":
    sys.exit(main())
