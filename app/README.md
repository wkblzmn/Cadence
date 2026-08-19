# Cadence — the app

The whole analysis pipeline, on the PC, in a window that tells you what it
thinks you are doing.

```
                        USB cable (default)                 no network at all
  camera board ────────────────────────────────────────────────────┐
       │                                                           │
       │  ...or MJPEG over WiFi, if a network happens to be there   │
       ▼                                                           ▼
  ┌────────────────────────────────────────────────────────────────────┐
  │  Cadence (this app)                                                │
  │    measure light -> enhance -> MediaPipe -> classify -> vote       │
  │    live readout in the window, one sample every 2s to the spool    │
  └────────────────────────────────────────────────────────────────────┘
                                     │
              board relay -> Vercel -> Neon   (when the board is on a network)
              or straight to Vercel           (when it is not)
              or nowhere, and the spool keeps it until something changes
```

The board is a camera. It carries frames one way and makes no decisions about
attention.

## How the frames get here

**Over USB, by default.** Measured from the PC over a single 40-second capture,
the wireless path delivered:

| | |
|---|---|
| minimum inter-frame gap | **98 ms** — the sensor can do ~10 fps |
| median | 615 ms |
| 95th percentile | 2671 ms |
| worst | 6331 ms |
| gaps under 150 ms | 2% |
| carrying | 0.12 Mbps at 9 KB/frame |

A stream limited by exposure clusters tightly around one gap. That spread is
the transport. Confirmed three ways: a raw HTTP read with no OpenCV and no JPEG
decode saw the same 1.7 fps, the board's own `frames_sent` counter agreed, and
WiFi power-save was already disabled. Every exposure and quality setting tried
landed between 0.6 and 1.8 fps.

The second reason matters more than the first. This has to be demonstrated on a
network nobody controls, and the wireless path cannot survive that trip:
credentials are compiled into `secrets.h` with no provisioning path,
WPA2-Enterprise cannot be joined by `WiFi.begin(ssid, pass)` at all, a captive
portal cannot be answered by a device with no browser, and client isolation
would hide the board from the laptop even after a successful join. Any one of
those ends the demonstration. A cable has none of them.

The protocol is in `firmware/cadence_cam/usb_stream.h`: a 12-byte header of
magic, length, CRC16 and sequence, then the JPEG. The same UART carries the boot
log, so the PC resynchronises on the magic and checks length and CRC before
decoding — a header that appears inside a log line dies at one of those gates.
At 2 Mbaud a 9 KB frame takes ~45 ms on the wire, so the cable ceiling is about
20 fps.

`--source wifi` still works wherever the network does.

## Running it

```bash
python -m cadence
```

Or double-click `cadence.pyw`, which runs the same thing without a console.

Calibrate first — the button is in the window, or:

```bash
python -m cadence --calibrate
```

| Flag | Effect |
|---|---|
| `--host` | camera board hostname or IP. Default `cadence-cam.local` |
| `--local` | start in local mode: the app owns the session, not the hub's button |
| `--calibrate` | measure the baseline on the console and exit |
| `--no-post` | classify and display, write nothing to the backend |
| `--source` | `usb` (default) or `wifi` |
| `--port` | serial port for USB, e.g. `COM4`. Auto-detected and remembered otherwise |
| `--hub` | follow the hub's timer button even on USB (needs both on a network) |
| `--no-tune` | leave the board's camera settings exactly as they are |

## Two ways a session starts, and why they are separate

**Follow the hub button** is the default. The app polls `GET /session` on the
board and records while it says `running`.

**Start here** is the app's own button, and it does *not* write
`/session?state=running` back to the board. The hub re-pushes the board's state
every 30 seconds (`CAM_HEARTBEAT_MS`), so anything the app wrote there would be
overwritten within half a minute and the recording would stop by itself. A
local session is held in the app, where nothing else can clear it.

## Low light

This is the case the desk is actually in, so it gets worked at from both ends.

**On the board**, the app asserts an analysis preset at startup rather than
inheriting whatever the camera was left in: `fast=1`, `q=8`, `gma=1`, and
`denoise=0` — the detail the sensor's denoiser removes is the eye and mouth
structure the landmarker reads.

`fast=1` is the important one. It buys brightness with *gain* rather than
exposure *time*, which is the right trade when the PC can denoise better than
the sensor can. Two earlier choices here were wrong and only measurement showed
it: `night=1` applied unconditionally is a manual-exposure preset built for
~30 lux, and in a lit room it produced **luma 226 out of 255 — a white frame —
at 0.6 fps**, against luma 62 at 1.8 fps with the auto loops running. And `q=4`
assumed spare bandwidth, spending the scarce resource (frame rate) to fix the
abundant one (0.12 Mbps of link). `night=1` is now the last rung of the ladder
rather than the starting point.

Resolution stays at VGA and the app checks it from the decoded pixels. Not a
preference: QVGA and CIF measured **zero frames** on this sensor, and XGA is
just more bytes for a model that downscales to a few hundred pixels square.

