// ─────────────────────────────────────────────────────────────────────────
//  Cadence — hub firmware skeleton
//  Target : ESP32-S3 DevKit N8R2 + LockerBox 3.2" ILI9341, rotation 1
//  Canvas : 320 x 240 landscape
//
//  In scope : display, pages, encoder nav, buttons, timer state machine,
//             BH1750 auto-dim, environment sensing, Wi-Fi, NTP, durable event
//             queue, backend ingest, the todo list and the week's focus totals
//             pulled from the dashboard, and arming the camera board's vision
//             page from the focus-timer button.
//  Not yet  : ESP-NOW satellite.
//  Removed  : the character face, in the 2026-08-17 Home redesign.
//
//  Threading: rendering and input run in loop() on core 1. All networking
//  runs in netTask on core 0, because HTTPClient blocks and a 2 s TLS
//  handshake in loop() would freeze the UI and drop encoder detents. The
//  event queue, the todo list and the stats block are each shared across both
//  and each guarded by its own mutex.
//
//  Libraries (Library Manager):
//    LovyanGFX               by lovyan03
//    Adafruit BME280 Library by Adafruit  (pulls Adafruit_Sensor, BusIO)
//    BH1750                  by Christopher Laws
//    DHT sensor library for ESPx           (DHTesp, for the DHT22)
//    U8g2                    by olikraus  (font tables only — LovyanGFX
//                                          renders them via lgfx::U8g2font)
//
//  Credentials live in secrets.h, in this folder, kept out of git.
// ─────────────────────────────────────────────────────────────────────────

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <DHTesp.h>
#include <BH1750.h>
#include <soc/gpio_reg.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
// Included for its font tables only — LovyanGFX does all the rendering. Each
// font lives in its own linker section, so --gc-sections (on by default in the
// ESP32 core) drops the ~2000 faces this sketch never names. If the binary
// ever jumps by hundreds of KB, that is the setting to check first.
#include <U8g2lib.h>
#include "secrets.h"
#include "espnow_sync.h"

// Where the camera board lives. Defaulted here rather than required in
// secrets.h so an existing secrets.h from before the vision tier still
// compiles — override it there if the name does not resolve.
//
// The name is what the camera already advertises over mDNS (CAM_HOSTNAME in
// its own secrets.h), so this survives a DHCP lease change that a hard-coded
// IP would not. The caveat is the demo network: mDNS is unreliable on some
// phone hotspots, which is exactly what docs/WIRING.md tells you to use. If
// the pill on Home reads NO CAM while the board is plainly up, put its IP
// here and reflash — the same tradeoff API_BASE already carries.
#ifndef CAM_HOST
#define CAM_HOST "cadence-cam.local"
#endif

// ═══════════════════════════════════════════════════ pins (spec §3, S3 map)

#define PIN_TFT_SCLK 12
#define PIN_TFT_MOSI 11
#define PIN_TFT_MISO 13
#define PIN_TFT_CS   10
#define PIN_TFT_DC   16
#define PIN_TFT_RST  17
#define PIN_TFT_BL   15

#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9

#define PIN_ENC_CLK   4
#define PIN_ENC_DT    5
#define PIN_ENC_SW    6
#define PIN_BTN       7

// DHT22 data line. Not I2C — a single-wire protocol, so it needs its own pin.
// 18 is from the free list in docs/WIRING.md: not strapping, not USB, not
// flash/PSRAM. Change this one define if you wire it elsewhere.
#define PIN_DHT      18

#define ADDR_BME    0x76      // hard-coded per spec §3. Change if 0x77.
#define ADDR_BH1750 0x23      // confirmed by bring-up 01.

#define SCREEN_W 320
#define SCREEN_H 240

