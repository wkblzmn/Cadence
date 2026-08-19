#pragma once
//
// Cadence — JPEG frames over the USB serial link.
//
// Why this exists
// ───────────────
// The Wi-Fi path was measured, from the PC, over a single 40-second capture:
//
//     minimum inter-frame gap     98 ms      (the sensor can do ~10 fps)
//     median                     615 ms
//     95th percentile           2671 ms
//     worst                     6331 ms
//     gaps under 150 ms              2 %
//     link carrying             0.12 Mbps at 9 KB per frame
//
// A stream limited by exposure clusters tightly around one gap. That spread is
// the transport, and no sensor register reaches it — every exposure and quality
// setting tried landed between 0.6 and 1.8 fps.
//
// The second reason matters more than the first. This board is going to a
// university to be demonstrated, on a network nobody controls. Credentials here
// are compile-time, there is no provisioning path, WPA2-Enterprise cannot be
// joined by WiFi.begin() at all, captive portals cannot be answered by a device
// with no browser, and client isolation would hide this board from the laptop
// even after a successful join. Any one of those ends the demonstration.
//
// A cable has none of those properties. The board is already tethered to the
// PC for power, so the link costs nothing that was not already spent.
//
// Framing
// ───────
// UART0 also carries boot logging, so frames are self-describing and the PC
// resynchronises by scanning for the magic. A corrupted or interrupted frame
// fails its CRC and is dropped rather than decoded.
//
//     0..3   magic   CA DE F0 0D
//     4..7   length  uint32, little endian, JPEG bytes to follow
//     8..9   crc16   CCITT-FALSE over the payload
//     10..11 seq     uint16, wraps; for spotting drops from the PC side
//     12..   payload
//
// Commands arrive from the PC as newline-terminated ASCII. Replies are one
// line beginning with '#', which is never the first byte of a frame header.
//
//     C1          start streaming        -> #OK stream on
//     C0          stop streaming         -> #OK stream off
//     P           ping                   -> #PONG <sent> <heap> <psram>
//     S<query>    camera settings, same keys as /set?...
//                 e.g.  Sq=8&fast=1      -> #OK q=8 fast=1
//
// Streaming is off at boot on purpose: it keeps the log readable, and it means
// plugging the board in to reflash it does not have to fight a binary firehose
// for the same UART.

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_camera.h"

// Defined in the sketch, below this include. Declared rather than moved so the
// Wi-Fi stream task and this one demonstrably take the same lock — two mutexes
// guarding one frame buffer would be a bug that only appears under load.
extern SemaphoreHandle_t camLock;

#ifndef SERIAL_BAUD
// CH343 is rated well past this; the ESP32-S3 UART is the nearer limit. At
// 2 Mbaud a 9 KB frame takes about 45 ms on the wire, so the cable ceiling is
// roughly 20 fps — an order of magnitude above what Wi-Fi was delivering.
// Drop to 921600 if the link proves unreliable; the PC tries both.
#define SERIAL_BAUD 2000000
#endif

static const uint8_t USB_MAGIC[4] = {0xCA, 0xDE, 0xF0, 0x0D};

static volatile bool usbStreaming = false;   // guards the log calls below
static uint32_t usbFramesSent = 0;
static uint16_t usbSeq = 0;

// Logging that steps aside while the link is carrying binary. Frames survive an
// interrupting log line — the PC resyncs on the magic and the CRC catches the
// damaged one — but losing a frame to a message nobody is reading is a bad
// trade, so the runtime chatter simply stops for the duration.
#define USBLOG(x)      do { if (!usbStreaming) Serial.println(x); } while (0)
#define USBLOGF(...)   do { if (!usbStreaming) Serial.printf(__VA_ARGS__); } while (0)

static uint16_t usbCrc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ── camera settings, the subset the app drives ──────────────────────────────
//
// Deliberately not a second copy of handleSet(): that one reads its arguments
// from the web server object and cannot be called without one. This parses the
// same key names so a setting means the same thing over either transport, and
// anything it does not recognise is reported rather than ignored — a setting
// silently dropped is worse than one refused, because the PC would go on
// believing it had been applied.
static bool usbApplySetting(const String &key, const String &val, String &err) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) { err = "no sensor"; return false; }
  int v = val.toInt();

  if (key == "q") {
    if (v < 4 || v > 63) { err = "q: 4-63"; return false; }
    s->set_quality(s, v);
  } else if (key == "fast") {
    bool fast = val != "0";
    s->set_aec2(s, fast ? 0 : 1);
    s->set_gainceiling(s, fast ? GAINCEILING_32X : GAINCEILING_2X);
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
  } else if (key == "night") {
    if (val != "0") {
      s->set_gainceiling(s, GAINCEILING_128X);
      s->set_exposure_ctrl(s, 0);
      s->set_aec_value(s, 700);
      s->set_gain_ctrl(s, 0);
      s->set_agc_gain(s, 20);
      s->set_ae_level(s, 2);
      s->set_brightness(s, 2);
    } else {
      s->set_exposure_ctrl(s, 1);
      s->set_gain_ctrl(s, 1);
      s->set_gainceiling(s, GAINCEILING_2X);
      s->set_ae_level(s, 0);
      s->set_brightness(s, 0);
    }
  } else if (key == "gma") {
    s->set_raw_gma(s, val != "0");
  } else if (key == "denoise") {
    if (v < 0 || v > 8) { err = "denoise: 0-8"; return false; }
    s->set_denoise(s, v);
  } else if (key == "ae") {
    if (v < -2 || v > 2) { err = "ae: -2..2"; return false; }
    s->set_ae_level(s, v);
  } else if (key == "bright") {
    if (v < -2 || v > 2) { err = "bright: -2..2"; return false; }
    s->set_brightness(s, v);
  } else if (key == "contrast") {
    if (v < -2 || v > 2) { err = "contrast: -2..2"; return false; }
    s->set_contrast(s, v);
  } else if (key == "exposure") {
    if (val == "auto") { s->set_exposure_ctrl(s, 1); }
    else {
      if (v < 0 || v > 1200) { err = "exposure: 0-1200"; return false; }
      s->set_exposure_ctrl(s, 0);
      s->set_aec_value(s, v);
    }
  } else if (key == "gain") {
    if (val == "auto") { s->set_gain_ctrl(s, 1); }
    else {
      if (v < 0 || v > 30) { err = "gain: 0-30"; return false; }
      s->set_gain_ctrl(s, 0);
      s->set_agc_gain(s, v);
    }
  } else {
    err = "unknown key " + key;
    return false;
  }
  return true;
}

