"""Frames over the USB cable, speaking the protocol in firmware/cadence_cam/usb_stream.h.

Why this transport exists is recorded in that header, in numbers. The short
version is that Wi-Fi delivered about one frame per second with six-second
stalls while the sensor was demonstrably capable of ten, and that a board whose
credentials are compiled in cannot be taken to a university and put on a
network nobody controls.

This presents exactly the interface capture.Stream does — open, read, reopen,
release, set — so nothing upstream of it knows or cares which way the frames
arrived.
"""

import binascii
import json
import os
import time

import cv2
import numpy as np
import serial
import serial.tools.list_ports

from . import config as cfg

MAGIC = b"\xCA\xDE\xF0\x0D"
HEADER = 12

# 2 Mbaud is what the firmware opens at. 921600 is the fallback: it is the
# fastest rate every USB-serial bridge and cable in a drawer will do, and at
# 9 KB a frame it still carries about ten frames a second — seven times the
# wireless path, from a rate nothing is likely to refuse.
BAUDS = (2000000, 921600)

# Ports worth trying. The CH343 is what is on these boards; the others cost
# nothing to include and save a support conversation if a board is replaced.
KNOWN_VIDS = {0x1A86, 0x10C4, 0x0403, 0x303A}


def _crc16(data):
    """CCITT-FALSE, matching usbCrc16() in the firmware.

    binascii.crc_hqx is the same algorithm — poly 0x1021, MSB first, no
    reflection — implemented in C. Seeded with 0xFFFF it agrees with the
    firmware bit for bit, verified against the 0x29B1 test vector and against
    the Python loop it replaces on a full frame. That loop cost 4 ms a frame;
    this costs 0.018 ms.
    """
    return binascii.crc_hqx(data, 0xFFFF)


def candidates():
    """Serial ports that could plausibly be a Cadence board."""
    out = []
    for p in serial.tools.list_ports.comports():
        if p.vid is None or p.vid in KNOWN_VIDS:
            out.append(p)
    return out


def _open_port(port, baud, timeout=1.0):
    """Open without asserting the auto-reset lines, where the driver allows it.

    DTR and RTS are wired to EN and IO0 on these boards, so a plain open can
    reset the chip. Setting both false before opening avoids it on most Windows
    drivers; where it does not, the board reboots in about a second and the
    caller's retry covers it. Worth the effort because the *hub* is on a port of
    the same description, and resetting that one during a session would throw
    away its state.
    """
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = timeout
    s.dtr = False
    s.rts = False
    s.open()
    try:
        # Windows only, and it matters: the board pushes ~210 KB/s and the
        # default driver buffer is a few KB. Overrunning it does not drop whole
        # frames, it punches holes in them, and a frame with a hole fails CRC.
        s.set_buffer_size(rx_size=1 << 20)
    except Exception:
        pass
    return s


def probe(port, baud=BAUDS[0], settle=0.4):
    """Ask a port whether it is the camera. Returns the #PONG line, or None."""
    try:
        s = _open_port(port, baud, timeout=0.6)
    except (serial.SerialException, OSError):
        return None
    try:
        time.sleep(settle)
        s.reset_input_buffer()
        s.write(b"P\n")
        s.flush()
        deadline = time.time() + 1.5
        while time.time() < deadline:
            line = s.readline()
            if line.startswith(b"#PONG"):
                return line.decode("ascii", "replace").strip()
        return None
    except (serial.SerialException, OSError):
        return None
    finally:
        try:
            s.close()
        except Exception:
            pass


