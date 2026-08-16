// ─────────────────────────────────────────────────────────────────────────
//  Cadence — camera board firmware
//  Target : ESP32-S3-CAM N16R8 + OV3660
//  Role   : put an MJPEG stream on the LAN. Nothing more.
//
//  This board does no vision work. It is a camera on the network; the phone
//  runs MediaPipe against this stream and POSTs focus samples to the backend.
//  Keeping inference off the S3 is the whole reason the vision tier is a
//  separate tier — on-device vision is parked in Argus, not built here.
//
//  Pin map confirmed on hardware by 07_bringup_camera: the layout the ESP32
//  core calls ESP32S3_EYE, which is also the generic S3-CAM layout. Sensor
//  reported PID 0x3660, a genuine OV3660.
//
//  Board settings — these DIFFER from the hub, which is an N8R2:
//    ESP32S3 Dev Module · 16MB flash · **OPI PSRAM** (octal, not quad)
//    USB CDC on Boot Disabled · flash over the UART port
//
//  Credentials live in secrets.h in this folder, kept out of git.
// ─────────────────────────────────────────────────────────────────────────

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

// ═══════════════════════════════════════════════════ pins (07_bringup_camera)

#define PIN_PWDN  -1
#define PIN_RESET -1
#define PIN_XCLK  15
#define PIN_SIOD   4
#define PIN_SIOC   5

#define PIN_D7    16
#define PIN_D6    17
#define PIN_D5    18
#define PIN_D4    12
#define PIN_D3    10
#define PIN_D2     8
#define PIN_D1     9
#define PIN_D0    11

#define PIN_VSYNC  6
#define PIN_HREF   7
#define PIN_PCLK  13

// Two servers on purpose.
//
// WebServer handles one client at a time and a streaming handler never
// returns while its viewer stays connected — so serving MJPEG from port 80
// makes the board deaf to every other request, including its own status
// page. A stale browser tab then locks everything out until a power cycle.
// Espressif's own example splits the ports for exactly this reason.
//
// Port 80: control. Always answers.
// Port 81: MJPEG, driven by its own task.
WebServer  server(80);
WiFiServer streamServer(81);

// The single active viewer. A new connection displaces the old one rather
// than being refused: the common case is moving from the laptop to the phone,
// and being told "busy" by a tab you already closed is useless.
WiFiClient streamClient;

volatile uint32_t framesSent = 0;
volatile uint32_t lastFrameMs = 0;

// ═══════════════════════════════════════════════════ camera

// The live config is kept so the camera can be torn down and brought back up
// at a different frame size. esp_camera_init() allocates the frame buffers
// from this, which is exactly why a resolution change cannot be a simple
// set_framesize() call — see camRestart().
camera_config_t camCfg = {};

// Held around every use of a frame buffer. A reconfigure frees the buffers a
// streaming frame may still be writing into, so the two must never overlap.
SemaphoreHandle_t camLock = nullptr;

static void camConfigure(framesize_t fs) {
  camera_config_t &cfg = camCfg;
  cfg.pin_pwdn     = PIN_PWDN;
  cfg.pin_reset    = PIN_RESET;
  cfg.pin_xclk     = PIN_XCLK;
  cfg.pin_sccb_sda = PIN_SIOD;
  cfg.pin_sccb_scl = PIN_SIOC;
  cfg.pin_d7 = PIN_D7; cfg.pin_d6 = PIN_D6; cfg.pin_d5 = PIN_D5; cfg.pin_d4 = PIN_D4;
  cfg.pin_d3 = PIN_D3; cfg.pin_d2 = PIN_D2; cfg.pin_d1 = PIN_D1; cfg.pin_d0 = PIN_D0;
  cfg.pin_vsync = PIN_VSYNC;
  cfg.pin_href  = PIN_HREF;
  cfg.pin_pclk  = PIN_PCLK;

  cfg.xclk_freq_hz = 20000000;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.ledc_channel = LEDC_CHANNEL_0;

  // VGA, not the OV3660's full 3MP. Face landmarking does not benefit from
  // more pixels, and every extra pixel is bandwidth on a shared Wi-Fi link
  // and decode time on the phone.
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = fs;
  cfg.jpeg_quality = 12;
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  // LATEST, not WHEN_EMPTY: a viewer that falls behind should skip frames
  // rather than watch an ever-growing lag. Gaze data that is three seconds
  // late is worse than no data.
  cfg.grab_mode    = CAMERA_GRAB_LATEST;
}

