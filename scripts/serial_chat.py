#!/usr/bin/env python3
"""Console for talking to the esp32s3-usb-hid-bridge over the USB serial port.

The firmware on the board listens for commands; this script sends them from
the computer and prints the replies. It is the host side of the conversation.

Run:
    python scripts/serial_chat.py          # auto-detect the board
    python scripts/serial_chat.py COM5     # or specify the port manually

Exit: type 'exit' (or press Ctrl+C).
"""
import sys
import threading

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial is missing. Install it:  pip install pyserial")
    sys.exit(1)

BAUD = 115200  # must match Serial/USBSerial in the firmware

# The board's USB chip is recognized by its VID, used to find the right port.
KNOWN = {
    0x1A86: "CH340/CH9102 — classic ESP32 mini",
    0x10C4: "CP210x — classic ESP32 mini",
    0x303A: "Espressif USB — ESP32-S2/S3/C3 board",
}


def pick_port() -> str:
    ports = list(list_ports.comports())
    if not ports:
        print("No COM ports found. Check: board plugged in? a data cable,")
        print("not a 'charge only' one? driver installed?")
        sys.exit(1)

    print("Detected ports:")
    candidates = []
    for p in ports:
        hint = KNOWN.get(p.vid or 0)
        mark = "  <-- looks like ESP32" if hint else ""
        print(f"  {p.device}: {p.description}  {hint or ''}{mark}")
        if hint:
            candidates.append(p.device)

    if not candidates:
        print(f"ESP32 not recognized, trying the first port: {ports[0].device}")
        return ports[0].device
    if len(candidates) > 1:
        print(f"Several candidates, using {candidates[0]}. Need another one? "
              f"pass it as an argument: python scripts/serial_chat.py COM7")
    return candidates[0]


def reader(port: serial.Serial) -> None:
    """Background thread: prints everything the board sends."""
    while True:
        try:
            data = port.readline()
        except (serial.SerialException, OSError):
            print("\n[connection lost — was the board unplugged?]")
            break
        if data:
            print(data.decode("utf-8", errors="replace").rstrip())


def main() -> None:
    name = sys.argv[1] if len(sys.argv) > 1 else pick_port()
    print(f"Connecting to {name} @ {BAUD}...")
    try:
        port = serial.Serial(name, BAUD, timeout=0.5)
    except serial.SerialException as e:
        print(f"Could not open the port: {e}")
        print("Common cause: the port is busy with the PlatformIO monitor — close it.")
        sys.exit(1)

    # Opening the port resets the board (via the DTR line), so the first thing
    # you see is its startup banner. That is expected.
    threading.Thread(target=reader, args=(port,), daemon=True).start()
    print("Ready. Type commands (e.g. help); 'exit' to quit.")
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
        print("Bye!")


if __name__ == "__main__":
    main()