def remembered():
    try:
        with open(cfg.PORTFILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def remember(port_info, baud):
    """Store the USB serial number, not the COM name.

    COM numbers are assigned by Windows and move when a board is plugged into a
    different socket; the serial number in the bridge chip does not. Storing the
    name would mean re-probing — and therefore possibly resetting the hub —
    every time the laptop was unplugged.
    """
    try:
        with open(cfg.PORTFILE, "w") as f:
            json.dump({"serial_number": port_info.serial_number,
                       "device": port_info.device, "baud": baud}, f, indent=2)
    except OSError:
        pass


def find_camera(preferred=None):
    """Locate the camera board. Returns (device, baud) or (None, None).

    Tries, in order: an explicitly named port, the one remembered from last
    time, then everything else. The ordering is what keeps this from poking the
    hub on every start.
    """
    ports = candidates()
    by_serial = {p.serial_number: p for p in ports if p.serial_number}

    order = []
    if preferred:
        order += [p for p in ports if p.device.upper() == preferred.upper()]
    mem = remembered()
    if mem and mem.get("serial_number") in by_serial:
        order.append(by_serial[mem["serial_number"]])
    order += [p for p in ports if p not in order]

    for p in order:
        bauds = BAUDS if not (mem and mem.get("baud")) else \
            (mem["baud"],) + tuple(b for b in BAUDS if b != mem["baud"])
        for baud in bauds:
            pong = probe(p.device, baud)
            if pong:
                cfg.log(f"camera found on {p.device} at {baud} baud — {pong}")
                remember(p, baud)
                return p.device, baud
    return None, None


class SerialStream:
    """The cable, wearing capture.Stream's interface."""

    def __init__(self, port=None, baud=None, on_status=None):
        self.port = port
        self.baud = baud
        self.ser = None
        self.connected = False
        self.buf = bytearray()
        self.dropped = 0          # frames that failed CRC or were resynced past
        self.last_seq = None
        self.gaps = 0             # sequence discontinuities: frames lost in transit
        self.skipped = 0          # whole frames dropped as stale, by design
        # Session state as the board last reported it. Set from #SESSION lines,
        # which the camera emits when ESP-NOW brings it a change from the hub.
        # This is how the physical timer button reaches the app with no network
        # anywhere in the picture.
        self.session_state = None
        self._on_status = on_status or (lambda s: None)

    # ── lifecycle ────────────────────────────────────────────────────────────
    def open(self, stop=None):
        delay, attempt = 2, 0
        while stop is None or not stop.is_set():
            attempt += 1
            if not self.port:
                self._on_status("looking for the camera")
                self.port, self.baud = find_camera()
            if self.port:
                try:
                    self.ser = _open_port(self.port, self.baud or BAUDS[0], timeout=1.0)
                    time.sleep(0.3)
                    self.ser.reset_input_buffer()
                    self.ser.write(b"C1\n")     # start streaming
                    self.ser.flush()
                    self.connected = True
                    self.buf.clear()
                    self._on_status(f"connected on {self.port}")
                    cfg.log(f"USB stream open on {self.port} at {self.baud} baud")
                    return True
                except (serial.SerialException, OSError) as e:
                    cfg.log(f"cannot open {self.port}: {e}")
                    self.port = None            # re-probe next time round

            self._on_status(f"no camera on USB (attempt {attempt})")
            if stop is not None:
                if stop.wait(delay):
                    break
            else:
                time.sleep(delay)
            delay = min(delay * 2, 15)
        return False

    def release(self):
        if self.ser is not None:
            try:
                self.ser.write(b"C0\n")         # stop the firehose before leaving
                self.ser.flush()
            except Exception:
                pass
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.connected = False

    def reopen(self, stop=None):
        cfg.log("USB stream read failed; reopening")
        self._on_status("reconnecting")
        self.release()
        return self.open(stop)

    # ── settings ─────────────────────────────────────────────────────────────
    def set(self, params):
        """Camera settings over the same cable. Same key names as /set."""
        if self.ser is None:
            return False, "not open"
        query = "&".join(f"{k}={v}" for k, v in params.items())
        try:
            self.ser.write(("S" + query + "\n").encode())
            self.ser.flush()
            return True, query
        except (serial.SerialException, OSError) as e:
            return False, str(e)

    # ── reading ──────────────────────────────────────────────────────────────
    def read(self, timeout=5.0):
        """The newest complete frame, or (False, None).

        Drains everything waiting and decodes only the LAST whole frame in it,
        discarding the rest. That is not an optimisation, it is the difference
        between working and not: the board streams ~20 fps of 10 KB frames,
        210 KB/s, and does not wait to be asked. Decoding every one of them is
        slower than they arrive, the driver buffer overruns, and the frames that
        do arrive have holes in them and fail CRC — which is what 0.3 fps looked
        like when the cable was measured doing 19.6.

        Dropping the backlog is also correct on the merits. A 2-second voting
        window fed a frame from four seconds ago attributes attention to the
        wrong window; the same reasoning put CAMERA_GRAB_LATEST in the firmware
        and BUFFERSIZE 1 on the Wi-Fi path.
        """
        if self.ser is None:
            return False, None
        deadline = time.time() + timeout

        while time.time() < deadline:
            n = self.ser.in_waiting
            try:
                chunk = self.ser.read(n if n else 1)
            except (serial.SerialException, OSError):
                self.connected = False
                return False, None
            if chunk:
                self.buf.extend(chunk)

            # Walk every complete frame in the buffer, keeping the last.
            best = None
            i = self.buf.find(MAGIC)
            while i >= 0 and len(self.buf) - i >= HEADER:
                length = int.from_bytes(self.buf[i + 4:i + 8], "little")
                if not (1000 <= length <= 200000):
                    i = self.buf.find(MAGIC, i + 4)      # bogus header, step on
                    continue
                end = i + HEADER + length
                if len(self.buf) < end:
                    break                                # still arriving
                best = (i, end, length)
                i = self.buf.find(MAGIC, end)

            if best is None:
                # No whole frame yet. Keep the buffer bounded; a partial frame
                # is at most ~200 KB, and anything older than that is unusable.
                if len(self.buf) > 400000:
                    cut = self.buf.rfind(MAGIC)
                    del self.buf[:cut if cut > 0 else len(self.buf) - 3]
                continue

            i, end, length = best
            head = bytes(self.buf[:i])          # everything we are about to drop
            payload = bytes(self.buf[i + HEADER:end])
            crc = int.from_bytes(self.buf[i + 8:i + 10], "little")
            seq = int.from_bytes(self.buf[i + 10:i + 12], "little")
            skipped = head.count(MAGIC)
            del self.buf[:end]

            # Session announcements can be anywhere in the bytes being dropped,
            # and a button press must not be thrown away with a stale frame.
            self._scan_text(head)

            if _crc16(payload) != crc:
                self.dropped += 1
                continue

            if self.last_seq is not None:
                step = (seq - self.last_seq) & 0xFFFF
                if step != 1:
                    self.gaps += step - 1
            self.last_seq = seq
            self.skipped += skipped

            frame = cv2.imdecode(np.frombuffer(payload, np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                self.dropped += 1
                continue
            return True, frame

        return False, None

    def _scan_text(self, blob):
        """Pull #SESSION out of a stretch of bytes we are discarding."""
        at = blob.find(b"#SESSION")
        while at >= 0:
            nl = blob.find(b"\n", at)
            if nl < 0:
                break
            parts = blob[at:nl].decode("ascii", "replace").split()
            if len(parts) > 1:
                if parts[1].strip() != self.session_state:
                    cfg.log(f"hub says: session {parts[1].strip()}")
                self.session_state = parts[1].strip()
            at = blob.find(b"#SESSION", nl)
