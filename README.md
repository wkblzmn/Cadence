# Cadence

A desk instrument that answers two questions a stopwatch cannot: **how long you
actually worked**, and **whether you were paying attention while you did it**.

One hardware button starts a session. A camera watches the desk, a computer-vision
pipeline on the PC decides focused / distracted / absent twice a second, and a web
dashboard folds it all into daily totals, longest unbroken stretch, and attention
quality.

```
   ┌──────────────┐         ESP-NOW          ┌──────────────┐
   │  cadence-hub │◄────── session state ───►│ cadence-cam  │
   │              │        camera health     │              │
   │  display     │                          │  OV3660      │
   │  encoder     │   no AP · no DHCP · no    │  ESP32-S3    │
   │  timer button│   SSID · no router        │              │
   └──────┬───────┘                          └──────┬───────┘
          │                                         │
          │ session events                          │ JPEG frames
          │ sensor readings                         │ 2 Mbaud USB
          ▼                                         ▼
   ┌───────────────────────────────────────────────────────────┐
   │  Cadence (PC app)                                         │
   │    light → enhance → MediaPipe → classify → majority vote │
   │    live window · one sample / 2 s · disk-spooled queue     │
   └────────────────────────────┬──────────────────────────────┘
                                ▼
                    Vercel  →  Neon Postgres  →  dashboard
```

---

## The three tiers

### 1. Sensing — two ESP32-S3 boards

**`cadence-hub`** carries a 320×240 display, a rotary encoder, a focus-timer
button, a BME/BMP280 and a BH1750. It runs the timer state machine, shows pages
(status, environment, tasks), pulls the task list from the dashboard, and posts
session events and sensor readings.

It **emits events and never accumulates totals**. Every figure in the system is
folded from an event log by SQL, so a device reboot cannot corrupt a total that
was only ever derived.

**`cadence-cam`** is an OV3660 on an ESP32-S3. It serves frames and nothing else
— it makes no decisions about attention.

### 2. Analysis — [`app/`](app/), on the PC

The whole vision pipeline. Frames arrive over USB, get measured for brightness
and enhanced if dark, run through MediaPipe Face Landmarker, and are classified
from head pose and gaze. A two-second majority vote produces one sample.

See [`app/README.md`](app/README.md) for the detail.

### 3. Presentation — [`dashboard/`](dashboard/)

Next.js on Vercel, Neon Postgres. Ingest routes are authenticated and idempotent;
every fold is a SQL view. See [`db/schema.sql`](db/schema.sql).

---

## Why the camera talks over a cable

The original design streamed MJPEG over WiFi. Measured from the PC over a single
40-second capture, that path delivered:

| | |
|---|---|
| median inter-frame gap | 615 ms |
| 95th percentile | 2671 ms |
| worst | 6331 ms |
| gaps under 150 ms | 2% |
| carrying | 0.12 Mbps at 9 KB/frame |

Over USB, the same board delivers **19.6 fps at 210 KB/s**, saturating the link.
The sensor was never the limit.

But frame rate is the smaller reason. This instrument has to be demonstrated on
a network nobody controls, and the wireless path cannot survive that trip:

- WiFi credentials are compiled into `secrets.h` — no provisioning path
- WPA2-Enterprise cannot be joined by `WiFi.begin(ssid, pass)` at all
- A captive portal cannot be answered by a device with no browser
- Client isolation hides the boards from the laptop even after a successful join
- mDNS is commonly blocked on managed networks

Any one of those ends the demonstration. A cable has none of them, and the boards
were already tethered for power.

**ESP-NOW** carries what the cable cannot: hub ↔ camera. It needs no access
point, no DHCP, no SSID and no router — two ESP32s on the same radio channel is
the entire dependency. The timer button reaches the camera, and the camera's
health reaches the hub's tracker, on a network-free desk.

---

## Repository layout

| Path | What |
|---|---|
| [`app/`](app/) | The PC application — capture, enhancement, classifier, window |
| [`firmware/cadence_hub/`](firmware/cadence_hub/) | Hub: display, encoder, timer, sensors |
| [`firmware/cadence_cam/`](firmware/cadence_cam/) | Camera: sensor, MJPEG, USB transport, relay |
| [`firmware/cadence_cam/usb_stream.h`](firmware/cadence_cam/usb_stream.h) | Framed JPEG over UART |
| `firmware/*/espnow_sync.h` | Shared hub ↔ camera radio link |
| [`dashboard/`](dashboard/) | Next.js dashboard and ingest API |
| [`db/schema.sql`](db/schema.sql) | Tables, idempotency guards, folding views |
| [`hardware/`](hardware/) | OpenSCAD enclosures |
| [`docs/WIRING.md`](docs/WIRING.md) | Pinout and wiring |

---

## Getting it running

### 1. Firmware

Arduino IDE, esp32 core 3.3.11. For **both** sketches:

- Board: **ESP32S3 Dev Module**
- PSRAM: **OPI PSRAM** — the N16R8 will not boot without it
- Flash Size: **16MB**
- Partition Scheme: **16M Flash (3MB APP / 9.9MB FATFS)**

Copy `secrets.h.example` to `secrets.h` in each sketch folder and fill it in.

Flash **both** boards together — the ESP-NOW message struct must match on each
end or they will ignore one another.

### 2. The app

```bash
pip install mediapipe opencv-python pillow requests numpy pyserial
```

Put the Face Landmarker model (`float16/1`, from Google's `mediapipe-models`
bucket) in `app/` as `face_landmarker.task`. It is 3.7 MB of redistributable
binary and is not in the repo.

Calibrate once, sitting the way you normally sit:

```bash
cd app && python -m cadence --calibrate
```

Then run it:

```bash
cd app && python -m cadence --hub
```

`--hub` follows the physical timer button. Without it the app owns the session
through its own Start button.

### 3. Dashboard

```bash
cd dashboard && npm install && npm run dev
```

Apply [`db/schema.sql`](db/schema.sql) to a Neon database and set `DATABASE_URL`
and the API token in the environment.

---

## Design rules the code actually follows

**Devices emit, the database folds.** No total is ever stored on a device. Every
figure — daily seconds, longest stretch, attention percentage — is derived from
an append-only event log by a SQL view, so no reboot or dropped packet can leave
a running total silently wrong.

**Every write is idempotent.** Unique indexes on `(device_id, ts)` plus
`ON CONFLICT DO NOTHING`, because both radios drop packets and both devices
retry. A retried batch is a no-op, not a second copy.

**Nothing is lost because a link was down.** The hub queues events against uptime
and stamps real UTC once NTP lands. The app spools unsent samples to disk and
recovers them on restart. A full session recorded with no internet at all uploads
later, intact.

**One definition of "focused".** The classifier used to exist twice — once in
Python, once in firmware — kept in step by a parity checker. The PC now owns it
outright and the camera's preview page is preview-only, because two
implementations of one threshold is how the meaning of stored data drifts.

**Measure before blaming.** Several conclusions in this project were wrong until
something was actually instrumented: the camera looked transport-limited and was
not, the app looked slow and was being starved by two blocking HTTP calls on its
frame thread, and a gaze term that read plausibly in code had never once fired.
Each is documented where it was fixed.

---

## Author

Wakibul Zaman Nithor