**On the PC**, every frame is measured and then given only the work its
darkness justifies — bilateral denoise, CLAHE on the L channel, then gamma, in
that order. Denoising last would smooth away the structure CLAHE had just
raised. A dark VGA frame costs about 10 ms of the 125 ms available at 8 Hz; a
well-lit frame costs 0.06 ms, because the ramp returns the original array
untouched.

**If both fail**, the app climbs an exposure ladder on the board — but only
when frames are dark *and* faces are actually being missed. A dim frame the
landmarker can still read is not worth spending frame rate on. It climbs and
never descends, because dropping back would oscillate against the condition it
is correcting.

`show what the model sees` in the window switches the preview between the
enhanced frame and the raw one.

## What changed from the headless host

Three defects, two of which redefine the data:

**The gaze term never fired.** The old classifier took `max()` over six mixed
ARKit blendshapes. Those are not zero at a neutral gaze, so the measured
baseline was 0.418, and after subtracting it a limit of 0.35 needed a raw score
above 0.77 to trip. A real glance down at a phone measures about 0.72 — under
the line. The classifier was head-pose only, missing the exact case its own
comments said mattered most. Gaze is now a signed `(horizontal, vertical)`
vector centred on zero, and includes look-up, which the old shape set omitted
entirely.

**Confidence was inverted.** It fell as you turned further away, so the least
ambiguous `distracted` samples reached the database carrying the lowest
confidence on the scale. It is now distance from the boundary, on whichever
side you are.

**Batches larger than eight samples could never be delivered.** The old host
posted up to 100 at a time; the firmware's `relayPush` rejects a body at or
over 640 bytes, which 100 samples exceed elevenfold. Normal timing sends five
per drain and squeaks under, so it worked — until a hiccup let the queue reach
nine, after which every retry failed identically and the queue could not drain
again. Batches are now measured in bytes against the firmware's real limit.

Detection also runs in MediaPipe's VIDEO mode rather than IMAGE, so tracking
carries between frames instead of every frame starting cold.

**Rows in `focus_samples` written before 2026-08-18 came from the old
definitions and are not directly comparable to anything written after.**

## Calibration

`calibration.json` is schema 2 and a schema-1 file is refused rather than
migrated — its `gaze` was a max() over mixed blendshapes and the new one is a
vector magnitude, so carrying the number forward would look calibrated while
feeding the classifier a baseline for a different quantity.

The run also checks its yield, not just its count. The file this replaces was
built from 8 usable frames and passed a guard that only rejected below 5; a
baseline that thin then silently defined "centre" for every session after it.

## When it says nothing is there

- **`waiting for cadence-cam.local`** — board off, or mDNS not resolving. Pass
  `--host` with the IP; `curl http://cadence-cam.local/status` says which.
- **Everything reads `distracted`** — not calibrated, or calibrated in a
  different seat. The window says so in orange. Recalibrate.
- **`Too dark to find a face reliably`** — the sensor is wide open and the PC
  side has done what it can. More light is the only fix left.
- **Samples queuing** — the relay, not the classifier.
  `curl http://cadence-cam.local/status` and read `relay_status`.

## Flashing the camera board

The USB transport needs firmware the board does not have yet. In the Arduino
IDE, open `firmware/cadence_cam/cadence_cam.ino` and set:

- **Board**: ESP32S3 Dev Module
- **PSRAM**: OPI PSRAM  — the N16R8 will not boot without it
- **Flash Size**: 16MB
- **Partition Scheme**: 16M Flash (3MB APP / 9.9MB FATFS)
- **Port**: the camera's COM port. If both boards are plugged in, the app
  identifies the camera by USB serial number — run `python -m cadence` once and
  read `serial_port.json`, or unplug the hub while flashing.

Verified to compile against esp32 core 3.3.11: 36% of program storage, 20% of
dynamic memory.

Three things change in that sketch. Frames now go out over the UART at 2 Mbaud
on request; the serial link is fast enough to carry them. And the Wi-Fi failure
path no longer ends in `while (true) delay(1000)` — it used to halt forever,
which was defensible when Wi-Fi was the only way to reach the board, and is not
now that the cable works without any network. Halting there would have stranded
the board before it ever reached the streaming task.

Streaming is off at boot; the app sends `C1` to start it. That keeps the log
readable and means reflashing does not have to fight a binary firehose for the
same UART.

## Posting when there is no network

Samples go through the board's relay whenever the board answers, because that
path keeps the API token in firmware where it was designed to live. On a cable
with no network there is no relay, so the app posts straight to the backend
using `backend.json` — copy `backend.json.example` and fill it in. It is
gitignored.

That does put the token on the PC. The original argument for the relay was
about a *web page served to the LAN*, which cannot keep a secret from anyone who
views its source; it does not carry over to a desktop app reading a file on the
owner's own machine.

If neither route works, nothing is lost: the queue is spooled to `spool.json`
and recovered on the next start, so a presentation with no internet at all still
records a full session and uploads it later.

## Setup

```bash
pip install mediapipe opencv-python pillow requests numpy pyserial
```

Then put the Face Landmarker model beside `cadence.pyw` as
`face_landmarker.task` — the float16/1 build from Google's `mediapipe-models`
bucket. It is 3.7 MB of redistributable binary and is not in the repo.

`start-at-login.vbs` runs it at sign-in; copy it into `shell:startup`.
