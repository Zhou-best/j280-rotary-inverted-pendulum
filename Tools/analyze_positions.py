# -*- coding: utf-8 -*-


import csv
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TARGETS = {0: 0.0, 2: -45.0, 3: 90.0, 1: 45.0, 4: -90.0}
GESTURE_MODE = {"stop": 0, "left": 2, "right": 3}


def load_csv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "t": float(r["t_s"]),
                    "alpha": float(r["alpha_deg"]),
                    "traj": int(r["traj_moving"]),
                    "fsm": int(r["fsm"]),
                    "motor": int(r["motor_active"]),
                })
            except (KeyError, ValueError):
                continue
    return rows


def load_commands(path):

    cmds = []
    pat = re.compile(r"^(\d\d):(\d\d):(\d\d).*gesture (\w+) -> mode (\d)")
    try:
        f = open(path, encoding="utf-8", errors="replace")
    except OSError:
        return cmds
    with f:
        for line in f:
            m = pat.search(line)
            if m:
                h, mi, s = int(m.group(1)), int(m.group(2)), int(m.group(3))
                mode = int(m.group(5))
                cmds.append((h * 3600 + mi * 60 + s, mode))
    return cmds


def segments(rows):

    segs = []
    start = None
    for i, r in enumerate(rows):
        if r["traj"] == 1 and start is None:
            start = i
        elif r["traj"] == 0 and start is not None:
            segs.append((start, i - 1))
            start = None
    if start is not None:
        segs.append((start, len(rows) - 1))
    return segs


def settle_alpha(rows, end_idx):

    t_end = rows[end_idx]["t"]
    vals = [r["alpha"] for r in rows[max(0, end_idx - 200):end_idx + 1]
            if r["t"] >= t_end - 0.5]
    return sum(vals) / len(vals) if vals else rows[end_idx]["alpha"]


def settle_after(rows, end_idx, span=2.0):

    t_end = rows[end_idx]["t"]
    vals = []
    for r in rows[end_idx + 1:]:
        if r["t"] > t_end + span:
            break
        if r["traj"] == 1:
            break
        vals.append(r["alpha"])
    return sum(vals) / len(vals) if vals else None


def nearest_target(a):
    return min(TARGETS, key=lambda m: abs(TARGETS[m] - a))


def main():
    if len(sys.argv) < 2:
        print("usage: analyze_positions.py <csv> [esp32_log]")
        return 1
    rows = load_csv(sys.argv[1])
    if not rows:
        print("no rows")
        return 1
    segs = segments(rows)
    cmds = load_commands(sys.argv[2]) if len(sys.argv) > 2 else []

    print("csv rows=%d  duration=%.1fs  segments=%d  commands=%d" % (
        len(rows), rows[-1]["t"], len(segs), len(cmds)))
    print()


    cmd_idx = 0
    for n, (s, e) in enumerate(segs, 1):
        a_end = settle_alpha(rows, e)
        a_set = settle_after(rows, e)
        a_pos = a_set if a_set is not None else a_end
        mode = nearest_target(a_pos)
        matched = None

        best = None
        for ct, cm in cmds:

            if cm in (0, 1, 2, 3, 4):
                if best is None:
                    best = (ct, cm)
                    break

        while cmd_idx < len(cmds):
            ct, cm = cmds[cmd_idx]
            cmd_idx += 1
            if cm == mode or abs(TARGETS.get(cm, 0) - a_end) < 30:
                matched = cm
                break
        target = TARGETS.get(matched if matched is not None else mode, 0.0)

        seg = rows[s:e + 1]
        alphas = [r["alpha"] for r in seg]
        dur = rows[e]["t"] - rows[s]["t"]
        print("段%2d  t=%7.1f→%7.1f (%5.1fs)  fsm=%d motor=%d" % (
            n, rows[s]["t"], rows[e]["t"], dur, rows[e]["fsm"], rows[e]["motor"]))
        print("      目标=%+6.1f°  参考停=%+7.2f°  停稳=%s  误差=%+6.2f°  段内[%+7.1f, %+7.1f]" % (
            target, a_end,
            ("%+7.2f°" % a_set) if a_set is not None else "  n/a  ",
            a_pos - target, min(alphas), max(alphas)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
