#!/usr/bin/env python3
"""Decode the clock register dump (bench request 0x4C): PLL rates, every enabled clock with its source, and
registers that changed since boot. PiZero_PLLA_plan.md phase 0. Register layout from Linux clk-bcm2835.c.

Usage: python3.14 firmware/PiZero/tools/clk_dump.py [--mailbox] [--all] [--raw FILE]
--mailbox also asks the firmware for its clock rates (mailbox calls that can disturb streaming).
--all lists every clock, not only enabled ones. --raw saves the reply for later comparison.
"""
import argparse
import struct
import sys

from uac2_stream_check import find_board

XOSC = 19.2e6
MAGIC = 0x444B4C43

CM_NAMES = {
    0x000: "GNRIC", 0x008: "VPU", 0x010: "SYS", 0x018: "PERIA", 0x020: "PERII", 0x028: "H264", 0x030: "ISP",
    0x038: "V3D", 0x040: "CAM0", 0x048: "CAM1", 0x050: "CCP2", 0x058: "DSI0E", 0x060: "DSI0P", 0x068: "DPI",
    0x070: "GP0", 0x078: "GP1", 0x080: "GP2", 0x088: "HSM", 0x090: "OTP", 0x098: "PCM", 0x0a0: "PWM",
    0x0a8: "SLIM", 0x0b0: "SMI", 0x0c8: "TEC", 0x0d0: "TD0", 0x0d8: "TD1", 0x0e0: "TSENS", 0x0e8: "TIMER",
    0x0f0: "UART", 0x0f8: "VEC", 0x158: "DSI1E", 0x160: "DSI1P", 0x168: "DFT", 0x190: "PULSE", 0x1a8: "SDC",
    0x1b0: "ARM", 0x1b8: "AVEO", 0x1c0: "EMMC", 0x1d0: "EMMC2",
}
VPU_CLOCKS = {"H264", "ISP", "SDC", "V3D", "VPU"}     # REGISTER_VPU_CLK in clk-bcm2835.c
PER_SRC = ["gnd", "xosc", "testdebug0", "testdebug1", "plla_per", "pllc_per", "plld_per", "pllh_aux"]
VPU_SRC = ["gnd", "xosc", "testdebug0", "testdebug1", "plla_core", "pllc_core0", "plld_core", "pllh_aux",
           "pllc_core1", "pllc_core2"]
# (name, CM_PLLx, A2W CTRL, FRAC, ANA0, pre-divider bit in ANA1, lock bit, {channel: offset})
PLLS = [
    ("PLLA", 0x104, 0x1100, 0x1200, 0x1010, 14, 8, {"dsi0": 0x1300, "core": 0x1400, "per": 0x1500, "ccp2": 0x1600}),
    ("PLLC", 0x108, 0x1120, 0x1220, 0x1030, 14, 10, {"core2": 0x1320, "core1": 0x1420, "per": 0x1520, "core0": 0x1620}),
    ("PLLD", 0x10c, 0x1140, 0x1240, 0x1050, 14, 11, {"dsi0": 0x1340, "core": 0x1440, "per": 0x1540, "dsi1": 0x1640}),
    ("PLLH", 0x110, 0x1160, 0x1260, 0x1070, 11, 12, {"aux": 0x1360, "rcal": 0x1460, "pix": 0x1560, "sts": 0x1660}),
    ("PLLB", 0x170, 0x11e0, 0x12e0, 0x10f0, 14, 9, {"arm": 0x13e0}),
]
MBOX_IDS = ["EMMC", "UART", "ARM", "CORE", "V3D", "H264", "ISP", "SDRAM", "PIXEL", "PWM", "HEVC", "EMMC2", "M2MC",
            "PIXEL_BVB"]


def fetch(dev, mailbox):
    raw = bytes(dev.ctrl_transfer(0xC0, 0x4C, 1 if mailbox else 0, 0, 8 + 112 + 160 * 12, timeout=5000))
    return raw


def parse(raw):
    magic, count = struct.unpack_from("<II", raw)
    if magic != MAGIC:
        raise SystemExit(f"FAIL: bad dump magic {magic:#x}")
    mset = struct.unpack_from("<14I", raw, 8)
    mmeas = struct.unpack_from("<14I", raw, 64)
    entries = [struct.unpack_from("<3I", raw, 120 + 12 * i) for i in range(count)]
    return mset, mmeas, entries


