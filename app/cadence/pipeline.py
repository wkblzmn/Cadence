"""The worker thread: frames in, samples out, live state for the window.

Tkinter owns the main thread and will not share it, and both halves of this
loop block — cap.read() waits on the radio, detect() waits on the CPU. So the
pipeline runs here and publishes a snapshot the window reads whenever it
repaints. Nothing in this file touches a widget.
"""

import threading
import time

import numpy as np
from collections import Counter, deque
from datetime import datetime, timezone

from . import config as cfg
from . import enhance, tuning
from .analysis import Detector, classify, gaze_vector, pose
from .capture import Stream
from .serialcam import SerialStream
from .relay import Relay, session_state

IDLE_HZ = 2.0     # keep classifying while idle, cheaply, so the window is live

# How many recent detections the miss rate is measured over. Forty at 8 Hz is
# about five seconds — long enough that a hand across the face does not trigger
# a camera reconfiguration, short enough to react within one dimming.
MISS_WINDOW = 40


def _now_iso():
    return (datetime.now(timezone.utc)
            .isoformat(timespec="milliseconds").replace("+00:00", "Z"))


class Pipeline:
    """Owns the stream, the detector and the session.

    Two things can start a recording, and they are deliberately not the same
    mechanism:

    `hub` follows the board, which the physical focus-timer button drives.
    `local` is the app's own Start button.

    The app never writes /session?state=. The hub re-pushes the board's state
    every 30 seconds, so anything this app wrote there would be overwritten
    within half a minute and the recording would stop on its own. Local
    sessions are held here instead, where nothing else can clear them.
    """

    def __init__(self, host, post=True, mode="hub", tune=True,
                 source=None, port=None):
        self.host = host
        self.mode = mode
        self.local_running = False
        self.tune = tune
        self.source = source or cfg.DEFAULT_SOURCE

        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread = None
        self._posting = threading.Event()

        # The cable by default. Over USB there is no host, no lease and no
        # name resolution to fail, which is the whole reason it is the default:
        # this has to work in a room on a network nobody controls.
        if self.source == "usb":
            self.stream = SerialStream(port=port, on_status=self._set_stream_status)
        else:
            self.stream = Stream(host, on_status=self._set_stream_status)
        self.relay = Relay(host, enabled=post, on_status=self._set_relay_status)
        self.lowlight = tuning.LowLight(self.stream, enabled=tune)
        self.centre = cfg.load_calibration()

        # Everything the window reads. Guarded by _lock; never a widget.
        self._snap = {
            "frame": None, "landmarks": None,
            "state": "absent", "conf": 0.0, "ratio": None,
            "yaw": None, "pitch": None, "gaze": None, "eyes": None,
            "recording": False, "board_state": "idle",
            "stream_status": "starting", "relay_status": "idle",
            "session_start": None,
            "counts": Counter(), "timeline": deque(maxlen=900),
            "queued": 0, "posted": 0, "fps": 0.0,
            "calibrated": self.centre is not None,
            "stale_calibration": cfg.calibration_is_stale(self.centre),
            # the input end: how dark it is, how hard we are working on it,
            # and whether any of that is rescuing the detections
            "luma": None, "enhance": 0.0, "miss_rate": 0.0,
            "board_tuning": tuning.LOW_LIGHT_LADDER[0],
            # Revisions, so the window can tell what actually changed. The
            # repaint timer runs faster than either of these moves, and
            # rebuilding a 640x480 image or a 900-bar strip for a frame that
            # has not changed is most of what a repaint costs.
            "frame_seq": 0, "timeline_rev": 0,
        }
        # What the window shows: the enhanced frame by default, because the
        # useful question when the light is bad is what the model is working
        # from, not what the room looks like.
        self.show_enhanced = True

    # ── snapshot plumbing ────────────────────────────────────────────────────
    def _update(self, **kw):
        with self._lock:
            self._snap.update(kw)

    def _publish_frame(self, frame, **kw):
        """Publish a frame and mark it new, in one lock.

        Bumping the counter separately would let the window read a new
        sequence against the previous frame and skip the one that followed.
        """
        with self._lock:
            self._snap.update(kw)
            self._snap["frame"] = frame
            self._snap["frame_seq"] += 1

    def snapshot(self):
        """A shallow copy, taken every repaint.

        The timeline is deliberately not materialised here — it is up to 900
        entries and the strip that draws it only changes once every two
        seconds, so copying it on every tick was pure waste. Ask for it with
        timeline_list() when timeline_rev says it moved.
        """
        with self._lock:
            s = dict(self._snap)
            s["counts"] = Counter(self._snap["counts"])
            del s["timeline"]
            return s

    def timeline_list(self):
        with self._lock:
            return list(self._snap["timeline"])

    def _set_stream_status(self, msg):
        self._update(stream_status=msg)

    def _set_relay_status(self, msg):
        self._update(relay_status=msg)

    # ── lifecycle ────────────────────────────────────────────────────────────
    def start(self):
        self._thread = threading.Thread(
            target=self._run, name="cadence-pipeline", daemon=True)
        self._thread.start()

    def stop(self, timeout=8.0):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout)

    def set_mode(self, mode):
        """hub or local. Switching either way ends the current recording."""
        self.mode = mode
        self.local_running = False

    def toggle_local(self):
        self.local_running = not self.local_running
        return self.local_running

    def reload_calibration(self):
        self.centre = cfg.load_calibration()
        self._update(calibrated=self.centre is not None,
                     stale_calibration=cfg.calibration_is_stale(self.centre))

    # ── the loop ─────────────────────────────────────────────────────────────
    def _run(self):
        """Restarts itself for as long as the app lives.

        A loop rather than recursion. An earlier version called the session
        loop from inside its own exception handler, which works and adds a
        stack frame every time the board reboots — a leak whose eventual
        symptom would be very hard to connect back to a camera power-cycle a
        week earlier.
        """
        while not self._stop.is_set():
            try:
                self._session_loop()
                return
            except Exception as e:
                cfg.log(f"restarting after {type(e).__name__}: {e}")
                self._update(stream_status=f"restarting after {type(e).__name__}")
                if self._stop.wait(5):
                    return

    def _session_loop(self):
        det = Detector()
        if self.centre:
            cfg.log("calibration: yaw {:+.3f}  pitch {:+.3f}  gaze ({:+.3f}, {:+.3f})"
                    .format(self.centre["yaw"], self.centre["pitch"],
                            self.centre["gaze_h"], self.centre["gaze_v"]))
        else:
            cfg.log("NO CALIBRATION — everything reads distracted until you calibrate")

        # Open first, then configure. Over USB the settings channel *is* the
        # open port, so there is nothing to configure until the link exists —
        # the reverse of the HTTP case, where /set worked before the stream did.
        if not self.stream.open(self._stop):
            det.close()
            return

        if self.tune:
            tuning.apply_analysis_preset(self.stream)

        cfg.log("watching {} over {} — mode {}".format(
            self.host if self.source == "wifi" else self.stream.port,
            self.source, self.mode))

        votes = []
        recording = False
        board = "idle"
        checked_size = not self.tune
        last_session = last_window = last_post = last_infer = 0.0
        last_light = 0.0
        misses = deque(maxlen=MISS_WINDOW)
        luma = None
        frames = 0
        fps_mark = time.time()

        try:
            while not self._stop.is_set():
                now = time.time()

                # ── what should we be doing ──────────────────────────────────
                if now - last_session >= cfg.SESSION_S:
                    last_session = now
                    if self.mode == "hub":
                        if self.source == "usb":
                            # The board tells us unprompted, over ESP-NOW from
                            # the hub. Never ask over HTTP here: that request
                            # has a 3 s timeout and runs on this thread, and
                            # this thread has to keep draining a 210 KB/s pipe.
                            # Blocking it lets the driver buffer overrun, which
                            # does not drop whole frames, it corrupts them —
                            # 18.9 fps measured without this call, 0.4 with it.
                            board = getattr(self.stream, "session_state", None) or "idle"
                        else:
                            board = session_state(self.host, board)
                    self._update(board_state=board)

                want = (board == "running") if self.mode == "hub" else self.local_running
                if want != recording:
                    recording = want
                    if recording:
                        votes.clear()   # frames before the press belong to no session
                        with self._lock:
                            self._snap.update(session_start=now, counts=Counter(),
                                              timeline=deque(maxlen=900),
                                              recording=True)
                            self._snap["timeline_rev"] += 1
                        cfg.log("session running — recording")
                    else:
                        # Close the open window and flush, rather than holding a
                        # partial batch that a crash would take with it.
                        if votes:
                            self._flush_window(votes)
                        self.relay.flush()
                        self._update(recording=False, queued=len(self.relay.pending),
                                     posted=self.relay.posted)
                        cfg.log("session idle — waiting")

                # ── always read, so the board keeps seeing a viewer ──────────
                # The connection is the point even when idle: it is what tells
                # the hub a host is attached. Frames are read rather than left
                # unread, because an unread MJPEG stream backs up on the board.
                ok, frame = self.stream.read()
                if not ok:
                    if not self.stream.reopen(self._stop):
                        break
                    continue

                # The board may have been left at another resolution. Checked
                # once, from the pixels, and it costs a stream restart — so it
                # happens here on the first real frame rather than blind.
                if not checked_size:
                    checked_size = True
                    if tuning.ensure_vga(self.stream, frame):
                        self.stream.reopen(self._stop)
                        continue

                frames += 1
                if now - fps_mark >= 1.0:
                    self._update(fps=frames / (now - fps_mark))
                    frames, fps_mark = 0, now

                # Classify while idle too, at a lower rate. It costs little and
                # it is how you check your framing and your baseline before
                # pressing anything.
                interval = 1.0 / (cfg.INFER_HZ if recording else IDLE_HZ)
                if now - last_infer < interval:
                    # Between inferences there is only a raw frame to show, so
                    # publishing it while the preview is set to the enhanced
                    # view would alternate between the two several times a
                    # second. The enhanced view holds the last classified frame
                    # instead; the raw view runs at the full stream rate.
                    if not self.show_enhanced:
                        self._publish_frame(frame)
                    continue
                last_infer = now

                # ── the input end ────────────────────────────────────────────
                # Measure, then spend only what the darkness justifies. On a
                # lit desk `level` is 0 and enhance() hands the frame straight
                # back, so none of this costs anything when it is not needed.
                luma = enhance.luma(frame)
                level = enhance.level_for(luma)
                shot = enhance.enhance(frame, level)

                result = det.detect(shot, now * 1000.0)
                c = classify(result, self.centre)
                lm = result.face_landmarks[0] if (result and result.face_landmarks) else None
                misses.append(lm is None)

                self._publish_frame(shot if self.show_enhanced else frame,
                                    landmarks=lm, state=c["state"], conf=c["conf"],
                                    ratio=c["ratio"], yaw=c["yaw"], pitch=c["pitch"],
                                    gaze=c["gaze"], eyes=c["eyes"],
                                    luma=luma, enhance=level)

                # Once a second, decide whether the board itself has to give up
                # frame rate for light. Only if the enhancement above is
                # already failing to find a face — a dark frame the landmarker
                # can still read is not worth slowing the sensor down for.
                if now - last_light >= 1.0:
                    last_light = now
                    miss_rate = (sum(misses) / len(misses)) if misses else 0.0
                    self._update(miss_rate=miss_rate)
                    if self.lowlight.consider(now, luma < enhance.READABLE_FLOOR,
                                              miss_rate):
                        self._update(
                            board_tuning=tuning.LOW_LIGHT_LADDER[self.lowlight.rung])
                        misses.clear()   # judge the new setting on its own frames

                if not recording:
                    continue

                votes.append(c["state"])

                # ── one sample per window, by majority vote ──────────────────
                if now - last_window >= cfg.WINDOW_S:
                    last_window = now
                    if votes:
                        self._flush_window(votes)

                if now - last_post >= cfg.POST_S:
                    last_post = now
                    # Same reason: an upload is an HTTP round trip and must not
                    # happen between two frame reads. The queue is already
                    # spooled to disk, so handing it to another thread risks
                    # nothing.
                    self._post_async()
        finally:
            # Whatever ends this — the window closing, a board reboot, an
            # unexpected error — the partial window and the queued batch go up
            # rather than dying with the process. A sample that was classified
            # and then dropped is worse than one never taken, because the gap
            # is invisible in the data.
            try:
                if votes:
                    self._flush_window(votes)
                self.relay.flush()
            except Exception:
                pass
            self.stream.release()
            det.close()
            cfg.log("posted {} samples this run, {} unsent"
                    .format(self.relay.posted, len(self.relay.pending)))

    def _post_async(self):
        """Drain the relay on a throwaway thread, one at a time."""
        if self._posting.is_set():
            return                       # a previous upload is still going
        self._posting.set()

        def work():
            try:
                self.relay.drain()
            except Exception as e:
                cfg.log(f"post failed: {type(e).__name__}: {e}")
            finally:
                self._update(queued=len(self.relay.pending),
                             posted=self.relay.posted)
                self._posting.clear()

        threading.Thread(target=work, name="cadence-post", daemon=True).start()

    def _flush_window(self, votes):
        """Collapse a window of frames into one sample by majority vote.

        A single frame must never decide a state: a blink or a glance is not
        distraction, and the vote is what makes those transients disappear.
        """
        tally = Counter(votes)
        state, n = tally.most_common(1)[0]
        conf = n / len(votes)
        votes.clear()

        self.relay.queue({"ts": _now_iso(), "state": state, "conf": conf})

        with self._lock:
            self._snap["counts"][state] += 1
            self._snap["timeline"].append(state)
            self._snap["timeline_rev"] += 1
            self._snap["queued"] = len(self.relay.pending)
            self._snap["posted"] = self.relay.posted


