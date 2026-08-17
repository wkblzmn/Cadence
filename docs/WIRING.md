# Cadence — Wiring Reference

**Hub:** ESP32-S3 DevKit N8R2 (8 MB flash, 2 MB quad PSRAM)
**Last verified on hardware:** 2026-08-12

This supersedes §3 of `cadence-spec.md`, which was written for an ESP32
WROOM-32. Classic ESP32 GPIO numbering does not apply to the S3 — GPIO 22–25
do not exist on this chip and 26–32 are wired to internal flash and PSRAM.

---

## Read this before plugging anything in

Three mistakes here destroy hardware. Everything else is recoverable.

| Never | Why |
|---|---|
| **TFT VCC to 5 V** | The LockerBox panel has no regulator and no level shifter. It is a 3.3 V-only board. 5 V kills the ILI9341. |
| **Encoder `+` to 5 V** | The HW-040 pulls CLK and DT up to whatever you feed `+`. On 5 V those lines exceed the S3's absolute maximum and degrade the GPIO pads — slowly, so you notice a week later as flaky counting. |
| **Anything into GPIO 26–32** | Internal SPI flash and PSRAM. Shorting these bricks the boot. |

Everything in this build runs on **3.3 V**. There is no 5 V anywhere.

---

## Master pin map

| Signal | GPIO | Goes to |
|---|---|---|
| TFT SCK | 12 | TFT pin 7 `SCK` |
| TFT MOSI | 11 | TFT pin 6 `SDI(MOSI)` |
| TFT MISO | 13 | *unwired* — SD slot only |
| TFT CS | 10 | TFT pin 3 `CS` |
| TFT DC | 16 | TFT pin 5 `DC` |
| TFT RST | 17 | TFT pin 4 `RESET` |
| TFT backlight | 15 | TFT pin 8 `LED` |
| I²C SDA | 8 | BME280 `SDA` + BH1750 `SDA` |
| I²C SCL | 9 | BME280 `SCL` + BH1750 `SCL` |
| Encoder CLK | 4 | HW-040 `CLK` |
| Encoder DT | 5 | HW-040 `DT` |
| Encoder SW | 6 | HW-040 `SW` |
| Timer button | 7 | Omron B3F, one leg |

**Do not use:** 0 (BOOT strap) · 19, 20 (native USB) · 26–32 (flash + PSRAM) ·
43, 44 (UART0) · 45, 46 (strapping).

**Free for later:** 1, 2, 3, 14, 18, 21, 38–42, 47, 48.

---

## Component 1 — Display

**LockerBox 3.2" TFT SPI 240×320 V1.0**, ILI9341 driver.

The header has **14 pins**. Count from the end nearest the mounting hole; the
silkscreen order is fixed. Only the first 9 are used, and MISO is optional.

| # | Silkscreen | Wire to | Note |
|---|---|---|---|
| 1 | `VCC` | **3V3** | 5 V destroys it |
| 2 | `GND` | GND | |
| 3 | `CS` | GPIO 10 | |
| 4 | `RESET` | GPIO 17 | |
| 5 | `DC` | GPIO 16 | sometimes labelled `RS` or `A0` |
| 6 | `SDI(MOSI)` | GPIO 11 | |
| 7 | `SCK` | GPIO 12 | |
| 8 | `LED` | GPIO 15 | control input, not a power rail |
| 9 | `SDO(MISO)` | *leave open* | needed only for the SD slot |
| 10 | `T_CLK` | **nothing** | touch controller |
| 11 | `T_CS` | **nothing** | touch controller |
| 12 | `T_DIN` | **nothing** | touch controller |
| 13 | `T_DO` | **nothing** | touch controller |
| 14 | `T_IRQ` | **nothing** | touch controller |

**The trap:** pins 10–14 are the XPT2046 touch chip. `T_DIN` and `T_CLK` sit
right next to the real MOSI and SCK and look almost identical on the
silkscreen. Wiring those gives a completely dead screen with no error message.

