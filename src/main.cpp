// esp32s3-usb-hid-bridge — composite USB device on the ESP32-S3
//
// With a single USB cable the board presents several devices at once
// (a composite USB device):
//   1) CDC  — a virtual COM port (VCP): receives text commands
//   2) HID keyboard — the board "presses" keys on the host
//   3) HID mouse (relative) — moves the cursor, clicks, scrolls
//   4) HID mouse (absolute) — a custom HID device that positions the
//      cursor directly (moveto), sharing the same HID interface via its
//      own Report ID (no extra USB endpoint is consumed).
//
// You send a command line to the COM port and the board executes it,
// literally and predictably. All timing decisions (delays, jitter,
// per-game logic) are the host's responsibility — this firmware only
// exposes deterministic HID primitives.
//
// WHY AN EXPLICIT USBCDC (instead of the built-in Serial):
// the automatic ARDUINO_USB_CDC_ON_BOOT path did not bring up the CDC
// interface when combined with HID. Creating the USBSerial object by
// hand guarantees its constructor registers the CDC interface.
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
USBHIDMouse Mouse;        // relative mouse (move/click/scroll)
USBHID HID;              // separate handle, only used to query HID.ready()

// ── Absolute mouse: a custom HID device on the SAME HID interface ────
// Report IDs used by the library: keyboard = 1, relative mouse = 2.
// We pick 10 for the absolute mouse so nothing collides.
#define RID_ABSMOUSE 10

// Report descriptor (raw bytes: TinyUSB has no ready-made abs-mouse macro
// in this build). Layout per report: 3 button bits + 5 padding bits, then
// X and Y as 16-bit absolute (logical 0..32767). No wheel field here —
// scrolling is handled by the relative mouse (RID 2); a dead relative
// wheel on an absolute pointer only wasted a report byte.
static const uint8_t ABSMOUSE_DESC[] = {
  0x05, 0x01,                    // Usage Page (Generic Desktop)
  0x09, 0x02,                    // Usage (Mouse)
  0xA1, 0x01,                    // Collection (Application)
  0x85, RID_ABSMOUSE,            //   Report ID
  0x09, 0x01,                    //   Usage (Pointer)
  0xA1, 0x00,                    //   Collection (Physical)
  0x05, 0x09,                    //     Usage Page (Buttons)
  0x19, 0x01, 0x29, 0x03,        //     Usage Minimum 1, Maximum 3
  0x15, 0x00, 0x25, 0x01,        //     Logical 0..1
  0x95, 0x03, 0x75, 0x01,        //     Count 3, Size 1
  0x81, 0x02,                    //     Input (Data,Var,Abs) — 3 buttons
  0x95, 0x01, 0x75, 0x05,        //     Count 1, Size 5
  0x81, 0x03,                    //     Input (Const) — padding
  0x05, 0x01,                    //     Usage Page (Generic Desktop)
  0x09, 0x30, 0x09, 0x31,        //     Usage X, Usage Y
  0x16, 0x00, 0x00,              //     Logical Minimum 0
  0x26, 0xFF, 0x7F,              //     Logical Maximum 32767
  0x75, 0x10, 0x95, 0x02,        //     Size 16, Count 2
  0x81, 0x02,                    //     Input (Data,Var,Abs) — X,Y absolute
  0xC0,                          //   End Collection (Physical)
  0xC0                           // End Collection (Application)
};

// Payload WITHOUT the report-id byte (SendReport prepends it). Packed so
// the compiler does not insert padding between fields (must be 5 bytes).
typedef struct __attribute__((packed)) {
  uint8_t  buttons;   // bit0=left, bit1=right, bit2=middle
  uint16_t x;         // 0..32767, little-endian (native on ESP32)
  uint16_t y;         // 0..32767
} abs_report_t;

