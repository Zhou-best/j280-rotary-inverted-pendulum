# -*- coding: utf-8 -*-


import argparse
import csv
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False


def load(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((
                    float(r["t_s"]),
                    float(r["alpha_deg"]),
                    float(r["dalpha_rad_s"]),
                    float(r["theta_deg"]),
                    int(r["fsm"]),
                    int(r["motor_active"]),
                ))
            except (KeyError, ValueError):
                continue
    return rows


def fit_sine(t, y, f_lo=0.07, f_hi=0.13):

    best = None
    for f in np.linspace(f_lo, f_hi, 241):
        w = 2 * np.pi * f
        X = np.column_stack([np.sin(w * t), np.cos(w * t), np.ones_like(t)])
        coef, *_ = np.linalg.lstsq(X, y, rcond=None)
        r = y - X @ coef
        rms = float(np.sqrt(np.mean(r * r)))
        if best is None or rms < best[0]:
            best = (rms, f, coef)
    rms, f, (a, b, c) = best
    A = float(np.hypot(a, b))
    phi = float(np.arctan2(b, a))
    return f, A, phi, float(c), rms


def auto_window(rows, win=20.0):
    t = np.array([r[0] for r in rows])
    a = np.array([r[1] for r in rows])
    best = None
    i = 0
    for j in range(len(rows)):
        while t[j] - t[i] > win:
            i += 1
        if j - i < 200:
            continue
        pp = float(np.max(a[i:j + 1]) - np.min(a[i:j + 1]))
        if best is None or pp > best[0]:
            best = (pp, i, j)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--start", type=float, default=None)
    ap.add_argument("--end", type=float, default=None)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    rows = load(args.csv)
    if len(rows) < 100:
        print("数据太少")
        return 1

    if args.start is None or args.end is None:
        best = auto_window(rows)
        if best is None:
            print("没找到正弦段")
            return 1
        s, e = best[1], best[2]
        start, end = rows[s][0], rows[e][0]
    else:
        start, end = args.start, args.end
        t_all = [r[0] for r in rows]
        s = int(np.searchsorted(t_all, start))
        e = int(np.searchsorted(t_all, end)) - 1

    seg = rows[s:e + 1]
    t = np.array([r[0] for r in seg]) - seg[0][0]
    alpha = np.array([r[1] for r in seg])
    dalpha = np.array([r[2] for r in seg])
    theta = np.array([r[3] for r in seg])
    fsm = np.array([r[4] for r in seg])

    f, A, phi, c, rms = fit_sine(t, alpha)
    fit = A * np.sin(2 * np.pi * f * t + phi) + c

    w = 2 * np.pi * f
    X = np.column_stack([np.sin(w * t), np.cos(w * t), np.ones_like(t)])
    coef_v, *_ = np.linalg.lstsq(X, dalpha, rcond=None)
    Av = float(np.hypot(coef_v[0], coef_v[1]))

    print("段: t=%.1f..%.1f s (%.1f s)  fsm=2 占比 %.0f%%" % (
        start, end, end - start, 100.0 * float(np.mean(fsm == 2))))
    print("正弦拟合: 频率 %.4f Hz  幅值 %.2f°  偏置 %+.2f°  残差 RMS %.2f°" % (
        f, A, c, rms))
    print("alpha 范围: [%.1f, %.1f]°   指令幅值 34.4°   实测/指令 = %.2f" % (
        alpha.min(), alpha.max(), A / 34.4))
    print("dalpha 幅值: %.3f rad/s (%.1f °/s)" % (Av, Av * 57.2958))
    print("摆杆 theta: max|%.2f|°  RMS %.2f°" % (
        float(np.max(np.abs(theta))), float(np.sqrt(np.mean(theta * theta)))))
    print("结论: 轨迹 %s, 摆杆 %s" % (
        "成立" if A > 20 else "幅值偏小，请检查 mode 5",
        "保持直立" if np.max(np.abs(theta)) < 10 else "theta 偏大，需检查"))

    out = args.out or args.csv.rsplit(".", 1)[0] + "_sine.png"
    fig, ax = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
    ax[0].plot(t, alpha, label="alpha 实际")
    ax[0].plot(t, fit, "--", label="正弦拟合")
    ax[0].set_ylabel("alpha (°)")
    ax[0].legend()
    ax[0].grid(True)
    ax[0].set_title("mode 5 正弦轨迹: %.3f Hz, 幅值 %.1f°" % (f, A))
    ax[1].plot(t, dalpha, color="tab:green")
    ax[1].set_ylabel("dalpha (rad/s)")
    ax[1].grid(True)
    ax[1].set_title("转臂速度曲线")
    ax[2].plot(t, theta, color="tab:orange")
    ax[2].set_ylabel("theta (°)")
    ax[2].set_xlabel("t (s)")
    ax[2].grid(True)
    ax[2].set_title("摆杆角度（应始终在 0° 附近）")
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    print("图:", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
