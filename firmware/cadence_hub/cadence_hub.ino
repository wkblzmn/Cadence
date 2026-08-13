// ─────────────────────────────────────────────────────────────────────────
//  Cadence — hub firmware skeleton
//  Target : ESP32-S3 DevKit N8R2 + LockerBox 3.2" ILI9341, rotation 1
//  Canvas : 320 x 240 landscape
//
//  In scope : display, pages, encoder nav, buttons, timer state machine,
//             BH1750 auto-dim, BME280 environment, character face,
//             Wi-Fi, NTP, durable event queue, and backend ingest.
//  Not yet  : GET /api/todos, ESP-NOW satellite, camera.
//
//  Threading: rendering and input run in loop() on core 1. All networking
//  runs in netTask on core 0, because HTTPClient blocks and a 2 s TLS
//  handshake in loop() would freeze the UI and drop encoder detents. The
//  event queue is shared across both and guarded by a mutex.
//
//  Libraries (Library Manager):
//    LovyanGFX               by lovyan03
//    Adafruit BME280 Library by Adafruit  (pulls Adafruit_Sensor, BusIO)
//    BH1750                  by Christopher Laws
//
//  Credentials live in secrets.h, in this folder, kept out of git.
// ─────────────────────────────────────────────────────────────────────────

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include <soc/gpio_reg.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

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

// Palette. Warm-neutral, readable at low brightness.
#define C_BG      0x0861      // near-black
#define C_PANEL   0x18E3      // card
#define C_TEXT    0xEF7D      // off-white
#define C_DIM     0x8410      // muted
#define C_ACCENT  0x05DF      // cyan
#define C_WARN    0xFC00      // amber
#define C_GOOD    0x2E6B      // green
#define C_FACE    0x05DF

// ═══════════════════════════════════════════════════ sensors

Adafruit_BME280 bme;
BH1750 lightMeter(ADDR_BH1750);

bool  bmeOk   = false;
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

  if (bmeOk && now - lastSlow > 2000) {
    lastSlow = now;
    tempC     = bme.readTemperature();
    humidity  = bme.readHumidity();
    pressHpa  = bme.readPressure() / 100.0f;
    realFeelC = heatIndexC(tempC, humidity);
  }
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

// ═══════════════════════════════════════════════════ HTTP ingest

// Everything below runs on netTask (core 0) only. HTTPClient blocks, which is
// exactly why it is not allowed anywhere near loop().

uint32_t apiLastOk     = 0;      // millis of the last 2xx
uint16_t apiFailCount  = 0;
int      apiLastStatus = 0;

static int httpPostJson(const char *path, const String &body) {
  String url = String(API_BASE) + path;
  HTTPClient http;
  bool opened;

  if (url.startsWith("https://")) {
    // Certificate validation skipped — documented deviation. Pinning a CA
    // bundle costs ~8 KB of flash and needs rotating when the CA changes.
    static WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(10);
    opened = http.begin(tls, url);
  } else {
    static WiFiClient plain;
    opened = http.begin(plain, url);
  }
  if (!opened) return -1;

  http.setConnectTimeout(5000);
  http.setTimeout(10000);
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
  appendJsonFloat(body, "temp_c",       tempC,     bmeOk); body += ',';
  appendJsonFloat(body, "humidity",     humidity,  bmeOk); body += ',';
  appendJsonFloat(body, "pressure_hpa", pressHpa,  bmeOk); body += ',';
  appendJsonFloat(body, "lux",          lux,       luxOk); body += ',';
  appendJsonFloat(body, "real_feel_c",  realFeelC, bmeOk);
  body += "}";

  httpPostJson("/api/ingest/readings", body);
}

// ── the network task ─────────────────────────────────────────────────────

#define READING_INTERVAL_MS 60000UL
#define API_RETRY_MS         5000UL

