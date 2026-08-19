"""Driving the board's /set endpoint, so the pipeline gets the frames it wants.

The app asserts a known configuration on startup rather than inheriting
whatever the board was left in. A camera that has been power-cycled, poked at
from the /vision page, or left on last week's experiment is otherwise an
invisible variable in every session that follows.

Everything here rests on measurements already in the firmware's own comments —
they were made on this hardware, and they are not the obvious answers:

  * VGA is a floor, not a ceiling. QVGA and CIF measured **zero frames**: this
    is a 3MP sensor and the small sizes engage a scaler path that stalls. XGA
    is no better, because MediaPipe downscales to a few hundred pixels square
    before inference. So VGA, and nothing else.

  * Denoise is turned off on purpose. On a face, the detail it removes is the
    eye and mouth landmarks. Noise is better dealt with on this side of the
    link, where there is a CPU that can do it selectively — see enhance.py.

And one thing the firmware's comments could not have known, because it is a
property of the link rather than the sensor: over WiFi this board delivers
about 1 frame per second, with a median inter-frame gap of 615 ms, a 95th
percentile of 2.7 s and a worst case of 6.3 s — while its *minimum* gap is
98 ms. A stream limited by exposure would cluster tightly around one value.
That spread is the transport, not the camera, and it means none of the knobs
below can buy back more than a fraction of the frame rate. They are worth
setting correctly; they are not worth mistaking for the problem.
"""

from . import config as cfg

# Applied once at startup. `size` is deliberately absent: /set?size= does a full
# camera teardown and restart, which drops the stream, so it is only sent when
# the decoded frame proves the board is not already at VGA.
ANALYSIS_PRESET = {
    "q":       8,
    "gma":     1,
    "denoise": 0,
    "fast":    1,
}

# Two settings in the first version of this file were wrong, and measuring them
# on the actual board is the only reason that is known:
#
#   night=1 was applied unconditionally. It is a MANUAL exposure preset built
#   for ~30 lux, and the firmware says plainly that a bright room will blow it
#   out. Measured in a lit room it gave luma 226 out of 255 — a white frame — at
#   0.6 fps, against luma 62 at 1.8 fps with the auto loops running. It is now
#   the last rung of the ladder, reached only when the room is genuinely dark,
#   rather than the starting point.
#
#   q=4 assumed spare bandwidth. The link carries 0.12 Mbps and the constraint
#   turned out to be frame *rate*, not compression, so asking for larger frames
#   spent the scarce resource to fix the abundant one. Back to the firmware's
#   own default of 8.
#
# fast=1 replaces both as the default: it turns off the extended night exposure
# and raises the gain ceiling, so the sensor buys brightness with gain instead
# of time. Noise is this side's problem to solve, and enhance.py is better at
# it than a sensor with two registers.

# Brighter and slower, in that order. Each rung buys visibility with exposure
# time, and exposure time is frame rate — which is why the app only climbs it
# when the frames are dark *and* faces are actually being missed, rather than
# on brightness alone. A dim frame the landmarker can still read is not a
# problem worth trading frame rate for.
LOW_LIGHT_LADDER = [
    {"fast": 1},                       # gain, not time: auto exposure, 32x ceiling
    {"exposure": 300, "gain": 30},     # short manual exposure, gain at the ceiling
    {"night": 1},                      # exposure 700 — bright, and the slowest
]


def apply_analysis_preset(stream):
    """Assert the analysis configuration. Safe to call on every start.

    Takes the open transport rather than a hostname: over USB there is no
    hostname to take, and the settings have to travel down the same cable the
    frames come up.
    """
    ok, detail = stream.set(ANALYSIS_PRESET)
    cfg.log(f"board preset {'applied' if ok else 'FAILED'}: "
            f"{ANALYSIS_PRESET} — {detail}")
    return ok


def ensure_vga(stream, frame):
    """Restart the camera at VGA only if the frame proves it is not there.

    Checked against the decoded frame rather than /status's `framesize`, which
    reports an enum whose numbering has changed between esp32-camera releases.
    The pixels do not lie and do not need a lookup table.
    """
    if frame is None:
        return False
    h, w = frame.shape[:2]
    if (w, h) == (640, 480):
        return False
    cfg.log(f"board is streaming {w}x{h}, not VGA — restarting it at VGA")
    stream.set({"size": "vga"})
    return True      # the caller must expect the stream to drop


class LowLight:
    """Climbs the exposure ladder, and only for a reason.

    The trigger is a miss rate, not a brightness: frames dark enough to look
    alarming are often still perfectly landmarkable, and spending frame rate to
    fix an image that was never failing is a bad trade. It also only ever
    climbs during a run — dropping back down would oscillate against the very
    condition it is correcting.
    """

    def __init__(self, stream, enabled=True):
        self.stream = stream
        self.enabled = enabled
        self.rung = 0
        self.last_change = 0.0

    def describe(self):
        r = LOW_LIGHT_LADDER[self.rung]
        return ", ".join(f"{k}={v}" for k, v in r.items())

    def consider(self, now, dark, miss_rate, cooldown=20.0):
        """Called once a second with the last window's statistics.

        `dark` is the frame luma being under the readable floor; `miss_rate` is
        the fraction of recent detections that found no face at all.
        """
        if not self.enabled or self.rung >= len(LOW_LIGHT_LADDER) - 1:
            return False
        if not (dark and miss_rate > 0.4):
            return False
        if now - self.last_change < cooldown:
            return False

        self.rung += 1
        self.last_change = now
        rung = LOW_LIGHT_LADDER[self.rung]
        ok, detail = self.stream.set(rung)
        cfg.log(f"low light: climbing to {self.describe()} "
                f"(miss rate {miss_rate:.0%}) — {detail}")
        return ok
