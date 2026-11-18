// Copy this file to secrets.h and fill it in. secrets.h is git-ignored.
#pragma once

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// The homelab dashboard's status JSON (the same one Pringle on the Mac reads).
// Use the server's home-network IP: the ESP32 can't join Tailscale.
#define API_URL  "https://YOUR-SERVER-IP:PORT/api/menubar"
#define API_USER "viewer"
#define API_PASS "viewer-password"
