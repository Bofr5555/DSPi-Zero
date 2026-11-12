#!/usr/bin/env python3
"""PLLA phase 5 (PiZero_PLLA_plan.md): live fine trim of PLLA (bench request 0x46 op 6, 0.01 ppm units)
while a tone streams straight to the Pi (the Mac's default output is not changed). Checks that the SMI
clock follows the commanded trim, that FLOCKA never clears, and that the stream stays clean through single
steps, a +/-100 ppm staircase at 10 steps a second, and a 100 ppm jump.

Usage: python3.14 firmware/PiZero/tools/pll_trim.py [--rates 48000,44100]
"""
import argparse
import sys
import time

from uac2_stream_check import Tone, find_board
from smi_check import smi_stats
import pll_check as p
import smi_pll_check as q

XOSC = 19.2e6


def trim(dev, ppm):
    dev.ctrl_transfer(0x40, 0x46, 6, int(round(ppm * 100)) & 0xFFFF, None, timeout=1000)


def smi_ppm(dev, freq, gates=40):
    want = freq * 768
    gates = min(gates, int(2**24 / (want * 0.01)))
    hz = p.measure(dev, p.MUX_SMI, gates)[0]
    return (hz - want) / want * 1e6


def health(dev):
    st = smi_stats(dev)
    s = st["s"]
    ps = p.status(dev)
    return dict(late=st["late"], underrun_frames=s["slot0_underrun_frames"], ring_drops=s["ring_drops"],
                usb_underruns=s["underruns"], lock_lost=ps["lock_lost"], cm_lock=ps["cm_lock"])


def delta(a, b):
    return {k: b[k] - a[k] for k in a if k != "cm_lock"}


def run_rate(dev, rate, failures):
    print(f"== {rate} Hz")
    tone = Tone("DSPi Zero", rate=rate, set_device_rate=True)
    try:
        tone.start()
        time.sleep(2.0)
        q.set_source(dev, "plla")
        trim(dev, 0)
        time.sleep(0.2)
        base = smi_ppm(dev, rate)
        print(f"  trim 0: SMI clock {base:+.3f} ppm from nominal")
        h0 = health(dev)
        # Steps: the measured change must match the command.
        for t in (0.02, 1, 10, -10, 100, -100, 0):
            trim(dev, t)
            time.sleep(0.1)
            got = smi_ppm(dev, rate) - base
            ok = abs(got - t) <= 0.15
            print(f"  trim {t:+8.2f} ppm: measured {got:+9.3f} ppm {'ok' if ok else 'OFF'}")
            if not ok:
                failures.append(f"{rate}: trim {t} measured {got:+.3f}")
        print(f"  after the steps: {delta(h0, health(dev))}")
        # Staircase at 10 steps a second, with a measured plateau every 50 ppm.
        path = list(range(0, 101)) + list(range(99, -101, -1)) + list(range(-99, 1))
        worst = 0.0
        for t in path:
            trim(dev, t)
            if t % 50 == 0 and t:
                got = smi_ppm(dev, rate, 20) - base
                worst = max(worst, abs(got - t))
                print(f"  staircase plateau {t:+4d} ppm: measured {got:+8.3f}", flush=True)
            else:
                time.sleep(0.1)
        print(f"  after the staircase: {delta(h0, health(dev))}")
        if worst > 0.35:    # a plateau is read over 200 ms, where one count is up to 0.15 ppm
            failures.append(f"{rate}: staircase plateau off by {worst:.3f} ppm")
        # A 100 ppm jump, read back over 10 ms windows straight after.
        trim(dev, 100)
        jumps = [smi_ppm(dev, rate, 1) - base for _ in range(3)]
        got = smi_ppm(dev, rate) - base
        print(f"  jump to +100 ppm: 10 ms windows {', '.join(f'{j:+.0f}' for j in jumps)} ppm, then {got:+.3f} ppm")
        trim(dev, 0)
        time.sleep(0.5)
        h1 = health(dev)
        d = delta(h0, h1)
        print(f"  over the whole test: {d}, FLOCKA {'set' if h1['cm_lock'] & 0x100 else 'CLEAR'}")
        if any(d.values()) or not h1["cm_lock"] & 0x100:
            failures.append(f"{rate}: {d}")
    finally:
        tone.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="48000,44100")
    args = ap.parse_args()
    dev = find_board()
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    failures = []
    for r in args.rates.split(","):
        run_rate(dev, int(r), failures)
    trim(dev, 0)
    if failures:
        print("FAIL:", "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
