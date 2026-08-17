# Cadence — headless vision host

Runs the attention classifier without a browser, so the hub's focus-timer
button starts tracking on its own.

## Why this exists

The camera board is an ESP32-S3. It serves an MJPEG stream and a web page; it
cannot run the inference. So the vision tier has always needed a host somewhere
else, and until now that host was the page at `http://cadence-cam.local/vision`
— which meant a browser tab had to be open before the button on the desk could
record anything. Pressing a button and having nothing happen because a tab was
closed is a chore, not a design.

This host does the same job as a background process.

```
[timer button] -> hub -> GET /session?state=running -> camera board
                                                          |
                            cadence_vision.py  <-  polls /session
                                    |
                    MediaPipe on the desktop, no browser
                                    |
                        POST /focus -> camera relay -> Vercel -> Neon
```

It still posts **through the camera board's relay** rather than straight to the
backend. The API token lives in firmware and the relay exists to keep it there;
this host never sees it.

## Setup

Needs Python 3 and two downloads.

```bash
pip install mediapipe
```

Then put the Face Landmarker model beside the script as `face_landmarker.task`
— the float16/1 build from Google's `mediapipe-models` bucket. The browser page
fetched it from a CDN at runtime; a local host needs it on disk.

Calibrate once, sitting the way you normally sit:

```bash
python cadence_vision.py --calibrate
```

That averages your pose over six seconds and writes `calibration.json`. Without
it a screen below eye level reads as a permanent downward look and everything
comes back `distracted`. Averaging rather than sampling one frame is deliberate:
one frame catches whatever micro-movement you happened to be making, and the
baseline it produces is then wrong for every session after.

Run it:

```bash
python cadence_vision.py
```

It sits idle until the hub reports a running session, then records. Press the
timer button and the hub's pill goes to `tracking`.

## Flags

| Flag | Effect |
|---|---|
| `--host` | camera board hostname or IP. Default `cadence-cam.local`; set an IP if mDNS does not resolve |
| `--calibrate` | measure and store the centre baseline, then exit |
| `--free` | record continuously, ignoring the hub's session state |
| `--verbose` | print every frame's classification |

## The page is now preview-only

`/vision` still shows the stream, the live classification and the landmark
overlay, and it is still the right place to eyeball whether detection is working
— but it no longer records. **Only one of the two may record.** If both did,
they would classify the same seconds and post them under timestamps a few
milliseconds apart, which the `(device_id, ts)` unique index cannot collapse, so
every attention figure would quietly double whenever a tab happened to be open.

Use `?free=1` on the page to record from there deliberately, with this host
stopped.

## Keeping the two in step

The classifier exists twice — here and in `firmware/cadence_cam/vision_page.h`
— and the thresholds define what `focused` has meant in every row already in
`focus_samples`. A drift between them does not add new data, it changes the
meaning of the old data, and nothing about the symptom would point at a constant
in a header file.

```bash
python check_parity.py
```

Run that after touching either file. It compares the thresholds, the timing, the
landmark indices, the blendshape set, and which of the two owns recording.

## Running it at login

Windows, without a console window:

```bash
pythonw cadence_vision.py --host cadence-cam.local
```

Put a shortcut to that in `shell:startup`, or register a Scheduled Task at
logon if you want it to survive a crash. Nothing in the host needs a terminal;
the printing is for when you are watching it.

## When it says nothing is there

- **`cannot open .../stream`** — board off, or mDNS not resolving. Try `--host`
  with the IP; `curl http://cadence-cam.local/status` tells you which.
- **Everything reads `absent`** — check the light. The board runs a night preset
  and the bench log calls low light "the whole battle"; below roughly 30 lux the
  sensor gives frames a face detector cannot use. The hub's `light` reading is
  on Home.
- **Everything reads `distracted`** — not calibrated, or calibrated in a
  different seat. Re-run `--calibrate`.
- **`post failed`** — the relay, not the classifier. `curl
  http://cadence-cam.local/status` and read `relay_status`.