def calibrate(host, seconds=None, progress=None, stop=None, source=None):
    """Average the pose and gaze over a few seconds of sitting normally.

    Averaging rather than sampling one frame is deliberate: one frame catches
    whatever micro-movement you happened to be making, and the baseline it
    produces is then wrong for every session after.

    The yield is checked, not just the count. The file this replaces was
    written from 8 usable frames and passed a guard that only rejected below
    five — a baseline that thin is a coin toss, and it silently became the
    definition of centre for every session that followed.
    """
    det = Detector()
    stream = (SerialStream() if (source or cfg.DEFAULT_SOURCE) == "usb"
              else Stream(host))
    seconds = seconds or cfg.CALIB_SECONDS
    samples = []
    attempts = 0

    # The same preset and the same enhancement the run itself uses. A baseline
    # measured from frames that were processed differently is a baseline for a
    # different quantity — and calibrating on raw dark frames is how the file
    # this replaces ended up with eight usable samples.
    try:
        if not stream.open(stop):
            raise RuntimeError("could not open the stream")
        tuning.apply_analysis_preset(stream)

        deadline = time.time() + seconds
        while time.time() < deadline:
            if stop is not None and stop.is_set():
                raise RuntimeError("cancelled")
            attempts += 1
            ok, frame = stream.read()
            if not ok:
                continue
            frame = enhance.enhance(frame, enhance.level_for(enhance.luma(frame)))
            r = det.detect(frame, time.time() * 1000.0)
            if r and r.face_landmarks:
                p = pose(r.face_landmarks[0])
                gh = gv = 0.0
                if r.face_blendshapes:
                    scores = {c.category_name: c.score for c in r.face_blendshapes[0]}
                    gh, gv = gaze_vector(scores)
                samples.append((p["yaw"], p["pitch"], gh, gv))
            if progress:
                progress(max(0.0, deadline - time.time()), len(samples), attempts)
    finally:
        stream.release()
        det.close()

    if len(samples) < cfg.CALIB_MIN_FRAMES:
        raise RuntimeError(
            "only {} usable frames from {} — that is too thin to be a baseline. "
            "Check the light and that you are in shot. Nothing written."
            .format(len(samples), attempts))

    arr = np.array(samples)
    centre = cfg.save_calibration(arr[:, 0].mean(), arr[:, 1].mean(),
                                  arr[:, 2].mean(), arr[:, 3].mean(),
                                  len(samples), attempts)
    cfg.log("calibrated from {}/{} frames: yaw {:+.3f}  pitch {:+.3f}  "
            "gaze ({:+.3f}, {:+.3f})"
            .format(len(samples), attempts, centre["yaw"], centre["pitch"],
                    centre["gaze_h"], centre["gaze_v"]))
    return centre
