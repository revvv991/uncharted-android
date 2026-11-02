#!/usr/bin/env python3
"""motion_vs_trace.py <recording.mp4> <run log>: the screen recording's frames matched to the run's
`Frame trace` lines (BB_PRESENT_TRACE=1) by their refresh pattern on the screen, then each frame's
content step (the picture's shift, motion.py's measure) against the game's timing of that frame:
the interval between the guest's flip calls, between the requests taken, between the vsyncs.
The correlation says which clock the game's motion follows (needs numpy and opencv-python)."""
import re
import sys

import cv2
import numpy as np

REFRESH = 1000.0 / 60
PAT = re.compile(r"Frame trace: id (\d+), arg (-?\d+), guest ([-\d.]+) waited ([-\d.]+), queued ([-\d.]+), "
                 r"taken ([-\d.]+) due ([-\d.]+), presented ([-\d.]+), desired ([-\d.]+), shown ([-\d.]+), "
                 r"refreshes (-?\d+), gap (\d+)")


def shifts(path, band=(0.12, 0.45)):
    cap = cv2.VideoCapture(path)
    times, dxs, rs = [], [], []
    prev = window = None
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        t = cap.get(cv2.CAP_PROP_POS_MSEC)
        h, w = frame.shape[:2]
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        cut = gray[int(h * band[0]):int(h * band[1]), int(w * 0.1):int(w * 0.9)]
        small = cv2.resize(cut, (cut.shape[1] // 2, cut.shape[0] // 2), interpolation=cv2.INTER_AREA).astype(np.float32)
        if window is None:
            window = cv2.createHanningWindow((small.shape[1], small.shape[0]), cv2.CV_32F)
        if prev is not None:
            (dx, _), r = cv2.phaseCorrelate(prev, small, window)
            times.append(t)
            dxs.append(dx * 2)
            rs.append(r)
        prev = small
    return np.array(times), np.array(dxs), np.array(rs)


def main():
    t, dx, r = shifts(sys.argv[1])
    rows = []
    for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
        m = PAT.search(line)
        if m:
            rows.append((float(m.group(3)), float(m.group(6)), float(m.group(10)), int(m.group(11))))
    rows = np.array(rows)  # guest, taken, shown, refreshes
    rec_n = np.round(np.diff(np.concatenate([[t[0] - 33.3], t])) / REFRESH).astype(int)
    tr_n = rows[:, 3].astype(int)
    # the offset that matches the recording's refresh counts to the trace's best
    best = None
    for off in range(0, len(tr_n) - len(rec_n)):
        score = int(((tr_n[off:off + len(rec_n)] != 2) & (tr_n[off:off + len(rec_n)] == rec_n)).sum()) - int(
            ((tr_n[off:off + len(rec_n)] != rec_n) & ((rec_n != 2) | (tr_n[off:off + len(rec_n)] != 2))).sum())
        if best is None or score > best[0]:
            best = (score, off)
    off = best[1]
    seg = rows[off:off + len(rec_n)]
    agree = int((seg[:, 3].astype(int) == rec_n).sum())
    print(f"matched at trace frame {off}: refresh counts agree on {agree} of {len(rec_n)} frames "
          f"(non-2 frames: recording {int((rec_n != 2).sum())}, trace {int((seg[:, 3] != 2).sum())})")
    guest_dt = np.diff(seg[:, 0], prepend=np.nan)
    taken_dt = np.diff(seg[:, 1], prepend=np.nan)
    shown_dt = np.diff(seg[:, 2], prepend=np.nan)
    good = (r > 0.3) & np.isfinite(guest_dt)
    speed = np.median(dx[good] / shown_dt[good])
    step = dx / speed  # the content's step in ms of the turn
    print(f"turn {speed * 1000:.0f} px/s; content steps (ms of the turn): median {np.median(step[good]):.1f}, "
          f"p5 {np.percentile(step[good], 5):.1f}, p95 {np.percentile(step[good], 95):.1f}")
    for name, v in (("guest flip interval", guest_dt), ("taken interval", taken_dt), ("shown interval", shown_dt)):
        c = np.corrcoef(step[good], v[good])[0, 1]
        err = np.abs(step[good] - v[good])
        print(f"  vs {name:20s}: correlation {c:+.2f}, |step - interval| median {np.median(err):.1f} ms, "
              f"p90 {np.percentile(err, 90):.1f}")
    # the content step against the guest interval one and two frames earlier (a pipeline lag)
    for lag in (1, 2, 3):
        a, b = step[lag:], guest_dt[:-lag]
        ok = good[lag:] & np.isfinite(b)
        print(f"  vs the guest flip interval {lag} frame(s) earlier: correlation {np.corrcoef(a[ok], b[ok])[0, 1]:+.2f}")
    if "--list" in sys.argv:
        for i in range(min(len(step), 80)):
            print(f"  {t[i] / 1000:7.3f} s  step {step[i]:6.1f}  guest {guest_dt[i]:6.1f}  taken {taken_dt[i]:6.1f}  "
                  f"shown {shown_dt[i]:6.1f}  r {r[i]:.2f}")


main()
