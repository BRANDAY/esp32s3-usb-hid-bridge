// USBHelper — первая прошивка для ESP32 mini
//
// Что она делает:
//   1. Мигает встроенным светодиодом (видно, что плата живая)
//   2. Слушает команды от компьютера через USB (Serial)
//   3. Отвечает на них — это и есть "общение с ESP32 программно"
//
// Как это работает: код компилируется в машинный и записывается во
// flash-память платы. После этого плата выполняет его сама — компьютер
// нужен только как питание и собеседник по Serial-порту.

#include <Arduino.h>

// У большинства mini-плат светодиод на GPIO2. Если PlatformIO знает
// плату точнее, он сам подставит правильный номер через LED_BUILTIN.
#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

String line;                  // накапливаем символы, пока не придёт Enter
bool autoBlink = true;        // мигать ли автоматически
bool ledState = false;
unsigned long lastToggle = 0; // когда последний раз переключали светодиод

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help     - this help");
  Serial.println("  info     - chip info");
  Serial.println("  led on   - LED on (stops blinking)");
  Serial.println("  led off  - LED off (stops blinking)");
  Serial.println("  blink    - resume auto-blink");
  Serial.println("  echo <t> - repeat text back");
}

void printInfo() {
  Serial.printf("Chip:     %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("Cores:    %d\n", ESP.getChipCores());
  Serial.printf("CPU:      %u MHz\n", (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("Flash:    %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
  Serial.printf("Free RAM: %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
  Serial.printf("Uptime:   %lu s\n", millis() / 1000);
}

// Разбираем команду, пришедшую с компьютера
void handleCommand(String cmd) {
  cmd.trim();                 // убираем пробелы и \r по краям
  if (cmd.length() == 0) return;

  if (cmd == "help") {
    printHelp();
  } else if (cmd == "info") {
    printInfo();
  } else if (cmd == "led on") {
    autoBlink = false;
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("OK: LED on");
  } else if (cmd == "led off") {
    autoBlink = false;
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("OK: LED off");
  } else if (cmd == "blink") {
    autoBlink = true;
    Serial.println("OK: auto-blink");
  } else if (cmd.startsWith("echo ")) {
    Serial.println(cmd.substring(5));
  } else {
    Serial.print("Unknown command: ");
    Serial.println(cmd);
    Serial.println("Type 'help'");
  }
}

// setup() выполняется один раз при включении/перезагрузке платы
void setup() {
  Serial.begin(115200);           // открываем связь с ПК на 115200 бит/с
  pinMode(LED_BUILTIN, OUTPUT);   // ножка светодиода будет выходом
  delay(500);                     // даём порту время подняться
  Serial.println();
  Serial.println("=== USBHelper: ESP32 is alive! ===");
  Serial.println("Type 'help' + Enter");
}

// loop() крутится бесконечно после setup()
void loop() {
  // 1. Автомигание. Заметь: без delay()! Сравниваем время через millis(),
  //    чтобы плата оставалась отзывчивой и успевала читать команды.
  if (autoBlink && millis() - lastToggle >= 500) {
    lastToggle = millis();
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  }

  // 2. Читаем всё, что компьютер прислал по Serial
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {              // Enter = команда закончена
      handleCommand(line);
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
}
