#!/usr/bin/env python3
"""Sample where DSPi's block time goes on the Pi: plays a quiet tone straight to the Pi (the Mac's default
output is not changed), samples the PC inside process_input_block() and prints the hottest functions.

Usage: python3.14 firmware/PiZero/tools/z4_prof.py ELF [period_us] [--lines N]
"""
import argparse
import bisect
import collections
import struct
import subprocess
import time

from uac2_stream_check import Tone, find_board

MAX = 16384


def symbols(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-n", "--defined-only", elf], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW" and not parts[2].startswith("$"):
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("period", nargs="?", type=int, default=37)
    ap.add_argument("--lines", type=int, default=0, help="also show the N hottest source lines")
    args = ap.parse_args()

    dev = find_board()
    if dev is None:
        raise SystemExit("FAIL: no DSPi Zero")
    tone = Tone("DSPi Zero")
    try:
        tone.start()
        time.sleep(2.0)
        dev.ctrl_transfer(0x40, 0x4D, args.period, 0, None)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 30:
            time.sleep(1.0)
            n = struct.unpack("<I", bytes(dev.ctrl_transfer(0xC0, 0x4E, 0xFFFF, 0, 4)))[0]
            if n >= MAX:
                break
        dev.ctrl_transfer(0x40, 0x4D, 0, 0, None)
    finally:
        tone.stop()
    n = struct.unpack("<I", bytes(dev.ctrl_transfer(0xC0, 0x4E, 0xFFFF, 0, 4)))[0]
    raw = b""
    for chunk in range((n * 4 + 4095) // 4096):
        raw += bytes(dev.ctrl_transfer(0xC0, 0x4E, chunk, 0, 4096, timeout=1000))
    pcs = struct.unpack("<%dI" % n, raw[:n * 4])

    addrs, names = symbols(args.elf)
    by_fn = collections.Counter()
    for pc in pcs:
        i = bisect.bisect_right(addrs, pc) - 1
        by_fn[names[i] if i >= 0 else hex(pc)] += 1
    print(f"{n} samples inside process_input_block()")
    for name, c in by_fn.most_common(30):
        print(f"{100 * c / n:6.1f}%  {name}")
    if args.lines:
        hot = collections.Counter(pcs).most_common(args.lines)
        out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-C", "-e", args.elf] + [hex(a) for a, _ in hot],
                             capture_output=True, text=True).stdout.splitlines()
        print("hottest PCs:")
        for k, (a, c) in enumerate(hot):
            print(f"{100 * c / n:6.2f}%  {a:08x}  {out[2 * k]}  {out[2 * k + 1].split('/')[-1]}")
    return 0


if __name__ == "__main__":
    main()