static bool camStart() {
  esp_err_t err = esp_camera_init(&camCfg);
  if (err != ESP_OK) {
    Serial.printf("FATAL: camera init failed (0x%04x %s)\n", err, esp_err_to_name(err));
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("Camera  : ok, sensor PID 0x%04X\n", s ? s->id.PID : 0);

  // OV3660-specific correction, as in Espressif's own example: this sensor
  // comes up vertically flipped and over-saturated.
  if (s && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  // Favour frame rate over exposure from the start. This device lives on a
  // desk that is often dim — the light sensor next door has been reporting
  // single-digit lux — and left to itself the sensor lengthens exposure until
  // the stream drops to a few frames a second. Gain is the cheaper currency
  // here: the model wants a current frame more than a clean one. Reversible
  // at runtime with /set?fast=0.
  if (s) {
    s->set_aec2(s, 0);                     // no night-mode frame-rate drop
    s->set_gainceiling(s, GAINCEILING_32X);
    s->set_gain_ctrl(s, 1);                // AGC on, to spend that headroom
    s->set_ae_level(s, 2);                 // aim brighter; gain pays, not time
    s->set_brightness(s, 1);
  }
  return true;
}

// A resolution change has to be a full teardown, not set_framesize().
//
// esp_camera_init() allocates the frame buffers for the size in the config.
// set_framesize() changes what the sensor emits but never touches those
// buffers, so going UP writes past the end of them — that is a heap overflow,
// and it wedged this board once already. Going down "fits" but produced zero
// frames in testing, because the DMA descriptors still describe the old
// geometry. Neither direction is safe.
//
// So: stop the world, free everything, allocate for the new size.
static bool camRestart(framesize_t fs) {
  xSemaphoreTake(camLock, portMAX_DELAY);
  esp_camera_deinit();
  camConfigure(fs);
  bool ok = camStart();
  xSemaphoreGive(camLock);
  Serial.printf("[CAM] reconfigured to framesize %d: %s\n", (int)fs, ok ? "ok" : "FAILED");
  return ok;
}

// ═══════════════════════════════════════════════════ http

// Every response carries CORS. This is not optional politeness: the vision
// page draws these frames into a canvas and hands the pixels to MediaPipe,
// and a cross-origin image without CORS taints the canvas so the pixels
// cannot be read back at all. The failure looks like a security error deep
// inside the model, nowhere near the <img> that caused it.
static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

static void handleRoot() {
  cors();
  String ip = WiFi.localIP().toString();
  String body =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Cadence camera</title>"
    "<body style='font-family:system-ui;background:#0d0d0d;color:#eee;margin:0;padding:16px'>"
    "<h1 style='font-size:16px'>Cadence camera</h1>"
    "<p style='color:#999;font-size:13px'>Stream on port 81 &middot; "
    "single frame at <code>/jpg</code> &middot; <a style='color:#3987e5' href='/status'>status</a></p>"
    "<img src='http://" + ip + ":81/stream' style='max-width:100%;border-radius:8px'>"
    "<p style='color:#666;font-size:12px'>http://" + ip + ":81/stream<br>"
    "Only one viewer at a time — opening it somewhere else takes it over.</p>"
    "</body>";
  server.send(200, "text/html", body);
}

static void handleJpg() {
  xSemaphoreTake(camLock, portMAX_DELAY);
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(camLock);
    server.send(503, "text/plain", "capture failed");
    return;
  }
  cors();
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.client().write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(camLock);
}

// Live tuning without a reflash. The stream exists to feed a model, and the
// right size for that is not the right size for looking at, so both need to
// be adjustable while it runs.
static void handleSet() {
  sensor_t *s = esp_camera_sensor_get();
  cors();
  if (!s) { server.send(500, "text/plain", "no sensor"); return; }

  if (server.hasArg("size")) {
    String v = server.arg("size");
    framesize_t fs;
    if      (v == "qqvga") fs = FRAMESIZE_QQVGA;   // 160x120
    else if (v == "qvga")  fs = FRAMESIZE_QVGA;    // 320x240
    else if (v == "cif")   fs = FRAMESIZE_CIF;     // 400x296
    else if (v == "vga")   fs = FRAMESIZE_VGA;     // 640x480
    else if (v == "svga")  fs = FRAMESIZE_SVGA;    // 800x600
    else if (v == "xga")   fs = FRAMESIZE_XGA;     // 1024x768
    else { server.send(400, "text/plain", "size: qqvga|qvga|cif|vga|svga|xga"); return; }

    // Full restart, not set_framesize — the buffers must be reallocated.
    if (!camRestart(fs)) {
      server.send(500, "text/plain", "camera failed to restart at that size");
      return;
    }
    s = esp_camera_sensor_get();     // the old handle is gone after deinit
    if (!s) { server.send(500, "text/plain", "no sensor after restart"); return; }
  }

  if (server.hasArg("q")) {
    int q = server.arg("q").toInt();
    if (q < 4 || q > 63) { server.send(400, "text/plain", "q: 4-63, lower is better"); return; }
    s->set_quality(s, q);
  }

  // Low-light frame rate. This is the control that actually matters on a
  // desk at night: with auto-exposure free to lengthen the exposure, the
  // sensor buys brightness with time and the frame rate collapses — a few
  // lux gives a few fps, which reads as "the stream is laggy" and is in fact
  // the sensor doing its job.
  //
  // fast=1 turns off aec2 (the extended night-mode exposure) and raises the
  // gain ceiling, so the sensor buys brightness with gain instead. Noisier
  // and faster. For gaze and head pose that is the right trade — the model
  // needs a current frame far more than a clean one.
  if (server.hasArg("fast")) {
    bool fast = server.arg("fast") != "0";
    s->set_aec2(s, fast ? 0 : 1);
    s->set_gainceiling(s, fast ? GAINCEILING_32X : GAINCEILING_2X);
    s->set_gain_ctrl(s, 1);        // leave AGC on to use that headroom
    s->set_exposure_ctrl(s, 1);
  }

  // Brightness without spending time. ae_level moves the auto-exposure
  // *target*, and with night mode off and a high gain ceiling the sensor
  // reaches that target using gain — so the picture brightens and the frame
  // rate does not drop. This is the knob to reach for first when the image is
  // dim; raising exposure directly is what made it slow in the first place.
  if (server.hasArg("ae")) {
    int v = server.arg("ae").toInt();
    if (v < -2 || v > 2) { server.send(400, "text/plain", "ae: -2..2"); return; }
    s->set_ae_level(s, v);
  }

  if (server.hasArg("bright")) {
    int v = server.arg("bright").toInt();
    if (v < -2 || v > 2) { server.send(400, "text/plain", "bright: -2..2"); return; }
    s->set_brightness(s, v);
  }

  if (server.hasArg("contrast")) {
    int v = server.arg("contrast").toInt();
    if (v < -2 || v > 2) { server.send(400, "text/plain", "contrast: -2..2"); return; }
    s->set_contrast(s, v);
  }

  // Raising this lets AGC buy more brightness with gain. Costs noise, not fps.
  if (server.hasArg("gainceiling")) {
    int v = server.arg("gainceiling").toInt();
    gainceiling_t g;
    switch (v) {
      case 2:   g = GAINCEILING_2X;   break;
      case 4:   g = GAINCEILING_4X;   break;
      case 8:   g = GAINCEILING_8X;   break;
      case 16:  g = GAINCEILING_16X;  break;
      case 32:  g = GAINCEILING_32X;  break;
      case 64:  g = GAINCEILING_64X;  break;
      case 128: g = GAINCEILING_128X; break;
      default: server.send(400, "text/plain", "gainceiling: 2|4|8|16|32|64|128"); return;
    }
    s->set_gainceiling(s, g);
    s->set_gain_ctrl(s, 1);
  }

  server.send(200, "text/plain", "ok");
}

static void handleStatus() {
  sensor_t *s = esp_camera_sensor_get();
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"rssi\":%d,\"ip\":\"%s\",\"framesize\":%d,\"quality\":%d,"
           "\"ae_level\":%d,\"brightness\":%d,\"agc_gain\":%d,\"aec_value\":%d,"
           "\"frames_sent\":%lu,\"viewer\":%s,\"uptime_s\":%lu}",
           WiFi.RSSI(), WiFi.localIP().toString().c_str(),
           s ? (int)s->status.framesize : -1,
           s ? (int)s->status.quality : -1,
           s ? (int)s->status.ae_level : 0,
           s ? (int)s->status.brightness : 0,
           s ? (int)s->status.agc_gain : 0,
           s ? (int)s->status.aec_value : 0,
           (unsigned long)framesSent,
           (streamClient && streamClient.connected()) ? "true" : "false",
           (unsigned long)(millis() / 1000));
  cors();
  server.send(200, "application/json", buf);
}

