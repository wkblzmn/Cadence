#!/usr/bin/env python3
"""
Cadence — headless vision host.

Replaces the browser tab. Polls the camera board for the session state, pulls
frames from its MJPEG stream while a session is running, classifies attention
with MediaPipe, and posts batches back to the board's /focus relay.

Why it exists: the camera board is an ESP32-S3 and cannot run the inference, so
the vision tier has always needed a host. That host used to be a page served at
/vision, which meant a browser tab had to be open before the focus-timer button
could record anything. Pressing a button on a desk device and having nothing
happen because a tab was closed is not a design, it is a chore.

Why it still posts through the board rather than straight to the backend: the
API token lives in firmware and the relay exists to keep it there. This host
never sees it.

    python cadence_vision.py --calibrate     # sit normally, look at the screen
    python cadence_vision.py                 # run it

The classifier below is a faithful port of firmware/cadence_cam/vision_page.h.
Every threshold, landmark index and blendshape name is the same, deliberately:
those numbers define what "focused" has meant in every row already sitting in
focus_samples, and a second implementation that drifts would silently change
the meaning of the history rather than adding to it. If you change one, change
both, and know that you have redefined the older data.
"""

import argparse
import json
import math
import os
import sys
import time
from collections import Counter
from datetime import datetime, timezone

import cv2
import numpy as np
import requests
import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision as mp_vision

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(HERE, "face_landmarker.task")
CALIB = os.path.join(HERE, "calibration.json")

# ── tunables, matching vision_page.h ─────────────────────────────────────────
YAW_LIMIT   = 0.20      # fraction of eye span
PITCH_LIMIT = 0.28
GAZE_LIMIT  = 0.35      # blendshape score
HZ          = 4.0       # inference rate
WINDOW_S    = 2.0       # one sample per window, by majority vote
POST_S      = 10.0      # batch upload interval
SESSION_S   = 2.0       # how often to ask the board what it should be doing

# Landmark indices, from the same source.
L_EYE, R_EYE, NOSE, BROW, CHIN = 33, 263, 1, 168, 152

GAZE_SHAPES = ("eyeLookOutLeft", "eyeLookOutRight",
               "eyeLookInLeft", "eyeLookInRight",
               "eyeLookDownLeft", "eyeLookDownRight")


def pose(lm):
    """Head pose from landmark geometry rather than the transformation matrix.

    The matrix's axis convention varies between versions; the ratio of the
    nose's offset to the eye span does not, and it is scale-invariant, so it
    does not care how far away you sit.
    """
    L, R, nose, brow, chin = lm[L_EYE], lm[R_EYE], lm[NOSE], lm[BROW], lm[CHIN]
    span = math.hypot(R.x - L.x, R.y - L.y) or 1e-6
    mid_x, mid_y = (L.x + R.x) / 2, (L.y + R.y) / 2
    return {
        "yaw":   (nose.x - mid_x) / span,
        "pitch": (nose.y - mid_y) / span - (chin.y - brow.y) / span * 0.25,
    }


def classify(result, centre):
    if not result or not result.face_landmarks:
        return {"state": "absent", "conf": 0.9, "yaw": None, "pitch": None,
                "gaze": None, "eyes": None}

    lm = result.face_landmarks[0]
    p = pose(lm)
    yaw, pitch = p["yaw"], p["pitch"]
    if centre:
        yaw -= centre["yaw"]
        pitch -= centre["pitch"]

    # Gaze from the ARKit-standard blendshapes the model already returns. Head
    # pose alone misses the case that matters most: glancing at a phone on the
    # desk moves the eyes far more than the head.
    closed = gaze = 0.0
    if result.face_blendshapes:
        scores = {c.category_name: c.score for c in result.face_blendshapes[0]}
        closed = max(scores.get("eyeBlinkLeft", 0.0), scores.get("eyeBlinkRight", 0.0))
        gaze = max(scores.get(n, 0.0) for n in GAZE_SHAPES)

    gaze_rel = max(0.0, gaze - centre["gaze"]) if centre else gaze

    head_off = abs(yaw) > YAW_LIMIT or abs(pitch) > PITCH_LIMIT
    eyes_off = gaze_rel > GAZE_LIMIT

    # Eyes shut is not distraction. A blink is 100-400 ms and the window vote
    # absorbs it; sustained closure is drowsiness, a different signal this
    # project does not claim to detect. So closure is reported, never scored.
    state = "distracted" if (head_off or eyes_off) else "focused"
    conf = min(1.0, max(0.5, 1.0 - max(abs(yaw) / YAW_LIMIT,
                                       abs(pitch) / PITCH_LIMIT,
                                       gaze_rel / GAZE_LIMIT) * 0.3))
    return {"state": state, "conf": conf, "yaw": yaw, "pitch": pitch,
            "gaze": gaze_rel, "eyes": closed, "raw_gaze": gaze, "raw": p}


