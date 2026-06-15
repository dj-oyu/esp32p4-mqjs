import sys, time, serial

PORT = "COM8"
WINDOW = float(sys.argv[1]) if len(sys.argv) > 1 else 26.0

# Wait for the port to (re)appear, then open it. Opening COM8 on the Tab5 can
# itself fire a USB reset -> fresh boot, which is what we want to capture.
deadline = time.time() + 15
ser = None
while time.time() < deadline:
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.3)
        break
    except Exception:
        time.sleep(0.4)
if ser is None:
    print("could not open", PORT)
    sys.exit(2)

# Hold resets inactive (Tab5 misroutes DTR/RTS); just read the secondary console.
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

end = time.time() + WINDOW
buf = b""
while time.time() < end:
    data = ser.read(4096)
    if data:
        buf += data
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
ser.close()