**Backlight:** the board carries its own transistor (`Q1`) plus `R5`/`R6`. The
`LED` pin is a low-current control input, so GPIO 15 drives it directly — no
MOSFET needed. Firmware runs it as LEDC PWM via LovyanGFX `Light_PWM`, which is
what makes BH1750 auto-dim work.

**Orientation:** rotation **1** is locked. 320 wide × 240 tall, landscape.
Every layout coordinate in the firmware assumes this.

---

## Component 2 — Light sensor (working)

**BH1750**, I²C address **`0x23`**.

| Module pin | Wire to |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 8 |
| SCL | GPIO 9 |
| ADDR | *leave open* → `0x23`. To 3V3 → `0x5C`. |

**Known oddity:** this sensor will keep answering on the bus with VCC
disconnected. Current leaks in through the ESD diodes on SDA and SCL. The
readings are garbage when that happens. If lux looks wrong, check VCC first —
"it still responds" does not mean it is powered.

---

## Component 3 — Environment sensor (dead, awaiting replacement)

**GYBMEP 4-pin breakout**, I²C address **`0x76`**.

| Module pin | Wire to |
|---|---|
| VIN | 3V3 |
| GND | GND |
| SCL | GPIO 9 |
| SDA | GPIO 8 |

Four pins only — CSB and SDO are tied on the board, so there is nothing to
jumper and the address is fixed at `0x76`.

**Status:** the current unit is dead. Proven with `05_bme_isolation`: the
module's own pull-ups read powered with the ESP32's internal pull-ups off, and
it then failed to acknowledge at four bus speeds in both pin orientations,
alone on the bus, with no breadboard in the path. Power confirmed, wiring
confirmed, chip silent.

**When buying the replacement — test it in the shop.** This PCB's silkscreen
reads `BME/BMP280` for *both* parts, so visual identification is impossible.
Run `01_bringup_i2c` and read register `0xD0`:

| Chip ID | Part | Verdict |
|---|---|---|
| `0x60` | BME280 | correct — has humidity |
| `0x58` | BMP280 | **no humidity sensor** — reject it |
| `0x61` | BME680 | works, but needs a different library |

Firmware degrades gracefully while it is missing: the environment page shows
`--` and readings post with null fields.

---

## Component 4 — Rotary encoder

**HW-040** (EC11 mechanism).

| Module pin | Wire to |
|---|---|
| `+` | **3V3** — never 5 V |
| `GND` | GND |
| `CLK` | GPIO 4 |
| `DT` | GPIO 5 |
| `SW` | GPIO 6 |

`SW` has no pull-up on the module, so firmware uses `INPUT_PULLUP`. CLK and DT
have onboard 10 kΩ pull-ups tied to `+`.

**This encoder is half-detent.** Measured with `06_encoder_trace`, not assumed:
one click produces **2** quadrature steps, and the rest position alternates
between `00` and `11` rather than always resting at `11`. The decoder therefore
emits at both rest states with a threshold of 2.

Consequence: there is no spare edge. A 4-step encoder can absorb one bounced
transition; this one cannot. If a single click ever jumps two pages, that is
contact bounce — fit **100 nF ceramic from CLK to GND and from DT to GND**.
With the module's 10 kΩ pull-ups that gives a ~1 ms filter, far below the
fastest human turn.

If clockwise counts backwards, swap the `PIN_ENC_CLK` and `PIN_ENC_DT` defines
in firmware. Do not rewire.

---

## Component 5 — Timer button

**Omron B3F**, 4 legs numbered:

```
    2      1
    4      3
```

**Use a diagonal pair: 1 and 4, or 2 and 3.**

The two legs on each side of the body are permanently shorted inside the
switch. A diagonal pair is guaranteed to span the two terminals regardless of
which way the internal bars run, so it is never wrong. Same-row or
same-column might be a dead short.

| Leg | Wire to |
|---|---|
| 1 | GPIO 7 |
| 4 | GND |
| 2, 3 | nothing |

