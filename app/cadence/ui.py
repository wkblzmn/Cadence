"""The window.

Tkinter, because it ships with Python and this app should not need an install
step on a machine that already runs the pipeline. Everything here reads a
snapshot the worker publishes and draws it; nothing here blocks, and nothing
in the worker touches a widget.

The palette is the dashboard's, from dashboard/app/globals.css, so the two
surfaces of this project do not disagree about what "focused" is coloured.
"""

import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

import cv2
from PIL import Image, ImageTk

from . import config as cfg
from . import enhance, pipeline as pipe

DARK = {
    "page": "#0d0d0d", "surface": "#1a1a19", "text": "#ffffff",
    "secondary": "#c3c2b7", "muted": "#898781", "gridline": "#2c2c2a",
    "good": "#0ca30c", "warn": "#fab219", "bad": "#d03b3b", "series": "#3987e5",
}
LIGHT = {
    "page": "#f9f9f7", "surface": "#fcfcfb", "text": "#0b0b0b",
    "secondary": "#52514e", "muted": "#898781", "gridline": "#e1e0d9",
    "good": "#0ca30c", "warn": "#fab219", "bad": "#d03b3b", "series": "#2a78d6",
}

PREVIEW_W, PREVIEW_H = 640, 480

# The five landmarks the pose actually reads. Drawing the full 478-point mesh
# looks impressive and tells you nothing about why a frame was classified the
# way it was; these are the ones the numbers come from.
OVERLAY_POINTS = [(33, "eye"), (263, "eye"), (1, "nose"), (168, "brow"), (152, "chin")]


def pick_font(candidates, size, weight="normal"):
    """First installed family from the list, else Tk's default.

    The dashboard sets its type in Old Standard and New Century; if those are
    on the machine the app matches it, and if not it falls back rather than
    rendering in whatever Tk substitutes silently.
    """
    have = set(tkfont.families())
    for name in candidates:
        if name in have:
            return (name, size, weight)
    return (tkfont.nametofont("TkDefaultFont").cget("family"), size, weight)


