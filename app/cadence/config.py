"""Tunables, paths, and the calibration file.

Every number the pipeline depends on lives here, so the classifier, the worker
loop and the window cannot end up disagreeing about one.
"""

import json
import os
import sys
from datetime import datetime, timezone

HERE  = os.path.dirname(os.path.abspath(__file__))
ROOT  = os.path.dirname(HERE)
MODEL = os.path.join(ROOT, "face_landmarker.task")
CALIB = os.path.join(ROOT, "calibration.json")
LOG   = os.path.join(ROOT, "cadence.log")
PORTFILE = os.path.join(ROOT, "serial_port.json")

# ── the board ────────────────────────────────────────────────────────────────
DEFAULT_HOST = "cadence-cam.local"
STREAM_PORT  = 81

# usb | wifi. USB is the default because it needs no network at all:
# no SSID compiled into firmware, no DHCP lease, no mDNS, and no
# access point that might isolate its clients from each other.
DEFAULT_SOURCE = "usb"

# ── classifier limits ────────────────────────────────────────────────────────
# Head pose, as fractions of the eye span. Unchanged: these two always worked.
YAW_LIMIT   = 0.20
PITCH_LIMIT = 0.28

# Gaze, as the length of the signed (horizontal, vertical) blendshape vector.
#
# NOT comparable to the old GAZE_LIMIT of 0.35. That one was measured against
# max() over six mixed blendshapes, a quantity that sat near 0.42 at a neutral
# gaze; this one is centred on zero by construction. See analysis.gaze_vector.
GAZE_LIMIT = 0.30

# ── timing ───────────────────────────────────────────────────────────────────
INFER_HZ  = 8.0    # classifications per second; the board rarely beats ~12 fps
WINDOW_S  = 2.0    # one posted sample per window, by majority vote
POST_S    = 10.0   # batch upload interval
SESSION_S = 2.0    # how often to ask the board what it should be doing

# The board expires a session it has not heard about in SESSION_TTL_MS (90s),
# and the hub refreshes it every CAM_HEARTBEAT_MS (30s). Both are firmware
# constants; this is here so the reason is readable from the app side.
#
# It is why the in-app Start button does not write /session?state=running: the
# hub's next heartbeat, at most 30 seconds later, would overwrite it with the
# hub's own idea of the state and stop the recording. Local sessions are the
# app's own authority instead, and the board is left to the hub.
HUB_HEARTBEAT_S = 30

# ── calibration ──────────────────────────────────────────────────────────────
CALIB_SCHEMA  = 2      # 1 was the scalar-gaze baseline; the meaning changed
CALIB_SECONDS = 8.0
CALIB_MIN_FRAMES = 20  # of roughly 60 attempts; below this the light is wrong


def load_calibration():
    """Read the baseline, or None if there is not a usable current one.

    A schema-1 file is refused rather than migrated. Its `gaze` was a max()
    over mixed blendshapes and the new one is a vector magnitude; carrying the
    old number forward would look like a calibrated host while feeding the
    classifier a baseline from a different quantity entirely.
    """
    if not os.path.exists(CALIB):
        return None
    try:
        with open(CALIB) as f:
            c = json.load(f)
    except (OSError, ValueError):
        return None
    if c.get("schema") != CALIB_SCHEMA:
        return None
    return c if {"yaw", "pitch", "gaze_h", "gaze_v"} <= c.keys() else None


def save_calibration(yaw, pitch, gaze_h, gaze_v, frames, attempts):
    centre = {
        "schema":   CALIB_SCHEMA,
        "yaw":      float(yaw),
        "pitch":    float(pitch),
        "gaze_h":   float(gaze_h),
        "gaze_v":   float(gaze_v),
        "frames":   int(frames),
        "attempts": int(attempts),
        "at":       datetime.now(timezone.utc).isoformat(),
    }
    with open(CALIB, "w") as f:
        json.dump(centre, f, indent=2)
    return centre


def calibration_is_stale(centre):
    """Old calibration is not wrong, but it is worth saying out loud.

    The baseline encodes where the chair was. Weeks later it usually is not
    there any more, and the symptom of a stale baseline — everything reads
    distracted — looks exactly like the classifier being broken.
    """
    if not centre or "at" not in centre:
        return False
    try:
        at = datetime.fromisoformat(centre["at"])
    except ValueError:
        return False
    return (datetime.now(timezone.utc) - at).days >= 14


def log(msg):
    """Print and append.

    Kept from the headless host: this can still be started from a login task
    with nothing watching stdout, and a traceback nobody reads is the same as
    no diagnosis at all.
    """
    line = f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}  {msg}"
    try:
        print(line, flush=True)
    except UnicodeEncodeError:
        # A Windows console at cp1252 cannot encode the punctuation used
        # throughout these messages. The log file is UTF-8 and keeps it; the
        # console gets a lossy version rather than losing the line entirely.
        #
        # This clause has to come first: UnicodeEncodeError is a subclass of
        # ValueError, so the broader handler below would otherwise swallow it
        # and the console would silently lose every line with an em dash.
        try:
            print(line.encode("ascii", "replace").decode(), flush=True)
        except (OSError, ValueError):
            pass
    except (OSError, ValueError):
        pass           # pythonw: no console attached, and that is fine
    try:
        if os.path.exists(LOG) and os.path.getsize(LOG) > 1_000_000:
            os.replace(LOG, LOG + ".1")
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass           # logging must never be the thing that fails


def windows_dark_mode():
    """Follow the OS theme, so the window does not glow at night.

    Returns True for dark, and defaults to dark anywhere the key is missing —
    the dashboard this app sits beside is dark-first too.
    """
    if sys.platform != "win32":
        return True
    try:
        import winreg
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize")
        with key:
            return not winreg.QueryValueEx(key, "AppsUseLightTheme")[0]
    except OSError:
        return True
