"""Talking to the board: what the session is doing, and posting samples back.

Samples go through the board's /focus relay rather than straight to the
backend. The API token lives in firmware and the relay exists to keep it there;
this app never sees it. The board splices its own device_id onto the front and
forwards the body verbatim.
"""

import json
import os

import requests

from . import config as cfg

# firmware/cadence_cam/cadence_cam.ino: RELAY_BODY_MAX is 640 and relayPush
# rejects a body at or over it — *after* handleFocus has spliced
# {"device_id":"cadence-hub-01", onto the front, which costs 29 bytes.
#
# This is not a theoretical limit. The host this app replaces batched up to 100
# samples per POST; at roughly 70 bytes each, anything past seven was refused
# with a 413, requeued whole, and refused again on the next attempt. Under
# normal timing a drain carries five and squeaks under, so it worked — until a
# network hiccup let the queue reach eight, at which point it could never drain
# again. Batches are measured here in bytes rather than counted.
RELAY_BODY_MAX  = 640
DEVICE_ID_SPLICE = 29
BODY_BUDGET     = RELAY_BODY_MAX - DEVICE_ID_SPLICE - 16   # 16 bytes of slack

# About an hour of windows. A queue that grows without limit turns a long
# outage into a memory leak, and the samples at the far end are stale anyway.
MAX_PENDING = 2000


def session_state(host, last="idle", timeout=3.0):
    """Ask the board what it should be doing.

    A failed poll returns `last`, not idle. One dropped request over WiFi is
    not a stopped session, and treating it as one would end a recording every
    time the radio hiccuped. The board applies its own 90-second TTL, so a
    genuinely stopped session still lands within a minute and a half.
    """
    try:
        r = requests.get(f"http://{host}/session", timeout=timeout)
        return r.json().get("state", last)
    except (requests.RequestException, ValueError):
        return last


def board_status(host, timeout=3.0):
    """The board's /status, for the light reading and the relay's own health."""
    try:
        r = requests.get(f"http://{host}/status", timeout=timeout)
        return r.json()
    except (requests.RequestException, ValueError):
        return None


def encode(samples):
    """The exact body /api/ingest/focus expects, minus device_id.

    Timestamps carry milliseconds and not microseconds, and confidence two
    decimals. Both are precision nobody reads, and on a 640-byte budget the
    difference is two extra samples per POST.
    """
    return json.dumps({
        "samples": [
            {"ts": s["ts"], "state": s["state"], "confidence": round(s["conf"], 2)}
            for s in samples
        ]
    }, separators=(",", ":"))


