#!/usr/bin/env python3
"""motion.py <recording.mp4> [--rows a:b] [--list]: how evenly the picture moves on the screen while the
camera turns at a constant rate (continue_run.sh RECORD=<secs>; needs numpy and opencv-python).

screenrecord stores a frame when the screen changes, with its time, so every stored frame is one
game frame as the display showed it. The horizontal shift between consecutive frames (phase
correlation over a band of rows, the upper part by default: the background, not the hero or the
HUD) against the time between them gives the motion: at a constant turn the shift per millisecond
is constant, and a frame whose shift is off what its time on the screen asks for is a jump
(more: the picture skipped forward; less: it held). The jump is printed in refreshes' worth of
motion (the median shift per 16.7 ms); 0.5 and more is visible."""
import sys

import cv2
import numpy as np

REFRESH = 1000.0 / 60


def main():
    path = sys.argv[1]
    band = (0.12, 0.45)
    for i, a in enumerate(sys.argv):
        if a == "--rows":
            lo, hi = sys.argv[i + 1].split(":")
            band = (float(lo), float(hi))
    listing = "--list" in sys.argv
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        sys.exit(f"cannot open {path}")
    times, shifts, responses = [], [], []
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
            (dx, _dy), response = cv2.phaseCorrelate(prev, small, window)
            times.append(t)
            shifts.append(dx * 2)  # full-resolution pixels
            responses.append(response)
        else:
            times.append(t)
            shifts.append(0.0)
            responses.append(1.0)
        prev = small
    cap.release()
    if len(times) < 10:
        sys.exit("too few frames")
    t = np.array(times)
    dt = np.diff(t)
    dx = np.array(shifts[1:])
    resp = np.array(responses[1:])
    good = (resp > 0.05) & (dt > 1)
    speed = np.median(dx[good] / dt[good])  # px per ms
    per_refresh = speed * REFRESH
    refreshes = np.round(dt / REFRESH).astype(int)
    print(f"{len(t)} frames over {(t[-1] - t[0]) / 1000:.1f} s ({len(t) / ((t[-1] - t[0]) / 1000):.1f} a second); "
          f"turn {speed * 1000:.0f} px/s, {per_refresh:.1f} px a refresh")
    hist = {k: int((refreshes == k).sum()) for k in sorted(set(refreshes.tolist()))}
    print("frames by refreshes on the screen (from the time between stored frames):",
          ", ".join(f"{k}: {v}" for k, v in hist.items()))
    if abs(per_refresh) < 2:
        print("the turn is too slow to measure jumps")
        return
    expected = speed * dt
    jump = (dx - expected) / per_refresh
    jump[~good] = 0
    secs = (t[-1] - t[0]) / 1000
    for limit in (0.5, 1.0):
        fwd = int((jump >= limit).sum())
        back = int((jump <= -limit).sum())
        print(f"jumps of {limit} refresh's worth or more: {fwd} forward, {back} back ({(fwd + back) / secs * 60:.0f} a "
              f"minute)")
    # the motion per frame on the screen against its time: the content's step per refresh shown
    step = dx / np.maximum(refreshes, 1) / per_refresh
    print(f"content steps per refresh shown: median {np.median(step[good]):.2f}, p5 {np.percentile(step[good], 5):.2f}, "
          f"p95 {np.percentile(step[good], 95):.2f}; low-confidence frames {int((~good).sum())}")
    if listing:
        for i in range(len(dt)):
            mark = " <-- jump" if abs(jump[i]) >= 0.5 else ""
            print(f"  {t[i + 1] / 1000:8.3f} s  dt {dt[i]:5.1f} ms ({refreshes[i]})  dx {dx[i]:7.1f}  "
                  f"jump {jump[i]:+5.2f}  r {resp[i]:.2f}{mark}")


main()
