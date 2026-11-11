#!/usr/bin/env python3
"""Z3 pass test: isochronous audio reaches the Pi at 1000 packets/s and survives 10 switch-away cycles.

Usage: python3.14 firmware/PiZero/tools/z3_stream_check.py [cycles]
Needs pyusb with libusb, and SwitchAudioSource (brew install switchaudio-osx). Plays a quiet 1 kHz tone
with afplay. "Away" is BlackHole 2ch, a silent virtual device, so nothing is heard during the test.
The default output device at the start is restored at the end.
"""
import math
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

import usb.core

VID, PID = 0x2E8B, 0xFEAA
REQ_Z_GET_STATS = 0x4F
AWAY_DEVICE = "BlackHole 2ch"
FIELDS = ("class_requests", "set_alt_count", "rate_sets", "sof_count", "fb_armed", "fb_sent",
          "out_packets", "out_bytes", "suspends", "resumes", "mounts", "fb_flushes")


def find_board():
    for dev in usb.core.find(find_all=True, idVendor=VID, idProduct=PID):
        try:
            if "Zero" in (dev.product or "") or read_stats(dev):
                return dev
        except usb.core.USBError:
            continue
    return None


def read_stats(dev):
    raw = bytes(dev.ctrl_transfer(0xC0, REQ_Z_GET_STATS, 0, 0, 64, timeout=500))
    magic, version, uptime_ms, rate, alt, mounted, _ = struct.unpack_from("<HHIIBBH", raw)
    if magic != 0x5A33:
        return None
    s = dict(zip(FIELDS, struct.unpack_from("<%dI" % len(FIELDS), raw, 16)))
    s.update(version=version, uptime_ms=uptime_ms, rate=rate, alt=alt, mounted=mounted)
    return s


def outputs():
    out = subprocess.run(["SwitchAudioSource", "-a", "-t", "output"], capture_output=True, text=True, check=True)
    return out.stdout.splitlines()


def current_output():
    return subprocess.run(["SwitchAudioSource", "-c", "-t", "output"], capture_output=True, text=True,
                          check=True).stdout.strip()


def set_output(name):
    subprocess.run(["SwitchAudioSource", "-t", "output", "-s", name], capture_output=True, check=True)


def write_tone(path, seconds, rate=48000):
    amp = int(32767 * 0.1)   # -20 dBFS
    frames = bytearray()
    for i in range(int(seconds * rate)):
        v = int(amp * math.sin(2 * math.pi * 1000 * i / rate))
        frames += struct.pack("<hh", v, v)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def measure(dev, seconds):
    a = read_stats(dev)
    time.sleep(seconds)
    b = read_stats(dev)
    dt = (b["uptime_ms"] - a["uptime_ms"]) / 1000
    return b, (b["out_packets"] - a["out_packets"]) / dt, (b["out_bytes"] - a["out_bytes"]) / dt


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    dev = find_board()
    if dev is None:
        print(f"FAIL: no DSPi Zero ({VID:04x}:{PID:04x}) answering request 0x{REQ_Z_GET_STATS:02x}")
        return 1
    names = outputs()
    pi_name = next((n for n in names if "DSPi" in n), None)
    if pi_name is None or AWAY_DEVICE not in names:
        print(f"FAIL: need both a DSPi output and {AWAY_DEVICE!r}; outputs are {names}")
        return 1

    start = read_stats(dev)
    print(f"board: {dev.product!r}, uptime {start['uptime_ms'] / 1000:.1f} s, alt {start['alt']}, "
          f"rate {start['rate']}, suspends {start['suspends']}, mounts {start['mounts']}")

    original = current_output()
    tone = os.path.join(tempfile.mkdtemp(prefix="z3_"), "tone_1k_48k.wav")
    write_tone(tone, 30 + cycles * 6)
    failures = []
    player = None
    try:
        set_output(pi_name)
        time.sleep(0.5)
        player = subprocess.Popen(["afplay", tone])
        time.sleep(2.0)
        s, pps, bps = measure(dev, 3.0)
        print(f"steady: {pps:.1f} packets/s, {bps:.0f} bytes/s, alt {s['alt']}, rate {s['rate']}, "
              f"feedback sent {s['fb_sent']}", flush=True)
        if not 998 <= pps <= 1002:
            failures.append(f"steady rate {pps:.1f} packets/s")

        for c in range(1, cycles + 1):
            set_output(AWAY_DEVICE)
            time.sleep(1.5)
            away, away_pps, _ = measure(dev, 1.0)
            set_output(pi_name)
            time.sleep(1.5)
            back, back_pps, back_bps = measure(dev, 2.0)
            fb_delta = back["fb_sent"] - away["fb_sent"]
            ok = 998 <= back_pps <= 1002 and back["mounted"] and back["suspends"] == start["suspends"]
            print(f"cycle {c:2d}: away {away_pps:6.1f} pkt/s alt {away['alt']}, back {back_pps:6.1f} pkt/s "
                  f"alt {back['alt']} {back_bps:.0f} B/s, feedback +{fb_delta}, suspends {back['suspends']} "
                  f"{'ok' if ok else 'FAIL'}", flush=True)
            if not ok:
                failures.append(f"cycle {c}")
            if player.poll() is not None:   # afplay may stop when its device goes away; start it again
                player = subprocess.Popen(["afplay", tone])
                time.sleep(1.0)
    finally:
        if player and player.poll() is None:
            player.terminate()
        set_output(original)

    end = read_stats(dev)
    print(f"end: packets {end['out_packets']}, bytes {end['out_bytes']}, set_alt {end['set_alt_count']}, "
          f"suspends {end['suspends']}, resumes {end['resumes']}, mounts {end['mounts']}, "
          f"feedback sent {end['fb_sent']} flushed {end['fb_flushes']}, output restored to "
          f"{original!r}")
    if failures:
        print("FAIL: " + ", ".join(failures))
        return 1
    print(f"PASS: 1000 packets/s steady and after {cycles} switch-away cycles, no suspends")
    return 0


if __name__ == "__main__":
    sys.exit(main())
