"""Read the board's serial log (115200 baud), optionally resetting it first.

Usage:
    python tools/serial_log.py [COMx|/dev/ttyUSBx] [seconds] [noreset]

The CH340 port is auto-detected. By default the board is reset via RTS (EN pin)
so you see the full boot log; pass "noreset" to just listen.
Requires: pip install pyserial
"""
import sys
import time

import serial
from serial.tools import list_ports

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x1A86:  # WCH CH340
            return p.device
    ports = [p.device for p in list_ports.comports() if p.device != "COM1"]
    return ports[0] if ports else None


def is_port(a):
    return a.upper().startswith("COM") or a.startswith("/dev/")


args = [a for a in sys.argv[1:] if a != "noreset"]
noreset = "noreset" in sys.argv
port = next((a for a in args if is_port(a)), None) or find_port()
secs = next((float(a) for a in args if not is_port(a)), 20)
if not port:
    sys.exit("Board not found")

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False
s.rts = False
s.open()
if not noreset:
    # reset pulse: EN low via RTS, GPIO0 stays high -> normal boot
    s.rts = True
    time.sleep(0.15)
    s.rts = False
print(f"--- {port}: {'listening' if noreset else 'reset'}, reading for {secs:.0f} s ---", flush=True)
end = time.time() + secs
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