static void netTask(void *) {
  uint32_t lastReading = 0;
  uint32_t lastAttempt = 0;

  netBegin();          // first attempt immediately; retries are handled below

  for (;;) {
    netService();
    ntpService();

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

// ═══════════════════════════════════════════════════ todos (stub)

// Replaced by GET /api/todos once the backend exists (spec §7).
struct Todo { const char *title; bool done; };
Todo todos[] = {
  { "Replace the BME280",        false },
  { "Solder 100nF on CLK / DT",  false },
  { "Wi-Fi + NTP on the hub",    false },
  { "POST readings to backend",  false },
  { "Todo sync from dashboard",  false },
  { "Camera board bring-up",     false },
};
const int todoCount = sizeof(todos) / sizeof(todos[0]);
int  todoSel = 0;
bool todoScrollMode = false;

// ═══════════════════════════════════════════════════ pages

// UI redesign, approved 2026-08-10, built after Wi-Fi + NTP landed as the
// build order required. Five pages collapse to three: a desk device you glance
// at should not require turning a knob to see the temperature.
enum Page : uint8_t { PAGE_HOME, PAGE_TASKS, PAGE_STATUS, PAGE_COUNT };
uint8_t page = PAGE_HOME;
const char *pageName[PAGE_COUNT] = { "HOME", "TASKS", "STATUS & SESSION" };

// ─────────────────────────────────────────── chrome

static void drawStatusBar() {
  canvas.fillRect(0, 0, SCREEN_W, 22, C_PANEL);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT, C_PANEL);
  canvas.drawString(pageName[page], 8, 11);

  canvas.setTextDatum(middle_right);
  canvas.setTextColor(timerState == T_RUNNING ? C_GOOD
                    : timerState == T_PAUSED  ? C_WARN : C_DIM, C_PANEL);
  canvas.drawString(timerState == T_RUNNING ? "FOCUS"
                  : timerState == T_PAUSED  ? "PAUSED" : "IDLE", SCREEN_W - 8, 11);
}

static void drawPageDots() {
  int cx = SCREEN_W / 2 - (PAGE_COUNT * 12) / 2 + 6;
  for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == page) canvas.fillCircle(cx + i * 12, SCREEN_H - 10, 3, C_ACCENT);
    else           canvas.fillCircle(cx + i * 12, SCREEN_H - 10, 2, C_DIM);
  }
}

// ─────────────────────────────────────────── page bodies

