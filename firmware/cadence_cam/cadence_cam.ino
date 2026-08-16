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

WebServer server(80);

// ═══════════════════════════════════════════════════ camera

static bool cameraBegin() {
  camera_config_t cfg = {};
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
  cfg.frame_size   = FRAMESIZE_VGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  // LATEST, not WHEN_EMPTY: a viewer that falls behind should skip frames
  // rather than watch an ever-growing lag. Gaze data that is three seconds
  // late is worse than no data.
  cfg.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&cfg);
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
  return true;
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
    "<p style='color:#999;font-size:13px'>MJPEG at <code>/stream</code> &middot; "
    "single frame at <code>/jpg</code></p>"
    "<img src='/stream' style='max-width:100%;border-radius:8px'>"
    "<p style='color:#666;font-size:12px'>http://" + ip + "/stream</p>"
    "</body>";
  server.send(200, "text/html", body);
}

static void handleJpg() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { server.send(503, "text/plain", "capture failed"); return; }
  cors();
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.client().write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// multipart/x-mixed-replace — the browser replaces the image in place for
// every part, which is what makes an <img> behave like video.
static void handleStream() {
  WiFiClient client = server.client();
  if (!client.connected()) return;

  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=cadenceframe\r\n"
    "\r\n");

  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;

    client.printf("--cadenceframe\r\n"
                  "Content-Type: image/jpeg\r\n"
                  "Content-Length: %u\r\n\r\n",
                  (unsigned)fb->len);
    size_t sent = client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);

    // A short write means the client went away mid-frame. Without this the
    // loop keeps grabbing frames for a viewer that is no longer there.
    if (sent != fb->len) break;

    // Yields to the Wi-Fi stack. Without a yield the watchdog eventually
    // fires on a saturated link.
    delay(1);
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
  if (!cameraBegin()) {
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
  server.on("/stream", handleStream);
  server.begin();

  Serial.printf("Wi-Fi   : up, ip=%s rssi=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("Stream  : http://%s/stream\n", WiFi.localIP().toString().c_str());
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