class App:
    def __init__(self, root, host, post=True, mode="hub", tune=True,
                 source=None, port=None):
        self.root = root
        self.host = host
        self.post = post
        self.tune = tune
        self.source = source
        self.port = port
        self.c = DARK if cfg.windows_dark_mode() else LIGHT
        self._photo = None
        self._frame_item = None
        self._frame_seq = -1
        self._tl_rev, self._tl_width = -1, -1
        self._calibrating = False
        self._closing = False

        root.title("Cadence")
        root.configure(bg=self.c["page"])
        root.minsize(1040, 560)
        root.protocol("WM_DELETE_WINDOW", self.close)

        self.f_display = pick_font(["Old Standard TT", "New Century Schoolbook",
                                    "Century Schoolbook", "Georgia"], 40)
        self.f_head    = pick_font(["Old Standard TT", "Georgia"], 13)
        self.f_body    = pick_font(["Segoe UI", "Helvetica"], 10)
        self.f_mono    = pick_font(["Cascadia Mono", "Consolas", "Courier New"], 9)

        self.pipeline = pipe.Pipeline(host, post=post, mode=mode, tune=tune,
                                      source=source, port=port)
        self._build()
        self.pipeline.start()
        self.root.after(50, self._tick)

    # ── layout ───────────────────────────────────────────────────────────────
    def _build(self):
        c = self.c
        outer = tk.Frame(self.root, bg=c["page"], padx=16, pady=16)
        outer.pack(fill="both", expand=True)

        # left: the picture
        left = tk.Frame(outer, bg=c["page"])
        left.pack(side="left", fill="both")

        self.canvas = tk.Canvas(left, width=PREVIEW_W, height=PREVIEW_H,
                                bg=c["surface"], highlightthickness=1,
                                highlightbackground=c["gridline"])
        self.canvas.pack()
        self.canvas.create_text(PREVIEW_W // 2, PREVIEW_H // 2, text="waiting for the board",
                                fill=c["muted"], font=self.f_body, tags="placeholder")

        self.input_line = tk.Label(left, text="", bg=c["page"], fg=c["muted"],
                                   font=self.f_mono, anchor="w")
        self.input_line.pack(fill="x", pady=(8, 0))

        # right: the readout
        right = tk.Frame(outer, bg=c["page"], padx=20)
        right.pack(side="left", fill="both", expand=True)

        self.state_label = tk.Label(right, text="—", bg=c["page"], fg=c["muted"],
                                    font=self.f_display, anchor="w")
        self.state_label.pack(fill="x")

        self.conf_label = tk.Label(right, text="", bg=c["page"], fg=c["secondary"],
                                   font=self.f_body, anchor="w")
        self.conf_label.pack(fill="x", pady=(0, 14))

        self._rule(right)

        self.session_label = tk.Label(right, text="no session", bg=c["page"],
                                      fg=c["text"], font=self.f_head, anchor="w")
        self.session_label.pack(fill="x", pady=(12, 2))

        self.split_label = tk.Label(right, text="", bg=c["page"], fg=c["secondary"],
                                    font=self.f_body, anchor="w")
        self.split_label.pack(fill="x", pady=(0, 8))

        self.timeline = tk.Canvas(right, height=26, bg=c["surface"],
                                  highlightthickness=1, highlightbackground=c["gridline"])
        self.timeline.pack(fill="x", pady=(0, 14))

        self._rule(right)

        self.status_label = tk.Label(right, text="", bg=c["page"], fg=c["secondary"],
                                     font=self.f_mono, anchor="w", justify="left")
        self.status_label.pack(fill="x", pady=(12, 12))

        # controls
        controls = tk.Frame(right, bg=c["page"])
        controls.pack(fill="x", pady=(4, 0))

        self.mode_var = tk.StringVar(value=self.pipeline.mode)
        for label, val in (("Follow the hub button", "hub"),
                           ("Start here", "local")):
            tk.Radiobutton(controls, text=label, value=val, variable=self.mode_var,
                           command=self._mode_changed, bg=c["page"], fg=c["secondary"],
                           selectcolor=c["surface"], activebackground=c["page"],
                           activeforeground=c["text"], font=self.f_body,
                           highlightthickness=0, bd=0, anchor="w").pack(fill="x")

        buttons = tk.Frame(right, bg=c["page"])
        buttons.pack(fill="x", pady=(10, 0))

        self.start_btn = tk.Button(buttons, text="Start session", command=self._toggle,
                                   font=self.f_body, bd=0, padx=14, pady=7,
                                   bg=c["series"], fg="#ffffff",
                                   activebackground=c["series"], state="disabled")
        self.start_btn.pack(side="left")

        self.calib_btn = tk.Button(buttons, text="Calibrate", command=self._calibrate,
                                   font=self.f_body, bd=0, padx=14, pady=7,
                                   bg=c["surface"], fg=c["text"],
                                   activebackground=c["gridline"])
        self.calib_btn.pack(side="left", padx=(8, 0))

        self.view_var = tk.BooleanVar(value=True)
        tk.Checkbutton(buttons, text="show what the model sees", variable=self.view_var,
                       command=self._view_changed, bg=c["page"], fg=c["muted"],
                       selectcolor=c["surface"], activebackground=c["page"],
                       activeforeground=c["text"], font=self.f_body,
                       highlightthickness=0, bd=0).pack(side="left", padx=(12, 0))

        self.note = tk.Label(right, text="", bg=c["page"], fg=c["warn"],
                             font=self.f_body, anchor="w", wraplength=340, justify="left")
        self.note.pack(fill="x", pady=(12, 0))

    def _rule(self, parent):
        tk.Frame(parent, height=1, bg=self.c["gridline"]).pack(fill="x")

    # ── controls ─────────────────────────────────────────────────────────────
    def _mode_changed(self):
        self.pipeline.set_mode(self.mode_var.get())
        local = self.mode_var.get() == "local"
        self.start_btn.configure(state="normal" if local else "disabled",
                                 text="Start session")

    def _view_changed(self):
        self.pipeline.show_enhanced = self.view_var.get()

    def _toggle(self):
        running = self.pipeline.toggle_local()
        self.start_btn.configure(text="Stop session" if running else "Start session")

    def _calibrate(self):
        """Stop the pipeline, measure, start it again.

        The board serves one stream client at a time, so calibrating while the
        worker holds the stream would have the two fighting over it. Stopping
        first is not politeness, it is the only way both get frames.
        """
        if self._calibrating:
            return
        self._calibrating = True
        self.calib_btn.configure(state="disabled", text="Calibrating…")
        self.note.configure(text="Sit the way you normally sit and look at the screen.",
                            fg=self.c["series"])

        def work():
            error = None
            try:
                self.pipeline.stop()
                pipe.calibrate(self.host, source=self.source)
            except Exception as e:
                error = str(e)
            finally:
                self.pipeline = pipe.Pipeline(self.host, post=self.post,
                                              mode=self.mode_var.get(), tune=self.tune,
                                              source=self.source, port=self.port)
                self.pipeline.show_enhanced = self.view_var.get()
                self.pipeline.start()
            self.root.after(0, lambda: self._calibrated(error))

        threading.Thread(target=work, daemon=True).start()

    def _calibrated(self, error):
        self._calibrating = False
        self.calib_btn.configure(state="normal", text="Calibrate")
        if error:
            self.note.configure(text=f"Calibration failed — {error}", fg=self.c["bad"])
        else:
            self.note.configure(text="Calibrated.", fg=self.c["good"])

    # ── the repaint ──────────────────────────────────────────────────────────
    def _tick(self):
        if self._closing:
            return                        # the root is gone; do not reschedule
        try:
            self._draw(self.pipeline.snapshot())
        except Exception as e:            # a repaint must never kill the window
            cfg.log(f"repaint error: {type(e).__name__}: {e}")
        self.root.after(50, self._tick)

    def _draw(self, s):
        c = self.c
        colour = {"focused": c["good"], "distracted": c["bad"]}.get(s["state"], c["muted"])

        self._draw_frame(s)

        if s["recording"]:
            self.state_label.configure(text=s["state"].upper(), fg=colour)
            self.conf_label.configure(text=f"confidence {s['conf']:.2f}")
        else:
            # Idle still classifies, but says so — a live readout nobody is
            # recording must not look like a running session.
            self.state_label.configure(text=s["state"].upper(), fg=c["muted"])
            self.conf_label.configure(text="not recording — preview only")

        self._draw_session(s)
        self._draw_timeline(s)

        self.input_line.configure(text=(
            f"{s['fps']:.1f} fps   "
            f"luma {('--' if s['luma'] is None else format(s['luma'], '.0f')):>3}   "
            f"enhance {s['enhance']:.0%}   "
            f"misses {s['miss_rate']:.0%}   "
            f"board {s['board_tuning']}"))

        queue = f"{s['queued']} queued" if s["queued"] else "queue clear"
        self.status_label.configure(text=(
            f"stream   {s['stream_status']}\n"
            f"relay    {s['relay_status']} · {s['posted']} posted · {queue}\n"
            f"board    session {s['board_state']}"))

        self._draw_note(s)

    def _draw_frame(self, s):
        """Repaint the preview, but only when there is a new frame to paint.

        Three things here were the app being laggy, and all three were this
        method doing work the timer asked for rather than work the data
        justified:

        * create_image() ran every tick and nothing ever removed the previous
          item, so the canvas accumulated one stacked image every 50 ms — 72,000
          of them in an hour, every one of which Tk composites on every redraw.
          The item is created once now and re-pointed after that.

        * A new PhotoImage was allocated per repaint. paste() writes into the
          buffer Tk already holds instead.

        * The timer runs at 20 Hz and the board delivers about 10 fps, so half
          of all this was rebuilding an image identical to the one on screen.
        """
        frame = s["frame"]
        if frame is None or s["frame_seq"] == self._frame_seq:
            return
        self._frame_seq = s["frame_seq"]
        self.canvas.delete("placeholder")

        lm = s["landmarks"]
        if lm is not None:
            # Only copy when something is about to be drawn on it. The array
            # belongs to the worker and must not be annotated in place.
            shown = frame.copy()
            h, w = shown.shape[:2]
            colour = (12, 163, 12) if s["state"] == "focused" else (59, 59, 208)
            for idx, _ in OVERLAY_POINTS:
                p = lm[idx]
                cv2.circle(shown, (int(p.x * w), int(p.y * h)), 3, colour, -1)
            # The eye line, which is the span every pose number is divided by.
            l, r = lm[33], lm[263]
            cv2.line(shown, (int(l.x * w), int(l.y * h)),
                     (int(r.x * w), int(r.y * h)), colour, 1)
        else:
            shown = frame

        rgb = cv2.cvtColor(shown, cv2.COLOR_BGR2RGB)
        img = Image.fromarray(rgb)
        if img.size != (PREVIEW_W, PREVIEW_H):     # VGA already is; do not resample
            img = img.resize((PREVIEW_W, PREVIEW_H), Image.BILINEAR)

        if self._photo is None or self._photo.width() != img.width \
                or self._photo.height() != img.height:
            self._photo = ImageTk.PhotoImage(img)  # a reference, or Tk drops it
            if self._frame_item is None:
                self._frame_item = self.canvas.create_image(
                    0, 0, anchor="nw", image=self._photo, tags="frame")
                self.canvas.tag_lower("frame")
            else:
                self.canvas.itemconfigure(self._frame_item, image=self._photo)
        else:
            self._photo.paste(img)

    def _draw_session(self, s):
        counts = s["counts"]
        total = sum(counts.values())
        if not s["recording"] and total == 0:
            self.session_label.configure(text="no session")
            self.split_label.configure(text="")
            return

        elapsed = int(time.time() - s["session_start"]) if s["session_start"] else 0
        mins, secs = divmod(elapsed, 60)
        self.session_label.configure(text=f"{mins:d}:{secs:02d}")

        if total:
            pct = counts.get("focused", 0) / total
            self.split_label.configure(
                text=(f"{pct:.0%} focused  ·  {counts.get('focused', 0)} focused, "
                      f"{counts.get('distracted', 0)} distracted, "
                      f"{counts.get('absent', 0)} absent"))
        else:
            self.split_label.configure(text="waiting for the first window")

    def _draw_timeline(self, s):
        """One bar per posted window, oldest at the left.

        Redrawn when the data or the width changes, and not otherwise. This
        rebuilt every rectangle on every tick — with a 900-window session in a
        strip 340 pixels wide, that was eighteen thousand canvas items a second
        to draw a picture that changes once every two seconds.

        It is also capped at one bar per pixel. Beyond that the extra
        rectangles are narrower than the screen can show, so they cost time to
        produce something nobody can see.
        """
        c = self.c
        width = self.timeline.winfo_width() or 340
        if s["timeline_rev"] == self._tl_rev and width == self._tl_width:
            return
        self._tl_rev, self._tl_width = s["timeline_rev"], width

        self.timeline.delete("all")
        tl = self.pipeline.timeline_list()
        if not tl:
            return

        if len(tl) > width:
            # Nearest-neighbour down to one bar per pixel. A majority vote per
            # bucket would be defensible too, but this strip is a glance at the
            # shape of a session, not a readout, and sampling keeps the runs of
            # distraction visible rather than averaging them away.
            step = len(tl) / width
            tl = [tl[int(i * step)] for i in range(width)]

        bar = width / len(tl)
        for i, state in enumerate(tl):
            fill = {"focused": c["good"], "distracted": c["bad"]}.get(state, c["gridline"])
            self.timeline.create_rectangle(i * bar, 0, (i + 1) * bar, 26,
                                           fill=fill, outline="")

    def _draw_note(self, s):
        """One line, for the thing most worth saying right now."""
        c = self.c
        if self._calibrating:
            return
        if not s["calibrated"]:
            self.note.configure(
                text="Not calibrated — everything will read distracted. "
                     "Press Calibrate.", fg=c["bad"])
        elif s["stale_calibration"]:
            self.note.configure(
                text="Calibration is over two weeks old. If the chair has moved, "
                     "recalibrate.", fg=c["warn"])
        elif s["luma"] is not None and s["luma"] < enhance.VERY_DARK and s["miss_rate"] > 0.4:
            self.note.configure(
                text="Too dark to find a face reliably, even with the sensor wide "
                     "open. More light is the only fix left.", fg=c["warn"])
        elif s["relay_status"] == "failing":
            self.note.configure(
                text="Samples are queuing — the board is not relaying. "
                     "Check its /status.", fg=c["warn"])
        else:
            self.note.configure(text="")

    # ── shutdown ─────────────────────────────────────────────────────────────
    def close(self):
        """Stop the worker before tearing the window down.

        The order is the point: the pipeline's shutdown path flushes the
        partial window and the queued batch, and doing that after destroy()
        would mean doing it with no way to report that it failed.
        """
        self._closing = True
        self.note.configure(text="Flushing the queue…", fg=self.c["secondary"])
        self.root.update_idletasks()
        self.pipeline.stop()
        self.root.destroy()


def run(host, post=True, mode="hub", tune=True, source=None, port=None):
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    App(root, host, post=post, mode=mode, tune=tune, source=source, port=port)
    root.mainloop()
