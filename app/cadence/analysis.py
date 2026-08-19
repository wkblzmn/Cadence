"""The classifier: a frame in, a state out. This is the whole analysis tier.

It used to exist twice — here and in firmware/cadence_cam/vision_page.h — with
check_parity.py keeping the two identical. The PC now owns the pipeline, the
page is preview only, and this file is the single definition of what `focused`
means.

Three things changed on 2026-08-18, and two of them redefine the data:

1. Gaze is a vector, not a max().
   The old code took max() over six ARKit blendshapes mixing look-out, look-in
   and look-down. Those are not zero at a neutral gaze — opposing eyes carry
   partial scores — so the measured baseline was 0.418, leaving 0.582 of
   headroom for a limit of 0.35. Firing needed a raw score above 0.77, which
   never happened. The gaze term was dead and the classifier was head pose
   only, which is precisely the case the design cared most about: a phone on
   the desk moves the eyes and not the head.

2. Confidence follows whichever state was reported.
   The old formula fell as you turned further away, so the least ambiguous
   `distracted` samples went into focus_samples carrying the lowest confidence
   on the scale. It was a confidence-in-focused score wearing the wrong label.

3. Detection runs in VIDEO mode rather than IMAGE mode.
   IMAGE treats every frame as unrelated to the last. On a stream that is both
   wasteful and less stable than letting MediaPipe track across frames.

Rows in focus_samples written before that date came from the old definition.
They are not directly comparable to anything written after it.
"""

import math
import os

import cv2
import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision as mp_vision

from . import config as cfg

# Landmark indices, unchanged.
L_EYE, R_EYE, NOSE, BROW, CHIN = 33, 263, 1, 168, 152


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


def gaze_vector(scores):
    """Signed (horizontal, vertical) eye direction from ARKit blendshapes.

    The shapes come in opposing pairs, and looking one way raises two of them
    at once: a glance to your left is eyeLookOutLeft on the left eye and
    eyeLookInRight on the right. Averaging each direction's pair and then
    subtracting the opposite direction gives a component that is near zero when
    you look straight ahead and signed by which way you looked — which is the
    property max() destroyed.

    Up is folded in as well. The old set had look-down but no look-up, so a
    glance at a second monitor above the main one was invisible.
    """
    def s(name):
        return scores.get(name, 0.0)

    left  = (s("eyeLookOutLeft")  + s("eyeLookInRight")) / 2
    right = (s("eyeLookInLeft")   + s("eyeLookOutRight")) / 2
    down  = (s("eyeLookDownLeft") + s("eyeLookDownRight")) / 2
    up    = (s("eyeLookUpLeft")   + s("eyeLookUpRight")) / 2
    return left - right, down - up


def blink(scores):
    return max(scores.get("eyeBlinkLeft", 0.0), scores.get("eyeBlinkRight", 0.0))


def classify(result, centre):
    """One frame's verdict.

    Returns the state, a confidence in *that* state, and the raw quantities the
    window uses for its readout.
    """
    if not result or not result.face_landmarks:
        return {"state": "absent", "conf": 0.9, "yaw": None, "pitch": None,
                "gaze": None, "eyes": None, "ratio": None}

    p = pose(result.face_landmarks[0])
    yaw, pitch = p["yaw"], p["pitch"]

    gh = gv = closed = 0.0
    if result.face_blendshapes:
        scores = {c.category_name: c.score for c in result.face_blendshapes[0]}
        gh, gv = gaze_vector(scores)
        closed = blink(scores)

    if centre:
        yaw   -= centre["yaw"]
        pitch -= centre["pitch"]
        gh    -= centre["gaze_h"]
        gv    -= centre["gaze_v"]

    gaze_mag = math.hypot(gh, gv)

    # One ratio across all three axes: how far past its own limit the worst
    # offender is. Above 1.0 is distracted, and the distance from 1.0 is how
    # sure we are either way.
    ratio = max(abs(yaw)   / cfg.YAW_LIMIT,
                abs(pitch) / cfg.PITCH_LIMIT,
                gaze_mag   / cfg.GAZE_LIMIT)

    # Eyes shut is not distraction. A blink is 100-400 ms and the window vote
    # absorbs it; sustained closure is drowsiness, a different signal this
    # project does not claim to detect. So closure is reported, never scored.
    state = "distracted" if ratio > 1.0 else "focused"
    conf  = min(1.0, max(0.5, 0.5 + abs(ratio - 1.0) * 0.5))

    return {"state": state, "conf": conf, "yaw": yaw, "pitch": pitch,
            "gaze": gaze_mag, "gaze_h": gh, "gaze_v": gv, "eyes": closed,
            "ratio": ratio}


class Detector:
    """MediaPipe in VIDEO mode, which needs monotonic timestamps.

    Wrapped in a class only because that timestamp is state: VIDEO mode rejects
    a frame stamped at or before the previous one, and a stream that reconnects
    can otherwise hand it the same millisecond twice.
    """

    def __init__(self):
        # RuntimeError and not SystemExit. This is constructed on the worker
        # thread, and SystemExit derives from BaseException — the supervisor's
        # `except Exception` would not catch it, so a missing model would kill
        # the thread without a word and leave the window saying "starting"
        # forever. The CLI path turns this back into an exit message.
        if not os.path.exists(cfg.MODEL):
            raise RuntimeError(
                f"missing model: {cfg.MODEL}\n"
                "Download face_landmarker.task (float16/1) from Google's "
                "mediapipe-models bucket and put it in the app/ directory.")
        opts = mp_vision.FaceLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=cfg.MODEL),
            output_face_blendshapes=True,
            num_faces=1,
            running_mode=mp_vision.RunningMode.VIDEO,
        )
        self._lm = mp_vision.FaceLandmarker.create_from_options(opts)
        self._ts = 0

    def detect(self, frame_bgr, ts_ms):
        self._ts = max(self._ts + 1, int(ts_ms))
        rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        return self._lm.detect_for_video(image, self._ts)

    def close(self):
        try:
            self._lm.close()
        except Exception:
            pass