// ═══════════════════════════════════════════════════ display

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_TFT_SCLK;
      cfg.pin_mosi    = PIN_TFT_MOSI;
      cfg.pin_miso    = PIN_TFT_MISO;
      cfg.pin_dc      = PIN_TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { auto cfg = _panel.config();
      cfg.pin_cs           = PIN_TFT_CS;
      cfg.pin_rst          = PIN_TFT_RST;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    { auto cfg = _light.config();
      cfg.pin_bl      = PIN_TFT_BL;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
LGFX_Sprite canvas(&tft);

// ── type scale ───────────────────────────────────────────────────────────
//
// Bitmap faces from U8g2, rendered by LovyanGFX through lgfx::U8g2font.
// Font7 and Font2 are gone from Home: a seven-segment clock and an 8px
// blocky bitmap are most of why the screen read as a debug readout.
//
// The _tf suffix is the full Latin-1 range, which carries a real degree sign,
// so temperatures are typeset rather than faked with a drawn ring.
//
// The clock is _tr (all of ASCII) and not the much smaller _tn digit subset:
// _tn is 724 bytes against 4625, but whether it carries a colon is not
// something the datasheet-free font table will tell you, and a clock with no
// colon is not worth 4 KB of a 3 MB partition.
//
// Three levels, far apart. The previous set ran 10/14/24/42, where the 10 and
// 14 collapsed into one level and did the work of neither — contrast is what
// makes a hierarchy, not variety.
//
// A display face for the numerals and a text face for the small copy is a
// deliberate pairing rather than an inconsistency: Old Standard is drawn for
// large sizes and its hairlines disappear at 12px, New Century is the reverse.
//
// Chosen 2026-08-17 from rendered specimens rather than from font names.
// Old Standard Bold is a high-contrast Didone — the genre the task board wrote
// off as "not achievable" for the original mockup. It was in an installed
// library the whole time.
//
// The comment beside each is the REAL metric, decoded from the u8g2 header, not
// the number in the name. That distinction caused the clipping this replaces:
// the name is the cap height and the box is far taller, so a layout spaced by
// the name overlaps itself. Positioning is by baseline for the same reason —
// a 68px box costs nothing for a digit string that has no descenders.
//
//                                              box  baseline  cap
static const lgfx::U8g2font fontHero (u8g2_font_osb41_tf);   //  68   53   41
static const lgfx::U8g2font fontTimer(u8g2_font_osb21_tf);   //  36   28   21
static const lgfx::U8g2font fontText (u8g2_font_ncenB12_tf); //  19   16   12
static const lgfx::U8g2font fontTiny (u8g2_font_ncenB08_tf); //  13   11    8

// Pages 02 and 03 take the same text face a size up: they are lists and
// readouts, read rather than glanced at.
static const lgfx::U8g2font fontBody (u8g2_font_ncenB14_tf); //  23   19   14
static const lgfx::U8g2font fontLabel(u8g2_font_ncenB10_tf); //  18   15   11

// Palette. Warm-neutral, readable at low brightness.
#define C_BG      0x0861      // near-black
#define C_PANEL   0x18E3      // card
#define C_TEXT    0xEF7D      // off-white
#define C_DIM     0x8410      // muted
#define C_ACCENT  0x05DF      // cyan
#define C_WARN    0xFC00      // amber
#define C_GOOD    0x2E6B      // green

// Hairline, between C_BG and C_PANEL. Three rules on the page, not five.
#define C_RULE    0x2124
// Chart top gridline and the unfilled part of the attention bar.
#define C_TRACK   0x10A2
// A fourth text level below C_DIM, for the footer. C_DIM reads far brighter on
// the panel than its numbers suggest, so "quiet" needed its own value rather
// than a reuse — on hardware the old idle timer in C_DIM competed with the
// clock while saying nothing.
#define C_FAINT   0x5B0B
// Muted accent for the six days that are not today. The hierarchy inside the
// sparkline is carried entirely by this against C_ACCENT.
#define C_ACCENT_DIM 0x1ACE

// ═══════════════════════════════════════════════════ sensors

// Two possible environment sensors on the same address. The GYBMEP silkscreen
// reads "BME/BMP280" for both parts and they are visually identical, so which
// one is fitted is not knowable until the chip ID is read at boot:
//   0x60 -> BME280, temperature + pressure + humidity
//   0x58 -> BMP280, temperature + pressure only, no humidity sensor on the die
// The BME280 library refuses anything that is not 0x60, so a BMP280 needs its
// own driver rather than a flag. Detect and adapt instead of assuming.
Adafruit_BME280 bme;
Adafruit_BMP280 bmp(&Wire);
DHTesp dht;
BH1750 lightMeter(ADDR_BH1750);

// Which parts are actually fitted. A BME280 alone covers all three readings;
// a BMP280 plus a DHT22 covers the same ground with two cheaper parts, since
// the DHT22 supplies exactly the humidity the BMP280 lacks.
bool srcBme = false;    // BME280 — temperature, humidity, pressure
bool srcBmp = false;    // BMP280 — temperature, pressure
bool srcDht = false;    // DHT22  — temperature, humidity

// Derived from the above, so the render and post paths never have to know
// which combination of parts produced a value.
bool hasTemp     = false;
bool hasHumidity = false;
bool hasPressure = false;

const char *envName = "absent";

bool  luxOk   = false;

float tempC   = NAN;
float humidity= NAN;
float pressHpa= NAN;
float lux     = 0;
float luxEma  = 40;
float realFeelC = NAN;

// NOAA heat index. Below 26.7 C the correction is meaningless, so the
// apparent temperature is just the dry-bulb reading.
static float heatIndexC(float tC, float rh) {
  if (isnan(tC) || isnan(rh)) return NAN;
  float tF = tC * 9.0f / 5.0f + 32.0f;
  if (tF < 80.0f) return tC;

  float hi = -42.379f + 2.04901523f * tF + 10.14333127f * rh
           - 0.22475541f * tF * rh - 0.00683783f * tF * tF
           - 0.05481717f * rh * rh + 0.00122874f * tF * tF * rh
           + 0.00085282f * tF * rh * rh - 0.00000199f * tF * tF * rh * rh;

  if (rh < 13.0f && tF >= 80.0f && tF <= 112.0f)
    hi -= ((13.0f - rh) / 4.0f) * sqrtf((17.0f - fabsf(tF - 95.0f)) / 17.0f);
  else if (rh > 85.0f && tF >= 80.0f && tF <= 87.0f)
    hi += ((rh - 85.0f) / 10.0f) * ((87.0f - tF) / 5.0f);

  return (hi - 32.0f) * 5.0f / 9.0f;
}

static void readSensors() {
  static uint32_t lastSlow = 0, lastFast = 0;
  uint32_t now = millis();

  if (luxOk && now - lastFast > 500) {
    lastFast = now;
    float l = lightMeter.readLightLevel();
    if (l >= 0) {
      lux = l;
      luxEma += 0.15f * (lux - luxEma);   // smoothed, drives the backlight
    }
  }

  // I2C only. The DHT22 is read on core 0 by netTask — see readDht().
  if ((srcBme || srcBmp) && now - lastSlow > 2000) {
    lastSlow = now;
    if (srcBme) {
      tempC     = bme.readTemperature();
      humidity  = bme.readHumidity();
      pressHpa  = bme.readPressure() / 100.0f;
      realFeelC = heatIndexC(tempC, humidity);
    } else {
      pressHpa = bmp.readPressure() / 100.0f;
      // When a DHT22 is present its temperature is the one paired with the
      // humidity going into the heat index, so it owns tempC and this reading
      // is skipped. Two sensors disagreeing by half a degree would otherwise
      // make the real-feel figure quietly inconsistent with the temperature
      // shown beside it.
      if (!srcDht) tempC = bmp.readTemperature();
    }
  }
}

// Called from netTask on core 0, never from loop(). DHTesp takes a critical
// section for the ~5 ms bit-banged read, and portENTER_CRITICAL on ESP32 masks
// interrupts on the calling core only. The encoder ISR is registered on core 1
// (attachInterrupt runs from setup, which lives on the Arduino task), so
// reading here cannot cost it an edge — and this decoder is the half-detent
// kind with no spare edge to lose.
static void readDht() {
  if (!srcDht) return;

  static uint32_t last = 0;
  // A DHT22 tops out at 0.5 Hz and the library enforces ~2 s itself; asking
  // more often just returns the cached sample.
  if (millis() - last < 2500) return;
  last = millis();

  TempAndHumidity v = dht.getTempAndHumidity();
  if (dht.getStatus() != DHTesp::ERROR_NONE || isnan(v.humidity)) return;

  // A failed read keeps the previous values rather than blanking the row. One
  // dropped sample on a bit-banged protocol is normal and not worth showing.
  humidity  = v.humidity;
  tempC     = v.temperature;
  realFeelC = heatIndexC(tempC, humidity);
}

// Auto-dim (spec §2). The screen lights the room it is measuring, so the
// floor is set high enough that dimming never chases its own tail.
static void applyAutoDim() {
  static uint8_t shown = 0;
  static uint32_t last = 0;
  if (millis() - last < 250) return;
  last = millis();

  float l = constrain(luxEma, 1.0f, 400.0f);
  uint8_t target = (uint8_t)(28 + (l - 1.0f) * (255 - 28) / 399.0f);
  if (abs((int)target - (int)shown) > 3) {
    shown = target;
    tft.setBrightness(shown);
  }
}

// ═══════════════════════════════════════════════════ encoder

static const int8_t QDEC[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

// Half-detent encoder, confirmed by trace: one click = 2 quadrature steps,
// resting alternately at 00 and 11. Do not raise this to 3 or 4 — a click
// never produces that many steps and every click would be dropped.
#define ENC_DETENT_THRESHOLD 2

volatile int32_t  encPos    = 0;
volatile int8_t   encSub    = 0;
volatile uint8_t  encPrev   = 0;

static inline uint8_t readEncPins() {
  uint32_t g = REG_READ(GPIO_IN_REG);
  return (((g >> PIN_ENC_CLK) & 1) << 1) | ((g >> PIN_ENC_DT) & 1);
}

void IRAM_ATTR encISR() {
  uint8_t curr = readEncPins();
  int8_t  step = QDEC[(encPrev << 2) | curr];
  encPrev = curr;
  if (step == 0) return;

  encSub += step;

  // MEASURED: this HW-040 is a half-detent encoder. One click is 2 quadrature
  // steps and the rest position alternates 00 / 11, so both count as a detent.
  // Threshold is 2 because a click only ever produces 2 steps — unlike a
  // 4-step encoder there is no spare edge to lose, so this decoder has no
  // bounce tolerance. If a click ever double-counts, fit the 100nF caps.
  if (curr == 0b11 || curr == 0b00) {
    if      (encSub >=  ENC_DETENT_THRESHOLD) encPos++;
    else if (encSub <= -ENC_DETENT_THRESHOLD) encPos--;
    encSub = 0;
  }
}

// ═══════════════════════════════════════════════════ buttons

enum BtnEvent : uint8_t { BTN_NONE, BTN_SHORT, BTN_LONG };

struct Button {
  uint8_t pin;
  bool     stable    = HIGH;
  uint32_t lastEdge  = 0;
  uint32_t pressedAt = 0;
  bool     longFired = false;

  void begin() { pinMode(pin, INPUT_PULLUP); }

  BtnEvent poll() {
    bool raw = digitalRead(pin);
    uint32_t now = millis();
    BtnEvent ev = BTN_NONE;

    if (raw != stable && now - lastEdge > 25) {
      stable = raw;
      lastEdge = now;
      if (stable == LOW) { pressedAt = now; longFired = false; }
      else if (!longFired) ev = BTN_SHORT;
    }
    if (stable == LOW && !longFired && now - pressedAt >= 800) {
      longFired = true;
      ev = BTN_LONG;
    }
    return ev;
  }
};

Button encSw   { PIN_ENC_SW };
Button timerBtn{ PIN_BTN };

// ═══════════════════════════════════════════════════ Wi-Fi

// Non-blocking. Never spin on WiFi.status() in a loop — that freezes the
// render and drops encoder detents every time the link flaps.

enum NetState : uint8_t { NET_DOWN, NET_CONNECTING, NET_UP };
NetState netState = NET_DOWN;

uint32_t netAttemptMs  = 0;
uint32_t netRetryMs    = 0;
uint16_t netRetryCount = 0;

#define NET_ATTEMPT_TIMEOUT 15000
#define NET_RETRY_BASE      3000
#define NET_RETRY_MAX      60000

static void netBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // sleep adds latency and hurts NTP jitter
  WiFi.setAutoReconnect(false);    // this state machine owns reconnection
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  netState      = NET_CONNECTING;
  netAttemptMs  = millis();
  Serial.printf("[WIFI] connecting to \"%s\"\n", WIFI_SSID);
}

static void netService() {
  uint32_t now = millis();

  switch (netState) {
    case NET_DOWN:
      if (now - netRetryMs >= min((uint32_t)NET_RETRY_MAX,
                                  (uint32_t)(NET_RETRY_BASE * (netRetryCount + 1))))
        netBegin();
      break;

    case NET_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        netState      = NET_UP;
        netRetryCount = 0;
        Serial.printf("[WIFI] up, ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
      } else if (now - netAttemptMs > NET_ATTEMPT_TIMEOUT) {
        WiFi.disconnect();
        netState   = NET_DOWN;
        netRetryMs = now;
        if (netRetryCount < 20) netRetryCount++;
        Serial.printf("[WIFI] attempt timed out (status %d), retry %u\n",
                      WiFi.status(), netRetryCount);
      }
      break;

    case NET_UP:
      if (WiFi.status() != WL_CONNECTED) {
        netState   = NET_DOWN;
        netRetryMs = now;
        Serial.println("[WIFI] link lost");
      }
      break;
  }
}

// ═══════════════════════════════════════════════════ NTP / time

// Spec §7: ts is ISO 8601 UTC, NTP at boot and re-sync hourly, and the hub
// buffers rather than sending garbage timestamps if NTP has never succeeded.

bool     timeValid    = false;
uint32_t lastSyncMs   = 0;
time_t   bootEpoch    = 0;      // wall-clock epoch at millis() == 0

#define NTP_RESYNC_MS 3600000UL

// Dhaka: UTC+6, no DST. POSIX TZ inverts the sign — east of Greenwich is
// negative here. This affects localtime() only; the wire format stays UTC.
#define TZ_DHAKA "<+06>-6"

static void ntpBegin() {
  configTzTime(TZ_DHAKA, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  Serial.println("[NTP] sync requested");
}

static void ntpService() {
  if (netState != NET_UP) return;

  static bool requested = false;
  if (!requested) { ntpBegin(); requested = true; }

  if (timeValid && millis() - lastSyncMs < NTP_RESYNC_MS) return;

  time_t now = time(nullptr);
  if (now < 1700000000) return;          // sentinel: clock still unset

  // Anchor the boot instant so events queued before sync can be back-filled
  // with real timestamps instead of being thrown away.
  bootEpoch  = now - (time_t)(millis() / 1000);
  lastSyncMs = millis();

  if (!timeValid) {
    timeValid = true;
    struct tm lt;
    localtime_r(&now, &lt);
    Serial.printf("[NTP] synced: %04d-%02d-%02d %02d:%02d:%02d local\n",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
  }
}

// ISO 8601 UTC, the exact format spec §7 expects on the wire.
static void isoUtc(time_t t, char *out, size_t n) {
  struct tm g;
  gmtime_r(&t, &g);
  snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
           g.tm_hour, g.tm_min, g.tm_sec);
}

// ═══════════════════════════════════════════════════ event queue

// Events are stored against uptime, not wall clock, so one that happens
// before NTP lands is still recoverable — bootEpoch converts it later.
// Spec §4: the device emits events, never totals.

struct QueuedEvent {
  uint32_t uptimeMs;
  char     type[8];      // start / pause / resume / stop
};

#define EVQ_SIZE 32
QueuedEvent evq[EVQ_SIZE];
uint8_t  evqHead = 0, evqCount = 0;
uint16_t evqDropped = 0;

// Written by loop() on core 1, read and drained by netTask on core 0.
SemaphoreHandle_t evqLock = nullptr;

#define EVQ_TAKE()  xSemaphoreTake(evqLock, portMAX_DELAY)
#define EVQ_GIVE()  xSemaphoreGive(evqLock)

static void evqPush(const char *type) {
  EVQ_TAKE();
  if (evqCount == EVQ_SIZE) {
    evqHead = (evqHead + 1) % EVQ_SIZE;   // drop oldest
    evqCount--;
    evqDropped++;
  }
  QueuedEvent &e = evq[(evqHead + evqCount) % EVQ_SIZE];
  e.uptimeMs = millis();
  snprintf(e.type, sizeof(e.type), "%s", type);
  evqCount++;
  EVQ_GIVE();
}

// Snapshot destination. A file-scope buffer rather than a parameter: the
// Arduino IDE injects auto-generated prototypes above all user declarations,
// so a signature naming QueuedEvent fails to compile. Only netTask reads or
// writes this, so single ownership makes it safe without a second lock.
QueuedEvent evqSnap[EVQ_SIZE];
uint8_t     evqSnapCount = 0;

// Copies out up to EVQ_SIZE events into evqSnap without removing them. The
// network call then happens with the lock released — holding a mutex across a
// 2 s TLS handshake would block the UI thread on its next button press.
static uint8_t evqSnapshot() {
  EVQ_TAKE();
  evqSnapCount = evqCount;
  for (uint8_t i = 0; i < evqSnapCount; i++)
    evqSnap[i] = evq[(evqHead + i) % EVQ_SIZE];
  EVQ_GIVE();
  return evqSnapCount;
}

// Removes exactly the n events that were snapshotted. Anything pushed during
// the POST stays queued, which is why this counts rather than clearing.
static void evqCommit(uint8_t n) {
  EVQ_TAKE();
  if (n > evqCount) n = evqCount;
  evqHead = (evqHead + n) % EVQ_SIZE;
  evqCount -= n;
  EVQ_GIVE();
}

static uint8_t evqDepth() {
  EVQ_TAKE();
  uint8_t n = evqCount;
  EVQ_GIVE();
  return n;
}

// ═══════════════════════════════════════════════════ todos

// The list now comes from GET /api/todos (spec §7), which returns open items
// only, position-ordered. Declared here rather than beside the pages because
// refreshTodos() below needs the type, and this file is compiled top-down.
//
// Written by netTask on core 0, read by render() on core 1 — so it needs its
// own mutex, exactly like the event queue. Reusing evqLock would couple a
// 15fps render to the network drain for no reason.

#define TODO_MAX        12
#define TODO_TITLE_LEN  64

struct Todo { char title[TODO_TITLE_LEN]; };

Todo todos[TODO_MAX];
int  todoCount = 0;
int  todoSel = 0;
bool todoScrollMode = false;
bool todosFetched = false;      // false until one GET has actually succeeded

SemaphoreHandle_t todoLock = nullptr;

#define TODO_TAKE()  xSemaphoreTake(todoLock, portMAX_DELAY)
#define TODO_GIVE()  xSemaphoreGive(todoLock)


// ═══════════════════════════════════════════════════ HTTP ingest

// Everything below runs on netTask (core 0) only. HTTPClient blocks, which is
// exactly why it is not allowed anywhere near loop().
//
// One client of each kind, shared by every request. A second WiFiClientSecure
// would mean a second mbedTLS context, and the handshake is already the
// largest thing this task allocates.
static WiFiClientSecure tlsClient;
static WiFiClient       plainClient;

// Certificate validation skipped — documented deviation. Pinning a CA bundle
// costs ~8 KB of flash and needs rotating when the CA changes.
static bool httpBegin(HTTPClient &http, const String &url) {
  bool opened;
  if (url.startsWith("https://")) {
    tlsClient.setInsecure();
    tlsClient.setTimeout(10);
    opened = http.begin(tlsClient, url);
  } else {
    opened = http.begin(plainClient, url);
  }
  if (!opened) return false;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  return true;
}

uint32_t apiLastOk     = 0;      // millis of the last 2xx
uint16_t apiFailCount  = 0;
int      apiLastStatus = 0;

static int httpPostJson(const char *path, const String &body) {
  String url = String(API_BASE) + path;
  HTTPClient http;

  if (!httpBegin(http, url)) return -1;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " API_TOKEN);

  int status = http.POST(body);
  String resp = status > 0 ? http.getString() : String();
  http.end();

  apiLastStatus = status;
  if (status >= 200 && status < 300) {
    apiLastOk    = millis();
    apiFailCount = 0;
  } else {
    if (apiFailCount < 1000) apiFailCount++;
    Serial.printf("[API] %s -> %d %s\n", path, status, resp.c_str());
  }
  return status;
}

// ── session events (spec §7) ─────────────────────────────────────────────

static void postSessionEvents() {
  if (!timeValid) return;                 // §7: never send garbage timestamps

  uint8_t n = evqSnapshot();
  if (n == 0) return;

  // Batched: draining 32 events as 32 separate TLS handshakes would take a
  // minute and give 32 chances to fail.
  String body = String("{\"device_id\":\"") + DEVICE_ID + "\",\"events\":[";
  for (uint8_t i = 0; i < n; i++) {
    char ts[32];
    isoUtc(bootEpoch + (time_t)(evqSnap[i].uptimeMs / 1000), ts, sizeof(ts));
    if (i) body += ',';
    body += "{\"ts\":\"";  body += ts;
    body += "\",\"type\":\""; body += evqSnap[i].type;
    body += "\",\"source\":\"button\"}";
  }
  body += "]}";

  int status = httpPostJson("/api/ingest/session-events", body);

  if (status >= 200 && status < 300) {
    evqCommit(n);
    Serial.printf("[API] %u event(s) accepted\n", n);
    if (evqDropped) {
      Serial.printf("[API] warning: %u events were dropped before sending "
                    "(queue overflow)\n", evqDropped);
      evqDropped = 0;
    }
  } else if (status >= 400 && status < 500) {
    // The server rejected the payload itself. Retrying cannot fix that, and
    // keeping it would wedge the queue forever behind one bad row.
    evqCommit(n);
    Serial.printf("[API] %u event(s) REJECTED (%d) and discarded\n", n, status);
  }
  // 5xx or transport failure: leave them queued and try again later.
}

// ── readings (spec §7) ───────────────────────────────────────────────────

static void appendJsonFloat(String &s, const char *key, float v, bool ok) {
  s += "\""; s += key; s += "\":";
  if (!ok || isnan(v)) { s += "null"; return; }
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", v);
  s += buf;
}

static void postReading() {
  if (!timeValid) return;

  char ts[32];
  isoUtc(time(nullptr), ts, sizeof(ts));

  // Nulls where the BME280 is absent. §7 allows it and the dashboard shows a
  // gap, which is the honest representation of a missing sensor.
  String body = String("{\"device_id\":\"") + DEVICE_ID + "\",\"ts\":\"" + ts + "\",";
  appendJsonFloat(body, "temp_c",       tempC,     hasTemp);     body += ',';
  appendJsonFloat(body, "humidity",     humidity,  hasHumidity); body += ',';
  appendJsonFloat(body, "pressure_hpa", pressHpa,  hasPressure); body += ',';
  appendJsonFloat(body, "lux",          lux,       luxOk);       body += ',';
  appendJsonFloat(body, "real_feel_c",  realFeelC, hasHumidity);
  body += "}";

  httpPostJson("/api/ingest/readings", body);
}

// ── todo list (spec §7) ──────────────────────────────────────────────────

// No Authorization header: GET /api/todos is unauthenticated by design. The
// route says why — the hub fetches it on a timer, the list is not sensitive,
// and baking the token into a GET buys nothing.
static int httpGetBody(const char *path, String &out) {
  String url = String(API_BASE) + path;
  HTTPClient http;

  if (!httpBegin(http, url)) return -1;

  int status = http.GET();
  if (status > 0) out = http.getString();
  http.end();
  return status;
}

// Titles arrive as raw UTF-8 and the panel fonts are 8-bit, so anything above
// ASCII has to be folded down or it draws as mojibake. This is the common case
// rather than an edge case: an em-dash or a curly apostrophe typed into the
// dashboard is two or three bytes by the time it reaches here.
static char foldCodepoint(unsigned v) {
  switch (v) {
    case 0x2013: case 0x2014: return '-';    // en / em dash
    case 0x2018: case 0x2019: return '\'';   // curly single quotes
    case 0x201C: case 0x201D: return '"';    // curly double quotes
    case 0x2026: return '.';                 // ellipsis
    case 0x00A0: return ' ';                 // non-breaking space
  }
  return (v >= 32 && v < 127) ? (char)v : '?';
}

// Reads a JSON string value. `p` points just past the opening quote; returns
// the position after the closing quote, or nullptr if it never terminates.
//
// Escapes are decoded rather than ignored. Titles are free text typed into the
// dashboard, so a title containing a quote would otherwise end the string early
// and shift every following field — the kind of bug that only appears once
// somebody types an apostrophe-heavy task months from now.
static const char *jsonReadString(const char *p, char *out, size_t n) {
  size_t o = 0;
  while (*p) {
    char c = *p++;

    if (c == '"') {                       // closing quote — done
      out[o < n ? o : n - 1] = '\0';
      return p;
    }

    if (c == '\\') {
      char e = *p++;
      if (!e) break;
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = ' ';  break;
        case 'r': case 'b': case 'f': continue;   // nothing sane to draw
        case 'u': {
          // \uXXXX. The panel fonts are 8-bit, so anything above ASCII could
          // not render anyway; substitute rather than emit a broken glyph.
          unsigned v = 0; int k = 0;
          for (; k < 4 && p[k]; k++) {
            char h = p[k];
            v <<= 4;
            if      (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
            else break;
          }
          if (k == 4) { p += 4; c = foldCodepoint(v); }
          else        { c = '?'; }
          break;
        }
        default: c = e; break;            // \" \\ \/ and anything else literal
      }

    } else if ((unsigned char)c >= 0x80) {
      // Raw UTF-8. Next.js sends these as bytes rather than \u escapes, so
      // this is the path an em-dash actually takes. Decode the sequence to a
      // codepoint and fold it, otherwise the continuation bytes each draw as
      // a separate garbage glyph.
      unsigned char uc = (unsigned char)c;
      unsigned v = 0;
      int extra = 0;
      if      ((uc & 0xE0) == 0xC0) { v = uc & 0x1Fu; extra = 1; }
      else if ((uc & 0xF0) == 0xE0) { v = uc & 0x0Fu; extra = 2; }
      else if ((uc & 0xF8) == 0xF0) { v = uc & 0x07u; extra = 3; }
      else continue;                      // stray continuation byte — drop it

      for (int k = 0; k < extra; k++) {
        if (((unsigned char)*p & 0xC0) != 0x80) break;   // truncated sequence
        v = (v << 6) | ((unsigned char)*p++ & 0x3Fu);
      }
      c = foldCodepoint(v);
    }

    if (o + 1 < n) out[o++] = c;
  }
  return nullptr;                          // unterminated string
}

// Staging lives at file scope on purpose. netTask has an 8 KB stack and the
// TLS handshake takes roughly 6 KB of it; a ~780 byte local here would be held
// across that handshake. Only netTask touches this, so single ownership makes
// it safe without a second lock — the same reasoning as evqSnap.
static Todo todoStaging[TODO_MAX];

static void refreshTodos() {
  String body;
  int status = httpGetBody("/api/todos", body);

  // Any failure leaves the previous list on screen. A transient 5xx should not
  // blank the panel — a stale list is more useful than an empty one.
  if (status != 200) {
    Serial.printf("[TODO] GET -> %d\n", status);
    return;
  }

  // Only the titles are needed, and they arrive already ordered by position,
  // so this walks the payload for title values instead of parsing JSON in
  // general. Same reasoning as the hand-rolled validators on the server: one
  // less dependency, and every rule it enforces is visible right here.
  int n = 0;
  const char *p = body.c_str();
  while (n < TODO_MAX) {
    const char *k = strstr(p, "\"title\":\"");
    if (!k) break;
    const char *after = jsonReadString(k + 9, todoStaging[n].title, TODO_TITLE_LEN);
    if (!after) break;
    p = after;
    n++;
  }

  TODO_TAKE();
  for (int i = 0; i < n; i++) todos[i] = todoStaging[i];
  todoCount = n;
  // The list can shrink under the selection while the user is scrolling it.
  if (todoSel >= todoCount) todoSel = todoCount > 0 ? todoCount - 1 : 0;
  todosFetched = true;
  TODO_GIVE();

  Serial.printf("[TODO] %d task%s from the dashboard\n", n, n == 1 ? "" : "s");
}

// ── week and attention stats (spec §8) ───────────────────────────────────
//
// §4 forbids the firmware accumulating totals, and this does not: every number
// is computed by session_daily() and focus_daily() in Postgres and only drawn
// here. The Status page's old comment said today's figure belonged on-device
// "once it can fetch it" — httpGetBody exists now, so it can.
//
// One endpoint serves both the chart and the attention bar, because the TLS
// handshake is the expensive part of asking. /api/stats/hub is shaped to be
// read by a scanner rather than a parser: `days` is a flat array of 7 integers
// and every other field is an integer too, so there are no floats, no
// booleans and no nested objects to walk.

#define STATS_DAYS 7

static SemaphoreHandle_t statsLock = nullptr;
#define STATS_TAKE() xSemaphoreTake(statsLock, portMAX_DELAY)
#define STATS_GIVE() xSemaphoreGive(statsLock)

// Guarded by statsLock: written on netTask (core 0), read by the renderer.
long statDays[STATS_DAYS] = {0};
long statPeak = 0, statToday = 0;
bool statHasAttn = false;
long statFocused = 0, statDistracted = 0, statAway = 0;
int  statRatio = 0;
bool statsFetched = false;

// Finds "key": and returns the position just past the colon.
//
// Scanning, not parsing — the same trade the todo reader makes, and safe for
// the same reason: this walks a document this project also writes, so every
// key is known. It would not be safe over arbitrary user text.
static const char *jsonFind(const char *body, const char *key) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(body, pat);
  if (!p) return nullptr;
  p = strchr(p + strlen(pat), ':');
  return p ? p + 1 : nullptr;
}

// Returns false on a miss rather than yielding 0. A missing field and a real
// zero have to stay distinguishable: a failed fetch that quietly produced
// zeros would draw an empty chart, which looks exactly like a week with no
// work in it and is the same class of lie as the 201 on a discarded reading.
static bool jsonReadLong(const char *body, const char *key, long *out) {
  const char *p = jsonFind(body, key);
  if (!p) return false;
  while (*p == ' ') p++;
  char *end = nullptr;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  *out = v;
  return true;
}

static int jsonReadLongArray(const char *body, const char *key, long *out, int n) {
  const char *p = jsonFind(body, key);
  if (!p) return 0;
  while (*p == ' ') p++;
  if (*p != '[') return 0;
  p++;

  int i = 0;
  while (i < n) {
    while (*p == ' ' || *p == ',') p++;
    if (*p == ']' || *p == '\0') break;
    char *end = nullptr;
    long v = strtol(p, &end, 10);
    if (end == p) break;                 // not a number — stop, do not spin
    out[i++] = v;
    p = end;
  }
  return i;
}

static void refreshStats() {
  String body;
  int status = httpGetBody("/api/stats/hub", body);
  if (status != 200) {
    Serial.printf("[STATS] GET failed: %d\n", status);
    return;                              // keep whatever is already on screen
  }

  const char *b = body.c_str();

  long days[STATS_DAYS] = {0};
  int n = jsonReadLongArray(b, "days", days, STATS_DAYS);
  if (n != STATS_DAYS) {
    // A short array would shift every column by a day instead of failing
    // visibly, so it is refused outright rather than partially accepted.
    Serial.printf("[STATS] wanted %d days, got %d - keeping previous\n",
                  STATS_DAYS, n);
    return;
  }

  long today = 0, peak = 0, attn = 0, foc = 0, dis = 0, away = 0, ratio = 0;
  jsonReadLong(b, "today_seconds", &today);
  jsonReadLong(b, "peak_seconds",  &peak);
  jsonReadLong(b, "data",          &attn);
  jsonReadLong(b, "focused",       &foc);
  jsonReadLong(b, "distracted",    &dis);
  jsonReadLong(b, "away",          &away);
  jsonReadLong(b, "ratio",         &ratio);

  STATS_TAKE();
  memcpy(statDays, days, sizeof(statDays));
  statToday      = today;
  statPeak       = peak;
  statHasAttn    = (attn == 1);
  statFocused    = foc;
  statDistracted = dis;
  statAway       = away;
  statRatio      = (int)ratio;
  statsFetched   = true;
  STATS_GIVE();

  Serial.printf("[STATS] peak %lds today %lds attention %s %d%%\n",
                peak, today, statHasAttn ? "yes" : "none", (int)ratio);
}

// ── the camera board (spec §8, vision tier) ──────────────────────────────
//
// The focus-timer button starts the vision page's recording. It has to,
// because the alternative is a page that samples whenever a tab happens to be
// open — which is how a tab left open overnight becomes an unbroken four-hour
// focus that never happened.
//
// The hub cannot address a browser tab, so it does not try. It tells the
// camera board, the board holds the state, and the page polls the board. Each
// hop is between two things that can actually name each other.
//
// Plain HTTP, not TLS: this is one board talking to another on the same LAN,
// and a handshake per heartbeat would cost more than the exchange it guards.
//
// The heartbeat runs whatever the state, not only while a session is running.
// It is what keeps the board's TTL from expiring a live session, it refreshes
// the pill on Home, and it means a single dropped "idle" cannot leave the
// camera recording — the next beat corrects it either way.

#define CAM_HEARTBEAT_MS 30000UL   // camera expires the session at 3x this

static volatile uint8_t camWantState = 0;   // 0 idle · 1 running · 2 paused
static volatile bool    camDirty     = false;

bool     camOnline     = false;
// When the radio last heard from the camera. Separate from camLastOk, which
// HTTP also writes: the HTTP failure path must be able to ask "is the other
// road still up?" without answering its own question.
uint32_t camRadioAt    = 0;
bool     camViewer     = false;    // something is pulling the stream
uint32_t camLastOk     = 0;
int      camLastStatus = 0;

static const char *camStateName(uint8_t s) {
  return s == 1 ? "running" : s == 2 ? "paused" : "idle";
}

// Called from loop() on core 1. Records the intent and returns immediately —
// the request itself happens on netTask, for the same reason every other
// request does.
// The hub implements this one; the session callback is the camera's job.
void cadenceNowOnSession(uint8_t) {}

// The camera, heard directly over the radio. This is what makes the camera
// tracker work with no network: camOnline and camViewer used to be set from
// the HTTP response to camPushSession, and that request is the first thing to
// fail off-network — leaving the tracker reading offline for a camera that is
// sitting right there, working.
//
// Deliberately does not touch camLastStatus, which means "what did HTTP say".
// Conflating the two would hide which road is actually up.
void cadenceNowOnCamStatus(uint8_t, uint8_t flags) {
  camOnline  = true;
  camLastOk  = millis();
  camRadioAt = millis();
  camViewer = (flags & CADENCE_NOW_F_STREAMING) != 0;
}

static void camNotify(uint8_t state) {
  camWantState = state;
  camDirty     = true;

  // Straight out over the radio as well, and from this function rather than
  // from netTask, because ESP-NOW does not need the network to be up and must
  // not be made to wait for it. The HTTP push stays: where a network exists it
  // still works, and two roads to the same board is the point.
  cadenceNowSend(CADENCE_NOW_SESSION, state, 0);
}

// `announce` is set when this push carries an actual state change rather than
// being a heartbeat, so the monitor shows button presses and reachability
// flips without a line every 30 seconds burying everything else.
static void camPushSession(bool announce) {
  String url = String("http://") + CAM_HOST + "/session?state="
             + camStateName(camWantState);

  HTTPClient http;
  // A failed HTTP push says nothing about the camera when the radio is still
  // hearing it every two seconds — which is the normal case off-network, and
  // the case this whole device is being carried into.
  if (!httpBegin(http, url)) {
    if (millis() - camRadioAt > 10000UL) { camOnline = false; camViewer = false; }
    return;
  }

  // Tighter than httpBegin's defaults, which are sized for a TLS handshake to
  // Vercel. This is a LAN GET to a device that either answers in milliseconds
  // or is switched off, and a camera that is off must not hold up the todo
  // fetch queued behind it on this task.
  http.setConnectTimeout(1500);
  http.setTimeout(2000);

  int status = http.GET();
  String resp = status > 0 ? http.getString() : String();
  http.end();

  bool wasOnline = camOnline;
  camLastStatus  = status;
  bool httpOk    = (status >= 200 && status < 300);
  camOnline      = httpOk || (millis() - camRadioAt <= 10000UL);

  // Only HTTP may read HTTP's answer. When camOnline is true because the radio
  // said so, `resp` is empty, and parsing it would report "no viewer" for a
  // camera the radio just told us is streaming.
  if (httpOk) {
    camLastOk = millis();
    // Scanned, not parsed. One boolean out of a response this project also
    // writes does not justify a parser; the hand-rolled one used for todos
    // exists only because todo titles are arbitrary user text.
    camViewer = resp.indexOf("\"viewer\":true") >= 0;
  } else if (!camOnline) {
    camViewer = false;
  }

  // "Armed the camera" and "shouted into the void" have to look different in
  // the monitor. Every silent-success defect this project has hit — the 201
  // on a discarded reading, the status line that read healthy while every
  // request failed — was something that reported fine while losing data.
  if (announce || wasOnline != camOnline) {
    Serial.printf("[CAM] %s -> %s (%d)%s\n",
                  camStateName(camWantState),
                  camOnline ? "ok" : "unreachable",
                  camLastStatus,
                  (camOnline && !camViewer) ? "  no viewer - is /vision open?" : "");
  }
}

// ── the network task ─────────────────────────────────────────────────────

#define READING_INTERVAL_MS 60000UL
#define API_RETRY_MS         5000UL
#define TODO_INTERVAL_MS    60000UL
#define TODO_FIRST_TRY_MS    5000UL
#define STATS_INTERVAL_MS   60000UL
#define STATS_FIRST_TRY_MS   7000UL

static void netTask(void *) {
  uint32_t lastReading = 0;
  uint32_t lastAttempt = 0;
  uint32_t lastTodo    = 0;
  uint32_t lastStats   = 0;
  uint32_t lastCamPush = 0;

  netBegin();          // first attempt immediately; retries are handled below

  // ESP-NOW starts HERE, and not in setup(), which is where it was and where it
  // panicked: LoadProhibited on core 1, a null dereference at 0x4c.
  //
  // esp_now_init() needs the Wi-Fi driver already started, and on this board
  // the driver is started by netBegin() above — on core 0, from this task,
  // which setup() creates and then immediately runs past. Calling it from
  // setup() therefore did two wrong things at once: it ran before
  // WiFi.mode(WIFI_STA) had initialised anything, and it raced this task while
  // it was initialising.
  //
  // It stays outside any NET_UP check. It needs the driver, not an association
  // — which is the entire point of using ESP-NOW for this link.
  cadenceNowBegin(false);

  for (;;) {
    netService();
    ntpService();
    readDht();          // core 0 on purpose — see readDht()

    // Deliberately outside the timeValid gate below. Arming the camera needs
    // neither NTP nor the backend: a session started before the clock lands
    // should still start the tracking. The event it queued gets its real UTC
    // timestamp when NTP arrives, but the recording has to begin now.
    if (netState == NET_UP) {
      uint32_t now = millis();
      if (camDirty || now - lastCamPush >= CAM_HEARTBEAT_MS) {
        // A push that fails takes the flag with it, so a state change lost to
        // a camera that was briefly off is not retried immediately — the next
        // heartbeat carries the current state anyway, which is the whole
        // reason the heartbeat runs when idle as well as when running.
        bool announce = camDirty;
        camDirty      = false;
        lastCamPush   = now;
        camPushSession(announce);
      }
    }

    if (netState == NET_UP && timeValid) {
      uint32_t now = millis();

      // Backoff after failures so a dead backend doesn't hammer the radio.
      uint32_t wait = API_RETRY_MS * (apiFailCount > 6 ? 6 : apiFailCount + 1);
      if (now - lastAttempt >= wait) {
        if (evqDepth() > 0) { lastAttempt = now; postSessionEvents(); }
      }

      if (now - lastReading >= READING_INTERVAL_MS) {
        lastReading = now;
        postReading();
      }

      // Retry quickly until the first list lands, then settle to once a
      // minute — a task added on the dashboard should appear without waiting
      // out a full interval on boot.
      if (now - lastTodo >= (todosFetched ? TODO_INTERVAL_MS : TODO_FIRST_TRY_MS)) {
        lastTodo = now;
        refreshTodos();
      }

      // Offset from the todo fetch rather than sharing its timer, so the two
      // TLS handshakes do not land in the same 250 ms tick — netTask also has
      // the DHT read and the camera heartbeat to get through.
      if (now - lastStats >= (statsFetched ? STATS_INTERVAL_MS : STATS_FIRST_TRY_MS)) {
        lastStats = now;
        refreshStats();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ═══════════════════════════════════════════════════ focus timer

enum TimerState : uint8_t { T_IDLE, T_RUNNING, T_PAUSED };
TimerState timerState = T_IDLE;

// Display-only. Spec §4 forbids the firmware persisting totals — the server
// recomputes everything from the event stream. This resets on power loss by
// design. Do not write it to NVS. Do not let it become the source of truth.
uint32_t sessionAccumMs = 0;
uint32_t sessionStartMs = 0;

static uint32_t sessionElapsedMs() {
  return timerState == T_RUNNING
       ? sessionAccumMs + (millis() - sessionStartMs)
       : sessionAccumMs;
}

// Feeds POST /api/ingest/session-events (spec §7). Queues against uptime;
// netTask on core 0 sends it with a real UTC timestamp once NTP has landed.
// Never blocks, never invents a timestamp.
static void emitSessionEvent(const char *type) {
  evqPush(type);

  // The camera board mirrors the timer, so the vision page records only while
  // a session is actually running. Taken from timerState rather than from the
  // event name: they are the same fact, and every caller below sets the state
  // and then emits. Two switches over one fact is how they drift apart.
  camNotify(timerState == T_RUNNING ? 1 : timerState == T_PAUSED ? 2 : 0);

  Serial.printf("[EVENT] queued type=%s uptime_ms=%lu\n",
                type, (unsigned long)millis());
}

static void timerShortPress() {
  switch (timerState) {
    case T_IDLE:
      sessionAccumMs = 0;
      sessionStartMs = millis();
      timerState = T_RUNNING;
      emitSessionEvent("start");
      break;
    case T_RUNNING:
      sessionAccumMs += millis() - sessionStartMs;
      timerState = T_PAUSED;
      emitSessionEvent("pause");
      break;
    case T_PAUSED:
      sessionStartMs = millis();
      timerState = T_RUNNING;
      emitSessionEvent("resume");
      break;
  }
}

static void timerLongPress() {
  if (timerState == T_IDLE) return;
  if (timerState == T_RUNNING) sessionAccumMs += millis() - sessionStartMs;
  timerState = T_IDLE;
  emitSessionEvent("stop");
  sessionAccumMs = 0;
}

// ═══════════════════════════════════════════════════ pages

// UI redesign, approved 2026-08-10, built after Wi-Fi + NTP landed as the
// build order required. Five pages collapse to three: a desk device you glance
// at should not require turning a knob to see the temperature.
enum Page : uint8_t { PAGE_HOME, PAGE_TASKS, PAGE_STATUS, PAGE_COUNT };
uint8_t page = PAGE_HOME;
const char *pageName[PAGE_COUNT] = { "home", "tasks", "status & session" };

// ─────────────────────────────────────────── chrome

static void drawStatusBar() {
  canvas.fillRect(0, 0, SCREEN_W, 22, C_PANEL);
  canvas.drawFastHLine(0, 22, SCREEN_W, C_RULE);

  // Lower case, matching Home. Small caps in a 10px bitmap face lose the
  // ascender and descender shapes that make a word readable at a glance.
  canvas.setFont(&fontLabel);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT, C_PANEL);
  canvas.drawString(pageName[page], 10, 11);

  canvas.setTextDatum(middle_right);
  canvas.setTextColor(timerState == T_RUNNING ? C_GOOD
                    : timerState == T_PAUSED  ? C_WARN : C_DIM, C_PANEL);
  canvas.drawString(timerState == T_RUNNING ? "focus"
                  : timerState == T_PAUSED  ? "paused" : "idle", SCREEN_W - 10, 11);
}

// Moved to the very bottom edge. Home's bottom row now occupies y 220..236,
// where these used to sit at 230 — and losing them was not an option, since
// they are the only thing on the screen saying the knob turns.
//
// fillSmoothCircle rather than fillCircle: at r=2 an aliased circle is a
// square with the corners bitten off.
static void drawPageDots() {
  int cx = SCREEN_W / 2 - (PAGE_COUNT * 12) / 2 + 6;
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == page) canvas.fillSmoothCircle(cx + i * 12, SCREEN_H - 3, 2, C_ACCENT);
    else           canvas.fillSmoothCircle(cx + i * 12, SCREEN_H - 3, 1, C_DIM);
  }
}

// ─────────────────────────────────────────── page bodies

static void fmtHMS(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  snprintf(out, n, "%02lu:%02lu:%02lu",
           (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
           (unsigned long)(s % 60));
}

// The character face is gone, deliberately — a Phase 2 deliverable removed
// after it proved to be the smallest thing on a screen it was meant to anchor.
//
// This is the fourth layout. The three before it failed in instructive ways:
// equal weight on every band (dense and unreadable), then a hierarchy bought by
// deleting most of the content, then the right content and hierarchy spaced
// against font metrics that were simply wrong.
//
// That third failure is the one worth writing down. A u8g2 font's name is its
// CAP HEIGHT, not its box height: helvR08 occupies 13 rows, helvR12 occupies
// 20, logisoso38 occupies 57. Spacing rows by the name overlaps them, and
// because drawString with a two-argument setTextColor paints an opaque
// background, each row then ERASED the bottom of the one above it. The symptom
// on the panel was letters looking cut in half.
//
// Two rules come out of it, and both are load-bearing here:
//   1. Position by baseline, never by box top. A 68px Didone box costs nothing
//      for "8:52" because digits have no descenders.
//   2. Single-argument setTextColor. LovyanGFX treats fore == back as
//      transparent, so glyphs cannot scrub their neighbours.

// Compact duration for chart labels: "45m", "1h20", "2h". A column is ~36px so
// the space in "1h 20m" is what goes.
static void fmtShort(long seconds, char *out, size_t n) {
  long m = (seconds + 30) / 60;
  if (m < 60) { snprintf(out, n, "%ldm", m); return; }
  long h = m / 60, r = m % 60;
  if (r) snprintf(out, n, "%ldh%02ld", h, r);
  else   snprintf(out, n, "%ldh", h);
}

// Widest digit in the current font. The draw and the measure below must agree
// on this or a right-aligned number lands in the wrong place.
static int digitCellWidth() {
  int cell = 0;
  for (char d = '0'; d <= '9'; d++) {
    char one[2] = { d, '\0' };
    int w = canvas.textWidth(one);
    if (w > cell) cell = w;
  }
  return cell;
}

// What drawSteadyNumber will actually occupy — not textWidth(), which measures
// natural proportional advances. Padding digits to a common cell can make the
// drawn string wider than measured, and for a right-aligned number that walks
// it off the panel edge.
static int steadyNumberWidth(const char *s) {
  const int cell = digitCellWidth();
  int total = 0;
  for (const char *p = s; *p; p++) {
    char one[2] = { *p, '\0' };
    total += (*p >= '0' && *p <= '9') ? cell : canvas.textWidth(one);
  }
  return total;
}

// Draw a proportional string with the digits on a fixed pitch, so a number
// that changes every second does not shuffle sideways. Old Standard is a
// Didone: its '1' is far narrower than its '8', and without this the colon in
// the clock wanders visibly every minute.
//
// `y` is the BASELINE. Punctuation and letters keep their natural widths, or
// "1h 47m" comes out spaced like a ransom note.
static int drawSteadyNumber(const char *s, int x, int y, uint16_t col) {
  canvas.setTextColor(col);
  const int cell = digitCellWidth();

  canvas.setTextDatum(baseline_left);
  int cx = x;
  for (const char *p = s; *p; p++) {
    char one[2] = { *p, '\0' };
    if (*p >= '0' && *p <= '9') {
      const int w = canvas.textWidth(one);
      canvas.drawString(one, cx + (cell - w) / 2, y);   // centred in its cell
      cx += cell;
    } else {
      canvas.drawString(one, cx, y);
      cx += canvas.textWidth(one);
    }
  }
  return cx;
}

static bool hasAttentionToday() {
  bool have; long total;
  STATS_TAKE();
  have  = statHasAttn;
  total = statFocused + statDistracted + statAway;
  STATS_GIVE();
  return have && total > 0;
}

// Seven-day focus chart, keeping its axis and labels: this is the second most
// important thing on the page and it can afford them.
//
// Only the peak and today carry a value label — seven would collide, and the
// dashboard's chart made the same call. Today's bar takes the full accent and
// the other six are muted, which is the whole hierarchy this element needs.
static void drawWeekChart(int x, int y, int w, int h) {
  long days[STATS_DAYS], peak;
  bool have;
  STATS_TAKE();
  memcpy(days, statDays, sizeof(days));
  peak = statPeak;
  have = statsFetched;
  STATS_GIVE();

  if (!have) {
    // Never an empty chart on a failed fetch: seven flat columns and a week
    // with no work look identical, and only one of them is true.
    canvas.setFont(&fontText);
    canvas.setTextDatum(baseline_center);
    canvas.setTextColor(C_FAINT);
    canvas.drawString("waiting for the backend", x + w / 2, y + h / 2);
    return;
  }

  static const long steps[] = { 900, 1800, 3600, 5400, 7200, 10800, 14400, 21600 };
  long yMax = steps[0];
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    yMax = steps[i];
    if (peak < steps[i]) break;
  }
  if (peak >= steps[sizeof(steps) / sizeof(steps[0]) - 1]) {
    yMax = ((peak / 3600) + 1) * 3600;
  }

  const int gut   = 24;
  const int plotX = x + gut;
  const int plotW = w - gut;
  const int top   = y + 9;                   // room for the value labels
  const int base  = y + h;
  const int plotH = base - top;
  if (plotH < 12 || plotW < 40) return;

  char buf[12];

  // Axis labels sit on the line they describe. fontTiny has a cap of 8, so a
  // baseline 4px below the line optically centres it.
  canvas.setFont(&fontTiny);
  canvas.setTextDatum(baseline_right);
  canvas.setTextColor(C_FAINT);
  fmtShort(yMax, buf, sizeof(buf));
  canvas.drawString(buf, plotX - 5, top + 4);
  canvas.drawString("0", plotX - 5, base + 4);

  canvas.drawFastHLine(plotX, top,  plotW, C_TRACK);
  canvas.drawFastHLine(plotX, base, plotW, C_RULE);

  int peakIdx = -1;
  for (int i = 0; i < STATS_DAYS; i++) {
    if (peak > 0 && days[i] == peak) { peakIdx = i; break; }
  }

  int wdayToday = -1;
  if (timeValid) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    wdayToday = lt.tm_wday;
  }
  static const char *initials = "smtwtfs";

  const int cellW = plotW / STATS_DAYS;
  int barW = cellW - 12;
  if (barW < 6) barW = 6;

  for (int i = 0; i < STATS_DAYS; i++) {
    const int cx = plotX + i * cellW + cellW / 2;
    const bool isToday = (i == STATS_DAYS - 1);

    int bh = 0;
    if (days[i] > 0) {
      bh = (int)((days[i] * (long)plotH) / yMax);
      if (bh < 1) bh = 1;                    // a worked minute must leave a mark
      if (bh > plotH - 9) bh = plotH - 9;    // always leave the label its room
      canvas.fillRect(cx - barW / 2, base - bh, barW, bh,
                      isToday ? C_ACCENT : C_ACCENT_DIM);
    }

    if (days[i] > 0 && (i == peakIdx || isToday)) {
      fmtShort(days[i], buf, sizeof(buf));
      canvas.setFont(&fontTiny);
      canvas.setTextDatum(baseline_center);
      canvas.setTextColor(isToday ? C_TEXT : C_DIM);
      canvas.drawString(buf, cx, base - bh - 3);
    }

    if (wdayToday >= 0) {
      const int wd = (wdayToday - (STATS_DAYS - 1 - i) + 14) % 7;
      char one[2] = { initials[wd], '\0' };
      canvas.setFont(&fontTiny);
      canvas.setTextDatum(baseline_center);
      canvas.setTextColor(isToday ? C_TEXT : C_FAINT);
      canvas.drawString(one, cx, base + 12);
    }
  }
}

// Today's attention as a part-to-whole across focused / distracted / away.
// The ratio counts focused against distracted and ignores time away, matching
// focus_daily() and the dashboard: being out of the room is not a lapse.
//
// `y` is the vertical centre of the bar.
static void drawAttentionBar(int x, int y, int w) {
  long foc, dis, away;
  int ratio;
  STATS_TAKE();
  foc = statFocused; dis = statDistracted; away = statAway; ratio = statRatio;
  STATS_GIVE();

  const long total = foc + dis + away;
  if (total <= 0) return;

  canvas.setFont(&fontTiny);
  canvas.setTextDatum(baseline_left);
  canvas.setTextColor(C_FAINT);
  canvas.drawString("attention", x, y + 4);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", ratio);
  canvas.setFont(&fontText);
  canvas.setTextDatum(baseline_right);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(buf, x + w, y + 6);
  const int pctW = canvas.textWidth(buf);

  const int barX = x + 54;
  const int barW = (x + w - pctW - 10) - barX;
  if (barW < 24) return;
  const int barH = 10, barY = y - barH / 2;

  canvas.fillRoundRect(barX, barY, barW, barH, barH / 2, C_TRACK);

  const int wf = (int)((foc * (long)barW) / total);
  const int wd = (int)((dis * (long)barW) / total);
  const int wa = barW - wf - wd;             // remainder, so rounding cannot gap

  int cx = barX;
  if (wf > 0) { canvas.fillRect(cx, barY, wf, barH, C_GOOD); cx += wf; }
  if (wd > 0) { canvas.fillRect(cx, barY, wd, barH, C_WARN); cx += wd; }
  if (wa > 0) {  canvas.fillRect(cx, barY, wa, barH, C_DIM); }
}

// Page 01 — Home.
//
// Every y below is a BASELINE, not a box top. Ink extends up by the face's cap
// height and down by its descender, both listed beside the font declarations.
//
//   48    clock (ink 7..48) · pm · elapsed right, state label baseline 16
//   64    date (ink 53..67)
//   74    rule
//   88    reading labels (ink 80..90)
//   106   reading values (ink 94..109)
//   116   chart top, 62px tall with the attention strip or 80px without
//   192   attention bar centre, only when today has data
//   204   rule
//   218   tasks · wi-fi · camera (ink 206..221)
//   237   page dots
static void pageHome() {
  char buf[64];

  const bool running = (timerState == T_RUNNING);
  const bool paused  = (timerState == T_PAUSED);
  const uint16_t stateCol = running ? C_GOOD : paused ? C_WARN : C_DIM;

  // ── clock, date, session ───────────────────────────────────────────────
  struct tm lt;
  bool haveTm = false, isPM = false;

  if (timeValid) {
    time_t now = time(nullptr);
    localtime_r(&now, &lt);
    haveTm = true;
    isPM = lt.tm_hour >= 12;
    int h12 = lt.tm_hour % 12;
    if (h12 == 0) h12 = 12;                  // midnight and noon are 12, not 0
    snprintf(buf, sizeof(buf), "%d:%02d", h12, lt.tm_min);
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }

  canvas.setFont(&fontHero);
  const int clockEnd = drawSteadyNumber(buf, 12, 48, haveTm ? C_TEXT : C_DIM);

  canvas.setFont(&fontText);
  canvas.setTextColor(C_DIM);
  canvas.setTextDatum(baseline_left);
  if (haveTm) {
    canvas.drawString(isPM ? "pm" : "am", clockEnd + 7, 48);

    strftime(buf, sizeof(buf), "%a %d %b", &lt);
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    canvas.drawString(buf, 13, 64);
  } else {
    canvas.drawString("waiting for ntp", 13, 64);
  }

  canvas.setFont(&fontTiny);
  canvas.setTextDatum(baseline_right);
  canvas.setTextColor(stateCol);
  canvas.drawString(running ? "focusing" : paused ? "paused" : "idle", 308, 16);

  fmtHMS(sessionElapsedMs(), buf, sizeof(buf));
  canvas.setFont(&fontTimer);
  drawSteadyNumber(buf, 308 - steadyNumberWidth(buf), 48,
                   (running || paused) ? stateCol : C_FAINT);

  canvas.drawFastHLine(0, 74, SCREEN_W, C_RULE);

  // ── readings ───────────────────────────────────────────────────────────
  // Five of them, value-forward with faint labels above, so the row reads as
  // data rather than as a form to fill in.
  static const int   colX[5]     = { 12, 74, 136, 198, 258 };
  static const char *colLabel[5] = { "temp", "humidity", "feels", "light", "pressure" };
  char colVal[5][14];

  if (hasTemp)     snprintf(colVal[0], sizeof(colVal[0]), "%.1f\xC2\xB0", tempC);
  else             snprintf(colVal[0], sizeof(colVal[0]), "--");
  if (hasHumidity) snprintf(colVal[1], sizeof(colVal[1]), "%.0f%%", humidity);
  else             snprintf(colVal[1], sizeof(colVal[1]), "--");
  if (hasHumidity) snprintf(colVal[2], sizeof(colVal[2]), "%.0f\xC2\xB0", realFeelC);
  else             snprintf(colVal[2], sizeof(colVal[2]), "--");
  if (!luxOk)         snprintf(colVal[3], sizeof(colVal[3]), "--");
  else if (lux > 999) snprintf(colVal[3], sizeof(colVal[3]), "999+");
  else                snprintf(colVal[3], sizeof(colVal[3]), "%.0f", lux);
  if (hasPressure) snprintf(colVal[4], sizeof(colVal[4]), "%.0f", pressHpa);
  else             snprintf(colVal[4], sizeof(colVal[4]), "--");

  const bool colOk[5] = { hasTemp, hasHumidity, hasHumidity, luxOk, hasPressure };

  for (int i = 0; i < 5; i++) {
    canvas.setFont(&fontTiny);
    canvas.setTextDatum(baseline_left);
    canvas.setTextColor(C_FAINT);
    canvas.drawString(colLabel[i], colX[i], 88);

    canvas.setFont(&fontText);
    canvas.setTextDatum(baseline_left);
    canvas.setTextColor(colOk[i] ? C_TEXT : C_FAINT);
    canvas.drawString(colVal[i], colX[i], 106);
  }

  // ── week chart, and attention when today has any ───────────────────────
  // 52 and 70, not 62 and 80. The day initials hang 12px below the chart's
  // baseline, and at the taller heights their ink ran into the attention bar
  // and through the rule below it. Measured, not estimated: see the ink audit.
  const bool attn = hasAttentionToday();
  drawWeekChart(6, 116, SCREEN_W - 12, attn ? 52 : 70);
  if (attn) drawAttentionBar(12, 192, SCREEN_W - 24);

  canvas.drawFastHLine(0, 204, SCREEN_W, C_RULE);

  // ── tasks · wi-fi · camera ─────────────────────────────────────────────
  TODO_TAKE();
  const int  remaining = todoCount;
  const bool fetched   = todosFetched;
  TODO_GIVE();

  canvas.setFont(&fontText);
  canvas.setTextDatum(baseline_left);
  canvas.setTextColor(remaining ? C_DIM : C_FAINT);
  if (!fetched) snprintf(buf, sizeof(buf), "tasks ...");
  else          snprintf(buf, sizeof(buf), "%d task%s",
                         remaining, remaining == 1 ? "" : "s");
  canvas.drawString(buf, 12, 218);

  canvas.drawFastVLine(104, 208, 14, C_RULE);
  canvas.drawFastVLine(208, 208, 14, C_RULE);

  canvas.setTextDatum(baseline_left);
  if (netState == NET_UP) {
    snprintf(buf, sizeof(buf), "wifi %d", WiFi.RSSI());
    canvas.setTextColor(C_FAINT);
  } else if (netState == NET_CONNECTING) {
    snprintf(buf, sizeof(buf), "connecting");
    canvas.setTextColor(C_WARN);
  } else {
    snprintf(buf, sizeof(buf), "no wifi");
    canvas.setTextColor(C_WARN);
  }
  canvas.drawString(buf, 116, 218);

  // Camera. Amber only while a session runs with nothing watching — an
  // unopened /vision tab is the normal resting state, not a fault, and a
  // warning colour permanently lit is how a screen teaches you to ignore it.
  const char *camText;
  uint16_t    camCol;
  if (!camOnline) {
    camText = camLastOk ? "cam lost" : "standalone";
    camCol  = camLastOk ? C_WARN : C_FAINT;
  } else if (!camViewer) {
    camText = "no viewer";
    camCol  = running ? C_WARN : C_FAINT;
  } else if (running) {
    camText = "tracking";
    camCol  = C_GOOD;
  } else {
    camText = "vision ready";
    camCol  = C_FAINT;
  }

  canvas.setTextDatum(baseline_right);
  canvas.setTextColor(camCol);
  canvas.drawString(camText, SCREEN_W - 12, 218);
}

// Page 02 — Tasks. Layout unchanged by the redesign; the contents now come
// from the dashboard rather than a hardcoded array.
//
// Drawn with todoLock held. The critical section is a handful of sprite writes
// and netTask only contends with it once a minute, so the simple thing is also
// the correct thing here — copying rows out first would buy nothing.
static void pageTasks() {
  const int rowH = 30, top = 34, visible = 5;

  TODO_TAKE();

  if (todoCount == 0) {
    bool fetched = todosFetched;
    TODO_GIVE();
    canvas.setFont(&fontBody);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(fetched ? C_DIM : C_WARN, C_BG);
    canvas.drawString(fetched ? "nothing open"
                              : "waiting for the dashboard...",
                      SCREEN_W / 2, SCREEN_H / 2);
    return;
  }

  int first = todoSel - visible / 2;
  if (first < 0) first = 0;
  if (first > todoCount - visible) first = todoCount - visible;
  if (first < 0) first = 0;

  canvas.setFont(&fontBody);
  for (int i = 0; i < visible && first + i < todoCount; i++) {
    int idx = first + i;
    int y = top + i * rowH;
    bool sel = (idx == todoSel);

    if (sel) canvas.fillRoundRect(8, y, SCREEN_W - 16, rowH - 4, 4,
                                  todoScrollMode ? C_ACCENT : C_PANEL);

    uint16_t bg = sel ? (todoScrollMode ? C_ACCENT : C_PANEL) : C_BG;
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(sel && todoScrollMode ? C_BG : C_DIM, bg);
    canvas.drawString("-", 18, y + rowH / 2 - 2);

    // No checkbox. Every item here is open by definition — the endpoint
    // filters done items out — and a checkbox on a device that cannot toggle
    // one would promise an interaction that does not exist.
    //
    // Titles are free text from the dashboard and drawString does not bound
    // itself, so a long one runs off the panel. Clipped by the renderer rather
    // than truncated at a character count: the face is proportional, so "33
    // characters" is one width for "iiii" and quite another for "WWWW", and
    // the old count was measured against Font2 anyway.
    canvas.setTextColor(sel && todoScrollMode ? C_BG : C_TEXT, bg);
    canvas.setClipRect(40, y, SCREEN_W - 16 - 40, rowH - 4);
    canvas.drawString(todos[idx].title, 40, y + rowH / 2 - 2);
    canvas.clearClipRect();
  }

  TODO_GIVE();

  canvas.setFont(&fontLabel);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString(todoScrollMode ? "turn to scroll  -  press to exit"
                                   : "press knob to scroll list",
                    SCREEN_W / 2, SCREEN_H - 22);
}

static void statusRow(int y, const char *label, const char *value, uint16_t col) {
  canvas.setFont(&fontLabel);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString(label, 12, y);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(col, C_BG);
  canvas.drawString(value, SCREEN_W - 12, y);
}

// Page 03 — Status & session. §8's session view in miniature: everything you
// need to answer "why is the dashboard not receiving?" without a serial cable.
static void pageStatus() {
  char buf[64];

  if (netState == NET_UP) {
    snprintf(buf, sizeof(buf), "%s  %ddBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
    statusRow(44, "Wi-Fi", buf, C_GOOD);
  } else if (netState == NET_CONNECTING) {
    snprintf(buf, sizeof(buf), "connecting to %s", WIFI_SSID);
    statusRow(44, "Wi-Fi", buf, C_WARN);
  } else {
    snprintf(buf, sizeof(buf), "down - retry %u", netRetryCount);
    statusRow(44, "Wi-Fi", buf, C_WARN);
  }

  if (timeValid) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d local",
             lt.tm_hour, lt.tm_min, lt.tm_sec);
    statusRow(70, "NTP", buf, C_GOOD);
  } else {
    statusRow(70, "NTP", "waiting for sync", C_WARN);
  }

  // Names the parts actually fitted, not the one the spec asked for.
  statusRow(96, "Env sensor", envName,
            hasHumidity ? C_GOOD : hasTemp ? C_WARN : C_WARN);

  if (luxOk) snprintf(buf, sizeof(buf), "ok  %.0f lx", lux);
  else       snprintf(buf, sizeof(buf), "absent");
  statusRow(122, "BH1750", buf, luxOk ? C_GOOD : C_WARN);

  // Order matters here. The previous version showed "synced Nm ago" whenever
  // there had EVER been a success, so the screen read healthy while every
  // current request was failing — which is exactly how a dead link hid for an
  // afternoon. A live failure now outranks any past success.
  uint8_t depth = evqDepth();
  if (depth > 0) {
    snprintf(buf, sizeof(buf), "%u event%s queued", depth, depth == 1 ? "" : "s");
    statusRow(148, "Backend", buf, C_WARN);
  } else if (apiFailCount > 0) {
    snprintf(buf, sizeof(buf), "failing (%d)", apiLastStatus);
    statusRow(148, "Backend", buf, C_WARN);
  } else if (apiLastOk) {
    uint32_t agoS = (millis() - apiLastOk) / 1000;
    if (agoS < 120) snprintf(buf, sizeof(buf), "synced %lus ago", (unsigned long)agoS);
    else            snprintf(buf, sizeof(buf), "synced %lum ago", (unsigned long)(agoS / 60));
    statusRow(148, "Backend", buf, C_GOOD);
  } else {
    statusRow(148, "Backend", "no sync yet", C_DIM);
  }

  // Current session only. §4 keeps totals on the server, and the hub has no
  // GET client yet, so today's figure belongs here once it can fetch it —
  // inventing one on-device is exactly what §4 forbids.
  fmtHMS(sessionElapsedMs(), buf, sizeof(buf));

  // The vision tier rides on this row rather than claiming one of its own:
  // the page is full at y=200 and the next slot down collides with the page
  // dots at 230. Only shown when the camera is actually reachable — a blank
  // here means standalone, which the pill on Home already says.
  if (camOnline) {
    strncat(buf, camViewer ? "  vision on" : "  no viewer",
            sizeof(buf) - strlen(buf) - 1);
  }

  statusRow(174, "Session", buf,
            timerState == T_RUNNING ? C_GOOD
          : timerState == T_PAUSED  ? C_WARN : C_DIM);

  // Pressure has no cell on Home once humidity takes the middle slot, so it
  // lives here rather than nowhere.
  if (hasPressure) snprintf(buf, sizeof(buf), "%.0f hPa", pressHpa);
  else             snprintf(buf, sizeof(buf), "--");
  statusRow(200, "Pressure", buf, hasPressure ? C_TEXT : C_DIM);
}

// ─────────────────────────────────────────── frame

static void render() {
  canvas.fillScreen(C_BG);

  // Home carries its own chrome — the clock strip is the header, and a title
  // bar on top of it would cost 22px the layout does not have. The other two
  // keep the bar so the page name and timer state stay visible.
  if (page != PAGE_HOME) drawStatusBar();

  switch (page) {
    case PAGE_HOME:   pageHome();   break;
    case PAGE_TASKS:  pageTasks();  break;
    case PAGE_STATUS: pageStatus(); break;
  }

  drawPageDots();
  canvas.pushSprite(0, 0);
}

// ═══════════════════════════════════════════════════ input handling

static void handleInput() {
  static int32_t lastEnc = 0;

  noInterrupts();
  int32_t pos = encPos;
  interrupts();

  int32_t delta = pos - lastEnc;
  if (delta != 0) {
    lastEnc = pos;
    if (todoScrollMode) {
      // constrain(x, 0, -1) on an empty list would pin the selection to -1 and
      // index out of bounds on the next frame.
      TODO_TAKE();
      todoSel = todoCount > 0 ? constrain(todoSel + (int)delta, 0, todoCount - 1) : 0;
      TODO_GIVE();
    } else {
      int p = (int)page + (int)delta;
      while (p < 0) p += PAGE_COUNT;
      page = (uint8_t)(p % PAGE_COUNT);
    }
  }

  switch (encSw.poll()) {
    case BTN_SHORT:
      if (page == PAGE_TASKS) todoScrollMode = !todoScrollMode;
      break;
    case BTN_LONG:
      todoScrollMode = false;
      break;
    default: break;
  }

  switch (timerBtn.poll()) {
    case BTN_SHORT: timerShortPress(); break;
    case BTN_LONG:  timerLongPress();  break;
    default: break;
  }
}

// ═══════════════════════════════════════════════════ setup / loop

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== Cadence hub ===");

  tft.init();
  tft.setRotation(1);                    // locked: 320 x 240 landscape
  tft.setBrightness(0);
  tft.fillScreen(C_BG);

  canvas.setPsram(true);                 // 150 KB frame buffer lives in PSRAM
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("FATAL: sprite allocation failed. Check PSRAM setting.");
    while (true) delay(1000);
  }

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

  // BME280 first, because it is the part the design wants. Its library reads
  // the chip ID and refuses anything that is not 0x60, so this returns false
  // for a BMP280 rather than half-working — which is why the fallback needs a
  // different driver instead of a flag.
  if (bme.begin(ADDR_BME, &Wire)) {
    srcBme = true;
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::FILTER_OFF);
  } else if (bmp.begin(ADDR_BME, BMP280_CHIPID)) {
    // The library defaults to 0x77; this module is strapped to 0x76.
    srcBmp = true;
    // Normal mode rather than forced: forced needs takeForcedMeasurement()
    // before every read, and readSensors() polls on its own 2 s timer.
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X1,
                    Adafruit_BMP280::SAMPLING_X1,
                    Adafruit_BMP280::FILTER_OFF,
                    Adafruit_BMP280::STANDBY_MS_500);
  }

  // A DHT22 supplies exactly what a BMP280 lacks, so it is only probed when
  // humidity is actually missing. A BME280 build pays nothing for this —
  // including the settling delay, which is the expensive part.
  if (!srcBme) {
    dht.setup(PIN_DHT, DHTesp::DHT22);
    delay(1200);                       // DHT22 needs ~1 s after power-up
    TempAndHumidity probe = dht.getTempAndHumidity();
    srcDht = (dht.getStatus() == DHTesp::ERROR_NONE) && !isnan(probe.humidity);
  }

  hasTemp     = srcBme || srcBmp || srcDht;
  hasHumidity = srcBme || srcDht;
  hasPressure = srcBme || srcBmp;

  envName = srcBme            ? "BME280"
          : (srcBmp && srcDht) ? "BMP280 + DHT22"
          : srcBmp             ? "BMP280 - no humidity"
          : srcDht             ? "DHT22 - no pressure"
                               : "absent";

  Serial.printf("Env     : %s  (temp %s, humidity %s, pressure %s)\n",
                envName,
                hasTemp     ? "yes" : "no",
                hasHumidity ? "yes" : "no",
                hasPressure ? "yes" : "no");

  luxOk = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, ADDR_BH1750, &Wire);
  Serial.printf("BH1750  : %s\n", luxOk ? "ok" : "ABSENT");

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT,  INPUT_PULLUP);
  encSw.begin();
  timerBtn.begin();
  encPrev = readEncPins();
  attachInterrupt(PIN_ENC_CLK, encISR, CHANGE);
  attachInterrupt(PIN_ENC_DT,  encISR, CHANGE);

  evqLock   = xSemaphoreCreateMutex();
  todoLock  = xSemaphoreCreateMutex();
  statsLock = xSemaphoreCreateMutex();
  if (!evqLock || !todoLock || !statsLock) {
    Serial.println("FATAL: could not create the queue mutexes.");
    while (true) delay(1000);
  }

  // Core 0 alongside the Wi-Fi stack; loop() keeps core 1 for rendering.
  // 8 KB stack — TLS needs roughly 6 KB of it during the handshake.
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);

  tft.setBrightness(180);
  Serial.printf("Backend: %s\n", API_BASE);
  Serial.println("Turn the knob to change page. Button starts the timer.\n");
}

void loop() {
  // Re-announce the session over the radio. The camera expires a session it
  // has not heard about in SESSION_TTL_MS (90 s), and broadcast ESP-NOW is
  // unacknowledged, so a single dropped packet must not be able to stop a
  // recording. Same 30 s cadence as the HTTP heartbeat, but outside the
  // NET_UP gate that one sits behind.
  static uint32_t lastNowPush = 0;
  if (millis() - lastNowPush >= 30000UL) {
    lastNowPush = millis();
    cadenceNowSend(CADENCE_NOW_SESSION, camWantState, 0);
  }

  static uint32_t lastFrame = 0;

  // Networking lives in netTask on core 0. Nothing blocking belongs here.
  handleInput();
  readSensors();
  applyAutoDim();

  if (millis() - lastFrame >= 66) {      // ~15 fps
    lastFrame = millis();
    render();
  }
}
