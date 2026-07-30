// USBHelper — составное USB-устройство на ESP32-S3
//
// Одним кабелем плата отдаёт компьютеру СРАЗУ ТРИ устройства (composite USB):
//   1) CDC — виртуальный COM-порт (VCP): по нему принимаем текстовые команды
//   2) HID-клавиатуру — плата "нажимает" клавиши на хосте
//   3) HID-мышь — плата двигает курсор, кликает, крутит колесо
//
// Ты пишешь в COM-порт строку команды, нажимаешь Enter — плата её исполняет.
// Компьютер видит настоящую клавиатуру и мышь, драйверы не нужны.
//
// ПОЧЕМУ ЯВНЫЙ USBCDC, а не Serial:
// автоматический режим ARDUINO_USB_CDC_ON_BOOT в связке с HID у нас не поднял
// CDC-интерфейс (в дескрипторе оставались только клавиатура+мышь). Поэтому
// создаём объект USBSerial вручную — его конструктор гарантированно
// регистрирует CDC. Это шаблон из официального примера CompositeDevice.
//
// Собирать в окружении s3hid (ARDUINO_USB_MODE=0, CDC_ON_BOOT=0).

#if ARDUINO_USB_MODE
#warning "hid_composite: нужен режим USB-OTG. Собирай окружение s3hid."
void setup() {}
void loop() {}
#else

#include <Arduino.h>
#include "USB.h"
#include "USBCDC.h"
#include "USBHID.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

USBCDC USBSerial;         // наш виртуальный COM-порт (CDC)
USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
USBHID HID;              // отдельный дескриптор — только чтобы спросить HID.ready()

String line;              // копим символы команды до Enter
bool armed = true;        // предохранитель: false => HID-команды игнорируются

// ── Индикация: коротко мигнуть встроенным RGB-светодиодом ────────────
void blip(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, r, g, b);
  delay(30);
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
}

// ── Ответы в COM-порт, чтобы в терминале было видно результат ────────
void ok(const String& msg)  { USBSerial.println("OK: "  + msg); blip(0, 12, 0); }
void err(const String& msg) { USBSerial.println("ERR: " + msg); blip(16, 0, 0); }

void printHelp() {
  USBSerial.println(F("=== USBHelper HID — команды (строка + Enter) ==="));
  USBSerial.println(F("КЛАВИАТУРА:"));
  USBSerial.println(F("  type <text>       напечатать текст (US-раскладка, ASCII)"));
  USBSerial.println(F("  key <name>        нажать спец-клавишу: enter esc tab space"));
  USBSerial.println(F("                    backspace delete up down left right home"));
  USBSerial.println(F("                    end pgup pgdn ins f1..f12"));
  USBSerial.println(F("  combo a+b+..      комбинация: combo ctrl+c | ctrl+alt+del"));
  USBSerial.println(F("                    win+r | alt+tab | ctrl+shift+esc"));
  USBSerial.println(F("  hold <key>        зажать и держать (модификатор или клавишу)"));
  USBSerial.println(F("  unhold            отпустить всё зажатое"));
  USBSerial.println(F("МЫШЬ:"));
  USBSerial.println(F("  move <dx> <dy>    сдвинуть курсор (пиксели, можно минус)"));
  USBSerial.println(F("  click [btn]       клик: left(по умолч.) right middle"));
  USBSerial.println(F("  dblclick [btn]    двойной клик"));
  USBSerial.println(F("  down <btn> / up <btn>   зажать/отпустить кнопку мыши"));
  USBSerial.println(F("  scroll <n>        колесо: + вверх, - вниз"));
  USBSerial.println(F("СЕРВИС:"));
  USBSerial.println(F("  ping   info   help   arm   disarm"));
  USBSerial.println(F("Предохранитель: disarm блокирует ввод, arm разрешает."));
}