def make_landmarker():
    if not os.path.exists(MODEL):
        sys.exit(f"missing model: {MODEL}\n"
                 "Download face_landmarker.task (float16/1) from Google's "
                 "mediapipe-models bucket and put it beside this script.")
    opts = mp_vision.FaceLandmarkerOptions(
        base_options=mp_python.BaseOptions(model_asset_path=MODEL),
        output_face_blendshapes=True,
        num_faces=1,
        running_mode=mp_vision.RunningMode.IMAGE,
    )
    return mp_vision.FaceLandmarker.create_from_options(opts)


def detect(landmarker, frame_bgr):
    rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
    image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
    return landmarker.detect(image)


def load_calibration():
    if not os.path.exists(CALIB):
        return None
    with open(CALIB) as f:
        c = json.load(f)
    return c if {"yaw", "pitch", "gaze"} <= c.keys() else None


def calibrate(host, landmarker, seconds=6.0):
    """Average the pose over a few seconds of sitting normally.

    The browser page took a single frame on a button press. Averaging is
    strictly better for an unattended host: one frame catches whatever
    micro-movement you happened to be making, and the baseline it produces is
    then wrong for the rest of the session.
    """
    cap = open_stream(host)
    samples = []
    deadline = time.time() + seconds
    print(f"calibrating for {seconds:.0f}s — look straight at your screen")
    while time.time() < deadline:
        ok, frame = cap.read()
        if not ok:
            continue
        r = detect(landmarker, frame)
        if r and r.face_landmarks:
            p = pose(r.face_landmarks[0])
            gaze = 0.0
            if r.face_blendshapes:
                scores = {c.category_name: c.score for c in r.face_blendshapes[0]}
                gaze = max(scores.get(n, 0.0) for n in GAZE_SHAPES)
            samples.append((p["yaw"], p["pitch"], gaze))
        time.sleep(1.0 / HZ)
    cap.release()

    if len(samples) < 5:
        sys.exit(f"only {len(samples)} usable frames — is the room lit and are "
                 "you in shot? Nothing written.")

    arr = np.array(samples)
    centre = {"yaw": float(arr[:, 0].mean()),
              "pitch": float(arr[:, 1].mean()),
              "gaze": float(arr[:, 2].mean()),
              "frames": len(samples),
              "at": datetime.now(timezone.utc).isoformat()}
    with open(CALIB, "w") as f:
        json.dump(centre, f, indent=2)
    print(f"wrote {CALIB} from {len(samples)} frames: "
          f"yaw {centre['yaw']:+.3f}  pitch {centre['pitch']:+.3f}  "
          f"gaze {centre['gaze']:.3f}")


LOG = os.path.join(HERE, "host.log")


def log(msg):
    """Print and append. This runs unattended from a login task, where a
    traceback on a stdout nobody is watching is the same as no diagnosis at
    all."""
    line = f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}  {msg}"
    print(line, flush=True)
    try:
        if os.path.exists(LOG) and os.path.getsize(LOG) > 1_000_000:
            os.replace(LOG, LOG + ".1")
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass                       # logging must never be the thing that fails


