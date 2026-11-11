# Pi Zero Z0 Bring-up: Boot, LED and UART

*Created: 2026-11-03*

This covers milestone Z0 from `PiZero_port_plan.md` on the first-generation Pi Zero (BCM2835). It explains how to build the image, prepare the SD card, wire the UART, read the result, and try USB boot with `rpiboot`.

## 1. What the Z0 image does

The image is `kernel.img`, built from `firmware/PiZero/`. The GPU firmware loads it at `0x8000` and starts it with the MMU and caches off. Our startup turns on the instruction cache and branch prediction, but leaves the data cache and MMU off. It then does four things.

1. It flashes the ACT LED three times quickly (5 Hz), then leaves it on. This happens before anything that could stall, so it proves our code runs and controls the LED.
2. It sets up the PL011 UART (UART0) on GPIO 14/15 at 115200 baud, 8N1. It prints a load-check report built only from computed characters (lines starting `LC`, then hex dumps), and then a banner.
3. It asks the VideoCore mailbox for the board revision, the firmware revision, the ARM memory split, and the ARM, core and UART clock rates, and prints them.
4. It blinks the ACT LED at 1 Hz (500 ms on, 500 ms off) from the 1 MHz system timer. Once per second it prints a heartbeat line with the ARM and core clock rates the firmware measures.

Any CPU exception prints a `FAULT:` line and makes the LED flash fast (10 Hz).

## 2. Build

The build needs `arm-none-eabi-gcc`, CMake and Ninja. It also needs Adafruit's `broadcom-peripherals` inside TinyUSB. That was fetched on 2026-11-03 and lives in the TinyUSB submodule as untracked files. On a fresh checkout, fetch it again first.

```bash
cd firmware/pico-sdk/lib/tinyusb && python3 tools/get_deps.py broadcom_32bit
```

