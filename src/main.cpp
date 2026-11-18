// Pringle desk panel: shows the homelab's state on the ESP32-2432S028R (CYD).
// Pringle runs while everything is fine (faster when the servers are busy), looks worried
// when something is wrong, and sleeps when the homelab can't be reached. Same moods and
// same /api/menubar data as the Mac menu bar app.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include "secrets.h"
#include "sprites.h"

// ---------- hardware
#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_SCK 25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define LED_R 4
#define LED_G 16
#define LED_B 17

TFT_eSPI tft;
TFT_eSprite pet(&tft);
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// ---------- colors (RGB565)
#define C565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t BG = C565(16, 19, 26), CARD = C565(27, 33, 48), LINE = C565(42, 50, 70), NAVBG = C565(11, 13, 18);
const uint16_t FG = C565(232, 236, 244), DIM = C565(138, 147, 166);
const uint16_t C_OK = C565(76, 211, 135), C_WARN = C565(255, 181, 71), C_BAD = C565(255, 93, 108), ACC = C565(74, 125, 240);
const uint16_t OK_BG = C565(26, 58, 46), BAD_BG = C565(64, 30, 38), OFF_BG = C565(36, 40, 50);

// ---------- layout (landscape 320 x 240)
const int W = 320, H = 240, TOP_H = 20, NAV_Y = 200, NAV_H = 40;

// ---------- data from the homelab
enum Reason { R_NONE, R_WIFI, R_SERVER, R_PASSWORD, R_BUSY_LIMIT, R_BAD_JSON };
struct Dev { char name[22]; bool online; float cpu, mem, temp; };
struct Snap {
  bool valid = false; Reason reason = R_WIFI; int code = 0;
  int nProb = 0; char prob[4][44];
  int nDev = 0; Dev dev[3];
  bool netOn = false; int ms = -1;
  bool hasAds = false, adsOn = false; long blocked = 0, queries = 0;
  bool hasPower = false; int watts = 0;
  bool stale = false; uint32_t okAt = 0;  // stale: the last request failed, showing older data
};
Snap snap;                       // written by the network task, read by the UI
volatile uint32_t snapVersion = 0;
SemaphoreHandle_t snapLock;

// ---------- UI state
enum Tab { T_HOME, T_SERVER, T_ADS, T_ALERTS };
enum Mood { M_RUN, M_SAD, M_SLEEP };
Tab tab = T_HOME;
Mood mood = M_SLEEP;
int frame = 0;
uint32_t drawnVersion = UINT32_MAX, lastFrameAt = 0, lastTouchAt = 0, lastClockAt = 0;
uint32_t runEveryMs = 160;
bool pageDirty = true;
int clockMinute = -1;

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
uint16_t levelColor(float v) { return v >= 85 ? C_BAD : v >= 65 ? C_WARN : C_OK; }

