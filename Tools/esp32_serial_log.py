# -*- coding: utf-8 -*-


import sys
import time
import serial


sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = "COM16"
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=0.2)
buf = b""
print("LOGGING %s @ %d" % (PORT, BAUD), flush=True)

try:
    while True:
        data = s.read(256)
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").rstrip("\r")
            if text:
                print(time.strftime("%H:%M:%S ") + text, flush=True)
except KeyboardInterrupt:
    pass
finally:
    s.close()