Then build from the repo root.

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build -G Ninja
```

```bash
cmake --build firmware/PiZero/build
```

The build writes one raw image per milestone, `firmware/PiZero/build/pizero_z0.img` and `pizero_z1.img`. The latest milestone, set by `SDCARD_TARGET` in `CMakeLists.txt`, is copied to `firmware/PiZero/build/sdcard/kernel.img` along with `firmware/PiZero/boot/config.txt`. To boot Z0 again, copy `pizero_z0.img` to the card as `kernel.img`.

## 3. GPU boot files

The BCM2835 boot ROM needs three closed-source files from Raspberry Pi: `bootcode.bin`, `start.elf` and `fixup.dat`. This script downloads them from `github.com/raspberrypi/firmware` at tag `1.20260915` and copies them into `build/sdcard/`.

```bash
firmware/PiZero/tools/fetch_boot_firmware.sh
```

After that, `firmware/PiZero/build/sdcard/` holds the whole boot partition. It contains `bootcode.bin`, `start.elf`, `fixup.dat`, `config.txt` and `kernel.img`.

## 4. Prepare the SD card (macOS)

Formatting erases the whole card, so check the disk number twice. The card needs an MBR partition table with one FAT32 partition. Any size from 1 GB up works.

1. Insert the card and find its disk number. It is the entry marked `(external, physical)` whose size matches the card.

   ```bash
   diskutil list external
   ```

2. Format it, replacing `diskN` with that number.

   ```bash
   diskutil eraseDisk FAT32 PIZERO MBRFormat /dev/diskN
   ```

3. Copy the boot files to the card.

   ```bash
   cp firmware/PiZero/build/sdcard/* /Volumes/PIZERO/
   ```

4. Eject it.

   ```bash
   diskutil eject /dev/diskN
   ```

For later builds only `kernel.img` changes, so step 3 can copy just that file.

## 5. Wire the UART

Use a 3.3 V USB-to-UART adapter. A 5 V adapter can damage the BCM2835. Do not connect the adapter's VCC or 5 V pin. The Zero has its own power.

| Adapter pin | Pi Zero header pin | Signal |
|---|---|---|
| GND | Pin 6 | Ground |
| RX | Pin 8 (GPIO 14) | Pi TXD0 |
| TX | Pin 10 (GPIO 15) | Pi RXD0 |

Pin 1 is the square pad at the end of the header nearest the SD card slot. Pins 6, 8 and 10 are on the outer row, counting from that end. A bare Zero needs a soldered header or test clips.

Open the adapter in a terminal. Its device name varies by chip, so list the candidates first.

```bash
ls /dev/tty.usb*
```

```bash
screen /dev/tty.usbserial-XXXX 115200
```

To leave `screen`, press Ctrl-A, then K, then Y.

### Using a Raspberry Pi Pico as the adapter

Any RP2040 Pico can act as the adapter, because both boards use 3.3 V logic. The bridge firmware is in `firmware/PiZero/tools/rp2040_uart_bridge/`. It links the Pico's UART0 at 115200 baud to a USB serial port, and the Pico's LED stays on while it runs.

```bash
cmake -S firmware/PiZero/tools/rp2040_uart_bridge -B firmware/PiZero/build/uart_bridge -G Ninja
```

```bash
cmake --build firmware/PiZero/build/uart_bridge
```

Flash `firmware/PiZero/build/uart_bridge/rp2040_uart_bridge.uf2` with `picotool load -x`, or copy it to the `RPI-RP2` drive in BOOTSEL mode. It then appears as `/dev/cu.usbmodem*`.

| Pico pin | Pi Zero header pin | Signal |
|---|---|---|
| Pin 3 (GND) | Pin 6 | Ground |
| Pin 2 (GP1, UART0 RX) | Pin 8 (GPIO 14) | Zero TXD0 to Pico RX |
| Pin 1 (GP0, UART0 TX) | Pin 10 (GPIO 15) | Pico TX to Zero RXD0 |

Do not connect the boards' 3V3, VSYS or VBUS pins together. Each board keeps its own USB power.

## 6. Power on and read the result

Start `screen` first. Then put the card in the Zero and power it from the **PWR** micro USB port.

The firmware prints its own boot log first, because `config.txt` sets `uart_2ndstage=1`. Then our banner appears, followed by one heartbeat line per second. The lines look like this, although the numbers will differ.

```
DSPi PiZero Z0 bring-up (Nov  3 2026 23:34:00)
board revision 0x00900093, firmware revision 0x...
ARM memory base 0x00000000 size 448 MB
set rates: arm 1000000000 Hz, core 400000000 Hz, uart 48000000 Hz
hb 1 s: arm 1000000000 Hz, core 400000000 Hz (measured)
hb 2 s: arm 1000000000 Hz, core 400000000 Hz (measured)
```

**Z0 passes** when the LED blinks at 1 Hz and the heartbeat lines show non-zero ARM and core rates.

If it does not pass, the LED narrows down where it stopped.

| What you see | What it means |
|---|---|
| No three quick flashes, LED dark or solid | Our code never ran, or GPIO 47 is not this board's LED. The UART boot log from `uart_2ndstage` tells these apart. |
| Three quick flashes, then solid | Our code ran but stopped before the blink loop. Every wait is time-bounded, so this should not happen. Report it with the UART output. |
| Three quick flashes, then 1 Hz blink, with no UART output | The code is fine. Check the TX/RX wiring (try swapping them), ground, and baud rate. |
| 1 Hz blink, rates print as 0 | The mailbox request failed or the firmware did not answer that tag. |
| LED flashes fast (10 Hz) without stopping | A CPU exception occurred. The `FAULT:` line gives its type and address. |
| Garbled UART text | The UART clock is not 48 MHz. The banner's `uart` rate shows the real value. |

**First hardware try (2026-11-03).** The first Z0 image showed a short flicker and then a solid LED, with no UART connected. That image had no early flash and used the library mailbox code described in section 8. It is not yet known whether the code stalled in the mailbox, or GPIO 47 is not the LED. The second image adds the three-flash signal and its own mailbox code to tell these apart.

**Second hardware try (2026-11-04).** The second image gave three quick flashes, then a steady 1 Hz blink. So our code runs, GPIO 47 is the ACT LED, active low is the right polarity, and nothing stalls before the blink loop. The first image's solid LED was most likely the library mailbox stall, although that was not confirmed. The UART output has not been checked yet.

**Third hardware try (2026-11-04).** The UART, read through an RP2040 bridge, showed the mailbox numbers correctly (ARM 700000000 Hz, core 250000000 Hz) but every fixed string as runs of `U`. A load-check build showed why. The firmware log says `Loaded 'kernel.img' to 0x0`, and our code reported running at `0x00000000`. The image is linked for `0x8000`, so every absolute address pointed at memory filled with `0x55`, while position-relative code still ran. The board reports `boardrev 9000c1`, a Pi Zero W. `config.txt` now sets `kernel_address=0x8000` and no longer sets `disable_commandline_tags`, which is the likely trigger.

**Fourth hardware try (2026-11-04): Z0 passed.** The firmware logged `Loaded 'kernel.img' to 0x8000`, and the load check reported `run@0x00008000` with all 8188 filler bytes intact. The heartbeat then printed once per second for 26 seconds:

```
board revision 0x009000c1, firmware revision 0x6aa809c1
ARM memory base 0x00000000 size 448 MB
set rates: arm 700000000 Hz, core 250000000 Hz, uart 48000000 Hz
hb 1 s: arm 1000104000 Hz, core 400000000 Hz (measured)
hb 2 s: arm 1000000000 Hz, core 400000000 Hz (measured)
```

The "set" rates (`GET_CLOCK_RATE`) are the base clocks, and the measured rates are the turbo clocks the firmware is actually running. The diagnostic load check, with its 8 KB filler and memory dumps, is still in the passing image. It can be cut down to the one-line `run@` report at Z1.

The ACT LED is GPIO 47 and is taken to be active low. If the LED is dark when the code says on, the polarity is reversed, but the 1 Hz blink still shows. Report it, and `BOARD_ACT_LED_ACTIVE_LOW` in `firmware/PiZero/board.h` gets flipped.

## 7. Try USB boot with rpiboot

The BCM2835 boot ROM falls back to USB device boot when it cannot boot from the SD card. The `rpiboot` tool on the Mac then sends the boot files over USB. If this works, every later build can be loaded without moving the SD card. Whether it works on this unit is not yet known.

1. Install the tool.

   ```bash
   brew install rpiboot
   ```

2. Take the SD card out of the Zero and unplug its power.
3. Start the file server on the Mac, pointing it at the staged boot files.

   ```bash
   rpiboot -d firmware/PiZero/build/sdcard
   ```

   It prints `Waiting for BCM2835/6/7/2711/2712...` and waits. It also asks you to fit an `nRPIBOOT` jumper. That applies to Compute Modules only, and on the Zero, removing the SD card does the same job. Adding `-l` keeps it running, so it serves the files again each time the Zero resets.
4. Connect a data micro USB cable from the Mac to the Zero's **USB** port, not PWR. That port powers the board as well.
5. `rpiboot` should report sending `bootcode.bin`, and then each file the firmware asks for, `kernel.img` included. The UART should then show the same output as an SD card boot.

On 2026-11-03, Homebrew `rpiboot` version `20261002~115811` started and waited without `sudo` on this Mac. If it reports a permission or "Failed to open the requested device" error once the Zero is connected, run the same command with `sudo`. If it never sees the device, check the cable, because many micro USB cables carry power only.

A successful USB boot also confirms the data path through the **USB** port that Z1 needs.

## 8. Notes for Z1

These came up while writing Z0 and matter once TinyUSB's `broadcom_32bit` BSP is used.

- `broadcom-peripherals` `boot.s` sets the IRQ stack twice, so the FIQ stack is never set. It also never enables the VFP, which our `-mfloat-abi=hard` build needs. `firmware/PiZero/startup.S` replaces it and fixes both.
- The ARM1176's VFP11 needs support code for denormals and some underflow cases. Our startup sets RunFast mode (flush-to-zero and default NaN) so it never needs that code. DSP code at Z4 must not rely on denormals.
- `gpio_set_pull()` in `broadcom-peripherals` writes the BCM2711 pull registers at offset `0xE4`. The BCM2835 instead uses `GPPUD` and `GPPUDCLK0/1` at `0x94` to `0x9C`. TinyUSB's `family.c` calls it for its LED pin. Avoid it on the Zero, or write a BCM2835 version.
- TinyUSB's `family.c` drives an LED on GPIO 18, not the ACT LED, and uses the mini UART, whose baud rate depends on the core clock. Z0 uses the PL011 instead, which has its own 48 MHz clock.
- `vcmailbox_request()` has three problems, so `firmware/PiZero/mbox.c` replaces it. It waits on mailbox 0's FULL flag before writing, but writes go to mailbox 1, so it should check `STATUS1`. It times out after 10 million polls rather than a set time, which with caches off and `-O0` is tens of seconds per failed call. It also passes the buffer's ARM address with no bus alias. The BCM2835 firmware enables the VideoCore L2 cache for ARM accesses by default, so the GPU should be given `address | 0x40000000`. Its `.c` file also lacks a declaration in `vcmailbox.h`.
- `gpio.h` needs `stdbool.h` included before it.
- Do not rely on the firmware's default load address. With firmware `1.20260915`, no device tree and `disable_commandline_tags=1`, a Zero W loaded `kernel.img` at `0x0`. `config.txt` must set `kernel_address=0x8000` to match `pizero.ld`. The boot report line `LC ... run@` prints the address our code actually started at.
- With the MMU off, every data access is strongly ordered and an unaligned access faults. The build uses `-mno-unaligned-access` for that reason. Z1 still runs with the MMU and D-cache off. They come in once DMA or DSP load needs them.

## 9. Z1: TinyUSB vendor echo

*Added: 2026-11-04*

The Z1 image is `pizero_z1.img`, which the build stages as `sdcard/kernel.img`. It runs TinyUSB 0.18.0's DWC2 device driver with one vendor-class interface (VID:PID `cafe:5a31`, TinyUSB's test VID). The interface has a 64-byte bulk OUT and IN pair, and every byte received on OUT is sent back on IN. The UART prints the controller's ID and hardware configuration registers at boot, then one heartbeat a second with the mount state, link speed, event counts and byte counts.

Three choices differ from TinyUSB's own `broadcom_32bit` board support.

- **Full Speed is forced through the HS PHY.** With `CFG_TUD_MAX_SPEED = OPT_MODE_FULL_SPEED`, TinyUSB selects the dedicated Full Speed PHY (`GUSBCFG.PHYSEL`) and sets `DCFG.DSPD = 3`. It is not confirmed that the BCM2835 has that PHY. Z1 builds for High Speed so TinyUSB sets up the UTMI+ PHY, then sets `DCFG.DSPD = 1` (Full Speed on the HS PHY) between `tud_disconnect()` and `tud_connect()`. The host therefore sees a 12 Mbit/s device and never gets the High Speed chirp.
- **The controller is polled.** The main loop calls `tud_int_handler(0)` and `tud_task()`, and CPU interrupts stay masked. `BP_EnableIRQ()` and `BP_DisableIRQ()` are no-op stubs, so the library's `interrupts.c` is not needed. Moving to the USB interrupt is a later step, probably at Z3.
- **USB power comes from our mailbox.** `mbox_set_power_state(VCMAILBOX_DEVICE_USB_HCD, 1)` replaces the library's `vcmailbox_set_power_state()`.

The pass test runs on the Mac with Homebrew's Python 3.14, which has pyusb 1.3.1 and libusb.

```bash
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/z1_vendor_echo.py
```

It checks that the device reports Full Speed. It then sends 17 payload sizes from 1 to 1024 bytes for 20 rounds, and compares every echoed byte.

The Zero's **USB** port (the one nearer the mini HDMI, not PWR IN) must connect to the Mac with a data cable. The first-generation Zero W never appeared on USB, either through the dock hub or for `rpiboot`, and its USB port is now suspected dead. The Zero 2 W passed through the same dock hub, so the hub is not the problem.

## 10. Zero 2 W bring-up

*Added: 2026-11-11*

The first-generation Zero W's USB port appears dead, so Z1 moved to a Zero 2 W. The same sources build for both boards, one build directory each.

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2
cmake --build firmware/PiZero/build-zero2
```

`PIZERO_BOARD=zero2` builds 32-bit code for the Cortex-A53 (`-mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard -marm`) with `BCM_VERSION=2837`. The register headers then use the `0x3F000000` peripheral base. The boot files are the same as the Zero's (`bootcode.bin`, `start.elf`, `fixup.dat` at `1.20260915`), and `config.txt` adds `arm_64bit=0`. The firmware boots the image from `0x8000` in 32-bit mode, with its built-in `armstub8-32`.

These are the differences from the BCM2835 build.

- **The firmware enters our code in HYP mode.** `CPS` cannot leave HYP, so `startup.S` drops to SVC with `ERET`. Before that it clears `HCPTR` (except its RES1 bits), `HSTR` and `HCR`, so floating-point and CP15 accesses cannot trap to an HYP vector we never set. Cores 1 to 3 stay parked by the firmware.
- **The ACT LED is GPIO 29.** `startup.S` flickers it for 4 s before any other setup, ending with the pin high, as a sign of life without a UART. The 3 flashes and 1 Hz blink from `board_led_init()` follow. Active low is assumed from the Linux device tree, but both states were visible, so polarity is not yet confirmed.
- **The mailbox uses the `0xC0000000` bus alias.** The BCM2837 ARM does not go through the VideoCore L2, so the GPU must read the buffer uncached.
- **The cache helpers are stubs.** The `broadcom-peripherals` BCM2837 cache code is AArch64 only, so `caches_v7.c` provides empty versions. The D-cache stays off, so there is nothing to maintain.
- **Barriers use `ISB`.** The ARMv6 CP15 prefetch flush stays for the BCM2835 build only.

The C library is newlib-nano's `thumb/v8-a+simd/hard` variant. Its `memcpy` and `memset` copy bytes, so they do not make unaligned accesses with the MMU off.

### rpiboot does not work from this Mac

The Zero 2 W appears as `BCM2710 Boot` (`0a5c:2764`) with no SD card, and `rpiboot -v -l -d firmware/PiZero/build-zero2/sdcard` sends `bootcode.bin`. `bootcode.bin` then serves `start.elf` and `fixup.dat` correctly. Every file `start.elf` asks for afterwards fails with `transfer error: timed out` and `sent 0 bytes`. `config.txt` never arrives, which shows because the firmware then searches the default kernel names (`kernel8.img`, `kernel8-32.img`, `kernel7.img`) instead of the `kernel=kernel.img` it sets. `kernel.img` never arrives either. The firmware still reports `Done: kernel.img`, but our code never runs, so the LED stays solid. Padding the files to a multiple of 512 bytes made no difference. The board was behind two dock hubs, so a direct Mac port and a Linux host are both untested. Without `-l`, `rpiboot` exits at the first timeout.

The Zero 2 W is therefore booted from the SD card, which is prepared as in section 4 from `build-zero2/sdcard`.

### Result

**2026-11-11: Z1 passed on the Zero 2 W.** Booted from the SD card, the LED gave the 4 s flicker, then the 3 flashes and the 1 Hz blink. The Mac enumerated `DSPi PiZero Z1 vendor echo` (`cafe:5a31`, serial `Z1-0001`) at Full Speed, through the dock hub. `z1_vendor_echo.py` passed 20 rounds (340 transfers, 113520 bytes each way). A 500-round soak also passed, with 8500 transfers and 2838000 bytes each way in 15.52 s, about 357 KiB/s round trip, and no mismatches. No UART was connected, so the heartbeat and register dumps were not read.

## 11. Z2: UAC1 enumeration as DSPi

*Added: 2026-11-11*

The Z2 image is `pizero_z2.img`, which the build now stages as `sdcard/kernel.img`. It enumerates with DSPi's real descriptors and answers the audio class requests, but does nothing with the audio yet.

- **Descriptors.** `firmware/DSPi/usb_descriptors.c` is used as it is, except that the product string becomes `Weeb Labs DSPi Zero`. CMake makes that change in a generated copy (`build*/z2/usb_descriptors_zero.c`) at configure time, regenerates it when the DSPi file changes, and stops if the original string is gone. Without `PICO_RP2350` it takes the RP2040 path, which offers stereo alts 1 (16-bit) and 2 (24-bit) at 44.1, 48 and 96 kHz, plus the vendor interface with its bulk notification endpoint `0x83`. Its only Pico SDK header, `pico/unique_id.h`, is replaced by an empty stand-in in `z2/shim`. The `z2` directory comes first in the include path, so its `tusb_config.h` wins over DSPi's Pico one.
- **Class driver.** `z2/uac1_z2.c` is cut down from `firmware/DSPi/usb_audio.c`. It keeps DSPi's open, `SET_INTERFACE` and `GET_INTERFACE` handling, the iso-alloc and iso-activate pattern, and the stall and clear-stall reset of the stream endpoints. It answers mute, volume (-60 to 0 dB, 1 dB steps) and sampling-rate requests, and stalls rates other than 44.1, 48 and 96 kHz. Feedback is the nominal 10.14 value for the selected rate, re-armed from the SOF callback. Received packets are counted and dropped. Vendor requests are not handled yet and stall.
- **Serial number.** The USB serial string is the board's 64-bit VideoCore serial (`mbox_board_serial()`), printed as 16 hex digits.
- **LED.** The LED blinks at 1 Hz while idle and 4 Hz while the host has alt 1 or 2 selected.

**2026-11-11: Z2 passed on the Zero 2 W**, booted from the SD card. This run used DSPi's product string unchanged. The rename to `Weeb Labs DSPi Zero` came afterwards. `system_profiler SPUSBDataType` showed `Weeb Labs DSPi`, `2E8B:FEAA`, version 2.04, serial `0000000065E417C0`, manufacturer `GitHub.com/WeebLabs`, at Full Speed. `system_profiler SPAudioDataType` listed `Weeb Labs DSPi` with 2 output channels, `Current SampleRate: 48000` and USB transport. macOS also made it the default output and system output device. No UART was connected.

## 12. Z3: isochronous data flowing

*Added: 2026-11-11*

Z3 runs on the Z2 image, with three fixes and a way to read its counters without a UART.

**Counters over USB.** The image answers vendor request `0x4F` (device recipient, IN, up to 64 bytes). `0x4F` is one of the few codes DSPi's command set leaves free. The reply starts with magic `0x5A33`, a version, uptime in ms, the selected rate, the alt and the mount state. It then holds counters for class requests, alt changes, rate sets, SOFs, feedback packets armed, sent and flushed, OUT packets and bytes, suspends, resumes and mounts. macOS passes this request to the board while its audio driver owns the device.

**Test.** `tools/z3_stream_check.py` needs pyusb and `SwitchAudioSource` (`brew install switchaudio-osx`).

```bash
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/z3_stream_check.py 10
```

It plays a -20 dBFS 1 kHz tone with `afplay`, and requires 998 to 1002 packets per second for 3 s. It then switches the output to BlackHole 2ch and back ten times, and requires the same rate after each switch, with the device mounted and no suspends. BlackHole is silent, so nothing is heard. The output device in use at the start is restored at the end.

**Fix 1: hang when streaming restarted.** DSPi's driver re-activates the iso endpoints on every alt change. TinyUSB's DWC2 `edpt_disable()` then disables the still-enabled OUT endpoint, which sets `DCTL.SGONAK` and waits with no time limit for `GINTSTS.BOUTNAKEFF` (`dcd_dwc2.c` lines 309-310). In slave mode that flag only sets once the global OUT NAK entry is popped from the RX FIFO. Our main loop is the only reader, and it is the code waiting, so the first switch back hung the board. Both alts use the same data OUT (582 bytes) and feedback IN (3 bytes) endpoints, so `z2/uac1_z2.c` now activates them once per bus reset and leaves them enabled. Alt 0 only stops re-arming. DSPi's stall and clear-stall pair, an RP2040 workaround, is not used on the data endpoint.

**Fix 2: frames lost to UART output.** The board's clock matched the Mac's to 0.01%, yet it counted 994 SOFs and 994 packets per second. The 1 s heartbeat printed about 120 characters at 115200 baud and blocked for about 10 ms, and packets and SOFs arriving in that time were lost. `uart.c` now has a buffered mode. `uart_putc()` queues into a 2 KB ring, dropping on overflow, and `uart_poll()` in the main loop moves only what the TX FIFO can take. `fault_handler()` switches back to blocking output first.

**Fix 3: feedback stuck on the wrong frame parity.** TinyUSB queues an iso IN packet for the next frame's parity (`dcd_dwc2.c` line 366). The feedback endpoint has `bRefresh = 2`, so the Mac polls it every 4 ms, always on frames of one parity. A packet queued on the other parity after a restart is never sent, and feedback dropped to 1 to 6 packets per switch cycle. The SOF callback now flushes a feedback packet still pending after 6 frames (stall, then clear stall, which disables the IN endpoint and flushes its FIFO), and the next SOF queues it on the other parity.

**2026-11-11: Z3 passed on the Zero 2 W.** The steady stream ran at 1000.0 packets per second and 192000 bytes per second (alt 1, 16-bit, 48 kHz). After each of the ten switches the rate was 999.5 to 1000.5 packets per second, with 849 to 870 feedback packets per 3.5 s window, which is about 250 per second. The run ended with 49149 packets, 9436608 bytes, 22 alt changes, 12277 feedback packets sent, 72 feedback flushes, 0 suspends and 1 mount. Before fixes 2 and 3, the same test measured 993 to 994 packets per second, and feedback stalled after the first switch-away.
