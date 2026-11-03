import argparse
import csv
import math
import os
import struct
import sys
import time

import serial

Q16 = 65536.0
RAD2DEG = 180.0 / math.pi

FSM_NAME = {0: "HANGING", 1: "SWINGUP", 2: "BALANCE", 3: "PROTECT"}


def frame_xor(body: bytes) -> int:
    ck = 0
    for b in body:
        ck ^= b
    return ck


def decode(body: bytes):

    th, dth, al, dal, duty = struct.unpack("<iiiih", body[:18])
    status = body[18]
    return th / Q16 * RAD2DEG, dth / Q16, al / Q16 * RAD2DEG, dal / Q16, duty, status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default=None, help="CSV 输出路径; 省略则只打印不存盘")
    ap.add_argument("--duration", type=float, default=0.0, help="秒; 0 = 一直录到 Ctrl+C")
    ap.add_argument("--sniff", type=float, default=0.0, help="仅嗅探 N 秒后退出(不写盘)")
    ap.add_argument("--stopfile", default=None, help="该文件一出现就正常收尾退出")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1.0)
    except Exception as e:
        print(f"[ERR] 打不开 {args.port}: {e}")
        print("      检查: 板子是否上电 / USB 线是否插在串口那一路 / 端口号对不对")
        return 1

    ser.reset_input_buffer()
    print(f"[OK] {args.port} @ {args.baud} 打开")

    fh = None
    wr = None
    if args.out and not args.sniff:
        fh = open(args.out, "w", newline="", encoding="utf-8")
        wr = csv.writer(fh)
        wr.writerow(["t_s", "theta_deg", "dtheta_rad_s", "alpha_deg", "dalpha_rad_s",
                     "duty", "status", "fsm", "lqr_en", "swing_en", "motor_active",
                     "edef", "calib_done", "traj_moving"])

    buf = bytearray()
    n_ok = 0
    n_bad = 0
    t0 = time.time()
    t_start = t0
    last_report = t0
    last_vals = None
    stop_at = None
    if args.sniff:
        stop_at = t0 + args.sniff
    elif args.duration > 0:
        stop_at = t0 + args.duration

    try:
        while True:
            chunk = ser.read(256)
            if chunk:
                buf.extend(chunk)


            while True:
                i = buf.find(b"\xAA\x55")
                if i < 0:
                    if len(buf) > 4096:
                        del buf[: len(buf) - 2]
                    break
                if i > 0:
                    del buf[:i]
                if len(buf) < 22:
                    break
                frame = bytes(buf[:22])
                if frame_xor(frame[2:21]) == frame[21]:
                    del buf[:22]
                    n_ok += 1
                    th, dth, al, dal, duty, status = decode(frame[2:21])
                    t = time.time() - t_start
                    if wr:
                        wr.writerow([f"{t:.4f}", f"{th:.4f}", f"{dth:.6f}",
                                     f"{al:.4f}", f"{dal:.6f}", duty, status,
                                     status & 3, (status >> 2) & 1, (status >> 3) & 1,
                                     (status >> 4) & 1, (status >> 5) & 1,
                                     (status >> 6) & 1, (status >> 7) & 1])
                    last_vals = (t, th, al, duty, status)
                else:

                    del buf[:2]
                    n_bad += 1

            now = time.time()
            if now - last_report >= 1.0:
                last_report = now
                if last_vals:
                    t, th, al, duty, status = last_vals
                    fsm = FSM_NAME.get(status & 3, "?")
                    print(f"\r t={t:7.2f}s  θ={th:+7.2f}°  α={al:+7.1f}°  "
                          f"duty={duty:+5d}  {fsm:<8s} "
                          f"traj={'1' if status & 0x80 else '0'}  "
                          f"frame={n_ok}/bad={n_bad}", end="", flush=True)
                else:
                    print(f"\r 等待数据... frame={n_ok}/bad={n_bad}", end="", flush=True)

            if stop_at and now >= stop_at:
                break
            if args.stopfile and os.path.exists(args.stopfile):
                print("\n[STOP] 检测到停止文件, 正常收尾")
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        if fh:
            fh.close()

    dt = time.time() - t_start
    print()
    print(f"[DONE] {n_ok} 帧 / {dt:.1f}s = {n_ok / dt:.1f} Hz, 坏帧 {n_bad}")
    if args.out and not args.sniff:
        print(f"[DONE] 已写入 {args.out}")
    if n_ok == 0:
        print("[WARN] 一帧都没收到 — 检查波特率/端口/板子是否在跑")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
