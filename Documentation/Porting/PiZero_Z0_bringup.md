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

## 13. USB loader: new builds without moving the SD card

*Added: 2026-11-11*

`rpiboot` fails from this Mac (section 10), so builds went onto the SD card by hand. The loader replaces that. The card is written once, and every later image goes over USB.

**Set up the card (once).** Build, stage the boot files, and copy `sdcard-loader/` to a FAT32 card as in section 4.

```bash
cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2
cmake --build firmware/PiZero/build-zero2
sh firmware/PiZero/tools/fetch_boot_firmware.sh "$PWD/firmware/PiZero/build-zero2"
```

`sdcard-loader/` holds the same `bootcode.bin`, `start.elf` and `fixup.dat`, the loader as `kernel.img`, and `boot/config_loader.txt` as `config.txt`. That config sets `kernel_address=0x02000000`, so the firmware loads the loader at 32 MB. `pizero_loader.ld` links it there, and `pizero.ld` still links every milestone image at `0x8000`. Both include `pizero_sections.ld`. With the loader card in, power the board from the Mac's USB. The LED flickers for 4 s, flashes 3 times, then gives two short flashes a second while the loader waits.

**Load an image.**

```bash
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/pizero_load.py firmware/PiZero/build-zero2/pizero_z2.img
```

If the loader is not on USB, the script asks any running image whose product name contains "Zero" to reboot to it, with vendor request `0x40`. It then waits up to 30 s for the loader. Matching by name means a real DSPi on the same `2E8B:FEAA` is never touched. The script sends the image, waits until the loader reports every byte, and sends GO.

**Protocol** (`loader/loader_proto.h`). The loader enumerates as `cafe:5a4c`, "DSPi PiZero USB loader", with one vendor interface. All requests are vendor requests to the device recipient.

| Request | Direction | Data |
|---|---|---|
| `0x01` LOAD_BEGIN | OUT | 8 bytes: size, then zlib CRC-32 |
| `0x02` STATUS | IN | 16 bytes: state, size, bytes received, maximum size |
| `0x03` GO | OUT | none. Stalls unless every byte arrived and the CRC matches |
| `0x04` REBOOT | OUT | none. Watchdog reset |

Image bytes go to bulk OUT EP `0x01` and are written straight to `0x8000`, up to the loader at 32 MB. GO invalidates the I-cache and branch predictor, then jumps to `0x8000` in SYS mode with interrupts masked. It passes the firmware's original r0 to r2, and r3 = `0x4C4F4144` ("LOAD"), which makes `startup.S` skip the 4 s flicker. The D-cache is off, so no data-cache cleaning is needed.

**Reboot to the loader.** `board_reboot()` in `board.c` does a full reset through the power manager watchdog (`PM_WDOG`, then `PM_RSTC` full reset, at peripheral base + `0x100000`). The GPU firmware then boots the loader from the card again. Z2 calls it for vendor request `0x40` once the status stage is done. `0x40` is one of the codes DSPi's command set leaves free. A hung image cannot answer, so it still needs a power cycle.

**2026-11-11: tested on the Zero 2 W.** With the loader waiting, `pizero_load.py` sent `pizero_z2.img` (30048 bytes, CRC-32 `15054b53`) in 0.07 s, and the board enumerated as `Weeb Labs DSPi Zero`. `z3_stream_check.py` then passed 3 cycles at 1000.0 packets per second. A second load went through reboot-to-loader. The loader was back 7.1 s after the request, the whole round trip took 7 s, and the reloaded image passed the check again. A CRC mismatch and an oversized image are rejected by the loader in code, but neither case has been tried on hardware.

## 14. UAC2 at High Speed

*Added: 2026-11-11*

The Zero 2 W's USB runs at High Speed, so the audio front end moved from UAC1 at Full Speed to UAC2 at 480 Mbit/s, using TinyUSB's own audio class (`audio_device.c`). DSPi Console only uses EP0 and the vendor interface, which keep their layout, so the audio class change does not affect it. The image is `pizero_uac2.img`, loaded over USB:

