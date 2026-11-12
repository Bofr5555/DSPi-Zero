#!/usr/bin/env python3
"""USB event accounting (bench request 0x45): events the DWC2 driver posted against events TinyUSB queued,
and the longest gap between tud_task() calls. A SETUP or transfer completion that was posted but never
queued was dropped because TinyUSB's event queue was full.

Usage: python3.14 firmware/PiZero/tools/usb_health.py [--clear]
"""
import argparse
import struct
import sys

import usb.core

KINDS = ("SETUP", "transfer complete", "bus events", "SOF and deferred calls")


def read(dev, clear=False):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x45, 1 if clear else 0, 0, 344, timeout=1000))
    v = struct.unpack_from("<53I", raw)
    if v[0] != 0x48425355:
        raise SystemExit("FAIL: no USB accounting (load the current pizero_z4.img)")
    setups = [(struct.unpack_from("<2I", raw, 212 + 16 * i), raw[220 + 16 * i:228 + 16 * i]) for i in range(8)]
    return dict(posted=v[1:5], queued=v[5:9], gap_max_us=v[9], gap_rate=v[10], gap_alt=v[11],
                gaps_over_4ms=v[12], tud_calls=v[13], usb_irqs=v[14], ep0_done=v[15], recoveries=v[16],
                trigger=v[17], trigger_ms=v[18], setups_since_done=v[19], armed=v[20], snap=v[21:53], setups=setups,
                wait_timeouts=struct.unpack_from("<I", raw, 340)[0] if len(raw) >= 344 else None)


SNAP = ("GAHBCFG", "GRSTCTL", "GINTSTS", "GINTMSK", "GRXSTSR", "DCTL", "DSTS", "DAINT", "DAINTMSK",
        "DIEPCTL0", "DIEPINT0", "DIEPTSIZ0", "DOEPCTL0", "DOEPINT0", "DOEPTSIZ0", "GRXFSIZ", "DOEPCTL1",
        "DOEPINT1", "DOEPTSIZ1", "DIEPCTL1", "DIEPINT1", "DIEPCTL2", "DIEPINT2", "DIEPCTL3", "GNPTXSTS",
        "GOTGCTL", "PCGCCTL", "GNPTXFSIZ", "IRQ pend1", "IRQ en1", "CPSR", "alt|rate<<8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clear", action="store_true", help="clear the longest gap after reading")
    args = ap.parse_args()
    dev = usb.core.find(idVendor=0x2E8B, idProduct=0xFEAA)
    if dev is None:
        print("FAIL: no DSPi Zero")
        return 1
    h = read(dev, args.clear)
    for i, k in enumerate(KINDS):
        lost = h["posted"][i] - h["queued"][i] if i < 2 else None
        print(f"  {k:22s} posted {h['posted'][i]:9d}  queued {h['queued'][i]:9d}"
              + (f"  DROPPED {lost}" if lost else ""))
    print(f"  longest gap between tud_task() calls {h['gap_max_us'] / 1000:.2f} ms (rate {h['gap_rate']}, alt "
          f"{h['gap_alt']}), gaps over 4 ms {h['gaps_over_4ms']}, tud_task calls {h['tud_calls']}")
    print(f"  USB IRQs {h['usb_irqs']}, EP0 completions {h['ep0_done']}, SETUPs since the last "
          f"{h['setups_since_done']}, hang recoveries {h['recoveries']}, DWC2 wait timeouts {h['wait_timeouts']}")
    if h["recoveries"]:
        why = {1: "SETUPs without EP0 completions", 2: "interrupt pending, handler not running",
               3: "no EP0 completion for 8 s"}
        print(f"  last recovery at {h['trigger_ms'] / 1000:.1f} s: {why.get(h['trigger'], h['trigger'])}")
        print("   " + ", ".join(f"{n} {x:#x}" for n, x in zip(SNAP, h["snap"])))
        print("  SETUPs before it, oldest first (ms, bmRequestType bRequest wValue wIndex wLength, completed):")
        for (ms, done), pkt in h["setups"]:
            if ms:
                bm, br, wv, wi, wl = struct.unpack("<BBHHH", pkt)
                print(f"    {ms / 1000:9.3f} s  {bm:02x} {br:02x} {wv:04x} {wi:04x} {wl:4d}  {'yes' if done else 'NO'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