class AbsMouse : public USBHIDDevice {
  USBHID hid;         // facade over the single TinyUSB HID instance
public:
  AbsMouse() {
    // Register once, at global construction, BEFORE USB.begin(). The
    // length here MUST equal what _onGetDescriptor() actually writes.
    hid.addDevice(this, sizeof(ABSMOUSE_DESC));
  }
  void begin() { hid.begin(); }
  uint16_t _onGetDescriptor(uint8_t* dst) override {
    memcpy(dst, ABSMOUSE_DESC, sizeof(ABSMOUSE_DESC));
    return sizeof(ABSMOUSE_DESC);
  }
  // x,y already validated to 0..32767 by the caller. 0,0 = top-left of
  // the PRIMARY monitor (Windows maps this range onto the primary display).
  bool moveTo(uint16_t x, uint16_t y, uint8_t buttons = 0) {
    if (x > 0x7FFF) x = 0x7FFF;
    if (y > 0x7FFF) y = 0x7FFF;
    abs_report_t r = { buttons, x, y };
    return hid.SendReport(RID_ABSMOUSE, &r, sizeof(r));
  }
};
AbsMouse AbsMouseDev;

String line;              // accumulates command characters until Enter
bool armed = true;        // safety interlock: false => HID commands ignored

// ── State tracking for held keys/buttons and the deadman timer ───────
static const uint8_t  MAX_HELD    = 8;
uint8_t  heldKeys[MAX_HELD];        // key codes currently pressed (incl. mods)
uint8_t  heldCount     = 0;
uint8_t  heldMouseMask = 0;         // OR of MOUSE_LEFT/RIGHT/MIDDLE held down

uint32_t lastCmdAt     = 0;         // millis() of the last accepted command
static const uint32_t DEADMAN_MS  = 5000;   // release everything after idle
static const uint32_t SEQ_GAP_MS  = 12;     // short fixed tap gap for `seq`
static const uint32_t HOLD_MS_MAX = 600000; // 1..10 min sanity ceiling
static const int32_t  MOVE_MAX    = 32767;  // clamp for relative move/scroll

// ── Non-blocking release scheduler (auto-release after N ms) ─────────
enum { PEND_NONE = 0, PEND_KEY = 1, PEND_MOUSE = 2 };
struct Pending {
  uint32_t dueAt;   // millis() deadline
  uint8_t  code;    // key HID code, or a single mouse-button mask
  uint8_t  kind;    // PEND_NONE / PEND_KEY / PEND_MOUSE
};
static const uint8_t MAX_PENDING = 12;
Pending pending[MAX_PENDING];