```bash
cmake --build firmware/PiZero/build-zero2
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/pizero_load.py firmware/PiZero/build-zero2/pizero_uac2.img
/opt/homebrew/bin/python3.14 firmware/PiZero/tools/uac2_stream_check.py 20
```

*Update (2026-11-11): data OUT now runs every 1 ms (`bInterval` 4, 582 bytes maximum, an 8-packet FIFO), not every microframe. See section 15 for why, and for the OUT parity search that makes it work.*

**Device** (`uac2/usb_descriptors.c`). The device is `2E8B:FEAA`, bcdDevice `0x0300`, "Weeb Labs DSPi Zero", with the board serial. The audio function is a UAC2 speaker with these entities:

- clock source 4, with 44.1, 48 and 96 kHz
- input terminal 1
- feature unit 2, master mute and volume, -60 to 0 dB in 1 dB steps
- output terminal 3

Streaming alt 1 is stereo 16-bit and alt 2 is stereo 24-bit (3-byte subslots). Data OUT EP `0x01` runs every microframe (`bInterval` 1, 78 bytes maximum). The explicit feedback IN EP `0x82` runs every 1 ms (`bInterval` 4, 16.16 format). The vendor interface 2 keeps the bulk notify EP `0x83` at 512 bytes, the High Speed bulk size. A small app class driver in `uac2/main_uac2.c` claims it. Feedback uses TinyUSB's FIFO-count method. A timer-paced consumer in the main loop reads the sample rate's worth of audio each millisecond, standing in for an audio output. It starts once the FIFO is half full.

**Interrupts** (`irq.c`, `startup.S`). The USB controller is GPU IRQ 9 on the legacy interrupt controller, which reaches core 0. The IRQ entry saves r0-r3, r12 and lr, FPSCR and the VFP caller-saved registers (d0-d7, plus d16-d31 on the A53), then calls `irq_handler()`. `irq.c` dispatches to registered handlers and implements TinyUSB's `BP_EnableIRQ`/`BP_DisableIRQ`, which mask the USB IRQ around TinyUSB's event queue. Images without `irq.c` get a weak `irq_handler()` in `board.c` that reports a fault, as before. The main loop only runs `tud_task()`, the consumer and buffered UART output.

