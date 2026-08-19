"""The MJPEG stream from the board, and staying attached to it.

The board is an ESP32-S3 that serves frames and nothing else. Every hard part
of this file exists because it is a small radio device on the other side of a
house network: it reboots, mDNS blips, and the PC finishes booting long before
the board has joined the WiFi.
"""

import time

import cv2

from . import config as cfg


class Stream:
    """A connection that reopens itself rather than ending the run.

    Exiting on a failed open was wrong for anything that starts at login: the
    PC boots faster than the camera board joins the network, so a host that
    gives up on the first attempt is a host that is never running when you sit
    down. `on_status` is called with a short human string whenever that story
    changes, because the window shows it and the log records it.
    """

    def __init__(self, host, on_status=None):
        self.host = host
        self.url  = f"http://{host}:{cfg.STREAM_PORT}/stream"
        self.cap  = None
        self.connected = False
        self._on_status = on_status or (lambda s: None)
        self._attempt = 0

    def _status(self, msg):
        self._on_status(msg)

    def open(self, stop=None):
        """Block until the stream opens, or until `stop` says to give up.

        `stop` is the app closing. Without it this loop would keep a dead
        window's thread alive forever, waiting on a board that is switched off.
        """
        delay = 2
        self._attempt = 0
        while stop is None or not stop.is_set():
            self._attempt += 1
            cap = cv2.VideoCapture(self.url)
            if cap.isOpened():
                # Keep the buffer shallow. A backlog would hand the classifier
                # frames from several seconds ago, which for a 2s voting window
                # is enough to attribute attention to the wrong window entirely.
                cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
                self.cap = cap
                self.connected = True
                if self._attempt > 1:
                    cfg.log(f"stream open after {self._attempt} attempts")
                self._status("connected")
                return True
            cap.release()
            self.connected = False
            if self._attempt in (1, 5) or self._attempt % 30 == 0:
                cfg.log(f"cannot open {self.url} (attempt {self._attempt})"
                        " — waiting for the board")
            self._status(f"waiting for {self.host} ({self._attempt})")
            # Interruptible sleep: a 30-second backoff must not be 30 seconds
            # of a window that will not close.
            if stop is not None:
                if stop.wait(delay):
                    break
            else:
                time.sleep(delay)
            delay = min(delay * 2, 30)
        return False

    def set(self, params):
        """Camera settings over HTTP, mirroring SerialStream.set().

        Both transports expose this so tuning.py can drive the camera without
        knowing which one it is holding.
        """
        import requests
        try:
            r = requests.get(f"http://{self.host}/set", params=params, timeout=4.0)
            return 200 <= r.status_code < 300, f"HTTP {r.status_code} {r.text[:60]}"
        except requests.RequestException as e:
            return False, f"{type(e).__name__}: {e}"

    def read(self):
        """One frame, or (False, None) if the connection has gone."""
        if self.cap is None:
            return False, None
        ok, frame = self.cap.read()
        if not ok:
            self.connected = False
        return ok, frame

    def reopen(self, stop=None):
        cfg.log("stream read failed; reopening")
        self._status("reconnecting")
        self.release()
        return self.open(stop)

    def release(self):
        if self.cap is not None:
            try:
                self.cap.release()
            except Exception:
                pass
            self.cap = None
        self.connected = False
