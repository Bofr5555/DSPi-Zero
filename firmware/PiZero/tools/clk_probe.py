#!/usr/bin/env python3
"""Firmware clock probes for the PLLA plan (phase 0): ask the firmware to change V3D or the ARM clock through
the mailbox (bench request 0x4A) and report which PLLA, V3D and lock registers it rewrote.

Usage: python3.14 firmware/PiZero/tools/clk_probe.py
The probes leave the firmware's clocks changed; reload the image afterwards to restore them.
"""
import sys
import time

from uac2_stream_check import find_board
import clk_dump as c

WATCH = {0x104: "CM_PLLA", 0x114: "CM_LOCK", 0x1100: "PLLA CTRL", 0x1200: "PLLA FRAC", 0x1014: "PLLA ANA1",
         0x1400: "PLLA CORE", 0x1500: "PLLA PER", 0x038: "V3D CTL", 0x03c: "V3D DIV", 0x1b0: "ARM CTL",
         0x11e0: "PLLB CTRL", 0x12e0: "PLLB FRAC", 0x13e0: "PLLB ARM", 0x1120: "PLLC CTRL", 0x1220: "PLLC FRAC"}


def regs(dev):
    _, _, entries = c.parse(c.fetch(dev, False))
    now = {}
    for off, _, n in entries:
        now.setdefault(off, n)
    return now


def probe(dev, label, op, arg):
    a = regs(dev)
    dev.ctrl_transfer(0x40, 0x4A, op, arg, None, timeout=5000)
    time.sleep(0.5)
    b = regs(dev)
    diff = [f"{WATCH[k]} {a[k]:#x}->{b[k]:#x}" for k in WATCH if a[k] != b[k]]
    v3d = c.pll_rates(b)["PLLA"]
    print(f"{label:28s} {'; '.join(diff) if diff else 'nothing watched changed'}  (PLLA VCO {v3d['vco'] / 1e6:.3f} MHz)",
          flush=True)


def main():
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    probe(dev, "V3D rate 250 MHz", 1, 250)
    probe(dev, "V3D rate 300 MHz", 1, 300)
    probe(dev, "V3D rate 400 MHz", 1, 400)
    probe(dev, "V3D clock off", 2, 0)
    probe(dev, "V3D clock on", 2, 1)
    probe(dev, "ARM clock to minimum", 3, 0)
    probe(dev, "ARM clock to maximum", 3, 0xFFFF)
    return 0


if __name__ == "__main__":
    sys.exit(main())