def open_stream(host, patient=True):
    """Open the MJPEG stream, retrying rather than exiting.

    Exiting was wrong for anything that starts at login: the PC boots faster
    than the camera board joins the network, so a host that gives up on the
    first attempt is a host that is never running when you sit down. It also
    has to survive the board rebooting mid-session, which it does.
    """
    url = f"http://{host}:81/stream"
    delay, attempt = 2, 0
    while True:
        attempt += 1
        cap = cv2.VideoCapture(url)
        if cap.isOpened():
            # Keep the buffer shallow: a backlog would hand the classifier
            # frames from several seconds ago, which for a 2s voting window is
            # enough to attribute attention to the wrong window entirely.
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            if attempt > 1:
                log(f"stream open after {attempt} attempts")
            return cap
        cap.release()
        if not patient:
            return None
        if attempt in (1, 5) or attempt % 30 == 0:
            log(f"cannot open {url} (attempt {attempt}) — waiting for the board")
        time.sleep(delay)
        delay = min(delay * 2, 30)     # back off, then keep trying forever


def session_state(host, timeout=3.0):
    try:
        r = requests.get(f"http://{host}/session", timeout=timeout)
        return r.json().get("state", "idle")
    except Exception:
        # One failed poll is not a stopped session — the board expires the
        # state by itself if the hub has really gone away, so hold what we have.
        return None


def post_batch(host, batch, timeout=10.0):
    try:
        r = requests.post(f"http://{host}/focus",
                          json={"samples": batch}, timeout=timeout)
        return r.status_code, r.text[:120]
    except Exception as e:
        return None, str(e)[:120]


def run(host, free_run=False, verbose=False):
    centre = load_calibration()
    if centre:
        log(f"calibration: yaw {centre['yaw']:+.3f}  pitch {centre['pitch']:+.3f}  "
            f"gaze {centre['gaze']:.3f}")
    else:
        log("NO CALIBRATION — run --calibrate. Until then everything reads distracted.")

    landmarker = make_landmarker()

    # Open the stream once, for the life of the process, and keep reading from
    # it even when no session is running.
    #
    # This is not an optimisation, it is what makes the hub's pill truthful. The
    # board reports `viewer` by asking whether anything is pulling the stream,
    # and the browser tab it replaced held that connection open the entire time
    # it was open. An earlier version of this host connected only while
    # recording, so between sessions the board saw no viewer and the hub said
    # "no viewer" — which reads as "your camera is broken" when the correct
    # answer is "a host is attached and waiting for the button".
    cap = open_stream(host)

    sampling = False
    votes = []
    pending = []
    sent = 0
    last_session = last_window = last_post = 0.0
    tick_interval = 1.0 / HZ

    log(f"watching {host} — recording follows the hub's focus-timer button"
        + (" (FREE RUN: ignoring it)" if free_run else ""))

    try:
        while True:
            now = time.time()

            # ── what should we be doing? ──────────────────────────────────
            if now - last_session >= SESSION_S:
                last_session = now
                want = True if free_run else (session_state(host) == "running")
                if want != sampling:
                    sampling = want
                    if sampling:
                        votes.clear()          # frames before the press belong to no session
                        log("session running — recording")
                    else:
                        # Close the open window and flush, rather than holding a
                        # partial batch that a crash would take with it.
                        if votes:
                            flush_window(votes, pending)
                        drain(host, pending)
                        log("session idle — waiting for the button")

            # ── always read, so the board keeps seeing a viewer ───────────
            # The connection is the point even when idle: it is what tells the
            # hub a host is attached. Frames are read and discarded rather than
            # left unread, because an unread MJPEG stream backs up on the board.
            ok, frame = cap.read()
            if not ok:
                log("stream read failed; reopening")
                cap.release(); cap = open_stream(host)
                time.sleep(0.5)
                continue

            if not sampling:
                time.sleep(0.5)              # idle: keep the pipe warm, cheaply
                continue

            r = detect(landmarker, frame)
            c = classify(r, centre)
            votes.append(c["state"])
            if verbose:
                print(f"  {c['state']:11} conf {c['conf']:.2f} "
                      f"yaw {c['yaw'] if c['yaw'] is None else round(c['yaw'], 3)} "
                      f"gaze {c['gaze'] if c['gaze'] is None else round(c['gaze'], 2)}")

            # ── one sample per window, by majority vote ───────────────────
            if now - last_window >= WINDOW_S:
                last_window = now
                if votes:
                    s = flush_window(votes, pending)
                    log(f"{s['state']:11} conf {s['confidence']:.2f}  "
                        f"queued {len(pending)}")

            if now - last_post >= POST_S:
                last_post = now
                sent += drain(host, pending)

            time.sleep(max(0.0, tick_interval - (time.time() - now)))

    except KeyboardInterrupt:
        log("stopping")
        raise
    finally:
        # Whatever ends this — Ctrl-C, a board reboot, an unexpected error — the
        # partial window and the queued batch go up rather than dying with the
        # process. A sample that was classified and then dropped is worse than
        # one never taken, because the gap is invisible in the data.
        try:
            if votes:
                flush_window(votes, pending)
            if pending:
                drain(host, pending)
        except Exception:
            pass
        try:
            cap.release()
        except Exception:
            pass
        log(f"posted {sent} samples this run")


