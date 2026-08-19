#pragma once
//
// Cadence — hub/camera session sync over ESP-NOW.
//
// The hub used to tell the camera about the timer button with an HTTP GET to
// /session?state=. That needs both boards associated to the same access point,
// able to route to each other, and it is the first thing to die on a network
// nobody controls — the exact situation this project is walking into.
//
// ESP-NOW needs none of it: no AP, no DHCP, no SSID, no mDNS, no client
// isolation to be defeated. Two ESP32s on the same radio channel, and that is
// the whole dependency.
//
// Addressed to the broadcast MAC on purpose. Unicast would mean each board
// knowing the other's MAC, which means either hardcoding it (wrong the moment a
// board is replaced) or a pairing exchange (more moving parts than the thing it
// would be protecting). The payload carries a magic word so anything else
// broadcasting on the channel is ignored, and the worst a hostile packet can do
// is start or stop a focus timer.
//
// Both sketches include this file. The camera defines cadenceNowOnSession() and
// receives; the hub calls cadenceNowSend() and does not.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define CADENCE_NOW_MAGIC   0xCADE0001UL
#define CADENCE_NOW_CHANNEL 1        // used only when not associated to an AP

// Two message types on one broadcast channel:
//   0  hub -> camera : the timer state, so the button reaches the recording
//   1  camera -> hub : liveness, so the hub's camera tracker works with no
//                      network. It used to learn that from the HTTP response
//                      to camPushSession, which is exactly the thing that does
//                      not exist in a lecture hall.
#define CADENCE_NOW_SESSION 0
#define CADENCE_NOW_STATUS  1

#define CADENCE_NOW_F_STREAMING 0x01   // a USB host is pulling frames

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  type;
  uint8_t  state;      // 0 idle · 1 running · 2 paused
  uint8_t  flags;
  uint32_t seq;
} CadenceNowMsg;

static bool     nowReady   = false;
static uint32_t nowSeq     = 0;
static uint32_t nowRxCount = 0;
static bool     nowLocked  = false;

// Consecutive Wi-Fi failures before we stop chasing it. Six is long enough
// that a router rebooting at home does not trip it, short enough that a
// demonstration settles within a couple of minutes of being switched on.
#define CADENCE_NOW_GIVEUP 6

// A second network is optional; an older secrets.h simply has no WIFI_SSID2.
#ifndef WIFI_SSID2
#define WIFI_SSID2 ""
#define WIFI_PASS2 ""
#endif

// Alternate between the two networks on successive attempts, so neither can
// starve the other. Empty second SSID collapses this to the first.
static inline const char *cadenceSsid(uint8_t attempt) {
  return (attempt % 2 && WIFI_SSID2[0]) ? WIFI_SSID2 : WIFI_SSID;
}
static inline const char *cadencePass(uint8_t attempt) {
  return (attempt % 2 && WIFI_SSID2[0]) ? WIFI_PASS2 : WIFI_PASS;
}

// Both sketches define both of these; each implements the one it cares about
// and leaves the other empty. Simpler than making the header conditional, and
// it keeps the two roles visible side by side in one place.
void cadenceNowOnSession(uint8_t state);                    // camera implements
void cadenceNowOnCamStatus(uint8_t state, uint8_t flags);   // hub implements

static void cadenceNowRecv(const esp_now_recv_info_t *, const uint8_t *data, int len) {
  if (len != (int)sizeof(CadenceNowMsg)) return;
  CadenceNowMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != CADENCE_NOW_MAGIC) return;
  if (m.state > 2) return;
  nowRxCount++;
  if (m.type == CADENCE_NOW_SESSION)     cadenceNowOnSession(m.state);
  else if (m.type == CADENCE_NOW_STATUS) cadenceNowOnCamStatus(m.state, m.flags);
}

// `receiver` registers the callback; the hub passes false.
static bool cadenceNowBegin(bool receiver) {
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

  // When associated, ESP-NOW must use the AP's channel and cannot be moved off
  // it. When not associated, nothing has chosen a channel, so both ends have to
  // be told the same one or they never hear each other.
  if (WiFi.status() != WL_CONNECTED) {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(CADENCE_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW : init failed");
    return false;
  }
  // Both ends register. The hub needs to hear the camera's status as much as
  // the camera needs to hear the hub's button.
  esp_now_register_recv_cb(cadenceNowRecv);

  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6);      // broadcast
  peer.channel = 0;                     // 0 = whatever channel we are on
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW : could not add the broadcast peer");
    return false;
  }

  nowReady = true;
  Serial.printf("ESP-NOW : ready on channel %d, %s\n",
                WiFi.channel(), receiver ? "listening" : "sending");
  return true;
}

// Stop the STA wandering, and nail the radio to the ESP-NOW channel.
//
// Away from a known network both boards retry Wi-Fi forever, and every attempt
// makes the STA scan — which moves the radio off channel and drops ESP-NOW
// traffic in both directions for the duration. The two boards scan on their own
// schedules, so the link degrades precisely where it is the only link there is.
//
// Called after enough consecutive failures to be sure there is nothing to join.
// It does not erase the credentials, so a power cycle goes back to trying.
static void cadenceNowLockChannel() {
  if (nowLocked) return;
  WiFi.disconnect(true, false);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CADENCE_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  nowLocked = true;
  Serial.printf("ESP-NOW : no Wi-Fi to join - locked to channel %d, "
                "radio is ESP-NOW only until reboot\n", CADENCE_NOW_CHANNEL);
}

static void cadenceNowSend(uint8_t type, uint8_t state, uint8_t flags) {
  if (!nowReady) return;
  CadenceNowMsg m = { CADENCE_NOW_MAGIC, type, state, flags, ++nowSeq };
  uint8_t bcast[6];
  memset(bcast, 0xFF, 6);
  esp_now_send(bcast, (const uint8_t *)&m, sizeof(m));
}