No resistor. Firmware uses `INPUT_PULLUP`: HIGH idle, LOW pressed.

**Failure tell:** if serial prints `PRESS` at boot and never releases, both
wires are on the same leg pair and the pin is shorted to ground. Rotate the
switch 90° in the breadboard.

Short press → start / pause / resume. Long press (800 ms) → stop.

---

## Rebuild order

If everything comes unplugged, wire in this sequence. Each step is verifiable
before you add the next, so a fault is always in the thing you just touched.

1. **Power rails.** 3V3 and GND from the devkit to the breadboard rails.
   Nothing else. Confirm the red LED on the S3 is lit.
2. **I²C sensors.** BH1750 and BME280 — VCC, GND, SDA→8, SCL→9, both in
   parallel. Run `01_bringup_i2c`. Expect `0x23`, and `0x76` once the sensor is
   replaced.
3. **Display.** Eight wires, pins 1–8. Nothing from pin 10 onward. Run
   `02b_bringup_tft_lovyan`. Expect red/green/blue, four corner boxes, a full
   1 px border, and a smooth brightness ramp.
4. **Encoder and button.** Run `03_bringup_controls`. One click must print
   exactly `+1` or `-1`. Press the knob and the button; both must report SHORT
   and LONG.
5. **Full firmware.** Flash `cadence_hub.ino`.

---

## Board settings — Arduino IDE

These are as load-bearing as the wiring. Wrong PSRAM setting alone causes a
boot loop.

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Disabled** |
| CPU Frequency | 240 MHz (WiFi) |
| Flash Mode | QIO 80 MHz |
| Flash Size | 8 MB (64 Mb) |
| PSRAM | **QSPI PSRAM** — not OPI |
| Partition Scheme | 8 M with spiffs (3 MB APP / 1.5 MB SPIFFS) |
| Upload Speed | 921600 |

**Flash via the COM port, not the USB port.** The board has two USB sockets.
COM goes through a separate bridge chip that stays enumerated through resets
and crashes; the native USB port is the S3 itself, so it disappears on every
reset and you lose the first second of serial output — which is exactly where
sensor failures print.

`USB CDC On Boot` must be **Disabled** to match. Enabled routes `Serial` to the
native USB port and the monitor stays silent on COM.

If upload fails: hold BOOT, tap RESET, release BOOT.

---

## Libraries

| Library | Author | Note |
|---|---|---|
| LovyanGFX | lovyan03 | **not TFT_eSPI** — see below |
| Adafruit BME280 Library | Adafruit | pulls in Adafruit_Sensor + BusIO |
| Adafruit BMP280 Library | Adafruit | the part actually fitted |
| DHT sensor library for ESPx | beegee_tokyo | `DHTesp`, for the DHT22 |
| BH1750 | Christopher Laws | |
| U8g2 | olikraus | **font tables only** — see below |

**U8g2 is not driving a display here.** It is included for its font data, which
LovyanGFX renders through `lgfx::U8g2font`. That combination is what retired
the seven-segment `Font7` clock and the 8px `Font2` labels: the hub now uses
real Helvetica bitmaps and a `logisoso` clock face, and the `_tf` variants
carry the full Latin-1 range so `°C` is typeset rather than drawn.

Each U8g2 font lives in its own linker section, so `--gc-sections` (on by
default in the ESP32 core) drops the ~2000 faces the sketch never names. If the
binary ever grows by hundreds of KB, that is the setting to check first.

**Do not switch back to TFT_eSPI.** It crashes on `init()` with the ESP32-S3 on
Arduino-ESP32 core 3.x — `StoreProhibited`, register dump confirmed, reproduced
with two independent sketches including the library's own diagnostic example.
LovyanGFX resolved it and puts the panel config in the sketch instead of a
library header, which also removes a whole class of "did my User_Setup take"
confusion.

---

## Diagnostic sketches

Kept in `firmware/bringup/`. Each proves one thing.

