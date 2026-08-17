#!/usr/bin/env python3
"""
Assert that the headless host and the browser page classify identically.

There are two implementations of one classifier, and that is a deliberate but
uncomfortable position. The thresholds define what "focused" has meant in every
row already in focus_samples, so a drift between them does not add new data --
it silently changes the meaning of the old data, and nothing about the symptom
would point at a constant in a header file.

Run this after touching either file. It is not a unit test of the maths; it is
a guard on the numbers.

    python check_parity.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "firmware", "cadence_cam", "vision_page.h")
HOST = os.path.join(HERE, "cadence_vision.py")

page = open(PAGE, encoding="utf-8").read()
host = open(HOST, encoding="utf-8").read()

fail = 0


def check(name, ok, detail=""):
    global fail
    print(("  ok    " if ok else "FAIL    ") + name + (f"  {detail}" if detail else ""))
    if not ok:
        fail += 1


def one(pattern, text, what):
    m = re.search(pattern, text)
    if not m:
        check(f"could not find {what}", False)
        return None
    return m.group(1)


print("thresholds")
for label, pr, hr in [
    ("yaw",   r'get\("yaw"\)\s*\?\?\s*"([\d.]+)"',   r'YAW_LIMIT\s*=\s*([\d.]+)'),
    ("pitch", r'get\("pitch"\)\s*\?\?\s*"([\d.]+)"', r'PITCH_LIMIT\s*=\s*([\d.]+)'),
    ("gaze",  r'get\("gaze"\)\s*\?\?\s*"([\d.]+)"',  r'GAZE_LIMIT\s*=\s*([\d.]+)'),
    ("hz",    r'get\("hz"\)\s*\?\?\s*"([\d.]+)"',    r'HZ\s*=\s*([\d.]+)'),
]:
    a, b = one(pr, page, f"page {label}"), one(hr, host, f"host {label}")
    if a and b:
        check(label, float(a) == float(b), f"page {a} / host {b}")

print("timing")
a, b = one(r'get\("window"\)\s*\?\?\s*"(\d+)"', page, "page window"), one(r'WINDOW_S\s*=\s*([\d.]+)', host, "host window")
if a and b:
    check("window", int(a) / 1000 == float(b), f"page {a}ms / host {b}s")
a, b = one(r'get\("post"\)\s*\?\?\s*"(\d+)"', page, "page post"), one(r'POST_S\s*=\s*([\d.]+)', host, "host post")
if a and b:
    check("post interval", int(a) / 1000 == float(b), f"page {a}ms / host {b}s")

print("landmark indices")
m = re.search(r'const L = lm\[(\d+)\], R = lm\[(\d+)\], nose = lm\[(\d+)\], '
              r'brow = lm\[(\d+)\], chin = lm\[(\d+)\]', page)
if not m:
    check("page landmark line", False)
else:
    names = ["L_EYE", "R_EYE", "NOSE", "BROW", "CHIN"]
    hm = re.search(r'L_EYE, R_EYE, NOSE, BROW, CHIN = ([\d,\s]+)', host)
    if not hm:
        check("host landmark line", False)
    else:
        hv = [v.strip() for v in hm.group(1).split(",")]
        for i, n in enumerate(names):
            check(n, m.group(i + 1) == hv[i], f"page {m.group(i+1)} / host {hv[i]}")

print("blendshapes")
page_shapes = set(re.findall(r'g\("(eyeLook[A-Za-z]+)"\)', page))
host_shapes = set(re.findall(r'"(eyeLook[A-Za-z]+)"', host))
check("gaze blendshape set", page_shapes == host_shapes,
      f"page {len(page_shapes)} / host {len(host_shapes)}"
      + ("" if page_shapes == host_shapes else f"  diff {page_shapes ^ host_shapes}"))
for s in ("eyeBlinkLeft", "eyeBlinkRight"):
    check(s, s in page and s in host)

print("recording ownership")
# Exactly one of them may record by default, or attention doubles whenever the
# page is open: both would post the same seconds milliseconds apart, and the
# (device_id, ts) unique index cannot collapse that.
check("page does not sample by default",
      "preview only" in page and "setSampling(false" in page)
check("host samples when the session runs",
      'session_state(host) == "running"' in host)

print()
if fail:
    print(f"{fail} PARITY FAILURE(S) — the two classifiers disagree")
    sys.exit(1)
print("the two classifiers agree")
