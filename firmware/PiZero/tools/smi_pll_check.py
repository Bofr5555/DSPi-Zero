#!/usr/bin/env python3
"""PLLA phase 2 (PiZero_PLLA_plan.md): put the SMI clock on plla_per with an integer divider, check its
frequency with the clock manager's counter, and toggle between PLLA and PLLD while a tone plays straight
to the Pi (the Mac's default output is not changed).

Usage: python3.14 firmware/PiZero/tools/smi_pll_check.py [--rate HZ] [--toggles N] [--leave plla|plld]
Each toggle interval is checked for a jump in the SMI's frame count (a whole ring is 128 frames).
"""
import argparse
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import smi_stats
import pll_check as p

SRC = {"plld": 0, "plla": 1}


def set_source(dev, name):
    dev.ctrl_transfer(0x40, 0x46, 4, SRC[name], None, timeout=1000)
    time.sleep(0.05)


def smi_clock(dev):
    s = p.status(dev)
    src = "plla" if (s["smi_ctl"] & 15) == 4 else "plld" if (s["smi_ctl"] & 15) == 6 else f"src{s['smi_ctl'] & 15}"
    divi, divf = (s["smi_div"] >> 12) & 15, (s["smi_div"] >> 4) & 255
    return src, divi + divf / 256, (s["smi_ctl"] >> 9) & 3, s


def counters(st):
    s = st["s"]
    return dict(late=st["late"], underrun_frames=s["slot0_underrun_frames"], ring_drops=s["ring_drops"],
                usb_underruns=s["underruns"])


def report_clock(dev, label):
    src, div, mash, s = smi_clock(dev)
    want = smi_stats(dev)["freq"] * 768
    hz = p.measure(dev, p.MUX_SMI, min(40, int(2**24 / (want * 0.01))))[0]     # the counter is 24 bits
    print(f"{label}: CM_SMICTL {s['smi_ctl']:#x} DIV {s['smi_div']:#x}, SMI from {src}, divider {div:.4f}, MASH {mash}, counter {hz / 1e6:.6f} MHz "
          f"({(hz - want) / want * 1e6:+.3f} ppm from {want / 1e6:.6f}), PLLA row {s['row']} "
          f"{p.STATES[s['state']]}, in use {s['in_use']}, refused {s['refused']}", flush=True)
    return src, (hz - want) / want * 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--toggles", type=int, default=10)
    ap.add_argument("--leave", default="plla", choices=SRC)
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    failures = []
    tone = Tone("DSPi Zero", rate=args.rate, set_device_rate=True)
    try:
        tone.start()
        time.sleep(2.0)
        report_clock(dev, "start")
        set_source(dev, "plld")
        st = smi_stats(dev)
        plld_rate = 500e6 * 256 / st["div_q8"] / 768
        set_source(dev, "plla")
        src, err = report_clock(dev, "PLLA")
        if src != "plla" or abs(err) > 0.1:
            failures.append(f"PLLA clock {src} {err:+.3f} ppm")
        first = smi_stats(dev)
        rates = {"plla": first["freq"], "plld": plld_rate}
        want_total, worst = 0.0, 0.0
        prev, src_now = first, "plla"
        for i in range(2 * args.toggles):
            name = "plld" if i % 2 == 0 else "plla"
            for step in ("toggle", "hold"):
                if step == "toggle":
                    set_source(dev, name)
                else:
                    time.sleep(1.0)
                cur = smi_stats(dev)
                dt = (cur["s"]["uptime_ms"] - prev["s"]["uptime_ms"]) / 1000
                r = rates[src_now] if step == "toggle" else rates[name]
                want_total += dt * r
                if step == "hold":
                    err = ((cur["frames"] - prev["frames"]) & 0xFFFFFFFF) - dt * r
                    worst = max(worst, abs(err))
                prev = cur
            src_now = name
        last = smi_stats(dev)
        want_total += (last["s"]["uptime_ms"] - prev["s"]["uptime_ms"]) / 1000 * rates[src_now]
        total_err = ((last["frames"] - first["frames"]) & 0xFFFFFFFF) - want_total
        print(f"frame count over {2 * args.toggles} toggles: {total_err:+.0f} frames from the clock rates "
              f"(a lost or added ring is 128; 1 ms of timestamp rounding is {first['freq'] / 1000:.0f}), "
              f"largest 1 s interval error {worst:.0f}")
        if abs(total_err) > first["freq"] / 1000 + 20:
            failures.append(f"frame count off by {total_err:+.0f}")
        last = smi_stats(dev)
        d = {k: v - counters(first)[k] for k, v in counters(last).items()}
        print(f"counters over the toggles: {d}")
        if any(d.values()):
            failures.append(f"counters {d}")
        set_source(dev, args.leave)
        report_clock(dev, "left on")
    finally:
        tone.stop()
    if failures:
        print("FAIL:", "; ".join(failures[:8]))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