void addHeldKey(uint8_t k) {
  for (uint8_t i = 0; i < heldCount; i++) if (heldKeys[i] == k) return;
  if (heldCount < MAX_HELD) heldKeys[heldCount++] = k;
}
void removeHeldKey(uint8_t k) {
  for (uint8_t i = 0; i < heldCount; i++)
    if (heldKeys[i] == k) { heldKeys[i] = heldKeys[--heldCount]; return; }
}
void cancelPending(uint8_t kind, uint8_t code) {  // drop a scheduled release
  for (uint8_t i = 0; i < MAX_PENDING; i++)
    if (pending[i].kind == kind && pending[i].code == code)
      pending[i].kind = PEND_NONE;
}
// True if schedule(kind,code) is guaranteed to succeed (a matching slot to
// reuse, or a free slot). Lets callers reserve capacity BEFORE they press,
// so a full table never leaves a key/button physically stuck down.
bool hasFreeSlot(uint8_t kind, uint8_t code) {
  for (uint8_t i = 0; i < MAX_PENDING; i++)
    if (pending[i].kind == kind && pending[i].code == code) return true;
  for (uint8_t i = 0; i < MAX_PENDING; i++)
    if (pending[i].kind == PEND_NONE) return true;
  return false;
}
bool schedule(uint8_t kind, uint8_t code, uint32_t ms) {
  int slot = -1;
  for (uint8_t i = 0; i < MAX_PENDING; i++) {
    if (pending[i].kind == kind && pending[i].code == code) { slot = i; break; }
    if (pending[i].kind == PEND_NONE && slot < 0) slot = i;
  }
  if (slot < 0) return false;                     // table full
  pending[slot].dueAt = millis() + ms;
  pending[slot].code  = code;
  pending[slot].kind  = kind;
  return true;
}
// True while any auto-release is still scheduled (used to keep the deadman
// from killing an explicitly-timed hold before its timer expires).
bool anyPending() {
  for (uint8_t i = 0; i < MAX_PENDING; i++)
    if (pending[i].kind != PEND_NONE) return true;
  return false;
}
void releaseEverything() {
  Keyboard.releaseAll();
  if (heldMouseMask) Mouse.release(MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
  heldCount = 0;
  heldMouseMask = 0;
  for (uint8_t i = 0; i < MAX_PENDING; i++) pending[i].kind = PEND_NONE;
}

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
  USBSerial.println(F("  key <name>        tap a key: enter esc tab space bksp"));
  USBSerial.println(F("                    del ins up down left right home end"));
  USBSerial.println(F("                    pgup pgdn caps f1..f12 (mods too)"));
  USBSerial.println(F("  combo a+b+..      key combo: combo ctrl+c | win+r"));
  USBSerial.println(F("  keydown <name>    press and HOLD one key"));
  USBSerial.println(F("  keyup <name>      release one key (not everything)"));
  USBSerial.println(F("  hold <name>       hold one key (until unhold/deadman)"));
  USBSerial.println(F("  hold <name> <ms>  hold, auto-release after ms (non-blocking)"));
  USBSerial.println(F("  seq <k1> <k2>..   tap each key in order, short fixed gap"));
  USBSerial.println(F("  unhold            release everything held"));
  USBSerial.println(F("MOUSE:"));
  USBSerial.println(F("  move <dx> <dy>    relative move, each -32767..32767"));
  USBSerial.println(F("  moveto <x> <y>    absolute move, x,y = 0..32767"));
  USBSerial.println(F("  park              cursor to top-left corner (0,0)"));
  USBSerial.println(F("                    moveto/park address the PRIMARY monitor"));
  USBSerial.println(F("                    only (0,0 = its top-left corner)"));
  USBSerial.println(F("  click [btn]       click: left(default) right middle"));
  USBSerial.println(F("  dblclick [btn]    double click"));
  USBSerial.println(F("  down <btn> / up <btn>   press/release a mouse button"));
  USBSerial.println(F("  mousehold <btn> <ms>    press, auto-release after ms"));
  USBSerial.println(F("  scroll <n>        wheel: + up, - down (-32767..32767)"));
  USBSerial.println(F("SERVICE:"));
  USBSerial.println(F("  ping   info   help   arm   disarm"));
  USBSerial.println(F("Safety: disarm blocks input and releases all."));
  USBSerial.println(F("Deadman: if armed and something is held with NO pending"));
  USBSerial.println(F("timer, all is released after 5s idle. Timed holds"));
  USBSerial.println(F("(hold/mousehold <ms>) run to completion. Send any command"));
  USBSerial.println(F("to keep indefinite holds alive."));
}