def supervise(host, free_run=False, verbose=False):
    """Restart run() for as long as the process lives.

    This starts at login and is expected to outlive the things it talks to: the
    board reboots, mDNS blips, a frame arrives malformed. None of those should
    end the day's tracking.

    A loop rather than recursion. An earlier version called run() from inside
    run()'s own exception handler, which works and adds a stack frame every time
    the board reboots — a leak whose eventual symptom would be very hard to
    connect back to a camera power-cycle a week earlier.
    """
    while True:
        try:
            run(host, free_run=free_run, verbose=verbose)
            return                       # a clean return means we are finished
        except KeyboardInterrupt:
            return
        except Exception as e:
            log(f"restarting after {type(e).__name__}: {e}")
            time.sleep(5)


def flush_window(votes, pending):
    """Collapse a window of frames into one sample by majority vote.

    A single frame must never decide a state: a blink or a glance is not
    distraction, and the vote is what makes those transients disappear.
    """
    tally = Counter(votes)
    state, n = tally.most_common(1)[0]
    sample = {"ts": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
              "state": state,
              "confidence": round(n / len(votes), 3)}
    votes.clear()
    pending.append(sample)
    return sample


def drain(host, pending):
    """Post in batches. Every POST costs the board a TLS handshake upstream, so
    one per sample was slower than the samples arrived — that is what dropped a
    third of them before batching existed. A failed batch goes back to the front
    of the queue, so a transient error costs a delay rather than the data."""
    if not pending:
        return 0
    batch = pending[:100]
    del pending[:len(batch)]
    code, body = post_batch(host, batch)
    if code and 200 <= code < 300:
        log(f"posted {len(batch)} -> HTTP {code}")
        return len(batch)
    log(f"post failed ({code}: {body}); requeued {len(batch)}")
    pending[:0] = batch
    return 0


def main():
    ap = argparse.ArgumentParser(description="Cadence headless vision host")
    ap.add_argument("--host", default="cadence-cam.local",
                    help="camera board hostname or IP (default: cadence-cam.local)")
    ap.add_argument("--calibrate", action="store_true",
                    help="measure and store the centre baseline, then exit")
    ap.add_argument("--free", action="store_true",
                    help="record continuously, ignoring the hub's session state")
    ap.add_argument("--verbose", action="store_true", help="print every frame")
    a = ap.parse_args()

    if a.calibrate:
        calibrate(a.host, make_landmarker())
        return
    supervise(a.host, free_run=a.free, verbose=a.verbose)


if __name__ == "__main__":
    main()
