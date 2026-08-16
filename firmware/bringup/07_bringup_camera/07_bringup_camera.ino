// ─────────────────────────────────────────────────────────────────────────
//  07_bringup_camera — identify the ESP32-S3-CAM pin map and sensor
//
//  Target : the CAMERA board (ESP32-S3 N16R8 + OV3660), not the hub.
//
//  A camera board's module marking identifies the MCU, never the carrier
//  board, and the carrier is what decides the camera pins. Guessing gives a
//  silent failure — esp_camera_init() returns an error, or worse it succeeds
//  and every frame is black. So try each known layout in turn and report
//  which one actually brings the sensor up, then read the sensor's own PID to
//  confirm which part is fitted.
//
//  Board settings for THIS board (they differ from the hub):
//    ESP32S3 Dev Module · 16MB flash · **OPI PSRAM** (N16R8 is octal, the
//    hub's N8R2 is quad) · USB CDC on Boot Disabled
//  To flash: jumper GPIO0 to GND, tap reset, release the jumper.
//
//  Prints once and stops — there is nothing to watch change.
// ─────────────────────────────────────────────────────────────────────────

#include "esp_camera.h"

struct CamPins {
  const char *name;
  int pwdn, reset, xclk, sda, scl;
  int d7, d6, d5, d4, d3, d2, d1, d0;
  int vsync, href, pclk;
};

// Taken verbatim from the pin maps the ESP32 core ships in
// libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h, so these are
// not remembered numbers. The first is the de facto layout for generic
// ESP32-S3-CAM boards and is what the core calls ESP32S3_EYE.
static const CamPins CANDIDATES[] = {
  { "ESP32S3_EYE / generic S3-CAM",
    -1, -1, 15, 4, 5,
    16, 17, 18, 12, 10, 8, 9, 11,
    6, 7, 13 },

  { "ESP32S3_CAM_LCD",
    -1, -1, 40, 17, 18,
    39, 41, 42, 12, 3, 14, 47, 13,
    21, 38, 11 },

  { "XIAO_ESP32S3",
    -1, -1, 10, 40, 39,
    48, 11, 12, 14, 16, 18, 17, 15,
    38, 47, 13 },
};
static const int CANDIDATE_COUNT = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

static const char *sensorName(uint16_t pid) {
  switch (pid) {
    case 0x3660: return "OV3660  — the part this build expects";
    case 0x2640: case 0x26: return "OV2640";
    case 0x5640: return "OV5640";
    case 0x7725: case 0x77: return "OV7725";
    case 0x7670: case 0x76: return "OV7670";
    case 0x9650: return "OV9650";
    default: return "unknown sensor";
  }
}

static bool tryPins(const CamPins &p) {
  camera_config_t cfg = {};
  cfg.pin_pwdn     = p.pwdn;
  cfg.pin_reset    = p.reset;
  cfg.pin_xclk     = p.xclk;
  cfg.pin_sccb_sda = p.sda;
  cfg.pin_sccb_scl = p.scl;
  cfg.pin_d7 = p.d7;  cfg.pin_d6 = p.d6;  cfg.pin_d5 = p.d5;  cfg.pin_d4 = p.d4;
  cfg.pin_d3 = p.d3;  cfg.pin_d2 = p.d2;  cfg.pin_d1 = p.d1;  cfg.pin_d0 = p.d0;
  cfg.pin_vsync = p.vsync;
  cfg.pin_href  = p.href;
  cfg.pin_pclk  = p.pclk;

  cfg.xclk_freq_hz = 20000000;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.ledc_channel = LEDC_CHANNEL_0;

  // Small and single-buffered: this only has to prove the bus works, and a
  // modest allocation keeps a failed attempt from starving the next one.
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = FRAMESIZE_QVGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count     = 1;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("  no  (0x%04x %s)\n", err, esp_err_to_name(err));
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  Serial.printf("  YES — sensor PID 0x%04X  %s\n", pid, sensorName(pid));

  // Initialising proves the SCCB control bus. Only a real frame proves the
  // parallel data bus, and a wrong data-pin map fails exactly here.
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("  ...but no frame captured — control bus OK, data pins wrong.");
    esp_camera_deinit();
    return false;
  }
  Serial.printf("  frame captured: %u bytes, %ux%u\n",
                (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height);
  esp_camera_fb_return(fb);
  esp_camera_deinit();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n\n=== Cadence camera bring-up ===");

  if (!psramFound()) {
    Serial.println("WARNING: no PSRAM. N16R8 needs the OPI PSRAM board setting,");
    Serial.println("not QSPI. Frame buffers will fail to allocate without it.");
  } else {
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());
  }

  int found = -1;
  for (int i = 0; i < CANDIDATE_COUNT; i++) {
    Serial.printf("\n[%d/%d] %s\n", i + 1, CANDIDATE_COUNT, CANDIDATES[i].name);
    if (tryPins(CANDIDATES[i])) { found = i; break; }
    delay(200);
  }

  Serial.println("\n────────────────────────────────────────────");
  if (found < 0) {
    Serial.println("No candidate pin map worked.");
    Serial.println("Check the ribbon is seated and locked, that the board has");
    Serial.println("OPI PSRAM selected, and send the board's silkscreen photo.");
  } else {
    const CamPins &p = CANDIDATES[found];
    Serial.printf("USE THIS MAP: %s\n", p.name);
    Serial.printf("  XCLK %d  SIOD %d  SIOC %d\n", p.xclk, p.sda, p.scl);
    Serial.printf("  D7..D0 %d %d %d %d %d %d %d %d\n",
                  p.d7, p.d6, p.d5, p.d4, p.d3, p.d2, p.d1, p.d0);
    Serial.printf("  VSYNC %d  HREF %d  PCLK %d\n", p.vsync, p.href, p.pclk);
  }
}

void loop() {
  delay(10000);
}