| Sketch | Proves |
|---|---|
| `01_bringup_i2c` | Bus works; Bosch chip ID distinguishes BME280 from BMP280 |
| `02b_bringup_tft_lovyan` | Colour order, full addressability, backlight PWM, fill rate, rotation |
| `03_bringup_controls` | Encoder detent accuracy and bounce rate; both buttons |
| `04_bme_power_probe` | Whether a sensor's rail is live, by powering it from a GPIO |
| `05_bme_isolation` | **Pull-up presence probe — substitutes for a multimeter.** Internal pull-ups off; a line still reading solidly HIGH proves an external resistor is powered, which proves the module's VCC. Reusable on any I²C part. |
| `06_encoder_trace` | Captures every quadrature transition of one click. This is what identified the half-detent encoder. |

---

## Network

Credentials live in `firmware/cadence_hub/secrets.h`, which is gitignored.

- **SSID and password are compile-time constants.** Changing networks means
  reflashing. Campus Wi-Fi with a captive portal will not work at all — use a
  phone hotspot with an SSID you control and set it before the demo.
- **`API_BASE`** points at the deployed dashboard. A LAN address works for
  development but is a DHCP lease and will move.
- **`CAM_HOST`** is optional and points at the camera board. Left unset it
  defaults to `cadence-cam.local`, the mDNS name the camera already
  advertises. Set it to a bare IP if mDNS does not resolve — see below.
- The ESP32-S3 has **no 5 GHz radio**. The network must be 2.4 GHz.

Timezone is `<+06>-6` for Dhaka. The POSIX sign is inverted — east of Greenwich
is negative. This affects local display only; timestamps on the wire are UTC.

---

## Vision tier — how the button reaches the camera

There is no wire between the two boards. They meet on the LAN, and the chain
has one link in it that is not a device at all.

```
[timer button] -> hub -> GET /session?state=running -> camera board
                                                          |
                              browser tab on /vision  <- polls /session
                                      |
                           MediaPipe (in the tab, not on the board)
                                      |
                           POST /focus -> camera relay -> Vercel -> Neon
```

**The camera board does not do the tracking.** It has no inference on it at
all — it serves an MJPEG stream and a web page, and the page runs MediaPipe in
the browser. So "the camera is tracking" always means *a browser tab is open
on `http://cadence-cam.local/vision`*. Nothing records if it is closed.

The hub therefore cannot start tracking directly; it can only arm it. It tells
the camera board what the session is doing, the board holds that state, and
the open page polls for it every 2 s and starts or stops recording to match.

| Hub pill (Home, bottom right) | Means |
|---|---|
| `STANDALONE` | No camera has ever answered. Supported mode, not a fault. |
| `CAM LOST` | It answered before and has stopped. Check power and Wi-Fi. |
| `NO VIEWER` | Board is up, **the `/vision` tab is not open.** Nothing is being recorded. |
| `READY` | Tab open, waiting for the timer button. |
| `TRACKING` | Session running and being recorded. |

**`NO VIEWER` is the one to know.** Press the button, get a session, and record
nothing — because the page that does the work was never opened. The pill is
the only place that is visible from the chair.

### Failure tells

| Symptom | Cause |
|---|---|
| Pill stuck on `STANDALONE`, board serves `/vision` fine in a browser | mDNS not resolving. Set `CAM_HOST` to the board's IP in the hub's `secrets.h` and reflash. |
| Session state on the page lags the button by a few seconds | Normal. The page polls every 2 s; `?sess=1000` halves it. |
| Page keeps recording ~90 s after the hub loses power | Also normal, and deliberate. The board expires a session it stops hearing about; the hub re-asserts every 30 s and the TTL is 3× that. |
| Nothing in `focus_samples` despite `TRACKING` | The relay, not the gating. Check `/status` on the camera for `relay_status`. |

The page can still be run without a hub: `http://cadence-cam.local/vision?free=1`
records continuously, which is what calibration on a bare bench needs. It is
not the default, because samples belonging to no session are exactly what made
a tab left open overnight report an unbroken four-hour focus.