static void usbHandleSet(const String &query) {
  String applied, err, remaining = query;
  bool ok = true;

  while (remaining.length()) {
    int amp = remaining.indexOf('&');
    String pair = (amp < 0) ? remaining : remaining.substring(0, amp);
    remaining   = (amp < 0) ? String()   : remaining.substring(amp + 1);

    int eq = pair.indexOf('=');
    if (eq < 0) continue;
    String k = pair.substring(0, eq), v = pair.substring(eq + 1);
    String e;
    if (usbApplySetting(k, v, e)) {
      applied += k + "=" + v + " ";
    } else {
      ok = false; err = e; break;
    }
  }
  Serial.printf(ok ? "#OK %s\n" : "#ERR %s\n", ok ? applied.c_str() : err.c_str());
}

static void usbHandleCommand(const String &line) {
  if (!line.length()) return;
  char c = line[0];

  if (c == 'C') {
    usbStreaming = (line.length() > 1 && line[1] == '1');
    // Written directly rather than through USBLOG: the acknowledgement for
    // turning streaming *on* has to escape the guard that turning it on just
    // raised, or the PC never hears that its command landed.
    Serial.printf("#OK stream %s\n", usbStreaming ? "on" : "off");
  } else if (c == 'P') {
    Serial.printf("#PONG %lu %lu %lu\n", (unsigned long)usbFramesSent,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram());
  } else if (c == 'S') {
    usbHandleSet(line.substring(1));
  } else {
    Serial.println("#ERR unknown command");
  }
}

// Runs on its own task. Writing a 9 KB frame at 2 Mbaud occupies the wire for
// about 45 ms and Serial.write() blocks for the duration, which is exactly why
// this cannot live on the loop task alongside the web server.
static void usbTask(void *) {
  String line;

  for (;;) {
    // Commands first, so a stop is honoured before another frame goes out.
    while (Serial.available()) {
      char ch = (char)Serial.read();
      if (ch == '\n' || ch == '\r') {
        if (line.length()) { usbHandleCommand(line); line = ""; }
      } else if (line.length() < 200) {
        line += ch;
      }
    }

    if (!usbStreaming) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

    // The same lock the Wi-Fi stream task takes. The buffer belongs to the
    // driver until it is returned, and a reconfigure in that window would free
    // it mid-write.
    xSemaphoreTake(camLock, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(camLock);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint8_t hdr[12];
    memcpy(hdr, USB_MAGIC, 4);
    uint32_t len = (uint32_t)fb->len;
    uint16_t crc = usbCrc16(fb->buf, fb->len);
    memcpy(hdr + 4, &len, 4);
    memcpy(hdr + 8, &crc, 2);
    memcpy(hdr + 10, &usbSeq, 2);
    usbSeq++;

    Serial.write(hdr, sizeof(hdr));
    Serial.write(fb->buf, fb->len);

    esp_camera_fb_return(fb);
    xSemaphoreGive(camLock);

    usbFramesSent++;

    // Yield properly. Each frame is a ~10 KB Serial.write that blocks about
    // 50 ms at 2 Mbaud, nineteen times a second — so at a 1 ms yield this task
    // owned core 1 almost continuously and starved server.handleClient() on
    // the loop task beside it. Measured effect: /status went from 20-60 ms to
    // a 104 ms median with a 7.3 s worst case, past the hub's 1500 ms timeout,
    // which is why the hub showed the camera as offline while it was working
    // perfectly. Five milliseconds costs about two frames a second and hands
    // the web server back a scheduling slot.
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Call BEFORE Serial.begin(). setTxBufferSize has no effect once the port is
// open, and the default 256 bytes would make write() block dozens of times per
// frame waiting for the UART to drain.
static void usbStreamPrepare() {
  Serial.setTxBufferSize(8192);
}

static void usbStreamBegin() {
  // Priority 1, not 2. The Arduino loop task runs at 1 on this core, and a
  // higher-priority task that blocks on the UART preempts it rather than
  // sharing with it.
  xTaskCreatePinnedToCore(usbTask, "usb", 4096, nullptr, 1, nullptr, 1);
}
