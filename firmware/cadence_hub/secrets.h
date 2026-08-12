// Cadence — credentials. Keep this OUT of git and out of report screenshots.
// Add a .gitignore line: secrets.h
//
// Put this file in the same folder as cadence_hub.ino. The Arduino IDE opens
// it as a second tab automatically.

#pragma once

#define WIFI_SSID "3B 2.0"
#define WIFI_PASS "abcd1234"

// Device identity — spec §6 `devices.id`.
#define DEVICE_ID "cadence-hub-01"

// Backend. Not used yet; the endpoints don't exist. Filled in at Phase 3.
#define API_BASE  ""
#define API_TOKEN ""
