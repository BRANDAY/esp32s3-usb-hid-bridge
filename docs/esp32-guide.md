# ESP32 from scratch — a starter guide

This is your entry point. Read top to bottom, try the commands, and when you
want the next step, just ask.

## 1. What an ESP32 is

A microcontroller is a tiny computer on a single chip: a processor, memory, and
"pins" it uses to control the outside world. Unlike a normal computer:

- there is no operating system, no windows, no files — only your program runs;
- the program starts instantly on power-up and runs as long as it has power;
- in exchange it drives pins directly: lighting LEDs, reading buttons and
  sensors, spinning motors.

The ESP32 is a microcontroller family from Espressif and a great choice for
learning. This project uses the **ESP32-S3**:

| Feature | ESP32-S3 | For comparison: Arduino Uno |
|---|---|---|
| Processor | 2 cores, 240 MHz | 1 core, 16 MHz |
| RAM | 512 KB (+ 2 MB PSRAM) | 2 KB |
| Flash (for firmware) | 4 MB | 32 KB |
| Wi-Fi | yes | no |
| Bluetooth | BLE (no Classic) | no |
| Native USB | yes (USB-OTG) | no |

For roughly the same price you get orders of magnitude more speed and memory,
plus a radio and native USB.

## 2. This board: LOLIN S3 Mini

| | |
|---|---|
| Board | WEMOS **LOLIN S3 Mini** |
| Chip | ESP32-S3 (silk screen on the metal can reads `ESP32-S3`) |
| USB | **native USB built into the chip** — no separate USB-UART chip |
| Port | shows up as `VID:PID=303A:...` in the port list |
| On-board LED | RGB (WS2812) on GPIO47 |

To list serial ports and confirm the board:

```
python -m serial.tools.list_ports -v
```

A `VID:PID=303A:...` entry means the USB is built into the chip (that is the
S3 / S2 / C3 family). Classic ESP32 boards instead show a separate USB-UART
chip (`1A86` for CH340, `10C4` for CP210x).

## 3. What is on the board

- **Metal-can module** — the ESP32-S3 itself: processor and flash inside; the
  zig-zag trace on the board edge is the printed Wi-Fi antenna.
- **USB connector** — power **and** the data link to the computer. On the S3
  the USB goes straight into the chip (there is no CH340/CP2102 bridge).
- **Voltage regulator** — turns the 5 V from USB into 3.3 V for the chip.
- **Buttons**: RST (also EN) — reset; BOOT (also IO0) — enters flashing mode.
- **Pin headers (GPIO)** — where LEDs, buttons and sensors connect.

⚠️ **Golden rule**: ESP32 pins are **3.3 V**. Putting 5 V on a GPIO burns the
pin. Powering the board over USB is fine and expected.

## 4. What the ESP32 can do

| Capability | What it is | Example uses |
|---|---|---|
| GPIO | digital in/out (0 or 1) | LEDs, buttons, relays |
| PWM | fast on/off with a set duty cycle | LED brightness, servos, buzzer |
| ADC | measure a voltage on a pin | potentiometer, light sensor |
| Touch | capacitive inputs | a button made of foil |
| I2C | bus for smart sensors | OLED screen, BME280 sensor |
| SPI | fast bus | SD card, colour display |
| UART | serial ports | GPS module, link to another board |
| USB (native) | full USB device | this project: keyboard + mouse + serial |
| Wi-Fi | full 2.4 GHz | web server, internet requests, MQTT |
| BLE | Bluetooth Low Energy | talk to a phone |
| Deep sleep | ~10 µA sleep | years on a battery |
| RMT | precise timing | addressable WS2812 strips |

## 5. How your code gets onto the board

1. You write C++ in `src/main.cpp`.
2. PlatformIO compiles it into machine code → `firmware.bin` (it downloads the
   compiler and libraries itself on the first build).
3. esptool puts the board into flashing mode over USB and writes `firmware.bin`
   into flash.