// ================= network task (core 0) =================
void parseInto(Snap& s, const String& body) {
  JsonDocument filter;
  filter["problems"] = true;
  filter["devices"][0]["name"] = true;
  filter["devices"][0]["online"] = true;
  filter["devices"][0]["cpu"] = true;
  filter["devices"][0]["mem"] = true;
  filter["devices"][0]["temp"] = true;
  filter["internet"] = true;
  filter["ads"] = true;
  filter["power"]["watts"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { s.reason = R_BAD_JSON; return; }

  for (JsonVariant p : doc["problems"].as<JsonArray>()) {
    if (s.nProb >= 4) break;
    strlcpy(s.prob[s.nProb++], p.as<const char*>() ? p.as<const char*>() : "?", sizeof(s.prob[0]));
  }
  for (JsonObject d : doc["devices"].as<JsonArray>()) {
    if (s.nDev >= 3) break;
    Dev& v = s.dev[s.nDev++];
    strlcpy(v.name, d["name"] | "?", sizeof(v.name));
    v.online = d["online"] | true;
    v.cpu = d["cpu"].isNull() ? -1 : d["cpu"].as<float>();
    v.mem = d["mem"].isNull() ? -1 : d["mem"].as<float>();
    v.temp = d["temp"].isNull() ? -1 : d["temp"].as<float>();
  }
  s.netOn = doc["internet"]["online"] | false;
  s.ms = doc["internet"]["ms"] | -1;
  if (!doc["ads"].isNull()) {
    s.hasAds = true; s.adsOn = doc["ads"]["on"] | false;
    s.blocked = doc["ads"]["blocked"] | 0L; s.queries = doc["ads"]["queries"] | 0L;
  }
  if (!doc["power"].isNull()) { s.hasPower = true; s.watts = doc["power"]["watts"] | 0; }
  s.valid = true; s.reason = R_NONE;
}

void netTask(void*) {
  for (;;) {
    Snap s;
    uint32_t waitMs = 30000;
    if (WiFi.status() != WL_CONNECTED) {
      s.reason = R_WIFI; waitMs = 3000;
    } else {
      WiFiClientSecure client;
      client.setInsecure();  // the dashboard uses a self-signed certificate on the home network
      HTTPClient http;
      http.setTimeout(8000);
      http.setConnectTimeout(5000);
      if (http.begin(client, API_URL)) {
        http.setAuthorization(API_USER, API_PASS);
        int code = http.GET();
        s.code = code;
        if (code == 200) parseInto(s, http.getString());
        else if (code == 401) { s.reason = R_PASSWORD; waitMs = 600000; }  // dashboard locks out after 10 wrong tries
        else if (code == 429) { s.reason = R_BUSY_LIMIT; waitMs = 300000; }
        else s.reason = R_SERVER;
        http.end();
      } else {
        s.reason = R_SERVER;
      }
    }
    if (s.valid)
      Serial.printf("[net] ok: %d devices, %d problems, cpu %.1f%%, temp %.1fC, internet %d ms, ads %ld/%ld, %d W\n",
                    s.nDev, s.nProb, s.nDev ? s.dev[0].cpu : -1, s.nDev ? s.dev[0].temp : -1, s.ms, s.blocked, s.queries, s.watts);
    else
      Serial.printf("[net] failed: reason=%d http=%d (%s)\n", s.reason, s.code, snap.valid ? "keeping last data" : "no data yet");
    xSemaphoreTake(snapLock, portMAX_DELAY);
    if (s.valid) { s.okAt = millis(); snap = s; }
    else if (snap.valid) { snap.stale = true; snap.reason = s.reason; snap.code = s.code; }  // keep showing the last data
    else snap = s;
    snapVersion++;
    xSemaphoreGive(snapLock);
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
}

// ================= drawing helpers =================
void text(const char* s, int x, int y, uint16_t c, uint8_t size = 1, uint8_t datum = TL_DATUM, uint16_t bg = BG) {
  tft.setTextFont(1);
  tft.setTextSize(size);
  tft.setTextColor(c, bg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, y);
}

void bar(int x, int y, int w, int h, float pct, uint16_t c) {
  tft.fillRoundRect(x, y, w, h, h / 2, LINE);
  int fw = (int)(w * clampf(pct, 0, 100) / 100.0f);
  if (fw > 0) tft.fillRoundRect(x, y, max(fw, h), h, h / 2, c);
}

// Draws a 64x64 sprite scaled by `scale` into the pet sprite buffer, then pushes it.
void drawPet(const uint16_t* img, int x, int y, int scale, uint16_t bg) {
  int size = 64 * scale;
  if (pet.width() != size) { pet.deleteSprite(); pet.createSprite(size, size); }
  pet.fillSprite(bg);
  for (int py = 0; py < 64; py++)
    for (int px = 0; px < 64; px++) {
      uint16_t c = img[py * 64 + px];
      if (c != SPRITE_KEY) pet.fillRect(px * scale, py * scale, scale, scale, c);
    }
  pet.pushSprite(x, y);
}

const uint16_t* moodSprite() { return mood == M_SLEEP ? SPR_SLEEP : mood == M_SAD ? SPR_SAD : SPR_RUN[frame]; }

void setLed(uint16_t c) {  // the RGB LED is active low
  digitalWrite(LED_R, c == C_BAD || c == C_WARN ? LOW : HIGH);
  digitalWrite(LED_G, c == C_OK || c == C_WARN ? LOW : HIGH);
  digitalWrite(LED_B, c == ACC ? LOW : HIGH);
}

// ================= screens =================
void drawTopBar(const Snap& s) {
  tft.fillRect(0, 0, W, TOP_H, BG);
  tft.drawFastHLine(0, TOP_H - 1, W, LINE);
  struct tm t;
  char buf[32];
  if (getLocalTime(&t, 0)) { strftime(buf, sizeof buf, "%H:%M", &t); clockMinute = t.tm_min; }
  else strcpy(buf, "--:--");
  text(buf, 6, 6, FG);
  if (s.valid) {
    if (s.stale) {
      snprintf(buf, sizeof buf, "No update for %lu min", (unsigned long)((millis() - s.okAt) / 60000));
      text(buf, W / 2, 6, C_WARN, 1, TC_DATUM);
    } else {
      if (s.ms >= 0) snprintf(buf, sizeof buf, "Internet %d ms", s.ms); else strcpy(buf, "Internet down");
      text(buf, W / 2, 6, s.ms >= 0 ? DIM : C_BAD, 1, TC_DATUM);
    }
    if (s.hasAds) text(s.adsOn ? "Ads blocked" : "Ads allowed", W - 6, 6, s.adsOn ? DIM : C_WARN, 1, TR_DATUM);
  } else {
    text(WiFi.status() == WL_CONNECTED ? "WiFi ok" : "No WiFi", W - 6, 6, DIM, 1, TR_DATUM);
  }
}

void drawNav(const Snap& s) {
  static const char* names[] = {"Home", "Server", "Ads", "Alerts"};
  tft.fillRect(0, NAV_Y, W, NAV_H, NAVBG);
  tft.drawFastHLine(0, NAV_Y, W, LINE);
  for (int i = 0; i < 4; i++) {
    int cx = i * 80 + 40;
    bool on = i == tab;
    if (on) tft.fillRect(i * 80 + 14, NAV_Y + 1, 52, 3, ACC);
    text(names[i], cx, NAV_Y + 14, on ? FG : DIM, 2, TC_DATUM, NAVBG);
    if (i == T_ALERTS && s.nProb > 0) tft.fillCircle(i * 80 + 70, NAV_Y + 10, 4, C_BAD);
  }
}

void pill(int cx, int y, const char* label, uint16_t fg, uint16_t bg) {
  int w = strlen(label) * 12 + 16;
  tft.fillRoundRect(cx - w / 2, y, w, 22, 11, bg);
  text(label, cx, y + 4, fg, 2, TC_DATUM, bg);
}

void statCard(int x, int y, int w, const char* label, const char* value, float pct, uint16_t c) {
  tft.fillRoundRect(x, y, w, 52, 6, CARD);
  text(label, x + 8, y + 7, DIM, 1, TL_DATUM, CARD);
  text(value, x + w - 8, y + 18, FG, 3, TR_DATUM, CARD);
  bar(x + 8, y + 44, w - 16, 4, pct, c);
}

float avgCpu(const Snap& s) {
  float sum = 0; int n = 0;
  for (int i = 0; i < s.nDev; i++) if (s.dev[i].cpu >= 0) { sum += s.dev[i].cpu; n++; }
  return n ? sum / n : 0;
}

void drawHome(const Snap& s) {
  if (s.nProb) {
    char b[16]; snprintf(b, sizeof b, s.nProb == 1 ? "1 problem" : "%d problems", s.nProb);
    pill(70, 166, b, C_BAD, BAD_BG);
  } else {
    pill(70, 166, "All good", C_OK, OK_BG);
  }
  const Dev* d = s.nDev ? &s.dev[0] : nullptr;
  char b[16];
  float cpu = d && d->cpu >= 0 ? d->cpu : avgCpu(s);
  snprintf(b, sizeof b, "%d%%", (int)round(cpu));
  statCard(144, 28, 168, "CPU", b, cpu, levelColor(cpu));
  if (d && d->temp >= 0) {
    snprintf(b, sizeof b, "%dC", (int)round(d->temp));
    statCard(144, 86, 168, "Temperature", b, d->temp, levelColor(d->temp + 10));
  } else {
    float mem = d ? d->mem : 0;
    snprintf(b, sizeof b, "%d%%", (int)round(mem));
    statCard(144, 86, 168, "Memory", b, mem, levelColor(mem));
  }
  if (s.hasAds && s.queries > 0) {
    float pct = 100.0f * s.blocked / s.queries;
    snprintf(b, sizeof b, "%.1f%%", pct);
    statCard(144, 144, 168, "Ads blocked today", b, pct * 3, ACC);
  } else {
    snprintf(b, sizeof b, "%d W", s.watts);
    statCard(144, 144, 168, "Power now", s.hasPower ? b : "--", s.watts, ACC);
  }
}

void drawServer(const Snap& s) {
  if (!s.nDev) { text("No devices reported", W / 2, 100, DIM, 2, TC_DATUM); return; }
  int n = s.nDev, gap = 6, h = (176 - gap * (n - 1)) / n;
  if (h > 62) h = 62;
  for (int i = 0; i < n; i++) {
    const Dev& d = s.dev[i];
    int y = TOP_H + 4 + i * (h + gap);
    tft.fillRoundRect(6, y, W - 12, h, 6, CARD);
    text(d.name, 14, y + 6, FG, 2, TL_DATUM, CARD);
    text(d.online ? "online" : "OFFLINE", W - 14, y + 8, d.online ? C_OK : C_BAD, 1, TR_DATUM, CARD);
    struct { const char* l; float v; float shade; } rows[] = {{"CPU", d.cpu, d.cpu}, {"RAM", d.mem, d.mem}, {"Temp", d.temp, d.temp + 10}};
    int col = 0;
    for (auto& r : rows) {
      if (r.v < 0) continue;
      int x = 14 + col * 100, ry = y + h - 18;
      char b[12];
      snprintf(b, sizeof b, r.l[0] == 'T' ? "%s %dC" : "%s %d%%", r.l, (int)round(r.v));
      text(b, x, ry - 10, DIM, 1, TL_DATUM, CARD);
      bar(x, ry + 2, 88, 6, r.l[0] == 'T' ? r.v : r.v, levelColor(r.shade));
      col++;
    }
  }
}

void drawAds(const Snap& s) {
  if (!s.hasAds) { text("No ad blocker data", W / 2, 100, DIM, 2, TC_DATUM); return; }
  float pct = s.queries ? 100.0f * s.blocked / s.queries : 0;
  int cx = 70, cy = 110;
  tft.drawSmoothArc(cx, cy, 56, 44, 0, 360, LINE, BG, false);
  int end = (int)(360 * clampf(pct, 0, 100) / 100.0f);
  if (s.adsOn && end > 0) tft.drawSmoothArc(cx, cy, 56, 44, 180, (180 + end) % 360 == 180 ? 179 : (180 + end) % 360, ACC, BG, true);
  char b[24];
  snprintf(b, sizeof b, s.adsOn ? "%.0f%%" : "off", pct);
  text(b, cx, cy - 12, s.adsOn ? FG : C_WARN, 3, TC_DATUM);
  snprintf(b, sizeof b, "%ld", s.blocked);
  text(b, 146, 54, FG, 4);
  snprintf(b, sizeof b, "blocked of %ld", s.queries);
  text(b, 148, 92, DIM, 1);
  text("queries today", 148, 104, DIM, 1);
  pill(212, 140, s.adsOn ? "Blocking on" : "Blocking off", s.adsOn ? C_OK : C_WARN, s.adsOn ? OK_BG : OFF_BG);
}

void drawAlerts(const Snap& s) {
  if (!s.nProb) {
    text("Nothing wrong", 150, 70, FG, 2);
    char b[32];
    if (s.ms >= 0) snprintf(b, sizeof b, "Internet %d ms", s.ms); else strcpy(b, "Internet down");
    text(b, 150, 96, DIM, 1);
    if (s.hasPower) { snprintf(b, sizeof b, "Using %d W now", s.watts); text(b, 150, 110, DIM, 1); }
    return;
  }
  for (int i = 0; i < s.nProb; i++) {
    int y = TOP_H + 6 + i * 44;
    tft.fillRoundRect(6, y, W - 12, 38, 4, CARD);
    tft.fillRect(6, y, 4, 38, C_BAD);
    // wrap long problems onto two lines of 24 characters
    char line[26];
    strlcpy(line, s.prob[i], sizeof line);
    text(line, 18, y + 4, FG, 2, TL_DATUM, CARD);
    if (strlen(s.prob[i]) > 24) text(s.prob[i] + 24, 18, y + 24, DIM, 1, TL_DATUM, CARD);
  }
}

void drawOffline(const Snap& s) {
  tft.fillRect(0, TOP_H, W, H - TOP_H, BG);
  const char* title = "Can't reach the homelab";
  const char* why = "The server isn't answering.";
  const char* fix = "Trying again every 30 s";
  switch (s.reason) {
    case R_WIFI: title = "Connecting to WiFi"; why = WIFI_SSID; fix = "Check the WiFi name and password"; break;
    case R_PASSWORD: title = "Wrong viewer password"; why = "Fix API_PASS in secrets.h"; fix = "Waiting 10 min (lockout)"; break;
    case R_BUSY_LIMIT: title = "Too many tries"; why = "The dashboard asked to wait"; fix = "Trying again in 5 min"; break;
    case R_BAD_JSON: title = "Unexpected reply"; why = "The dashboard sent odd data"; break;
    default: break;
  }
  text(title, W / 2, 168, FG, 2, TC_DATUM);
  text(why, W / 2, 190, DIM, 1, TC_DATUM);
  text(fix, W / 2, 204, DIM, 1, TC_DATUM);
}

void drawPetForTab() {
  if (mood == M_SLEEP) drawPet(SPR_SLEEP, (W - 128) / 2, TOP_H + 12, 2, BG);
  else if (tab == T_HOME) drawPet(moodSprite(), 6, TOP_H + 10, 2, BG);
  else if (tab == T_ALERTS && snap.nProb == 0) drawPet(moodSprite(), 60, 60, 1, BG);
}

void drawAll() {
  Snap s;
  xSemaphoreTake(snapLock, portMAX_DELAY);
  s = snap;
  drawnVersion = snapVersion;
  xSemaphoreGive(snapLock);

  mood = !s.valid ? M_SLEEP : s.nProb ? M_SAD : M_RUN;
  float cpu = avgCpu(s);
  runEveryMs = (uint32_t)max(60.0f, 200.0f - cpu / 100.0f * 140.0f);
  setLed(!s.valid || s.stale ? ACC : s.nProb ? C_BAD : (s.hasAds && !s.adsOn) ? C_WARN : C_OK);

  drawTopBar(s);
  if (!s.valid) { drawOffline(s); drawPetForTab(); return; }
  tft.fillRect(0, TOP_H, W, NAV_Y - TOP_H, BG);
  switch (tab) {
    case T_HOME: drawHome(s); break;
    case T_SERVER: drawServer(s); break;
    case T_ADS: drawAds(s); break;
    case T_ALERTS: drawAlerts(s); break;
  }
  drawNav(s);
  drawPetForTab();
}

// ================= touch =================
// Raw XPT2046 range on the CYD, mapped to the landscape screen.
const int TX_MIN = 200, TX_MAX = 3700, TY_MIN = 240, TY_MAX = 3800;

bool readTouch(int& x, int& y) {
  if (!touch.tirqTouched() || !touch.touched()) return false;
  TS_Point p = touch.getPoint();
  x = constrain(map(p.x, TX_MIN, TX_MAX, 0, W), 0, W - 1);
  y = constrain(map(p.y, TY_MIN, TY_MAX, 0, H), 0, H - 1);
  Serial.printf("[touch] raw %d,%d -> %d,%d\n", p.x, p.y, x, y);
  return true;
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLed(ACC);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(BG);
  pet.setColorDepth(16);
  pet.createSprite(128, 128);

  touchSpi.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  // boot screen
  drawPet(SPR_RUN0, (W - 128) / 2, 30, 2, BG);
  text("Pringle", W / 2, 170, FG, 3, TC_DATUM);
  text("waking up the homelab panel", W / 2, 200, DIM, 1, TC_DATUM);

  snapLock = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTzTime("ICT-7", "pool.ntp.org", "time.google.com");  // Bangkok time
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  delay(1200);
}

void loop() {
  uint32_t now = millis();

  int x, y;
  if (readTouch(x, y) && now - lastTouchAt > 250) {
    lastTouchAt = now;
    if (y >= NAV_Y - 10) {
      Tab t = (Tab)constrain(x / 80, 0, 3);
      if (t != tab) { tab = t; pageDirty = true; }
    }
  }

  if (snapVersion != drawnVersion || pageDirty) { pageDirty = false; drawAll(); }

  struct tm t;
  if (now - lastClockAt > 5000) {
    lastClockAt = now;
    if (getLocalTime(&t, 0) && t.tm_min != clockMinute) pageDirty = true;
  }

  if (mood == M_RUN && now - lastFrameAt > runEveryMs * 2) {
    lastFrameAt = now;
    frame = (frame + 1) % 4;
    drawPetForTab();
  }
  delay(5);
}
