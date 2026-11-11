#!/usr/bin/env python3
"""Z1 pass test: checks the Pi Zero vendor echo device enumerates at Full Speed and echoes byte-perfect.

Usage: python3.14 firmware/PiZero/tools/z1_vendor_echo.py [rounds]. Needs pyusb with the libusb backend.
"""
import os
import sys
import time

import usb.core
import usb.util

VID, PID = 0xCAFE, 0x5A31
EP_OUT, EP_IN = 0x01, 0x81
SPEEDS = {1: "Low Speed (1.5 Mbit/s)", 2: "Full Speed (12 Mbit/s)", 3: "High Speed (480 Mbit/s)"}
# Payloads stay at or under the device's 1 KB echo buffers, because each write is read back before the next.
SIZES = [1, 2, 3, 63, 64, 65, 127, 128, 129, 255, 256, 511, 512, 513, 1000, 1023, 1024]


def read_exact(dev, n, timeout_ms=2000):
    out = bytearray()
    deadline = time.monotonic() + timeout_ms / 1000
    while len(out) < n and time.monotonic() < deadline:
        try:
            out += dev.read(EP_IN, max(64, n - len(out)), timeout=timeout_ms)
        except usb.core.USBTimeoutError:
            break
    return bytes(out)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"FAIL: no device {VID:04x}:{PID:04x} found")
        return 1

    speed = SPEEDS.get(dev.speed, f"unknown ({dev.speed})")
    print(f"found {VID:04x}:{PID:04x}: {usb.util.get_string(dev, dev.iManufacturer)} / "
          f"{usb.util.get_string(dev, dev.iProduct)}, serial {usb.util.get_string(dev, dev.iSerialNumber)}")
    print(f"speed: {speed}, bcdUSB {dev.bcdUSB:04x}, EP0 {dev.bMaxPacketSize0} bytes")

    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass  # macOS may already have configured it
    usb.util.claim_interface(dev, 0)

    # Drain anything left from an earlier run.
    try:
        while dev.read(EP_IN, 1024, timeout=50):
            pass
    except usb.core.USBTimeoutError:
        pass

    total = 0
    errors = 0
    start = time.monotonic()
    for r in range(rounds):
        for size in SIZES:
            payload = os.urandom(size)
            dev.write(EP_OUT, payload, timeout=2000)
            echoed = read_exact(dev, size)
            if echoed != payload:
                errors += 1
                first = next((i for i in range(min(len(echoed), size)) if echoed[i] != payload[i]), None)
                print(f"MISMATCH round {r} size {size}: got {len(echoed)} bytes, first difference at {first}")
            total += size
    elapsed = time.monotonic() - start
    usb.util.release_interface(dev, 0)

    print(f"{rounds} rounds, {rounds * len(SIZES)} transfers, {total} bytes each way in {elapsed:.2f} s "
          f"({2 * total / elapsed / 1024:.1f} KiB/s round trip)")
    if dev.speed != 2:
        print("FAIL: device is not at Full Speed")
        return 1
    if errors:
        print(f"FAIL: {errors} mismatched transfers")
        return 1
    print("PASS: Full Speed, every byte echoed correctly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
