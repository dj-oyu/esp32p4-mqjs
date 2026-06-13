"""Timed, passive serial capture (no DTR/RTS toggling) for Tab5 boot logs.

Tab5's USB auto-reset is ineffective, so merely opening the port does not
reset the running app — safe to attach after a watchdog-reset boot. Usage:
    python tools/capture_com8.py COM8 20
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

def try_open(deadline):
    """Retry opening across the USB re-enumeration that a watchdog reset
    triggers on the Tab5 (its serial is the chip's own USB)."""
    while time.time() < deadline:
        try:
            h = serial.Serial()
            h.port = port
            h.baudrate = baud
            h.timeout = 0.2
            h.dtr = False  # hold reset/boot lines steady on attach
            h.rts = False
            h.open()
            return h
        except serial.SerialException:
            time.sleep(0.2)
    return None


s = try_open(time.time() + 8.0)
if s is None:
    sys.stderr.write("could not open %s\n" % port)
    sys.exit(1)

end = time.time() + dur
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
s.close()