void printInfo() {
  USBSerial.printf("Chip:      %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  USBSerial.printf("USB:       composite CDC + HID keyboard + HID mouse\n");
  USBSerial.printf("HID ready: %s\n", HID.ready() ? "yes" : "no (хост не опросил?)");
  USBSerial.printf("Armed:     %s\n", armed ? "yes" : "no");
  USBSerial.printf("Uptime:    %lu s\n", millis() / 1000);
}

// ── Разбор имён клавиш ───────────────────────────────────────────────
// Возвращает HID-код по имени; good=false, если имя не распознано.
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
  if (n.length() == 1) return (uint8_t)n[0];        // одиночный символ
  good = false;
  return 0;
}

// Модификатор по имени; 0 если это не модификатор.
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
  return MOUSE_LEFT;  // по умолчанию и для "left", и для пустого
}

// Мышь двигается пакетами по int8 (-127..127) — дробим большой сдвиг.
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

// ── Исполнение одной команды ─────────────────────────────────────────
void handleCommand(const String& raw) {
  String input = raw;
  input.trim();
  if (input.length() == 0) return;

  // делим на первое слово (команда) и остаток (аргумент)
  int sp = input.indexOf(' ');
  String cmd = (sp < 0) ? input : input.substring(0, sp);
  String arg = (sp < 0) ? ""    : input.substring(sp + 1);
  cmd.toLowerCase();
  arg.trim();

  // --- сервисные команды работают всегда ---
  if (cmd == "help")   { printHelp(); return; }
  if (cmd == "info")   { printInfo(); return; }
  if (cmd == "ping")   { USBSerial.println("pong"); return; }
  if (cmd == "arm")    { armed = true;  ok("armed — ввод разрешён");  return; }
  if (cmd == "disarm") { armed = false; Keyboard.releaseAll(); ok("disarmed — ввод заблокирован"); return; }

  // --- всё, что реально управляет хостом, требует armed ---
  if (!armed) { err("disarmed: сначала команда arm"); return; }

  if (cmd == "type") {
    if (arg.length() == 0) { err("нужен текст: type Привет"); return; }
    Keyboard.print(arg);
    ok("typed " + String(arg.length()) + " chars");
    return;
  }

  if (cmd == "key") {
    bool good;
    uint8_t k = keyByName(arg, good);
    if (!good) { err("неизвестная клавиша: " + arg); return; }
    Keyboard.press(k);
    delay(8);
    Keyboard.releaseAll();
    ok("key " + arg);
    return;
  }

  if (cmd == "combo") {
    if (arg.length() == 0) { err("пример: combo ctrl+alt+del"); return; }
    int start = 0;
    bool failed = false;
    // разбираем токены, разделённые '+'
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
    if (failed) err("неизвестный токен в combo: " + arg);
    else        ok("combo " + arg);
    return;
  }

  if (cmd == "hold") {
    uint8_t m = modByName(arg);
    if (m) { Keyboard.press(m); ok("hold " + arg); return; }
    bool good;
    uint8_t k = keyByName(arg, good);
    if (!good) { err("неизвестная клавиша: " + arg); return; }
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
    if (s2 < 0) { err("нужно два числа: move 100 -40"); return; }
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

  err("неизвестная команда: " + cmd + " (введи help)");
}

void setup() {
  // имена, под которыми устройство будет видно в системе
  USB.productName("USBHelper HID");
  USB.manufacturerName("USBHelper");

  USBSerial.begin();     // поднимаем виртуальный COM-порт (CDC)
  Keyboard.begin();      // + HID-клавиатура
  Mouse.begin();         // + HID-мышь
  USB.begin();           // стартуем составное USB-устройство целиком

  delay(600);            // даём порту подняться
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif
  USBSerial.println();
  USBSerial.println("=== USBHelper HID готов ===");
  USBSerial.println("Введи 'help' для списка команд.");
}

void loop() {
  // читаем команды из COM-порта построчно
  while (USBSerial.available() > 0) {
    char c = (char)USBSerial.read();
    if (c == '\n') {
      handleCommand(line);
      line = "";
    } else if (c != '\r') {
      line += c;
      if (line.length() > 240) line = "";  // защита от мусора
    }
  }
}

#endif  // ARDUINO_USB_MODE
