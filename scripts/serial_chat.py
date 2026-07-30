#!/usr/bin/env python3
"""Чат с ESP32 по USB (Serial) — часть проекта USBHelper.

Прошивка на плате слушает команды, а этот скрипт отправляет их
с компьютера и печатает ответы. Это "вторая половина" общения.

Запуск:
    python scripts/serial_chat.py          # найдёт плату сам
    python scripts/serial_chat.py COM5     # или укажи порт вручную

Выход: набери exit (или Ctrl+C).
"""
import sys
import threading

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Нет pyserial. Поставь:  pip install pyserial")
    sys.exit(1)

BAUD = 115200  # должно совпадать с Serial.begin(115200) в прошивке

# USB-чип платы опознаётся по VID — так находим нужный порт
KNOWN = {
    0x1A86: "CH340/CH9102 — классическая ESP32 mini",
    0x10C4: "CP210x — классическая ESP32 mini",
    0x303A: "Espressif USB — плата на ESP32-S2/S3/C3",
}


def pick_port() -> str:
    ports = list(list_ports.comports())
    if not ports:
        print("COM-портов не найдено. Проверь: плата воткнута? кабель с данными,")
        print("а не 'только зарядка'? установлен драйвер? (docs/esp32-guide.md, п.9)")
        sys.exit(1)

    print("Найденные порты:")
    candidates = []
    for p in ports:
        hint = KNOWN.get(p.vid or 0)
        mark = "  <-- похоже на ESP32" if hint else ""
        print(f"  {p.device}: {p.description}  {hint or ''}{mark}")
        if hint:
            candidates.append(p.device)

    if not candidates:
        print(f"ESP32 не опознан, пробую первый порт: {ports[0].device}")
        return ports[0].device
    if len(candidates) > 1:
        print(f"Несколько кандидатов, беру {candidates[0]}. Нужен другой — "
              f"укажи аргументом: python scripts/serial_chat.py COM7")
    return candidates[0]


def reader(port: serial.Serial) -> None:
    """Фоновый поток: печатает всё, что плата присылает."""
    while True:
        try:
            data = port.readline()
        except (serial.SerialException, OSError):
            print("\n[связь потеряна — плату отключили?]")
            break
        if data:
            print(data.decode("utf-8", errors="replace").rstrip())


def main() -> None:
    name = sys.argv[1] if len(sys.argv) > 1 else pick_port()
    print(f"Подключаюсь к {name} @ {BAUD}...")
    try:
        port = serial.Serial(name, BAUD, timeout=0.5)
    except serial.SerialException as e:
        print(f"Порт не открылся: {e}")
        print("Частая причина: порт занят монитором PlatformIO — закрой его.")
        sys.exit(1)

    # Открытие порта сбрасывает плату (так устроена линия DTR) —
    # первым делом увидишь её стартовое приветствие. Это нормально.
    threading.Thread(target=reader, args=(port,), daemon=True).start()
    print("Готово. Пиши команды (например: help), 'exit' — выход.")
    try:
        while True:
            cmd = input()
            if cmd.strip().lower() == "exit":
                break
            port.write((cmd + "\n").encode("utf-8"))
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        port.close()
        print("Пока!")


if __name__ == "__main__":
    main()