void printInfo() {
  uint8_t mouseHeld = __builtin_popcount(heldMouseMask);
  USBSerial.printf("Chip:      %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  USBSerial.printf("USB:       CDC + HID keyboard + rel mouse + abs mouse\n");
  USBSerial.printf("HID ready: %s\n", HID.ready() ? "yes" : "no (host not polling?)");
  USBSerial.printf("Armed:     %s\n", armed ? "yes" : "no");
  USBSerial.printf("Held keys: %u   Held mouse btns: %u\n", heldCount, mouseHeld);
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

// Resolve a token to a key code: try modifier first, then named/ASCII key.
bool resolveKey(const String& name, uint8_t& code) {
  uint8_t m = modByName(name);
  if (m) { code = m; return true; }
  bool good;
  uint8_t k = keyByName(name, good);
  if (good) { code = k; return true; }
  return false;
}

uint8_t mouseButtonByName(String n) {
  n.toLowerCase();
  if (n == "right")  return MOUSE_RIGHT;
  if (n == "middle") return MOUSE_MIDDLE;
  return MOUSE_LEFT;  // default, and for "left" or empty
}

// ── Small helpers: whitespace tokenizer + validated numeric parse ────
uint8_t tokenize(const String& s, String* out, uint8_t maxTok) {
  uint8_t n = 0;
  int i = 0, len = s.length();
  while (i < len && n < maxTok) {
    while (i < len && s[i] == ' ') i++;             // skip spaces
    if (i >= len) break;
    int start = i;
    while (i < len && s[i] != ' ') i++;
    out[n++] = s.substring(start, i);
  }
  return n;
}
// Digits only, inclusive [lo,hi] range check (boundary validation).
// Overflow-safe: accumulates in uint64 and bails the moment it exceeds a
// 32-bit range, so a long digit string can never wrap into a valid value.
bool parseUInt(const String& s, uint32_t lo, uint32_t hi, uint32_t& out) {
  if (s.length() == 0) return false;
  uint64_t v = 0;
  for (uint16_t i = 0; i < s.length(); i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (uint64_t)(s[i] - '0');
    if (v > 0xFFFFFFFFULL) return false;            // overflow guard
  }
  if (v < lo || v > hi) return false;
  out = (uint32_t)v;
  return true;
}
// Optional leading '-', digits only, inclusive [lo,hi]. Overflow-safe.
bool parseInt(const String& s, long lo, long hi, long& out) {
  int len = s.length();
  if (len == 0) return false;
  int i = 0;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; if (len == 1) return false; }
  long long v = 0;
  for (; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (long long)(s[i] - '0');
    if (v > 2147483647LL) return false;             // far beyond any range
  }
  if (neg) v = -v;
  if (v < (long long)lo || v > (long long)hi) return false;
  out = (long)v;
  return true;
}

// The relative mouse moves in int8 packets (-127..127) — split deltas.
// dx/dy are pre-validated to |v| <= MOVE_MAX, so this loop is bounded
// (<= ~258 packets per axis). USB polling paces delivery; no added delay.
void mouseMove(int dx, int dy) {
  while (dx != 0 || dy != 0) {
    int sx = constrain(dx, -127, 127);
    int sy = constrain(dy, -127, 127);
    Mouse.move((int8_t)sx, (int8_t)sy);
    dx -= sx;
    dy -= sy;
  }
}

void mouseScroll(int amount) {
  while (amount != 0) {
    int s = constrain(amount, -127, 127);
    Mouse.move(0, 0, (int8_t)s);
    amount -= s;
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

  // Every accepted line refreshes the deadman timer (ping keeps alive too).
  lastCmdAt = millis();

  // --- service commands always work ---
  if (cmd == "help")   { printHelp(); return; }
  if (cmd == "info")   { printInfo(); return; }
  if (cmd == "ping")   { USBSerial.println("pong"); return; }
  if (cmd == "arm")    { armed = true; ok("armed — input enabled"); return; }
  if (cmd == "disarm") { armed = false; releaseEverything(); ok("disarmed — input blocked"); return; }

  // --- anything that actually drives the host requires armed ---
  if (!armed) { err("disarmed: send 'arm' first"); return; }

  if (cmd == "type") {
    if (arg.length() == 0) { err("text required: type Hello"); return; }
    Keyboard.print(arg);
    ok("typed " + String(arg.length()) + " chars");
    return;
  }

  if (cmd == "key") {                     // tap one key (modifiers accepted too)
    uint8_t k;
    if (!resolveKey(arg, k)) { err("unknown key: " + arg); return; }
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
    while (start <= arg.length()) {
      int plus = arg.indexOf('+', start);
      String tok = (plus < 0) ? arg.substring(start) : arg.substring(start, plus);
      tok.trim();
      if (tok.length() > 0) {
        uint8_t code;
        if (!resolveKey(tok, code)) { failed = true; break; }
        Keyboard.press(code);
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

  if (cmd == "keydown") {                 // press and HOLD one key
    uint8_t k;
    if (!resolveKey(arg, k)) { err("unknown key: " + arg); return; }
    Keyboard.press(k);
    addHeldKey(k);
    ok("keydown " + arg);
    return;
  }

  if (cmd == "keyup") {                    // release ONE key (not releaseAll)
    uint8_t k;
    if (!resolveKey(arg, k)) { err("unknown key: " + arg); return; }
    Keyboard.release(k);
    removeHeldKey(k);
    cancelPending(PEND_KEY, k);
    ok("keyup " + arg);
    return;
  }

  if (cmd == "hold") {                     // hold <name> [ms]
    String tok[3];
    uint8_t nt = tokenize(arg, tok, 3);
    if (nt == 0) { err("usage: hold <name> [ms]"); return; }
    uint8_t k;
    if (!resolveKey(tok[0], k)) { err("unknown key: " + tok[0]); return; }
    if (nt >= 2) {                          // timed, non-blocking auto-release
      uint32_t ms;
      if (!parseUInt(tok[1], 1, HOLD_MS_MAX, ms)) {
        err("bad ms (1.." + String(HOLD_MS_MAX) + ")"); return;
      }
      // Reserve the scheduler slot BEFORE pressing, so a full table never
      // leaves a key physically stuck down with no way to auto-release.
      if (!hasFreeSlot(PEND_KEY, k)) { err("scheduler full"); return; }
      Keyboard.press(k);
      addHeldKey(k);
      schedule(PEND_KEY, k, ms);            // guaranteed after hasFreeSlot
      ok("hold " + tok[0] + " " + String(ms) + "ms");
    } else {
      Keyboard.press(k);
      addHeldKey(k);
      ok("hold " + tok[0] + " (until unhold/deadman)");
    }
    return;
  }

  if (cmd == "seq") {                       // sequential taps, short fixed gap
    String tok[12];
    uint8_t nt = tokenize(arg, tok, 12);
    if (nt == 0) { err("usage: seq a b c"); return; }
    for (uint8_t i = 0; i < nt; i++) {      // validate all first
      uint8_t k;
      if (!resolveKey(tok[i], k)) { err("unknown key: " + tok[i]); return; }
    }
    for (uint8_t i = 0; i < nt; i++) {
      uint8_t k; resolveKey(tok[i], k);
      Keyboard.press(k);
      delay(6);
      Keyboard.release(k);
      if (i + 1 < nt) delay(SEQ_GAP_MS);
    }
    ok("seq " + String(nt) + " keys");
    return;
  }

  if (cmd == "unhold" || cmd == "release") {
    releaseEverything();
    ok("released all");
    return;
  }

  if (cmd == "move") {                      // relative, each axis bounded
    String tok[2];
    uint8_t nt = tokenize(arg, tok, 2);
    long dx, dy;
    if (nt < 2 ||
        !parseInt(tok[0], -MOVE_MAX, MOVE_MAX, dx) ||
        !parseInt(tok[1], -MOVE_MAX, MOVE_MAX, dy)) {
      err("usage: move <dx> <dy> (-32767..32767)"); return;
    }
    mouseMove((int)dx, (int)dy);
    ok("move " + String(dx) + " " + String(dy));
    return;
  }

  if (cmd == "moveto") {                    // absolute, 0..32767
    String tok[2];
    uint8_t nt = tokenize(arg, tok, 2);
    uint32_t x, y;
    if (nt < 2 || !parseUInt(tok[0], 0, 32767, x) || !parseUInt(tok[1], 0, 32767, y)) {
      err("usage: moveto <x> <y> (0..32767)"); return;
    }
    if (!AbsMouseDev.moveTo((uint16_t)x, (uint16_t)y)) { err("abs mouse not ready"); return; }
    ok("moveto " + String(x) + " " + String(y));
    return;
  }

  if (cmd == "park") {                      // cursor to the top-left corner
    if (!AbsMouseDev.moveTo(0, 0)) { err("abs mouse not ready"); return; }
    ok("park");
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
    uint8_t b = mouseButtonByName(arg);
    Mouse.press(b);
    heldMouseMask |= b;
    ok("mouse down " + (arg.length() ? arg : String("left")));
    return;
  }

  if (cmd == "up") {
    uint8_t b = mouseButtonByName(arg);
    Mouse.release(b);
    heldMouseMask &= ~b;
    cancelPending(PEND_MOUSE, b);
    ok("mouse up " + (arg.length() ? arg : String("left")));
    return;
  }

  if (cmd == "mousehold") {                 // mousehold <btn> <ms>
    String tok[2];
    uint8_t nt = tokenize(arg, tok, 2);
    if (nt < 2) { err("usage: mousehold <btn> <ms>"); return; }
    uint8_t b = mouseButtonByName(tok[0]);
    uint32_t ms;
    if (!parseUInt(tok[1], 1, HOLD_MS_MAX, ms)) {
      err("bad ms (1.." + String(HOLD_MS_MAX) + ")"); return;
    }
    // Reserve the slot BEFORE pressing (same reason as `hold`).
    if (!hasFreeSlot(PEND_MOUSE, b)) { err("scheduler full"); return; }
    Mouse.press(b);
    heldMouseMask |= b;
    schedule(PEND_MOUSE, b, ms);            // guaranteed after hasFreeSlot
    ok("mousehold " + tok[0] + " " + String(ms) + "ms");
    return;
  }

  if (cmd == "scroll") {
    long n;
    if (!parseInt(arg, -MOVE_MAX, MOVE_MAX, n)) {
      err("usage: scroll <n> (-32767..32767)"); return;
    }
    mouseScroll((int)n);
    ok("scroll " + String(n));
    return;
  }

  err("unknown command: " + cmd + " (type help)");
}

// ── Non-blocking services, called every loop() iteration ─────────────
void serviceScheduler() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_PENDING; i++) {
    if (pending[i].kind == PEND_NONE) continue;
    if ((int32_t)(now - pending[i].dueAt) >= 0) {   // signed diff: rollover-safe
      if (pending[i].kind == PEND_KEY) {
        Keyboard.release(pending[i].code);
        removeHeldKey(pending[i].code);
      } else {
        Mouse.release(pending[i].code);
        heldMouseMask &= ~pending[i].code;
      }
      pending[i].kind = PEND_NONE;
    }
  }
}
void serviceDeadman() {
  if (!armed) return;
  if (heldCount == 0 && heldMouseMask == 0) return;
  // A pending timed release means the host explicitly asked for this hold
  // duration — don't treat that as "idle". Let the scheduler run it to
  // completion. Only indefinite holds (no pending timer) get deadman'd.
  if (anyPending()) return;
  if ((int32_t)(millis() - lastCmdAt) > (int32_t)DEADMAN_MS) {
    releaseEverything();
    err("deadman: idle > 5s, released all");
  }
}

void setup() {
  // names the device is shown under in the OS
  USB.productName("USBHelper HID");
  USB.manufacturerName("USBHelper");

  USBSerial.begin();     // bring up the virtual COM port (CDC)
  Keyboard.begin();      // + HID keyboard
  Mouse.begin();         // + HID relative mouse
  AbsMouseDev.begin();   // + HID absolute mouse (same interface, own report id)
  USB.begin();           // start the whole composite USB device — once, last

  releaseEverything();   // clear any state left logically pressed after a reset
  lastCmdAt = millis();

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

  serviceScheduler();    // fire any due auto-releases (non-blocking)
  serviceDeadman();      // release everything if armed and idle too long
}

#endif  // ARDUINO_USB_MODE