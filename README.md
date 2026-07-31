# esp32s3-usb-hid-bridge

Turn a **LOLIN S3 Mini (ESP32-S3)** into a USB **keyboard + mouse** that you
drive with plain-text commands over a virtual COM port.

Plug the board into any computer with a single USB cable and it shows up as
three devices at once (a *composite* USB device):

- a **HID keyboard** — the board "presses" keys on the host;
- a **HID mouse** — moves the cursor, clicks, scrolls;
- a **CDC serial port (VCP)** — you send text commands, the board executes them.

No drivers needed — the host sees a real keyboard and mouse. You type a line
like `type hello`, `combo ctrl+c`, or `move 100 0` into the serial port, and
the board performs it as genuine input.

> This is a personal learning project for getting started with the ESP32-S3
> and understanding how USB HID works. Use it only on machines you own or are
> authorized to test.

## Target hardware

| | |
|---|---|
| **Board** | WEMOS **LOLIN S3 Mini** |
| **Chip** | ESP32-S3 (dual-core Xtensa @ 240 MHz, Wi-Fi + BLE) |
| **Flash / PSRAM** | 4 MB flash, 2 MB PSRAM |
| **USB** | Native USB built into the chip (USB-Serial/JTAG + USB-OTG / TinyUSB) |
| **On-board LED** | RGB (WS2812) on GPIO47 — used here for command feedback |

Why the S3 specifically: its **native USB** is what makes real USB HID
possible. Classic ESP32 and the C3 mini can only do keyboard/mouse over
**BLE**, not USB. The S2 mini works too (same USB HID API), but the C3 does
not have USB-OTG.

## Features

- Composite USB: keyboard **+** mouse **+** serial port on one cable.
- Simple line-based text protocol over the virtual COM port.
- Keyboard: type strings, named special keys, and modifier combos.
- Mouse: relative movement, clicks, double-click, button hold, scroll wheel.
- Safety interlock: `disarm` blocks all input, `arm` re-enables it.
- RGB status blink on every executed command.

## Repository layout

| Path | What it is |
|---|---|
| `src/main.cpp` | Firmware: USB keyboard + mouse + CDC serial command interface |
| `scripts/serial_chat.py` | Host-side console for sending commands (Python + pyserial) |
| `platformio.ini` | Build configuration (PlatformIO), environment `s3hid` |
| `docs/esp32-guide.md` | Beginner's guide to the ESP32 (in Russian) |

## Requirements

- [PlatformIO Core](https://platformio.org/) (`pip install platformio`)
- [pyserial](https://pyserial.readthedocs.io/) for the host console
  (`pip install pyserial`)

## Build & flash

```bash
# 1) Put the board into download mode (needed for the HID firmware, see note):
#    unplug USB, hold BOOT, plug USB back in, release BOOT.

# 2) Build and upload:
python -m platformio run -t upload

# 3) Power-cycle the board (unplug/replug, or press RST) so it boots the app.
```

### ⚠️ Why the manual steps

Once the board is running as a keyboard (USB-OTG mode), the temporary
programming port disappears and esptool's auto-reset does not work over native
USB. So you enter **download mode** by hand before flashing, and do a real
**power-cycle** afterwards to start the application. This is a one-time dance
per flash — normal for native-USB ESP32-S3 boards.

## Usage

Open the host console (it auto-detects the board's COM port):

```bash
python scripts/serial_chat.py
```

Then type commands and press Enter. To test safely, open an empty Notepad and
keep it focused before sending keyboard/mouse commands.

### Command reference

**Keyboard**

| Command | Action |
|---|---|
| `type <text>` | Type a string (US layout, ASCII) |
| `key <name>` | Tap a key or modifier: `enter esc tab space bksp del ins up down left right home end pgup pgdn caps f1..f12 ctrl shift alt win` |
| `combo <a>+<b>+…` | Key combo, e.g. `combo ctrl+c`, `combo ctrl+alt+del`, `combo win+r` |
| `keydown <name>` | Press and hold one key |
| `keyup <name>` | Release **one** key (not everything) — enables e.g. hold-and-tap |
| `hold <name>` | Hold one key (until `unhold` or the deadman timer) |
| `hold <name> <ms>` | Hold, then auto-release after `ms` (non-blocking) |
| `seq <k1> <k2> …` | Tap each key in order, short fixed gap |
| `unhold` | Release everything held |

**Mouse**

| Command | Action |
|---|---|
| `move <dx> <dy>` | Relative move, each `-32767..32767` |
| `moveto <x> <y>` | Absolute move, `x,y = 0..32767` (immune to pointer acceleration) |
| `park` | Cursor to top-left corner `(0,0)` |
| `click [btn]` | Click `left` (default), `right`, or `middle` |
| `dblclick [btn]` | Double-click |
| `down <btn>` / `up <btn>` | Press / release a mouse button |
| `mousehold <btn> <ms>` | Press, auto-release after `ms` (non-blocking) |
| `scroll <n>` | Scroll wheel (`+` up, `-` down), `-32767..32767` |

`moveto` and `park` use an **absolute-positioning HID mouse** (like a graphics
tablet): the cursor lands exactly at the given point, unaffected by Windows
pointer acceleration. They address the **primary monitor** only (`0,0` is its
top-left corner).

**Service**

| Command | Action |
|---|---|
| `arm` / `disarm` | Enable / block all input (`disarm` also releases everything) |
| `ping` / `info` / `help` | Diagnostics and command list |

**Safety.** A deadman timer releases everything if the board is `armed` and
something is held with no pending timer after 5 s idle; timed holds
(`hold`/`mousehold <ms>`) always run to completion. The firmware executes
commands deterministically — all timing is the host's responsibility.

Example session:

```
type Hello from ESP32-S3
key enter
keydown shift
seq h e l l o
keyup shift
moveto 16384 16384
click
```

## How it works

PlatformIO compiles the C++ into `firmware.bin`, esptool writes it to the
board's flash over USB, and the board runs it as the only program on the chip.

For USB HID on the ESP32-S3 two things are essential and easy to miss:

1. **`ARDUINO_USB_MODE=0`** — selects USB-OTG (TinyUSB). In the default
   hardware-CDC mode (`=1`), HID is unavailable.
2. **An explicit `USBCDC` object** — the automatic `CDC_ON_BOOT` path did not
   create the serial interface when combined with HID, so the firmware
   instantiates `USBCDC USBSerial;` directly. Its constructor registers the
   CDC interface, guaranteeing the COM port shows up alongside the keyboard
   and mouse.

Both are configured in `platformio.ini` (env `s3hid`) and `src/main.cpp`.
