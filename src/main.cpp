// esp32s3-usb-hid-bridge — composite USB device on the ESP32-S3
//
// With a single USB cable the board presents THREE devices at once
// (a composite USB device):
//   1) CDC  — a virtual COM port (VCP): receives text commands
//   2) HID keyboard — the board "presses" keys on the host
//   3) HID mouse    — moves the cursor, clicks, scrolls
//
// You send a command line to the COM port and the board executes it.
// The host sees a real keyboard and mouse; no drivers required.
//
// WHY AN EXPLICIT USBCDC (instead of the built-in Serial):
// the automatic ARDUINO_USB_CDC_ON_BOOT path did not bring up the CDC
// interface when combined with HID (only the keyboard and mouse showed up
// in the descriptor). Creating the USBSerial object by hand guarantees its
// constructor registers the CDC interface. This mirrors the official
// CompositeDevice example.
//
// Build in the s3hid environment (ARDUINO_USB_MODE=0, CDC_ON_BOOT=0).

#if ARDUINO_USB_MODE
#warning "This firmware needs USB-OTG mode. Build the s3hid environment."
void setup() {}
void loop() {}
#else

#include <Arduino.h>
#include "USB.h"
#include "USBCDC.h"
#include "USBHID.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

USBCDC USBSerial;         // our virtual COM port (CDC)
USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
USBHID HID;              // separate handle, only used to query HID.ready()

String line;              // accumulates command characters until Enter
bool armed = true;        // safety interlock: false => HID commands ignored

// ── Feedback: briefly blink the on-board RGB LED ─────────────────────
void blip(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, r, g, b);
  delay(30);
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
}

// ── Replies to the COM port so results are visible in the terminal ───
void ok(const String& msg)  { USBSerial.println("OK: "  + msg); blip(0, 12, 0); }
void err(const String& msg) { USBSerial.println("ERR: " + msg); blip(16, 0, 0); }

void printHelp() {
  USBSerial.println(F("=== esp32s3-usb-hid-bridge — commands (line + Enter) ==="));
  USBSerial.println(F("KEYBOARD:"));
  USBSerial.println(F("  type <text>       type a string (US layout, ASCII)"));
  USBSerial.println(F("  key <name>        tap a special key: enter esc tab space"));
  USBSerial.println(F("                    backspace delete up down left right home"));
  USBSerial.println(F("                    end pgup pgdn ins f1..f12"));
  USBSerial.println(F("  combo a+b+..      key combo: combo ctrl+c | ctrl+alt+del"));
  USBSerial.println(F("                    win+r | alt+tab | ctrl+shift+esc"));
  USBSerial.println(F("  hold <key>        press and hold (a modifier or a key)"));
  USBSerial.println(F("  unhold            release everything held"));
  USBSerial.println(F("MOUSE:"));
  USBSerial.println(F("  move <dx> <dy>    move the cursor (pixels, negative ok)"));
  USBSerial.println(F("  click [btn]       click: left(default) right middle"));
  USBSerial.println(F("  dblclick [btn]    double click"));
  USBSerial.println(F("  down <btn> / up <btn>   press/release a mouse button"));
  USBSerial.println(F("  scroll <n>        wheel: + up, - down"));
  USBSerial.println(F("SERVICE:"));
  USBSerial.println(F("  ping   info   help   arm   disarm"));
  USBSerial.println(F("Safety: disarm blocks input, arm re-enables it."));
}

