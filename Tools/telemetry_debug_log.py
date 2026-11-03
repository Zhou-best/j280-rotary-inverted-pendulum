# -*- coding: utf-8 -*-


import sys
import time
import struct
import serial

PORT = "COM19"
BAUD = 115200
DURATION = 300.0

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
s = serial.Serial(PORT, BAUD, timeout=0.2)
buf = b""
t0 = time.time()
n = 0
last_b3 = 0
last_b5 = 0
last_fsm = None
alpha_min = None
alpha_max = None
events = 0

print("DEBUG LOG START %s" % PORT, flush=True)
try:
    while time.time() - t0 < DURATION:
        buf += s.read(512)
        while len(buf) >= 22:
            i = buf.find(b"\xaa\x55")
            if i < 0:
                buf = buf[-1:]
                break
            if i > 0:
                buf = buf[i:]
            if len(buf) < 22:
                break
            f = buf[:22]
            chk = 0
            for b in f[2:21]:
                chk ^= b
            if chk != f[21]:
                buf = buf[1:]
                continue
            buf = buf[22:]
            n += 1
            st = f[20]
            fsm = st & 3
            b3 = (st >> 3) & 1
            b5 = (st >> 5) & 1
            alpha = struct.unpack("<i", f[10:14])[0] / 65536.0 * 57.2958
            if alpha_min is None or alpha < alpha_min:
                alpha_min = alpha
            if alpha_max is None or alpha > alpha_max:
                alpha_max = alpha
            if fsm != last_fsm:
                print("t=%.2f FSM %s -> %d status=0x%02X alpha=%.1f" % (
                    time.time() - t0, last_fsm, fsm, st, alpha), flush=True)
                last_fsm = fsm
            if b3 and not last_b3:
                events += 1
                print("t=%.2f BIT3_RISE fsm=%d status=0x%02X alpha=%.1f" % (
                    time.time() - t0, fsm, st, alpha), flush=True)
            if b5 and not last_b5:
                print("t=%.2f B5_RISE fsm=%d status=0x%02X alpha=%.1f" % (
                    time.time() - t0, fsm, st, alpha), flush=True)
            elif not b5 and last_b5:
                print("t=%.2f B5_FALL fsm=%d status=0x%02X alpha=%.1f" % (
                    time.time() - t0, fsm, st, alpha), flush=True)
            last_b3 = b3
            last_b5 = b5
finally:
    s.close()
    print("DEBUG LOG END frames=%d events=%d alpha=[%.2f, %.2f]" % (
        n, events, alpha_min if alpha_min is not None else 0,
        alpha_max if alpha_max is not None else 0), flush=True)