// ── the stream task ──────────────────────────────────────────────────────
//
// multipart/x-mixed-replace — the browser replaces the image in place for
// every part, which is what makes an <img> behave like video.

static void sendStreamHeader(WiFiClient &c) {
  c.print("HTTP/1.1 200 OK\r\n"
          "Access-Control-Allow-Origin: *\r\n"
          "Cache-Control: no-store\r\n"
          "Connection: close\r\n"
          "Content-Type: multipart/x-mixed-replace; boundary=cadenceframe\r\n"
          "\r\n");
}

static void streamTask(void *) {
  streamServer.begin();
  streamServer.setNoDelay(true);

  for (;;) {
    WiFiClient incoming = streamServer.available();
    if (incoming) {
      // Drain the request line and headers. Without this the request sits in
      // the socket buffer and the first frame is appended to it.
      uint32_t t0 = millis();
      while (incoming.connected() && millis() - t0 < 1000) {
        String line = incoming.readStringUntil('\n');
        if (line.length() <= 1) break;          // blank line ends the headers
      }

      if (streamClient && streamClient.connected()) streamClient.stop();
      streamClient = incoming;
      streamClient.setNoDelay(true);
      sendStreamHeader(streamClient);
      Serial.println("[STREAM] viewer attached");
    }

    if (!streamClient || !streamClient.connected()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Lock spans the whole frame: the buffer stays owned by the driver until
    // it is returned, and a reconfigure in that window would free it mid-write.
    xSemaphoreTake(camLock, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(camLock);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    streamClient.printf("--cadenceframe\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: %u\r\n\r\n",
                        (unsigned)fb->len);
    size_t sent = streamClient.write(fb->buf, fb->len);
    streamClient.print("\r\n");

    bool short_write = (sent != fb->len);
    esp_camera_fb_return(fb);
    xSemaphoreGive(camLock);

    if (short_write) {
      // The viewer went away mid-frame. Drop it rather than keep grabbing
      // frames for a socket nobody is reading.
      Serial.println("[STREAM] viewer left");
      streamClient.stop();
      continue;
    }

    framesSent++;
    lastFrameMs = millis();
    vTaskDelay(1);      // yield to the Wi-Fi stack
  }
}

// ═══════════════════════════════════════════════════ setup / loop

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n\n=== Cadence camera ===");

  if (!psramFound()) {
    Serial.println("FATAL: no PSRAM. N16R8 needs the OPI PSRAM board setting.");
    while (true) delay(1000);
  }
  camLock = xSemaphoreCreateMutex();
  if (!camLock) {
    Serial.println("FATAL: could not create the camera mutex.");
    while (true) delay(1000);
  }

  camConfigure(FRAMESIZE_VGA);
  if (!camStart()) {
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // sleep adds latency and stutters the stream
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Wi-Fi   : connecting to \"%s\"", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("FATAL: Wi-Fi did not connect. Check secrets.h — and note the");
    Serial.println("S3 has no 5 GHz radio, so the network must be 2.4 GHz.");
    while (true) delay(1000);
  }

  // A DHCP lease moves. mDNS gives the phone a name that does not, so the
  // vision page does not need re-pointing every time the router reshuffles.
  if (MDNS.begin(CAM_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS    : http://%s.local/stream\n", CAM_HOSTNAME);
  }

  server.on("/", handleRoot);
  server.on("/jpg", handleJpg);
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  server.begin();

  // Own task so a connected viewer can never block the control server.
  xTaskCreatePinnedToCore(streamTask, "stream", 4096, nullptr, 1, nullptr, 1);

  String ip = WiFi.localIP().toString();
  Serial.printf("Wi-Fi   : up, ip=%s rssi=%d\n", ip.c_str(), WiFi.RSSI());
  Serial.printf("Stream  : http://%s:81/stream\n", ip.c_str());
  Serial.printf("Preview : http://%s/\n", ip.c_str());
  Serial.printf("Status  : http://%s/status\n", ip.c_str());
  Serial.printf("Tune    : http://%s/set?size=qvga&q=15\n", ip.c_str());
}

void loop() {
  server.handleClient();

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] link lost, reconnecting");
      WiFi.reconnect();
    }
  }
}
