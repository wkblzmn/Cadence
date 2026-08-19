"""Making a dim frame readable, on the side of the link that has a CPU.

This is the half of the low-light problem the board cannot solve. The sensor's
only currencies are exposure time and gain: one costs frame rate, the other
costs noise, and past a certain darkness there is no setting that produces a
clean bright image. The PC is not so constrained — it can spend a millisecond
per frame instead.

The order matters and is not the intuitive one:

    denoise -> local contrast -> gamma

Denoising last would smooth the noise that CLAHE had just amplified, blurring
away the eye and mouth structure along with it. Denoising first gives CLAHE
something worth stretching.

None of this runs on a well-lit frame. Every step is gated on measured
brightness, because each one costs time and can only take detail away from an
image that already had enough.
"""

import cv2
import numpy as np

# Mean luma, 0-255. Below the floor a frame is dark enough to be worth work;
# below the dark point it needs everything available.
READABLE_FLOOR = 95.0
VERY_DARK      = 55.0

_GAMMA_LUT = {}


def _gamma_lut(g):
    """Cached 256-entry curve. Building one per frame is pure waste."""
    key = round(g, 2)
    if key not in _GAMMA_LUT:
        inv = 1.0 / key
        _GAMMA_LUT[key] = np.array(
            [((i / 255.0) ** inv) * 255 for i in range(256)], dtype=np.uint8)
    return _GAMMA_LUT[key]


def luma(frame):
    """Mean brightness of the frame, cheaply.

    Measured on a subsample rather than the whole frame: this runs on every
    frame that arrives, and a 4x decimation gives the same number to well
    inside a grey level for a sixteenth of the work.
    """
    return float(cv2.cvtColor(frame[::4, ::4], cv2.COLOR_BGR2GRAY).mean())


def enhance(frame, level):
    """Return a frame for the landmarker. `level` is 0.0 (none) to 1.0 (all).

    Returns the original array untouched at level 0, so a well-lit desk pays
    nothing for this module existing.
    """
    if level <= 0.0:
        return frame

    out = frame

    # 1. Denoise. Bilateral rather than non-local means: NLM is the better
    #    denoiser and takes 100+ ms a frame, which at 8 Hz is most of the
    #    budget. Bilateral is a few ms and preserves edges, and edges are what
    #    the landmarker is looking at.
    if level > 0.5:
        out = cv2.bilateralFilter(out, 5, 60, 60)

    # 2. Local contrast. CLAHE on L only, so colour is left alone — the
    #    landmarker works from structure and a colour shift would only mislead
    #    anyone watching the preview. Clip limit scales with how dark it is:
    #    on a merely dim frame a hard clip invents texture out of sensor noise.
    lab = cv2.cvtColor(out, cv2.COLOR_BGR2LAB)
    l, a, b = cv2.split(lab)
    clahe = cv2.createCLAHE(clipLimit=1.5 + 2.5 * level, tileGridSize=(8, 8))
    out = cv2.cvtColor(cv2.merge((clahe.apply(l), a, b)), cv2.COLOR_LAB2BGR)

    # 3. Gamma, last and only when it is genuinely dark. This lifts the
    #    midtones after the contrast stretch rather than before it, which is
    #    the difference between opening up the shadows and washing out the
    #    whole frame.
    if level > 0.6:
        out = cv2.LUT(out, _gamma_lut(1.0 + 0.8 * level))

    return out


def level_for(mean_luma):
    """How much work this frame justifies, from its own brightness.

    A ramp rather than a switch. A threshold would make the enhancement flicker
    on and off as the light drifted across it, and a landmarker fed alternately
    enhanced and raw frames tracks worse than one fed either consistently.
    """
    if mean_luma >= READABLE_FLOOR:
        return 0.0
    if mean_luma <= VERY_DARK:
        return 1.0
    span = READABLE_FLOOR - VERY_DARK
    return (READABLE_FLOOR - mean_luma) / span
