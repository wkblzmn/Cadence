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

// Backend base URL, no trailing slash.
//
// DEVELOPMENT: your dev box on the LAN. Take the "Network:" address that
// `npm run dev` prints — NOT localhost, which on the ESP32 means the ESP32.
// Plain HTTP: no TLS handshake, so posts finish in ~200 ms instead of ~2 s,
// and you can watch requests land in the terminal.
#define API_BASE "http://192.168.0.123:3000"

// DEMO: swap to the deployed app. HTTPS, so the firmware calls setInsecure()
// — certificate validation is skipped. Documented deviation, acceptable for a
// course project; pin a CA bundle if this ever ships.
// #define API_BASE "https://cadence-dashboard.vercel.app"

// Must match DEVICE_TOKEN in the dashboard's .env.local, character for
// character (spec §7).
#define API_TOKEN "6ce8b6e4f01e9a616eddb4772c4f072717e41b1048b440d7d60e79277d8bca3e"