void printInfo() {
  USBSerial.printf("Chip:      %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  USBSerial.printf("USB:       composite CDC + HID keyboard + HID mouse\n");
  USBSerial.printf("HID ready: %s\n", HID.ready() ? "yes" : "no (host not polling?)");
  USBSerial.printf("Armed:     %s\n", armed ? "yes" : "no");
  USBSerial.printf("Uptime:    %lu s\n", millis() / 1000);
}

// ── Key name parsing ─────────────────────────────────────────────────
// Returns the HID code for a name; good=false if the name is unknown.
uint8_t keyByName(String n, bool& good) {
  good = true;
  n.toLowerCase();
  if (n == "enter" || n == "return") return KEY_RETURN;
  if (n == "esc"   || n == "escape") return KEY_ESC;
  if (n == "tab")                    return KEY_TAB;
  if (n == "space")                  return ' ';
  if (n == "backspace" || n == "bksp") return KEY_BACKSPACE;
  if (n == "delete" || n == "del")   return KEY_DELETE;
  if (n == "insert" || n == "ins")   return KEY_INSERT;
  if (n == "up")                     return KEY_UP_ARROW;
  if (n == "down")                   return KEY_DOWN_ARROW;
  if (n == "left")                   return KEY_LEFT_ARROW;
  if (n == "right")                  return KEY_RIGHT_ARROW;
  if (n == "home")                   return KEY_HOME;
  if (n == "end")                    return KEY_END;
  if (n == "pgup"  || n == "pageup")   return KEY_PAGE_UP;
  if (n == "pgdn"  || n == "pagedown") return KEY_PAGE_DOWN;
  if (n == "caps"  || n == "capslock") return KEY_CAPS_LOCK;
  if (n.length() >= 2 && n[0] == 'f') {            // f1..f12
    int num = n.substring(1).toInt();
    if (num >= 1 && num <= 12) return KEY_F1 + (num - 1);
  }
  if (n.length() == 1) return (uint8_t)n[0];        // single character
  good = false;
  return 0;
}

// Modifier by name; 0 if it is not a modifier.
uint8_t modByName(String n) {
  n.toLowerCase();
  if (n == "ctrl" || n == "control") return KEY_LEFT_CTRL;
  if (n == "shift")                  return KEY_LEFT_SHIFT;
  if (n == "alt")                    return KEY_LEFT_ALT;
  if (n == "win" || n == "gui" || n == "cmd" || n == "meta") return KEY_LEFT_GUI;
  return 0;
}

uint8_t mouseButtonByName(String n) {
  n.toLowerCase();
  if (n == "right")  return MOUSE_RIGHT;
  if (n == "middle") return MOUSE_MIDDLE;
  return MOUSE_LEFT;  // default, and for "left" or empty
}

// The mouse moves in int8 packets (-127..127) — split large deltas.
void mouseMove(int dx, int dy) {
  while (dx != 0 || dy != 0) {
    int sx = constrain(dx, -127, 127);
    int sy = constrain(dy, -127, 127);
    Mouse.move((int8_t)sx, (int8_t)sy);
    dx -= sx;
    dy -= sy;
    if (dx != 0 || dy != 0) delay(2);
  }
}

void mouseScroll(int amount) {
  while (amount != 0) {
    int s = constrain(amount, -127, 127);
    Mouse.move(0, 0, (int8_t)s);
    amount -= s;
    if (amount != 0) delay(2);
  }
}

// ── Execute a single command ─────────────────────────────────────────
void handleCommand(const String& raw) {
  String input = raw;
  input.trim();
  if (input.length() == 0) return;

  // split into the first word (command) and the rest (argument)
  int sp = input.indexOf(' ');
  String cmd = (sp < 0) ? input : input.substring(0, sp);
  String arg = (sp < 0) ? ""    : input.substring(sp + 1);
  cmd.toLowerCase();
  arg.trim();

  // --- service commands always work ---
  if (cmd == "help")   { printHelp(); return; }
  if (cmd == "info")   { printInfo(); return; }
  if (cmd == "ping")   { USBSerial.println("pong"); return; }
  if (cmd == "arm")    { armed = true;  ok("armed — input enabled");  return; }
  if (cmd == "disarm") { armed = false; Keyboard.releaseAll(); ok("disarmed — input blocked"); return; }

  // --- anything that actually drives the host requires armed ---
  if (!armed) { err("disarmed: send 'arm' first"); return; }

  if (cmd == "type") {
    if (arg.length() == 0) { err("text required: type Hello"); return; }
    Keyboard.print(arg);
    ok("typed " + String(arg.length()) + " chars");
    return;
  }

  if (cmd == "key") {
    bool good;
    uint8_t k = keyByName(arg, good);
    if (!good) { err("unknown key: " + arg); return; }
    Keyboard.press(k);
    delay(8);
    Keyboard.releaseAll();
    ok("key " + arg);
    return;
  }

  if (cmd == "combo") {
    if (arg.length() == 0) { err("example: combo ctrl+alt+del"); return; }
    int start = 0;
    bool failed = false;
    // parse tokens separated by '+'
    while (start <= arg.length()) {
      int plus = arg.indexOf('+', start);
      String tok = (plus < 0) ? arg.substring(start) : arg.substring(start, plus);
      tok.trim();
      if (tok.length() > 0) {
        uint8_t m = modByName(tok);
        if (m) {
          Keyboard.press(m);
        } else {
          bool good;
          uint8_t k = keyByName(tok, good);
          if (!good) { failed = true; break; }
          Keyboard.press(k);
        }
        delay(6);
      }
      if (plus < 0) break;
      start = plus + 1;
    }
    delay(8);
    Keyboard.releaseAll();
    if (failed) err("unknown token in combo: " + arg);
    else        ok("combo " + arg);
    return;
  }

  if (cmd == "hold") {
    uint8_t m = modByName(arg);
    if (m) { Keyboard.press(m); ok("hold " + arg); return; }
    bool good;
    uint8_t k = keyByName(arg, good);
    if (!good) { err("unknown key: " + arg); return; }
    Keyboard.press(k);
    ok("hold " + arg);
    return;
  }

  if (cmd == "unhold" || cmd == "release") {
    Keyboard.releaseAll();
    ok("released");
    return;
  }

  if (cmd == "move") {
    int s2 = arg.indexOf(' ');
    if (s2 < 0) { err("two numbers required: move 100 -40"); return; }
    int dx = arg.substring(0, s2).toInt();
    int dy = arg.substring(s2 + 1).toInt();
    mouseMove(dx, dy);
    ok("move " + String(dx) + " " + String(dy));
    return;
  }

  if (cmd == "click") {
    Mouse.click(mouseButtonByName(arg));
    ok("click " + (arg.length() ? arg : String("left")));
    return;
  }

  if (cmd == "dblclick") {
    uint8_t b = mouseButtonByName(arg);
    Mouse.click(b);
    delay(40);
    Mouse.click(b);
    ok("dblclick " + (arg.length() ? arg : String("left")));
    return;
  }

  if (cmd == "down") {
    Mouse.press(mouseButtonByName(arg));
    ok("mouse down " + (arg.length() ? arg : String("left")));
    return;
  }

  if (cmd == "up") {
    Mouse.release(mouseButtonByName(arg));
    ok("mouse up " + (arg.length() ? arg : String("left")));
    return;
  }

  if (cmd == "scroll") {
    int n = arg.toInt();
    mouseScroll(n);
    ok("scroll " + String(n));
    return;
  }

  err("unknown command: " + cmd + " (type help)");
}

void setup() {
  // names the device is shown under in the OS
  USB.productName("USBHelper HID");
  USB.manufacturerName("USBHelper");

  USBSerial.begin();     // bring up the virtual COM port (CDC)
  Keyboard.begin();      // + HID keyboard
  Mouse.begin();         // + HID mouse
  USB.begin();           // start the whole composite USB device

  delay(600);            // give the port time to come up
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
  USBSerial.println();
  USBSerial.println("=== esp32s3-usb-hid-bridge ready ===");
  USBSerial.println("Type 'help' for the command list.");
}

void loop() {
  // read commands from the COM port line by line
  while (USBSerial.available() > 0) {
    char c = (char)USBSerial.read();
    if (c == '\n') {
      handleCommand(line);
      line = "";
    } else if (c != '\r') {
      line += c;
      if (line.length() > 240) line = "";  // guard against garbage
    }
  }
}

#endif  // ARDUINO_USB_MODE