static void fmtHMS(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  snprintf(out, n, "%02lu:%02lu:%02lu",
           (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
           (unsigned long)(s % 60));
}

// Character face, re-sized for the 296x92 band on Home instead of a full
// screen. Blink on a timer; expression follows the timer state.
//
// Measured against the band (y 82..174): eyes sit at cy-14 with r16, so they
// span 98..130; the smile bottoms out at cy+26+10 plus its 3px pen, so 167.
// Both clear the band with room, which is what keeps the face from colliding
// with the environment row above or the bottom bar below.
static void drawFace(int cx, int cy) {
  static uint32_t nextBlink = 3000;
  static uint32_t blinkEnd  = 0;
  uint32_t now = millis();

  if (now > nextBlink) { blinkEnd = now + 130; nextBlink = now + random(2600, 5200); }
  bool blinking = now < blinkEnd;

  const int eyeDx = 46, eyeR = 16;
  const int ey = cy - 14;

  uint16_t col = timerState == T_RUNNING ? C_GOOD
               : timerState == T_PAUSED  ? C_WARN : C_FACE;

  if (blinking) {
    canvas.fillRoundRect(cx - eyeDx - eyeR, ey - 3, eyeR * 2, 6, 3, col);
    canvas.fillRoundRect(cx + eyeDx - eyeR, ey - 3, eyeR * 2, 6, 3, col);
  } else if (timerState == T_RUNNING) {
    // Narrowed — concentrating.
    canvas.fillRoundRect(cx - eyeDx - eyeR, ey - 8, eyeR * 2, 16, 6, col);
    canvas.fillRoundRect(cx + eyeDx - eyeR, ey - 8, eyeR * 2, 16, 6, col);
  } else {
    canvas.fillCircle(cx - eyeDx, ey, eyeR, col);
    canvas.fillCircle(cx + eyeDx, ey, eyeR, col);
  }

  int my = cy + 26;
  if (timerState == T_RUNNING) {
    canvas.fillRoundRect(cx - 26, my - 3, 52, 6, 3, col);          // flat, focused
  } else if (timerState == T_PAUSED) {
    canvas.fillRoundRect(cx - 20, my - 3, 40, 6, 3, col);
  } else {
    for (int i = -30; i <= 30; i++)                                 // gentle smile
      canvas.fillCircle(cx + i, my + (int)(10 - i * i / 90.0f), 3, col);
  }
}

// Page 01 — Home. Everything ambient on one screen, so the common case needs
// no navigation at all.
//
// Vertical budget for the 240px panel:
//   0..51    top strip   clock left, labelled timer right
//   52..79   environment row
//   82..174  face band, 296 x 92
//   ~200     bottom bar  tasks remaining left, vision pill right
//   230      page dots
static void pageHome() {
  char buf[40];

  // ── top strip ──────────────────────────────────────────────────────────
  // 24-hour is mandatory, not a preference. Font7 HH:MM is ~126px and an
  // AM/PM suffix adds ~24px; with the timer beside it the row overflows 320.
  // Seconds are dropped for the same reason — HH:MM:SS runs to ~200px, which
  // leaves nothing for the timer. Seconds still tick on the Status page.
  canvas.setFont(&fonts::Font7);
  canvas.setTextDatum(middle_left);
  if (timeValid) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    canvas.setTextColor(C_TEXT, C_BG);
  } else {
    snprintf(buf, sizeof(buf), "--:--");
    canvas.setTextColor(C_DIM, C_BG);
  }
  canvas.drawString(buf, 10, 28);

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(timerState == T_RUNNING ? C_GOOD
                    : timerState == T_PAUSED  ? C_WARN : C_DIM, C_BG);
  canvas.drawString(timerState == T_RUNNING ? "FOCUS"
                  : timerState == T_PAUSED  ? "PAUSED" : "IDLE",
                    SCREEN_W - 10, 14);

  // The timer sits in Font4, not Font7, so the clock dominates the strip.
  fmtHMS(sessionElapsedMs(), buf, sizeof(buf));
  canvas.setFont(&fonts::Font4);
  canvas.setTextColor(timerState == T_IDLE ? C_DIM : C_TEXT, C_BG);
  canvas.drawString(buf, SCREEN_W - 10, 38);

  // ── environment row ────────────────────────────────────────────────────
  // Temperature · humidity · real feel. No weather box: §1 is explicit that
  // the device senses the room, and an external API would duplicate readings
  // already taken locally with better provenance.
  canvas.setFont(&fonts::Font2);

  canvas.setTextDatum(middle_left);
  canvas.setTextColor(bmeOk ? C_TEXT : C_DIM, C_BG);
  if (bmeOk) snprintf(buf, sizeof(buf), "%.1f C", tempC);
  else       snprintf(buf, sizeof(buf), "-- C");
  canvas.drawString(buf, 12, 66);

  canvas.setTextDatum(middle_center);
  if (bmeOk) snprintf(buf, sizeof(buf), "%.0f %%", humidity);
  else       snprintf(buf, sizeof(buf), "-- %%");
  canvas.drawString(buf, SCREEN_W / 2, 66);

  canvas.setTextDatum(middle_right);
  canvas.setTextColor(bmeOk ? C_ACCENT : C_DIM, C_BG);
  if (bmeOk) snprintf(buf, sizeof(buf), "feels %.1f C", realFeelC);
  else       snprintf(buf, sizeof(buf), "feels --");
  canvas.drawString(buf, SCREEN_W - 12, 66);

  // ── middle band, 296 x 92 ──────────────────────────────────────────────
  // The redesign costs us the full-screen timer. Rather than add a page back,
  // the band becomes the timer while a session runs: same page, different
  // state, no extra navigation.
  const int bandY = 82, bandH = 92;
  const int bandCx = SCREEN_W / 2, bandCy = bandY + bandH / 2;

  if (timerState == T_IDLE) {
    drawFace(bandCx, bandCy);
  } else {
    fmtHMS(sessionElapsedMs(), buf, sizeof(buf));
    canvas.setFont(&fonts::Font7);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(timerState == T_RUNNING ? C_GOOD : C_WARN, C_BG);
    canvas.drawString(buf, bandCx, bandCy);
  }

  // ── bottom bar ─────────────────────────────────────────────────────────
  int remaining = 0;
  for (int i = 0; i < todoCount; i++) if (!todos[i].done) remaining++;

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(remaining ? C_TEXT : C_DIM, C_BG);
  snprintf(buf, sizeof(buf), "%d task%s left", remaining, remaining == 1 ? "" : "s");
  canvas.drawString(buf, 12, 200);

  // Vision is Phase 4. Until the phone posts focus samples the honest state is
  // standalone, so the pill says so rather than implying a tier that is not
  // running.
  const int pillW = 104, pillH = 22;
  const int pillX = SCREEN_W - 12 - pillW, pillY = 200 - pillH / 2;
  canvas.fillRoundRect(pillX, pillY, pillW, pillH, pillH / 2, C_PANEL);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_DIM, C_PANEL);
  canvas.drawString("STANDALONE", pillX + pillW / 2, 200);
}

