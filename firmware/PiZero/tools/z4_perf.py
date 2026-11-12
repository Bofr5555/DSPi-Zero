#!/usr/bin/env python3
"""Z4 performance: where DSPi's block time goes on the Pi, from the Cortex-A53 counters (stats version 4).

Usage: python3.14 firmware/PiZero/tools/z4_perf.py [seconds]
Plays a quiet tone straight to the Pi by device name (the Mac's default output is not changed), then
reports per block: cycles, how many were spent in the USB interrupt, instructions, IPC and cache refills.
"""
import struct
import sys
import time

from uac2_stream_check import Tone, find_board, read_stats

import usb.core  # noqa: F401  (find_board needs pyusb)

EVENTS = ("inst", "l1i_refill", "l1d_refill", "l2_refill", "br_mispred")


def perf(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x4F, 0, 0, 264, timeout=500))
    s = read_stats(dev)
    if s["version"] < 4 or len(raw) < 264:
        raise SystemExit(f"FAIL: stats version {s['version']}, need 4 (load the new pizero_z4.img)")
    v = struct.unpack_from("<5I5II", raw, 220)
    p = dict(pmu_counters=v[0], blocks=v[1], cycles=v[2], irq_cycles=v[3], irqs=v[4], irq_all=v[10])
    p.update(zip(EVENTS, v[5:10]))
    p["uptime_ms"] = s["uptime_ms"]
    p["s"] = s
    return p


def diff(a, b, k):
    return (b[k] - a[k]) & 0xFFFFFFFF


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    tone = Tone("DSPi Zero")
    try:
        tone.start()
        time.sleep(2.0)
        a = perf(dev)
        time.sleep(seconds)
        b = perf(dev)
    finally:
        tone.stop()
    if not b["pmu_counters"]:
        print("FAIL: performance counters not in use")
        return 1
    n = diff(a, b, "blocks")
    dt = (b["uptime_ms"] - a["uptime_ms"]) / 1000
    cyc = diff(a, b, "cycles")
    irq = diff(a, b, "irq_cycles")
    s = b["s"]
    hz = 1e9
    print(f"{n} blocks in {dt:.2f} s, {s['frames'] / max(s['blocks'], 1):.1f} frames each, cpu0_load {s['cpu0']}%")
    print(f"per block: {cyc / n:.0f} cycles ({cyc / n / hz * 1e6:.1f} us at 1 GHz), of which USB IRQ "
          f"{irq / n:.0f} ({diff(a, b, 'irqs') / n:.2f} IRQs per block)")
    dsp = cyc - irq
    inst = diff(a, b, "inst")
    print(f"DSP only: {dsp / n:.0f} cycles, {inst / n:.0f} instructions, IPC {inst / max(dsp, 1):.2f}")
    for k in EVENTS[1:]:
        print(f"    {k}: {diff(a, b, k) / n:.1f} per block")
    print(f"USB IRQ overall: {diff(a, b, 'irq_all') / dt / hz * 100:.2f}% of the CPU")
    return 0


if __name__ == "__main__":
    sys.exit(main())