4. The board reboots and your code becomes the only program on the chip.

The Arduino program model is very simple:

```cpp
void setup() { /* runs once at start */ }
void loop()  { /* repeats forever    */ }
```

(Under the hood the ESP32 runs FreeRTOS — real multitasking across both cores.
There is plenty of room to grow into that later.)

### Flashing a USB-HID firmware — the one catch

Because this firmware turns the board into a USB keyboard/mouse (USB-OTG mode),
the temporary programming port disappears while it runs, and esptool's
auto-reset does not work over native USB. So:

1. **Enter download mode** by hand: unplug USB, hold **BOOT**, plug back in,
   release BOOT.
2. Flash: `python -m platformio run -t upload`.
3. **Power-cycle** the board (unplug/replug, or press RST) so it boots the app.
   A software reset does not reliably start the app on native USB.

## 6. How this project talks to the computer

This is the heart of the project: making the board a **composite USB device**
so one cable presents three devices at once.

### Level 1 — native USB (what this firmware does)

- **CDC (virtual COM port)** — the computer sends text command lines; the board
  reads them with `USBSerial`.
- **HID keyboard + HID mouse** — the board sends real key presses and cursor
  movement. The host sees genuine input devices, no drivers needed.

You type a command such as `type hello`, `key enter`, or `moveto 16384 16384`
into the COM port, and the firmware performs it as real input. See the
[README](../README.md) for the full command reference, or send `help` to the
board. `scripts/serial_chat.py` is the host-side console for this.

### Level 2 — Wi-Fi

The board joins your router and can then run a web server you open from a
phone, reach out to the internet (weather, an API, a Telegram message), or
speak MQTT for home-automation setups.

### Level 3 — BLE (Bluetooth Low Energy)

Talk to a phone directly, without a router. The S3 supports BLE (not classic
Bluetooth).

## 7. Workflow

Run commands inside the project folder. If the `pio` shortcut is not found, use
`python -m platformio` instead.

| Action | Command |
|---|---|
| Build the firmware | `python -m platformio run` |
| Build and flash | `python -m platformio run -t upload` |
| Watch board output | `python -m platformio device monitor` (Ctrl+C to exit) |
| Command console | `python scripts/serial_chat.py` |
| List COM ports | `python -m serial.tools.list_ports -v` |

Only one program can hold the port at a time — close the monitor/console before
flashing.

## 8. Where to go next

The firmware already does native USB HID (Level 1). Good directions from here,
each one a small step:

1. **A physical button** — read a GPIO, learn about pull-ups and contact
   bounce; wire it to trigger a stored keystroke.
2. **The RGB LED** — drive the WS2812 on GPIO47 to show status in colour.
3. **PWM** — smoothly fade an LED's brightness.
4. **ADC** — read a potentiometer's voltage.
5. **An I2C sensor** — BME280 (temperature/humidity/pressure) or an OLED.
6. **Wi-Fi web server** — control the board from a phone.
7. **BLE HID** — a wireless keyboard/mouse instead of USB.

Take each step slowly and read the code as you go.

## 9. If something does not work

| Symptom | Cause and fix |
|---|---|
| Board not in the port list | 1) a "charge only" cable — use a data cable; 2) try another USB port; 3) enter download mode (hold BOOT while plugging in) |
| No COM port after flashing HID firmware | native USB has no auto-reset; power-cycle the board (unplug/replug or RST) so the app starts |
| `Failed to connect to ESP32` while flashing | enter download mode: hold BOOT, tap RST, release BOOT, then flash |
| `could not open port` / port busy | close the monitor, console, or Arduino IDE — only one program can hold the port |
| Garbled text in the monitor | baud rate mismatch — it must be 115200 |
| Board resets when the port opens | normal: opening the port toggles the reset line |

## 10. Further reading

- randomnerdtutorials.com/projects-esp32 — a large collection of step-by-step
  projects (code is universal)
- docs.espressif.com — official Espressif documentation
- docs.platformio.org — build and configuration reference