// Page 02 — Tasks. Unchanged by the redesign.
static void pageTasks() {
  const int rowH = 30, top = 34, visible = 5;
  int first = todoSel - visible / 2;
  if (first < 0) first = 0;
  if (first > todoCount - visible) first = todoCount - visible;
  if (first < 0) first = 0;

  canvas.setFont(&fonts::Font2);
  for (int i = 0; i < visible && first + i < todoCount; i++) {
    int idx = first + i;
    int y = top + i * rowH;
    bool sel = (idx == todoSel);

    if (sel) canvas.fillRoundRect(8, y, SCREEN_W - 16, rowH - 4, 4,
                                  todoScrollMode ? C_ACCENT : C_PANEL);

    canvas.setTextDatum(middle_left);
    canvas.setTextColor(sel && todoScrollMode ? C_BG
                      : todos[idx].done ? C_DIM : C_TEXT,
                        sel ? (todoScrollMode ? C_ACCENT : C_PANEL) : C_BG);
    canvas.drawString(todos[idx].done ? "[x]" : "[ ]", 18, y + rowH / 2 - 2);
    canvas.drawString(todos[idx].title, 48, y + rowH / 2 - 2);
  }

  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString(todoScrollMode ? "Turn to scroll  -  press to exit"
                                   : "Press knob to scroll list",
                    SCREEN_W / 2, SCREEN_H - 28);
}

static void statusRow(int y, const char *label, const char *value, uint16_t col) {
  canvas.setFont(&fonts::Font2);
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

  statusRow(96, "BME280", bmeOk ? "ok" : "absent", bmeOk ? C_GOOD : C_WARN);

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
  statusRow(174, "Session", buf,
            timerState == T_RUNNING ? C_GOOD
          : timerState == T_PAUSED  ? C_WARN : C_DIM);
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
      todoSel = constrain(todoSel + (int)delta, 0, todoCount - 1);
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

  bmeOk = bme.begin(ADDR_BME, &Wire);
  Serial.printf("BME280  : %s\n", bmeOk ? "ok" : "ABSENT - env page shows --");
  if (bmeOk) {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::FILTER_OFF);
  }

  luxOk = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, ADDR_BH1750, &Wire);
  Serial.printf("BH1750  : %s\n", luxOk ? "ok" : "ABSENT");

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT,  INPUT_PULLUP);
  encSw.begin();
  timerBtn.begin();
  encPrev = readEncPins();
  attachInterrupt(PIN_ENC_CLK, encISR, CHANGE);
  attachInterrupt(PIN_ENC_DT,  encISR, CHANGE);

  evqLock = xSemaphoreCreateMutex();
  if (!evqLock) {
    Serial.println("FATAL: could not create the event queue mutex.");
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