class Relay:
    """The pending queue and the batching rule.

    A failed batch goes back to the front rather than being dropped: a
    transient error should cost a delay, not the data.
    """

    def __init__(self, host, enabled=True, on_status=None):
        self.host = host
        self.enabled = enabled
        self.pending = load_spool()
        self.posted = 0
        self.dropped = 0
        self.last_error = None
        self.direct = load_backend()
        self._on_status = on_status or (lambda s: None)
        if self.pending:
            cfg.log(f"recovered {len(self.pending)} unsent samples from the spool")

    def queue(self, sample):
        self.pending.append(sample)
        save_spool(self.pending)
        if len(self.pending) > MAX_PENDING:
            over = len(self.pending) - MAX_PENDING
            del self.pending[:over]
            self.dropped += over
            cfg.log(f"queue full — dropped {over} oldest samples "
                    f"({self.dropped} this run)")

    def _next_batch(self):
        """Take as many samples as fit in one relay body."""
        batch = []
        for s in self.pending:
            trial = batch + [s]
            if len(encode(trial).encode()) > BODY_BUDGET and batch:
                break
            batch = trial
        return batch

    def drain(self):
        """Post what fits. Returns how many samples went up."""
        if not self.pending:
            return 0
        if not self.enabled:
            # --no-post: classify and display, write nothing. The queue would
            # otherwise grow to its cap and start logging drops for no reason.
            n = len(self.pending)
            self.pending.clear()
            return 0

        batch = self._next_batch()
        code, text = self._send(batch)

        if code and 200 <= code < 300:
            del self.pending[:len(batch)]
            save_spool(self.pending)
            self.posted += len(batch)
            self.last_error = None
            cfg.log(f"posted {len(batch)} -> HTTP {code}  "
                    f"({len(self.pending)} still queued)")
            self._on_status("ok")
            return len(batch)

        self.last_error = f"HTTP {code}: {text}"
        cfg.log(f"post failed ({self.last_error}); {len(batch)} stays queued")
        self._on_status("failing")
        return 0

    def _send(self, batch):
        """Board relay first, backend second.

        The board is preferred whenever it answers, because that path keeps the
        token where it was designed to live. Direct posting is the fallback for
        a camera that is on a cable and nothing else.
        """
        if self.host:
            try:
                r = requests.post(f"http://{self.host}/focus", data=encode(batch),
                                  headers={"Content-Type": "application/json"},
                                  timeout=4.0)
                if 200 <= r.status_code < 300:
                    return r.status_code, r.text[:160]
            except requests.RequestException:
                pass                      # fall through to the backend

        if self.direct:
            # No 640-byte limit on this path — that was the firmware relay
            # buffer — so a backlog drains far faster than through the board.
            big = self.pending[:400] if batch else batch
            code, text = _direct_post(self.direct, big)
            if code and 200 <= code < 300:
                batch[:] = big
                return code, f"direct: {text}"
            return code, f"direct: {text}"

        return None, "no route: board unreachable and no backend.json"

    def flush(self):
        """Drain repeatedly at shutdown, while it is still making progress.

        Whatever ends the app, the queued batches go up rather than dying with
        the process. A sample that was classified and then dropped is worse
        than one never taken, because the gap is invisible in the data.
        """
        for _ in range(20):
            if not self.pending:
                break
            if self.drain() == 0:
                break


# ── going up without the board ───────────────────────────────────────────────
#
# The relay exists to keep the API token in firmware, and that argument was
# about a *web page* served to the LAN, which cannot hold a secret from anyone
# who views its source. It does not carry over to a desktop app reading a file
# on the owner's own machine — and it stops applying entirely when the board is
# on the end of a USB cable with no network at all, which is the case this
# whole change exists to serve.
#
# So: through the board when it can be reached, directly when it cannot, and
# nowhere at all if neither works — in which case the spool below keeps the
# samples until something changes.

BACKEND = os.path.join(cfg.ROOT, "backend.json")
SPOOL   = os.path.join(cfg.ROOT, "spool.json")


def load_backend():
    """{"api_base": ..., "token": ..., "device_id": ...} or None.

    Kept out of the repo. Not in the code, and not in an environment variable
    either: this app is started by double-clicking a .pyw from a Startup
    folder, where nothing has set one.
    """
    try:
        with open(BACKEND) as f:
            c = json.load(f)
        if {"api_base", "token", "device_id"} <= c.keys():
            return c
    except (OSError, ValueError):
        pass
    return None


def load_spool():
    try:
        with open(SPOOL) as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


def save_spool(pending):
    """Persist the unsent queue.

    Rewritten whole rather than appended: the queue is at most a few hundred
    short records, and a partial line from a process that died mid-write would
    cost the whole file on the next load. Written to a temporary name and moved
    into place so a crash leaves either the old file or the new one.
    """
    try:
        tmp = SPOOL + ".tmp"
        with open(tmp, "w") as f:
            json.dump(pending, f)
        os.replace(tmp, SPOOL)
    except OSError:
        pass


def _direct_post(cfgd, batch):
    body = {"device_id": cfgd["device_id"],
            "samples": [{"ts": s["ts"], "state": s["state"],
                         "confidence": round(s["conf"], 2)} for s in batch]}
    try:
        r = requests.post(cfgd["api_base"].rstrip("/") + "/api/ingest/focus",
                          json=body,
                          headers={"Authorization": "Bearer " + cfgd["token"]},
                          timeout=10.0)
        return r.status_code, r.text[:160]
    except requests.RequestException as e:
        return None, f"{type(e).__name__}: {e}"