**Patched DWC2 driver** (`tusb_dwc2/dcd_dwc2.c`, a copy of TinyUSB 0.18.0's driver with each change marked "DSPi PiZero patch"). TinyUSB's audio class re-activates the iso endpoints on every `SET_INTERFACE` and re-arms feedback on completion. On this controller in slave mode, that hit the same two problems as Z3, so the fixes now live in the driver.

1. **Disabling an enabled OUT endpoint.** The driver waits for global OUT NAK effective. In slave mode that status only arrives through the RX FIFO, so the wait loop now pops the RX FIFO itself.
2. **Frame parity for iso endpoints with `bInterval` > 1.**
   - **The problem.** The host polls these endpoints on (micro)frames of one parity, and TinyUSB only sets parity for `bInterval` 1.
   - **Learning the parity.** The driver records the host's parity per endpoint. It reads it from the RX status frame number for OUT, and from the `EONUM` bit after a sent packet for IN. From then on it queues every packet on that parity.
   - **Until it is learned.** The SOF handler holds each parity for 16 (micro)frames in turn, which covers at least two polls, and keeps the SOF interrupt on. A first version followed every frame instead. It failed permanently once the host's IN token came before our SOF interrupt in the microframe, so the packet always carried the previous frame's parity.
   - **Where it applies.** It works for the feedback IN endpoint. For OUT, this controller fixes the parity when the endpoint is enabled and ignores later writes. A 1 ms data endpoint therefore only worked when the first guess was right (1 stream in 11), which is why data stays at `bInterval` 1. There, TinyUSB's own next-frame parity is used.

The patched driver also exports `dcd_dwc2_iso_parity()` for diagnostics.

**TinyUSB workaround in the app.** When a stream stops, a feedback packet is usually still queued. On the next `SET_INTERFACE` the class calls `usbd_edpt_clear_stall()`, which clears `busy` but not `claimed` (`usbd.c` 1418-1419). The class's `usbd_edpt_claim()` then fails for good. `tud_audio_set_itf_cb()` releases the feedback endpoint whenever a streaming alt is selected.

**Bench counters.** Vendor request `0x4F` returns a `0x5532` ("U2") block. It holds uptime, rate, alt, mount state and speed, then counters, then diagnostics:

- counters: class requests, alt changes, rate sets, feedback sent, OUT packets and bytes, consumed bytes, consumer underruns, FIFO level, suspends, resumes, mounts, IRQs taken and spurious
- diagnostics: `DIEPCTL`, `DIEPTSIZ` and `DIEPINT` of EP 2 IN, `DOEPCTL` of EP 1 OUT, `GINTMSK`, `DSTS`, the feedback busy flag, and both learned parities

Request `0x40` reboots to the loader.

**Test** (`tools/uac2_stream_check.py`). It opens the Pi's output by name through PortAudio (`sounddevice`) and plays a -20 dBFS 1 kHz tone, so the Mac's default output is never changed. It stops with `abort()`, because `stop()` once hung inside CoreAudio. Each cycle stops the stream, which makes the host drop to alt 0, and starts it again. A window passes when four things hold:

- the data is within 0.2% of the sample rate in bytes per second
- the paced consumer had no underruns
- feedback ran at over 900 packets per second
- the device stayed mounted with no suspends

Packet counts are reported but not judged. With feedback reaching the host, it varies the frames per microframe, so about 7990 to 8000 packets carry exactly the sample rate.

**2026-11-11: passed on the Zero 2 W.** The device enumerated at High Speed, and Core Audio listed "Weeb Labs DSPi Zero" with 2 output channels at 48 kHz. The 20-cycle run passed:

| Measure | Result |
|---|---|
| Steady stream | 191952 bytes per second |
| Each of 20 restarts | 191882 to 192102 bytes per second, feedback at 1000 to 1006 per second, 0 underruns in the measured windows |
| Whole run | 99376860 bytes, 517541 feedback packets, 94 alt changes, 0 suspends, 8.8 million IRQs |

The 40 underruns outside the windows come at stream edges, as the FIFO drains after the host stops. Earlier runs went wrong in three ways before the fixes above:

- Every-microframe retargeting lost feedback for good after 7 restarts.
- 1 ms data packets only started 1 time in 11.
- An earlier per-SOF write of the feedback endpoint control register lost about 16 packets a second after a while.

## 15. Z4a: DSPi's pipeline on received audio

*Added: 2026-11-11*

`pizero_z4.img` runs DSPi's real RP2350 float pipeline on the UAC2 stream. The DSPi files are compiled unchanged with `PICO_RP2350=1` against stand-in Pico SDK headers in `pico_shim/`. Those files are `audio_pipeline`, `usb_audio`, `dsp_pipeline`, `crossover`, `loudness`, `crossfeed`, `leveller`, `psybass`, `subharm`, `tube`, `limiter`, `upmix`, `siggen`, `rta`, `rta_fft`, `rta_bass`, `output_latency`, `pdm_generator`, `audio_input`, `adat_output`, `notify` and `bg_job`. Only the Zero 2 W builds it, because `usb_audio.c`'s 24-bit unpack uses `sbfx` and `bfi`, which the ARM1176 lacks.

**How the pieces connect.**

- **Front end.** `z4/main_z4.c` reads the sample rate's worth of frames from the TinyUSB FIFO each millisecond. It decodes them into DSPi's `buf_l` and `buf_r` with `global_preamp_linear`, then calls `process_input_block()`.
- **Globals.** `z4/z4_globals.c` defines what DSPi's `main.c` and `flash_storage.c` would normally provide.
- **`usb_audio.c`.** It is compiled for its globals, with its UAC1 hook renamed so it is never registered.
- **Output.** The S/PDIF and I2S output rings are the real pico-extras structures. Each enabled ring is drained at the sample rate by the system timer, which stands in for an audio output until Z5.
- **Single core.** Core 1 stays off, so every output runs on the single-core path.
- **Init.** The sequence follows DSPi's `core0_init` without the hardware parts. There is no preset load, so factory defaults apply, including -20 dB master volume.
- **Pending.** Vendor commands, bulk params and the EQ update path come in Z4b.
- **Counters.** Vendor request `0x4F` now returns version 2 (204 bytes). It adds `cpu0_load` and block timing, peaks, clip flags and output-ring fill.

**Lost packets with data every microframe.** In the first run only about 5970 of 8000 packets a second arrived, about 25% of the audio. TinyUSB re-arms the data endpoint from `tud_task()` in the main loop, and a 300 us DSP block let two or three 125 us microframes pass with the endpoint unarmed. Data now goes every 1 ms (`bInterval` 4), which gives a block up to 1 ms. This needed the OUT half of patch 2. An enabled OUT endpoint keeps the parity it was enabled with, so while the host's parity is unknown, the SOF handler disables and re-queues the untouched transfer every 16 microframes. Each new queue takes the other parity, and the first packet that lands fixes it.

**Results on the Zero 2 W (2026-11-11).**

- **UAC2 alone, 1 ms data.** 10 of 10 restarts passed at 1000.0 packets per second and 192000 bytes per second, with feedback at about 1002 per second and 0 underruns. The interrupt rate fell from about 15000 to about 1550 per second of streaming.
- **Z4a, 10 of 10 restarts.** All passed with the same figures. DSPi's input peak was 3275 (a -20 dBFS tone) and its output peak 327 (factory master volume -20 dB).
- **Load.** DSPi reported `cpu0_load` 35%, with 339.8 us per block on average and 387 us at most, for 47.5 frames. That is single core with the D-cache off, so every access goes to SDRAM. It is the first load figure on the Cortex-A53, not a tuned one.

**Possible DSPi bug found here (not changed, `firmware/DSPi` is read-only for the port).**

- **The bug.** `db_to_vol[]` (`usb_audio.c:480`) is `uint16_t`, and its last entry, for 0 dB, is `0x8000`. `audio_set_volume()` stores it into `int16_t audio_state.vol_mul` (`usb_audio.c:510`, `usb_audio.h:21`), which makes it -32768. The pipeline scales `vol_mul` by 1/32768 (`audio_pipeline.c:348`).
- **The effect.** At 0 dB host volume the gain would be -1.0, which inverts polarity, and the ramp from the 59 to the 60 step would pass through zero.
- **Scope.** It would affect the RP boards too.

## 16. Z4b: DSPi's control plane, the D-cache, and Console

*Added: 2026-11-12*

`pizero_z4.img` now runs DSPi's control plane as well as its pipeline.

**DSPi files added unchanged.** These are `vendor_commands`, `bulk_params`, `flash_storage`, `usb_feedback_controller`, `input_source_names`, `lg_sound_sync`, `dac_hw_mute`, `uart_control`, `i2c_control`, `control_surfaces` (with `_display`, `_ir` and `_nouns`) and `usb_descriptors`. `usb_descriptors.c` is linked only for `desc_ms_os_20` and the serial string. Its descriptor callbacks are renamed, because the UAC2 descriptors are the ones in use.

**How requests and state are handled.**

- **Vendor requests.** `z4/main_z4.c` answers its own bench requests, then passes every other vendor request at every stage to DSPi's handler (renamed `dspi_vendor_control_xfer_cb`). The bench requests are `0x4F` stats and `0x40` reboot to loader, and only when sent to the device recipient. DSPi Console and DSPi's test harness address interface 2, so for them those codes stall as on an RP2350.
- **Main-loop handlers.** `z4/z4_mainloop.c` ports DSPi's `main.c` handlers for the pending flags that vendor commands set, calling DSPi's own functions, gated the same way. That covers bulk params, EQ recompute, rate change, presets, factory reset, output config, dynamics coefficients and background jobs. Hardware-only work is stubbed in `z4/dspi_standins.c`. S/PDIF, ADAT and I2S inputs never lock. Control-surface and UART/I2C bindings are configurable but never act. The ADAT output never starts.
- **Presets.** Presets live in a 128 KB RAM window that stands in for the top of a 4 MB flash part, with NOR erase and program rules. They are lost at reboot until SD storage exists.
- **Notifications.** The vendor app driver drains them to EP `0x83`, using the same claim, peek, transfer and commit sequence as DSPi's `usb_notify_drain`, with a 1-byte keep-alive after 100 ms.

**MMU and D-cache** (`caches_v7.c`, `mmu.h`).

- **The map.** `mmu_enable()` identity-maps SDRAM below `0x3F000000` as Normal write-back write-allocate memory, in 1 MB sections. The peripherals and the ARM local block are Device and execute-never. It invalidates the data caches by set/way before turning on M, C, I and Z, with alignment checking off.
- **Cache maintenance.** `data_clean()` and `data_invalidate()` are now real (DCCMVAC/DCCIMVAC, 64-byte lines). `mbox_call()` uses them, because the GPU reads the buffer from SDRAM. USB slave mode uses no DMA, so nothing else needs maintenance.
- **Effect.** With the cache on, a DSP block takes 44.6 us on average and 59 us at most for 48 frames, against 340 to 420 us with it off. `cpu0_load` fell from 35 to 62% to 4%. The higher figure was after the control plane went in, which also cost about 3.5% of packets.
- **Spurious IRQs.** The faster handler returned before the USB controller's interrupt-clear writes landed, so `usb_irq()` now reads `GINTSTS` back before returning.

**Platform identity and System Information.**

- **Board byte.** A `GET_PLATFORM` request for 8 or more bytes gets DSPi's 7 bytes plus byte 7 = `1`, which means Raspberry Pi Zero 2 W. Byte 0 stays RP2350, the code base every Console feature gate uses. A 7-byte request still goes to DSPi unchanged. DSPi Console (`StatsView.swift`) asks for 8 bytes and shows the board name in Device Information. Its `platformName` gating is unchanged.
- **Sensor readings.** `GET_STATUS` 13, 14 and 16 return the VideoCore's measured ARM clock, core voltage (tag `0x00030003`, id 1) and SoC temperature (tag `0x00030006`). The main loop caches them once a second, because a mailbox round trip is too slow for a control request. Measured values were 1000000000 Hz, 1256 mV and 36.48 °C.

**ARM clock.** The VideoCore keeps the ARM at `arm_freq_min` (600 MHz on the Zero 2 W) until the OS asks for more, as Linux's cpufreq driver does. The 1000 MHz read just after boot dropped to 600 MHz once the firmware's boot period ended. Z4 now asks for the maximum at boot, using GET_MAX_CLOCK_RATE `0x00030004` then SET_CLOCK_RATE `0x00038002` with skip-turbo 0. The clock then held 1000 MHz over a minute of checks. The firmware still throttles on temperature. The earlier block timings were taken soon after boot, at 1000 MHz, so they stand.

**DSPi Console** found the board on its own and polled it at about 36 requests a second with none stalled.

**DSPi's control-plane harness** (`python3.14 -m tools.dspi_test.run`, no flash tests) passed 110 of 138 tests, with 12 failures, 0 errors and 16 skipped, over 2805 GETs and 1249 SETs. That run had the D-cache off, and the harness restored the state byte for byte. The failures broke down as follows:

| Failures | Cause |
|---|---|
| 1 | The bench `0x4F` answered an interface request. Fixed since. |
| 4 | GPIO 12 is not rejected as on an RP2350, because pin rules differ. Expected. |
| 1 | Core 1 modes stay off in this single-core build. Expected. |
| 1 | Clock diagnostics answer zero-length, as release RP2350 builds do. Part of the read-everything test. |
| 2 | Stale test expectations. The subharmonic headroom (4.14 dB, `subharm.c:499-546`) and the tube drive floor (-30 dB, `tube.h:42`) match DSPi's current code, so an RP2350 fails these the same way. |
| 1 | The RTA config validation does not stall over USB. DSPi deliberately acknowledges a rejected SET on USB (`vendor_commands.c:4629-4634`). |
| 1 | RTA busy time over budget, measured with the D-cache off. |
| 1 | The RTA did not auto-stop, most likely because Console's spectrum view was reading the analyser at the same time. |

**Rerun on 2026-11-12, with the D-cache on and the ARM at 1000 MHz.**

- **All groups except RTA, Console open:** 107 of 130 passed, with 8 failures, 0 errors and 15 skipped. The failures were exactly the expected and stale ones above, and the `0x4F` test now passes.
- **RTA group, Console closed:** 5 of 8 passed, with 2 failures and 1 skipped.
  - Auto-off now passes, which confirms Console's spectrum view caused that failure.
  - Sine placement, pink-noise flatness, channel rotation and frame sequence all pass.
  - Config validation still fails, for DSPi's USB acknowledgement reason above.
  - Busy time fell from 23049 to 13760 us per second. That is 1.4% of the CPU, but still over the harness's 5000 us RP2350 budget. Cache alone gave only 1.7 times, so the window probably includes interrupt time, or the FFT is not using NEON. This is the one item still to investigate.

**Console check on 2026-11-12.** With audio playing straight to "Weeb Labs DSPi Zero", DSPi Console showed the input and output meters moving, and an EQ change took effect on the output. That completes Z4 on the Zero 2 W.

## 17. Z4 block time: newlib-nano's memset

*Added: 2026-11-12*

The 4% CPU figure was too high for a 1 GHz core next to the RP2350's 8 to 9% at 307.2 MHz. In cycles, the Pi was spending 44,700 per 1 ms block against roughly 26,000 on the Cortex-M33.

**Tools.** Two bench tools now show where the time goes.
- **Counters.** `z4/pmu.h` turns on the Cortex-A53 cycle counter and five event counters. Startup now sets HDCR so SVC mode owns them. The `0x4F` stats reply (version 4, 264 bytes) adds per-block cycles, the USB interrupt's share, instructions and cache refills. `tools/z4_perf.py` reports them.
- **Sampling profiler.** `z4/z4_prof.c` takes system timer channel 3 interrupts about every 37 us and records the interrupted PC, which `startup.S` now saves as `irq_return_pc`, while `process_input_block()` runs. Bench requests `0x4D` (start or stop) and `0x4E` (read) serve it. `tools/z4_prof.py ELF` maps the samples to functions.

**Cause.** 45% of the block time was in `memset`. We link newlib-nano, whose `memset` and `memcpy` are Thumb byte loops. DSPi clears its output and staging buffers with `memset` on every block, and on the RP2350 the Pico SDK replaces both with fast versions. The USB interrupt was not a factor: 343 cycles per block, and 0.88% of the CPU overall.

**Fix.** `mem_ops.c` supplies NEON `memset` and `memcpy` for `pizero_z4`. The per-file flags now match DSPi's `CMakeLists.txt` exactly, which adds `-O3` for seven control-path files and `-O3 -ffp-contract=off` for `rta_bass.c`.

**Result.** A block now takes 24,200 cycles (24.2 us), and `cpu0_load` reads 2%. That is 17,900 instructions at 0.74 instructions per cycle, with few cache misses. With Console's spectrum view open, the remaining time splits as follows.

| Share | Where |
|---|---|
| 42% | `process_input_block()` itself, a third of it in the output peak meters |
| 26% | `rta_bass_push`, the RTA's bass path, which runs only while the analyser is read |
| 18% | The output ring stand-ins, which copy the real drivers' bookkeeping and memory traffic |
| 4% | `memset` |

Per clock cycle the A53 is now only about as fast as the M33 on this code. DSPi's DSP code is scalar, single-precision floating point with long dependency chains, and the in-order A53 has a 4-cycle floating-point latency, where the M33's is about 1. Going further would mean restructuring DSPi's loops, for example processing channels side by side with NEON, which is outside this port's no-changes rule.