def pll_rates(regs):
    """Rates of each PLL's VCO and channels, from the live values (Linux bcm2835_pll_get_rate)."""
    out = {}
    for name, cm, ctrl, frac, ana0, prediv_bit, lock_bit, chans in PLLS:
        c, f, ana1 = regs[ctrl], regs[frac] & 0xFFFFF, regs[ana0 + 4]
        ndiv, pdiv = c & 0x3FF, (c >> 12) & 7
        prediv = (ana1 >> prediv_bit) & 1
        vco = XOSC * (ndiv + f / 2**20) * (2 if prediv else 1) / pdiv if pdiv else 0.0
        powered = not (c >> 16) & 1
        locked = (regs[0x114] >> lock_bit) & 1
        rates = {}
        for ch, off in chans.items():
            v = regs[off]
            div = (v & 0xFF) or 256
            rates[ch] = (0.0 if (v >> 8) & 1 or not powered else vco / div, div, (v >> 8) & 1)
        out[name] = dict(vco=vco, ndiv=ndiv, frac=f, pdiv=pdiv, prediv=prediv, powered=powered, locked=locked,
                         cm=regs[cm], ctrl=c, chans=rates)
    return out


def source_rate(src, plls):
    table = {"xosc": XOSC, "gnd": 0.0}
    for pll, ch in (("PLLA", "per"), ("PLLA", "core"), ("PLLC", "per"), ("PLLC", "core0"), ("PLLC", "core1"),
                    ("PLLC", "core2"), ("PLLD", "per"), ("PLLD", "core"), ("PLLH", "aux")):
        table[f"{pll.lower()}_{ch}"] = plls[pll]["chans"][ch][0]
    return table.get(src, 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mailbox", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--raw", help="save the reply here")
    ap.add_argument("--file", help="decode a saved reply instead of asking the board")
    args = ap.parse_args()
    if args.file:
        raw = open(args.file, "rb").read()
    else:
        dev = find_board()
        if dev is None:
            print("FAIL: no DSPi Zero")
            return 1
        raw = fetch(dev, args.mailbox)
        if args.raw:
            open(args.raw, "wb").write(raw)
    mset, mmeas, entries = parse(raw)

    now, boot, seen, ana_mismatch = {}, {}, {}, []
    for off, b, n in entries:
        if off in seen and seen[off] != n:
            ana_mismatch.append((off, seen[off], n))
        seen[off] = n
        now.setdefault(off, n)
        boot.setdefault(off, b)

    for label, regs in (("boot", boot), ("now", now)):
        plls = pll_rates(regs)
        print(f"== PLLs ({label})")
        for name, p in plls.items():
            chans = ", ".join(f"{ch} /{div}{' off' if dis else ''} {r / 1e6:.4f} MHz"
                              for ch, (r, div, dis) in p["chans"].items())
            print(f"  {name}: VCO {p['vco'] / 1e6:9.4f} MHz (NDIV {p['ndiv']}, FRAC {p['frac']:#07x}, PDIV {p['pdiv']}, "
                  f"prediv {p['prediv']}), {'on' if p['powered'] else 'powered down'}, "
                  f"{'locked' if p['locked'] else 'not locked'}, CM {p['cm']:#05x}; {chans}")
        if label == "boot":
            continue
        print(f"== Clocks ({label}){'' if args.all else ', enabled only'}")
        users_of_plla = []
        for off, name in CM_NAMES.items():
            ctl = regs.get(off)
            if ctl is None:
                continue
            div = regs.get(off + 4, 0)
            src_idx, enab, busy, mash = ctl & 0xF, (ctl >> 4) & 1, (ctl >> 7) & 1, (ctl >> 9) & 3
            srcs = VPU_SRC if name in VPU_CLOCKS else PER_SRC
            src = srcs[src_idx] if src_idx < len(srcs) else f"src{src_idx}"
            divi, divf = (div >> 12) & 0xFFF, div & 0xFFF
            d = divi + divf / 4096
            rate = source_rate(src, plls) / d if d and name != "ARM" else 0.0
            if enab and src.startswith("plla"):
                users_of_plla.append(name)
            if enab or busy or args.all:
                print(f"  {name:6s} {'ENAB' if enab else '    '}{' BUSY' if busy else '     '} src {src:10s} "
                      f"div {d:10.4f} MASH {mash}  {rate / 1e6:10.4f} MHz   (CTL {ctl:#010x} DIV {div:#010x})")
        print(f"== Enabled clocks on PLLA: {', '.join(users_of_plla) if users_of_plla else 'none'}")

    changed = [(off, boot[off], now[off]) for off in now if boot[off] != now[off]]
    print(f"== Changed since boot: {len(changed)}")
    for off, b, n in changed:
        print(f"  {off:#06x} {CM_NAMES.get(off, CM_NAMES.get(off - 4, '') + ' DIV' if off - 4 in CM_NAMES else ''):10s} "
              f"{b:#010x} -> {n:#010x}")
    print(f"== ANA registers read twice: {'identical' if not ana_mismatch else ana_mismatch}")
    if any(mset) or any(mmeas):
        print("== Firmware clock rates (set / measured), MHz")
        for i, name in enumerate(MBOX_IDS):
            print(f"  {i + 1:2d} {name:9s} {mset[i] / 1e6:10.3f} / {mmeas[i] / 1e6:10.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
