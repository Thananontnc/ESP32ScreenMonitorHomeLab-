// Pringle desk panel (v2 design): shows the homelab's state on the ESP32-2432S028R (CYD).
// Pringle runs while everything is fine (faster when the servers are busy), looks worried
// when something is wrong, and sleeps when the homelab can't be reached. Same moods and
// same /api/menubar data as the Mac menu bar app.
// v2 keeps the v1 layout and adds: a little sky scene behind Pringle (day, night, storm),
// pixel icons, smooth rounded cards and bars, and an icon tab bar.
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

// Adds fake servers named "demo-1", "demo-2"... after the real ones, to preview the
// Server tab with many machines. Set to 0 for normal use.
#ifndef DEMO_SERVERS
#define DEMO_SERVERS 5
#endif

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
const uint16_t BG = C565(14, 17, 25), CARD = C565(26, 32, 47), CARD_HI = C565(44, 53, 74), LINE = C565(40, 48, 68);
const uint16_t TOPBG = C565(24, 30, 45), NAVBG = C565(10, 12, 18), NAV_ACT = C565(28, 42, 78);
const uint16_t FG = C565(234, 238, 246), DIM = C565(132, 142, 162), FAINT = C565(84, 94, 116);
const uint16_t C_OK = C565(76, 211, 135), C_WARN = C565(255, 181, 71), C_BAD = C565(255, 93, 108), ACC = C565(90, 140, 255);
const uint16_t OK_BG = C565(22, 54, 42), BAD_BG = C565(62, 26, 34), WARN_BG = C565(60, 46, 22);
const uint16_t RING_IN = C565(20, 28, 48), DOTS = C565(28, 33, 46), OFFLINE_CARD = C565(30, 24, 32);
const uint16_t DAWN_TOP = C565(84, 86, 168), DAWN_BOT = C565(255, 172, 122), DUSK_TOP = C565(66, 56, 138), DUSK_BOT = C565(255, 132, 98);
const uint16_t SUN_LOW = C565(255, 150, 70), FIREFLY = C565(214, 255, 120), PETAL = C565(255, 150, 190), PETAL2 = C565(255, 255, 255), BIRD = C565(40, 50, 80);

// sky scene behind Pringle
const uint16_t DAY_TOP = C565(70, 132, 226), DAY_BOT = C565(168, 208, 246), SUN = C565(255, 212, 84), CLOUD = C565(244, 247, 252);
const uint16_t NIGHT_TOP = C565(10, 14, 38), NIGHT_BOT = C565(44, 50, 98), STAR = C565(255, 250, 214), MOON = C565(242, 236, 200);
const uint16_t STORM_TOP = C565(52, 58, 72), STORM_BOT = C565(110, 118, 134), RAIN = C565(150, 184, 236), STORMCLOUD = C565(150, 156, 170);
const uint16_t GRASS = C565(84, 168, 92), GRASS_DK = C565(58, 128, 70), GRASS_N = C565(38, 78, 60), GRASS_NDK = C565(28, 58, 46);

// ---------- layout (landscape 320 x 240)
const int W = 320, H = 240, TOP_H = 20, NAV_Y = 200, NAV_H = 40;

// ---------- 8x8 pixel icons
typedef const char* Icon[8];
const Icon IC_CPU = {"..X..X..", ".XXXXXX.", "XX....XX", ".X.XX.X.", ".X.XX.X.", "XX....XX", ".XXXXXX.", "..X..X.."};
const Icon IC_TEMP = {"...XX...", "..X..X..", "..X..X..", "..XXXX..", "..XXXX..", ".XXXXXX.", ".XXXXXX.", "..XXXX.."};
const Icon IC_RAM = {"........", "XXXXXXXX", "X.X.X.XX", "XXXXXXXX", "XXXXXXXX", ".X.X.X.X", "........", "........"};
const Icon IC_SHIELD = {"XXXXXXXX", "X......X", "X..XX..X", "X.XXXX.X", ".X.XX.X.", ".X....X.", "..X..X..", "...XX..."};
const Icon IC_BOLT = {"....XX..", "...XX...", "..XX....", ".XXXXXX.", "....XX..", "...XX...", "..XX....", ".X......"};
const Icon IC_HOME = {"...XX...", "..XXXX..", ".XXXXXX.", "XXXXXXXX", ".X....X.", ".X.XX.X.", ".X.XX.X.", ".XXXXXX."};
const Icon IC_SERVER = {"XXXXXXXX", "X.X....X", "XXXXXXXX", "........", "XXXXXXXX", "X.X....X", "XXXXXXXX", "........"};
const Icon IC_BELL = {"...XX...", "..XXXX..", ".XXXXXX.", ".XXXXXX.", ".XXXXXX.", "XXXXXXXX", "........", "...XX..."};
const Icon IC_WARN = {"...XX...", "...XX...", "..X..X..", "..X..X..", ".X.XX.X.", ".X....X.", "X..XX..X", "XXXXXXXX"};
const Icon IC_GLOBE = {"..XXXX..", ".X.X..X.", "XXXXXXXX", "X..X...X", "X..X...X", "XXXXXXXX", ".X.X..X.", "..XXXX.."};

// ---------- data from the homelab
enum Reason { R_NONE, R_WIFI, R_SERVER, R_PASSWORD, R_BUSY_LIMIT, R_BAD_JSON };
const int MAX_DEV = 12;
struct Dev { char name[22]; bool online; float cpu, mem, temp; int nProb; char prob[44]; };
struct Snap {
  bool valid = false; Reason reason = R_WIFI; int code = 0;
  int nProb = 0; char prob[4][44];
  int nDev = 0; Dev dev[MAX_DEV];
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
uint32_t tick = 0;  // advances every animation step; drives clouds, stars, rain and Zz
uint32_t drawnVersion = UINT32_MAX, lastFrameAt = 0, lastTouchAt = 0, lastClockAt = 0;
uint32_t runEveryMs = 160;
bool pageDirty = true;
int clockMinute = -1;
bool dataShown = false;
int nProbShown = 0;

// Server tab: a 2x2 grid of servers per page; tap one to open its detail view.
const int PER_PAGE = 4;
int serverPage = 0;
int detailDev = -1;   // index into the sorted order, -1 = grid
int order[MAX_DEV];   // servers sorted: offline first, then ones with problems, then the rest
int nOrder = 0;

// Short history for sparklines: one sample per successful poll (30 s), so 32 samples = 16 min.
const int HIST = 32;
struct Hist { char name[22]; uint8_t cpu[HIST], temp[HIST]; int n; };
Hist hist[MAX_DEV];
int nHist = 0;
uint32_t histOkAt = 0;

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
  filter["devices"][0]["problems"] = true;
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
    if (s.nDev >= MAX_DEV) break;
    Dev& v = s.dev[s.nDev++];
    strlcpy(v.name, d["name"] | "?", sizeof(v.name));
    v.online = d["online"] | true;
    v.cpu = d["cpu"].isNull() ? -1 : d["cpu"].as<float>();
    v.mem = d["mem"].isNull() ? -1 : d["mem"].as<float>();
    v.temp = d["temp"].isNull() ? -1 : d["temp"].as<float>();
    JsonArray dp = d["problems"].as<JsonArray>();
    v.nProb = dp.size();
    strlcpy(v.prob, v.nProb ? (dp[0] | "?") : "", sizeof(v.prob));
  }
  for (int i = 0; i < DEMO_SERVERS && s.nDev < MAX_DEV; i++) {
    Dev& v = s.dev[s.nDev++];
    snprintf(v.name, sizeof v.name, "demo-%d", i + 1);
    v.online = i != 2;
    v.cpu = v.online ? 12 + i * 17 % 80 : -1;
    v.mem = v.online ? 30 + i * 11 % 60 : -1;
    v.temp = i % 2 ? -1 : 40 + i * 7;
    v.nProb = (i == 2 || i == 4) ? 1 : 0;
    strlcpy(v.prob, i == 2 ? "No reply for 6 min" : i == 4 ? "Disk 91% full" : "", sizeof v.prob);
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
    snapVersion = snapVersion + 1;
    xSemaphoreGive(snapLock);
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
}

// ================= drawing helpers =================
void text(const char* s, int x, int y, uint16_t c, uint8_t size = 1, uint8_t datum = TL_DATUM, uint16_t bg = BG, uint8_t font = 1) {
  tft.setTextFont(font);
  tft.setTextSize(size);
  tft.setTextColor(c, bg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, y);
}

void icon(TFT_eSPI& g, const Icon& ic, int x, int y, uint16_t c, int scale = 1) {
  for (int r = 0; r < 8; r++)
    for (int col = 0; col < 8; col++)
      if (ic[r][col] == 'X') g.fillRect(x + col * scale, y + r * scale, scale, scale, c);
}

void commas(char* out, size_t n, long v) {
  char raw[16];
  snprintf(raw, sizeof raw, "%ld", v);
  int len = strlen(raw), j = 0;
  for (int i = 0; i < len && j < (int)n - 1; i++) {
    out[j++] = raw[i];
    int left = len - i - 1;
    if (left > 0 && left % 3 == 0 && j < (int)n - 1) out[j++] = ',';
  }
  out[j] = 0;
}

void card(int x, int y, int w, int h) {
  tft.fillSmoothRoundRect(x, y, w, h, 8, CARD, BG);
  tft.drawFastHLine(x + 8, y, w - 16, CARD_HI);  // soft top highlight
}

void bar(int x, int y, int w, int h, float pct, uint16_t c) {
  tft.fillSmoothRoundRect(x, y, w, h, h / 2, LINE, CARD);
  int fw = (int)(w * clampf(pct, 0, 100) / 100.0f);
  if (fw > 0) tft.fillSmoothRoundRect(x, y, max(fw, h), h, h / 2, c, LINE);
}

// value in Font 4 with a small unit after it, right-aligned at xr
void value(const char* num, const char* unit, int xr, int y, uint16_t c, uint16_t bg) {
  tft.setTextFont(1); tft.setTextSize(2);
  int uw = unit[0] ? tft.textWidth(unit) + 3 : 0;
  if (unit[0]) text(unit, xr, y + 8, DIM, 2, TR_DATUM, bg);
  text(num, xr - uw, y, c, 1, TR_DATUM, bg, 4);
}

void pill(int cx, int y, const char* label, uint16_t fg, uint16_t bg) {
  tft.setTextFont(1); tft.setTextSize(2);
  int w = tft.textWidth(label) + 34;
  tft.fillSmoothRoundRect(cx - w / 2, y, w, 24, 12, bg, BG);
  tft.fillSmoothCircle(cx - w / 2 + 13, y + 12, 4, fg, bg);
  text(label, cx - w / 2 + 24, y + 5, fg, 2, TL_DATUM, bg);
}

void wifiBars(int x, int y) {
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  int lit = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) tft.fillRect(x + i * 4, y + 8 - (i + 1) * 2, 3, (i + 1) * 2 + 1, i < lit ? FG : FAINT);
}

int hourNow() {
  struct tm t;
  return getLocalTime(&t, 0) ? t.tm_hour : 12;
}

// Fills an area with the background and a faint dot grid, so empty space has a little texture.
void bgFill(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, BG);
  for (int py = ((y + 11) / 12) * 12; py < y + h; py += 12)
    for (int px = ((x + 11) / 12) * 12; px < x + w; px += 12) tft.drawPixel(px, py, DOTS);
}

Hist* histFor(const char* name) {
  for (int i = 0; i < nHist; i++) if (!strcmp(hist[i].name, name)) return &hist[i];
  if (nHist >= MAX_DEV) return nullptr;
  Hist* h = &hist[nHist++];
  strlcpy(h->name, name, sizeof h->name);
  h->n = 0;
  return h;
}

void pushHist(uint8_t* a, int n, float v) {
  if (n >= HIST) memmove(a, a + 1, HIST - 1);
  a[min(n, HIST - 1)] = (uint8_t)clampf(v, 0, 100);
}

void recordHistory(const Snap& s) {
  if (!s.valid || s.okAt == histOkAt) return;  // only once per new poll
  histOkAt = s.okAt;
  for (int i = 0; i < s.nDev; i++) {
    Hist* h = histFor(s.dev[i].name);
    if (!h) continue;
    pushHist(h->cpu, h->n, max(0.0f, s.dev[i].cpu));
    pushHist(h->temp, h->n, max(0.0f, s.dev[i].temp));
    if (h->n < HIST) h->n++;
  }
}

// Sparkline with a soft area fill and a bright end dot.
void spark(int x, int y, int w, int h, const uint8_t* a, int n, float lo, float hi, uint16_t c, uint16_t bg) {
  if (n < 2) { text("collecting...", x, y + h / 2 - 4, FAINT, 1, TL_DATUM, bg); return; }
  if (hi < 0) {  // auto-scale around the samples, at least 10 units tall
    float mn = 255, mx = 0;
    for (int i = 0; i < n; i++) { mn = min(mn, (float)a[i]); mx = max(mx, (float)a[i]); }
    lo = max(0.0f, mn - 2); hi = max(mx + 2, lo + 10);
  }
  uint16_t fill = tft.alphaBlend(60, c, bg);
  int px = -1, py = -1;
  for (int i = 0; i < n; i++) {
    int xx = x + (int)((long)i * (w - 1) / (HIST - 1));
    int yy = y + h - 1 - (int)((clampf(a[i], lo, hi) - lo) / (hi - lo) * (h - 1));
    tft.drawFastVLine(xx, yy, y + h - yy, fill);
    if (px >= 0) tft.drawLine(px, py, xx, yy, c);
    px = xx; py = yy;
  }
  tft.fillSmoothCircle(px, py, 2, FG, bg);
}

// ---------- Pringle's little world: sky, weather and grass, then Pringle on top
void maskCorners(int size, int r, uint16_t bg) {
  for (int y = 0; y < r; y++)
    for (int x = 0; x < r; x++) {
      int dx = r - x, dy = r - y;
      if (dx * dx + dy * dy > r * r) {
        pet.drawPixel(x, y, bg); pet.drawPixel(size - 1 - x, y, bg);
        pet.drawPixel(x, size - 1 - y, bg); pet.drawPixel(size - 1 - x, size - 1 - y, bg);
      }
    }
}

void cloud(int x, int y, int s, uint16_t c) {
  pet.fillRect(x + 2 * s, y, 4 * s, 2 * s, c);
  pet.fillRect(x, y + 2 * s, 10 * s, 2 * s, c);
  pet.fillRect(x + 6 * s, y + s, 3 * s, s, c);
}

void bird(int x, int y, int s, bool up) {
  pet.fillRect(x, y + (up ? 0 : s), s, s, BIRD);
  pet.fillRect(x + s, y + s, s, s, BIRD);
  pet.fillRect(x + 2 * s, y + (up ? 0 : s), s, s, BIRD);
}

void flower(int x, int y, int s, uint16_t petal) {
  pet.fillRect(x, y - s, s, s, petal);
  pet.fillRect(x - s, y, 3 * s, s, petal);
  pet.fillRect(x, y + s, s, 2 * s, GRASS_DK);
  pet.fillRect(x, y, s, s, SUN);
}

void drawScene(int size, int s) {
  int h = hourNow();
  bool night = mood == M_SLEEP || h >= 19 || h < 5;
  bool dawn = !night && h < 7, dusk = !night && h >= 17;
  bool storm = mood == M_SAD;
  int ground = size - 9 * s;

  if (storm) pet.fillRectVGradient(0, 0, size, ground, STORM_TOP, STORM_BOT);
  else if (night) pet.fillRectVGradient(0, 0, size, ground, NIGHT_TOP, NIGHT_BOT);
  else if (dawn) pet.fillRectVGradient(0, 0, size, ground, DAWN_TOP, DAWN_BOT);
  else if (dusk) pet.fillRectVGradient(0, 0, size, ground, DUSK_TOP, DUSK_BOT);
  else pet.fillRectVGradient(0, 0, size, ground, DAY_TOP, DAY_BOT);

  if (!storm && (dawn || dusk)) {
    // low sun sitting on the horizon, one thin cloud
    pet.fillCircle((dawn ? 14 : 50) * s, ground - 2 * s, 8 * s, SUN_LOW);
    int drift = (tick / 3) % 80;
    cloud((drift - 12) * s, 10 * s, s, DAWN_BOT);
  } else if (storm) {
    int off = (tick * 2 * s) % (14 * s);
    cloud(4 * s, 4 * s, s, STORMCLOUD);
    cloud(36 * s, 8 * s, s, STORMCLOUD);
    for (int i = 0; i < 10; i++) {
      int rx = ((i * 13 + 5) % 60) * s, ry = (8 * s + off + (i * 7 % 14) * s) % ground;
      pet.fillRect(rx, ry, s, 2 * s, RAIN);
    }
  } else if (night) {
    static const uint8_t stars[][2] = {{6, 6}, {18, 14}, {30, 4}, {44, 22}, {58, 28}, {10, 24}, {24, 30}, {38, 12}};
    for (int i = 0; i < 8; i++)
      if (((tick / 3 + i) % 5) != 0) pet.fillRect(stars[i][0] * s, stars[i][1] * s, s, s, STAR);  // one star twinkles at a time
    pet.fillCircle(52 * s, 11 * s, 5 * s, MOON);
    pet.fillCircle(49 * s, 9 * s, 5 * s, NIGHT_TOP);  // crescent
  } else {
    // sun with blinking pixel rays, drifting clouds, a bird now and then
    pet.fillCircle(53 * s, 10 * s, 6 * s, SUN);
    if (tick % 4 < 2) {
      pet.fillRect(53 * s, 1 * s, s, 2 * s, SUN); pet.fillRect(53 * s, 18 * s, s, 2 * s, SUN);
      pet.fillRect(44 * s, 10 * s, 2 * s, s, SUN); pet.fillRect(61 * s, 10 * s, 2 * s, s, SUN);
    }
    int drift = (tick / 2) % 80;
    cloud((drift - 12) * s, 6 * s, s, CLOUD);
    cloud(((drift + 40) % 80 - 12) * s, 18 * s, s, CLOUD);
    int bx = (int)(tick % 140) - 10;
    if (bx < 70) bird(bx * s, 26 * s, s, (tick / 2) % 2);
  }

  // grass with pixel blades
  uint16_t g = night ? GRASS_N : GRASS, gd = night ? GRASS_NDK : GRASS_DK;
  pet.fillRect(0, ground, size, size - ground, g);
  for (int x = 0; x < 64; x += 3) pet.fillRect(x * s, ground - s, s, s, (x / 3) % 2 ? g : gd);
  for (int x = 1; x < 64; x += 5) pet.fillRect(x * s, ground + 4 * s, s, s, gd);
  if (!night && !storm) {
    flower(4 * s, ground + 3 * s, s, PETAL);
    flower(58 * s, ground + 2 * s, s, PETAL2);
    flower(36 * s, ground + 5 * s, s, PETAL);
  }
  if (night && !storm) {  // fireflies wandering above the grass
    for (int i = 0; i < 3; i++) {
      int fx = (int)((i * 23 + tick * (i + 1) / 2) % 64);
      int fy = ground / s - 6 - (int)((tick / 2 + i * 5) % 8);
      if ((tick + i * 3) % 6 < 4) pet.fillRect(fx * s, fy * s, s, s, FIREFLY);
    }
  }
}

void drawPet(const uint16_t* img, int x, int y, int scale) {
  int size = 64 * scale;
  if (pet.width() != size) { pet.deleteSprite(); pet.createSprite(size, size); }
  drawScene(size, scale);
  for (int py = 0; py < 64; py++)
    for (int px = 0; px < 64; px++) {
      uint16_t c = img[py * 64 + px];
      if (c != SPRITE_KEY) pet.fillRect(px * scale, py * scale, scale, scale, c);
    }
  if (mood == M_SLEEP) {  // floating Zz
    int rise = (tick % 12) * scale / 2;
    pet.setTextFont(1); pet.setTextDatum(TL_DATUM); pet.setTextColor(STAR);
    pet.setTextSize(scale); pet.drawString("z", 42 * scale, 26 * scale - rise);
    pet.setTextSize(scale + 1); pet.drawString("Z", 48 * scale, 16 * scale - rise);
  }
  maskCorners(size, 10, BG);
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
  tft.fillRectVGradient(0, 0, W, TOP_H, TOPBG, BG);
  tft.drawFastHLine(0, TOP_H - 1, W, LINE);
  struct tm t;
  char buf[32];
  if (getLocalTime(&t, 0)) { strftime(buf, sizeof buf, "%H:%M", &t); clockMinute = t.tm_min; }
  else strcpy(buf, "--:--");
  text(buf, 6, 6, FG, 1, TL_DATUM, TOPBG);

  if (s.valid) {
    uint16_t dot;
    if (s.stale) {
      snprintf(buf, sizeof buf, "No update for %lu min", (unsigned long)((millis() - s.okAt) / 60000));
      dot = C_WARN;
    } else if (s.ms >= 0) {
      snprintf(buf, sizeof buf, "Internet %d ms", s.ms); dot = C_OK;
    } else {
      strcpy(buf, "Internet down"); dot = C_BAD;
    }
    tft.setTextFont(1); tft.setTextSize(1);
    int tw = tft.textWidth(buf);
    tft.fillSmoothCircle(W / 2 - tw / 2 - 4, 9, 3, dot, TOPBG);
    text(buf, W / 2 + 4, 6, s.stale ? C_WARN : DIM, 1, TC_DATUM, TOPBG);
    if (s.hasAds) {
      icon(tft, IC_SHIELD, W - 76, 5, s.adsOn ? C_OK : C_WARN);
      text(s.adsOn ? "Ads on" : "Ads off", W - 65, 6, s.adsOn ? DIM : C_WARN, 1, TL_DATUM, TOPBG);
    }
  } else {
    text(WiFi.status() == WL_CONNECTED ? "WiFi ok" : "No WiFi", W - 26, 6, DIM, 1, TR_DATUM, TOPBG);
  }
  wifiBars(W - 20, 5);
}

void drawNav(const Snap& s) {
  static const char* names[] = {"Home", "Server", "Ads", "Alerts"};
  static const Icon* icons[] = {&IC_HOME, &IC_SERVER, &IC_SHIELD, &IC_BELL};
  tft.fillRect(0, NAV_Y, W, NAV_H, NAVBG);
  tft.drawFastHLine(0, NAV_Y, W, LINE);
  for (int i = 0; i < 4; i++) {
    int cx = i * 80 + 40;
    bool on = i == tab;
    uint16_t bg = on ? NAV_ACT : NAVBG;
    if (on) tft.fillSmoothRoundRect(cx - 34, NAV_Y + 4, 68, 33, 10, NAV_ACT, NAVBG);
    icon(tft, *icons[i], cx - 8, NAV_Y + 7, on ? ACC : DIM, 2);
    text(names[i], cx, NAV_Y + 26, on ? FG : DIM, 1, TC_DATUM, bg);
    if (i == T_ALERTS && s.nProb > 0) tft.fillSmoothCircle(cx + 12, NAV_Y + 8, 4, C_BAD, bg);
  }
}

// A stat card. With history it shows a 16-minute sparkline instead of a bar.
void statCard(int x, int y, int w, const Icon& ic, const char* label, const char* num, const char* unit, float pct, uint16_t c,
              const uint8_t* h = nullptr, int hn = 0, float lo = 0, float hi = 100) {
  card(x, y, w, 52);
  icon(tft, ic, x + 9, y + 8, c);
  text(label, x + 21, y + 8, DIM, 1, TL_DATUM, CARD);
  value(num, unit, x + w - 10, y + 15, FG, CARD);
  if (h) spark(x + 9, y + 21, 78, 24, h, hn, lo, hi, c, CARD);
  else bar(x + 9, y + 41, w - 18, 5, pct, c);
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
  Hist* hs = d ? histFor(d->name) : nullptr;
  char b[16];
  float cpu = d && d->cpu >= 0 ? d->cpu : avgCpu(s);
  snprintf(b, sizeof b, "%d", (int)round(cpu));
  statCard(144, 28, 168, IC_CPU, "CPU", b, "%", cpu, levelColor(cpu), hs ? hs->cpu : nullptr, hs ? hs->n : 0, 0, -1);
  if (d && d->temp >= 0) {
    snprintf(b, sizeof b, "%d", (int)round(d->temp));
    statCard(144, 86, 168, IC_TEMP, "Temperature", b, "C", d->temp, levelColor(d->temp + 10), hs ? hs->temp : nullptr, hs ? hs->n : 0, 0, -1);
  } else {
    float mem = d ? d->mem : 0;
    snprintf(b, sizeof b, "%d", (int)round(mem));
    statCard(144, 86, 168, IC_RAM, "Memory", b, "%", mem, levelColor(mem));
  }
  if (s.hasAds && s.queries > 0) {
    float pct = 100.0f * s.blocked / s.queries;
    snprintf(b, sizeof b, "%.1f", pct);
    statCard(144, 144, 168, IC_SHIELD, "Ads blocked today", b, "%", pct * 3, ACC);
  } else {
    snprintf(b, sizeof b, "%d", s.watts);
    statCard(144, 144, 168, IC_BOLT, "Power now", s.hasPower ? b : "--", "W", s.watts, ACC);
  }
}

// ---------- Server tab: 2x2 grid of tiles, 4 per page, tap a tile for details
const int TILE_W = 151, TILE_H = 64, TILE_X[2] = {6, 163}, TILE_Y[2] = {50, 120};
char detailName[22] = "";  // server shown in the detail view, "" = grid

void sortServers(const Snap& s) {
  nOrder = 0;
  for (int rank = 0; rank < 3; rank++)  // offline first, then servers with problems, then the rest
    for (int i = 0; i < s.nDev; i++) {
      const Dev& d = s.dev[i];
      int r = !d.online ? 0 : d.nProb ? 1 : 2;
      if (r == rank) order[nOrder++] = i;
    }
}

int pageCount() { return max(1, (nOrder + PER_PAGE - 1) / PER_PAGE); }

// fits a name into a pixel width in Font 2, adding ".." when cut
void fitName(char* out, size_t n, const char* name, int maxW) {
  strlcpy(out, name, n);
  tft.setTextFont(2); tft.setTextSize(1);
  int len = strlen(out);
  if (tft.textWidth(out) <= maxW) return;
  while (len > 1) {
    out[--len] = 0;
    char tmp[26]; snprintf(tmp, sizeof tmp, "%s..", out);
    if (tft.textWidth(tmp) <= maxW) { strlcpy(out, tmp, n); return; }
  }
}

void statusDot(int x, int y, bool online, uint16_t bg) {
  uint16_t st = online ? C_OK : C_BAD, halo = online ? OK_BG : BAD_BG;
  tft.fillSmoothCircle(x, y, 6, halo, bg);
  tft.fillSmoothCircle(x, y, 3, st, halo);
}

void miniRow(int x, int y, int w, const char* label, float v, float shade, bool deg, uint16_t bg) {
  text(label, x, y, DIM, 1, TL_DATUM, bg);
  if (v < 0) { text("--", x + w, y, FAINT, 1, TR_DATUM, bg); return; }
  char b[10];
  snprintf(b, sizeof b, deg ? "%dC" : "%d%%", (int)round(v));
  text(b, x + w, y, FG, 1, TR_DATUM, bg);
  int bx = x + 28, bw = w - 28 - 30;
  tft.fillSmoothRoundRect(bx, y + 2, bw, 4, 2, LINE, bg);
  int fw = (int)(bw * clampf(deg ? v : v, 0, 100) / 100.0f);
  if (fw > 0) tft.fillSmoothRoundRect(bx, y + 2, max(fw, 4), 4, 2, levelColor(shade), LINE);
}

void serverTile(int x, int y, const Dev& d) {
  uint16_t bg = d.online ? CARD : OFFLINE_CARD;
  tft.fillSmoothRoundRect(x, y, TILE_W, TILE_H, 8, bg, BG);
  tft.drawFastHLine(x + 8, y, TILE_W - 16, d.online ? CARD_HI : BAD_BG);
  if (!d.online || d.nProb) tft.drawSmoothRoundRect(x, y, 8, 7, TILE_W - 1, TILE_H - 1, d.online ? C_WARN : C_BAD, BG);
  statusDot(x + 13, y + 13, d.online, bg);
  char name[24];
  fitName(name, sizeof name, d.name, TILE_W - 26 - (d.nProb ? 14 : 4));
  text(name, x + 24, y + 5, FG, 1, TL_DATUM, bg, 2);
  if (d.nProb) icon(tft, IC_WARN, x + TILE_W - 15, y + 9, d.online ? C_WARN : C_BAD);
  if (!d.online) {
    text("OFFLINE", x + TILE_W / 2, y + 30, C_BAD, 2, TC_DATUM, bg);
    if (d.nProb) { char p[24]; strlcpy(p, d.prob, sizeof p); text(p, x + TILE_W / 2, y + 50, DIM, 1, TC_DATUM, bg); }
    return;
  }
  miniRow(x + 10, y + 26, TILE_W - 20, "CPU", d.cpu, d.cpu, false, bg);
  miniRow(x + 10, y + 38, TILE_W - 20, "RAM", d.mem, d.mem, false, bg);
  miniRow(x + 10, y + 50, TILE_W - 20, "Temp", d.temp, d.temp + 10, true, bg);
}

void chip(int x, int y, int w, int h, const char* label, bool enabled) {
  tft.fillSmoothRoundRect(x, y, w, h, h / 2, enabled ? NAV_ACT : CARD, BG);
  text(label, x + w / 2, y + h / 2 - 7, enabled ? FG : FAINT, 2, TC_DATUM, enabled ? NAV_ACT : CARD);
}

void drawServerGrid(const Snap& s) {
  int pages = pageCount();
  if (serverPage >= pages) serverPage = pages - 1;
  int up = 0;
  for (int i = 0; i < s.nDev; i++) up += s.dev[i].online;
  int down = s.nDev - up;

  // header: paging chips and a one-line summary
  if (pages > 1) { chip(6, 24, 46, 22, "<", true); chip(268, 24, 46, 22, ">", true); }
  char b[40];
  snprintf(b, sizeof b, "%d server%s", s.nDev, s.nDev == 1 ? "" : "s");
  tft.setTextFont(1); tft.setTextSize(1);
  char upS[12], downS[12];
  snprintf(upS, sizeof upS, "%d up", up);
  snprintf(downS, sizeof downS, "%d down", down);
  int wTotal = tft.textWidth(b) + 14 + tft.textWidth(upS) + (down ? 14 + tft.textWidth(downS) : 0);
  int x = W / 2 - wTotal / 2;
  icon(tft, IC_SERVER, x - 12, 26, DIM);
  text(b, x, 27, FG); x += tft.textWidth(b) + 8;
  tft.fillSmoothCircle(x + 2, 30, 2, C_OK, BG); x += 6;
  text(upS, x, 27, C_OK); x += tft.textWidth(upS) + 8;
  if (down) { tft.fillSmoothCircle(x + 2, 30, 2, C_BAD, BG); text(downS, x + 6, 27, C_BAD); }
  if (pages > 1)
    for (int p = 0; p < pages; p++) {
      int dx = W / 2 - (pages - 1) * 6 + p * 12;
      if (p == serverPage) tft.fillSmoothRoundRect(dx - 5, 39, 10, 4, 2, ACC, BG);
      else tft.fillSmoothCircle(dx, 41, 2, FAINT, BG);
    }

  for (int k = 0; k < PER_PAGE; k++) {
    int idx = serverPage * PER_PAGE + k;
    int tx = TILE_X[k % 2], ty = TILE_Y[k / 2];
    if (idx < nOrder) serverTile(tx, ty, s.dev[order[idx]]);
    else if (pages > 1 || k < 2) {  // empty slot: dashed outline so the grid keeps its shape
      for (int i = 8; i < TILE_W - 8; i += 6) { tft.drawFastHLine(tx + i, ty, 3, LINE); tft.drawFastHLine(tx + i, ty + TILE_H - 1, 3, LINE); }
      for (int i = 8; i < TILE_H - 8; i += 6) { tft.drawFastVLine(tx, ty + i, 3, LINE); tft.drawFastVLine(tx + TILE_W - 1, ty + i, 3, LINE); }
    }
  }
}

void detailRow(int x, int y, int w, const Icon& ic, const char* label, float v, float shade, bool deg) {
  uint16_t c = v < 0 ? FAINT : levelColor(shade);
  icon(tft, ic, x, y + 2, c);
  text(label, x + 14, y + 2, DIM, 1, TL_DATUM, CARD);
  char b[10];
  if (v < 0) strcpy(b, "--"); else snprintf(b, sizeof b, deg ? "%dC" : "%d%%", (int)round(v));
  text(b, x + w, y - 2, FG, 1, TR_DATUM, CARD, 2);
  int bx = x + 52, bw = w - 52 - 46;
  tft.fillSmoothRoundRect(bx, y + 2, bw, 8, 4, LINE, CARD);
  int fw = v < 0 ? 0 : (int)(bw * clampf(v, 0, 100) / 100.0f);
  if (fw > 0) tft.fillSmoothRoundRect(bx, y + 2, max(fw, 8), 8, 4, c, LINE);
}

void drawServerDetail(const Snap& s, const Dev& d) {
  chip(6, 24, 62, 26, "< Back", true);
  char name[24];
  fitName(name, sizeof name, d.name, 160);
  text(name, 78, 29, FG, 1, TL_DATUM, BG, 2);
  statusDot(W - 64, 37, d.online, BG);
  text(d.online ? "online" : "offline", W - 54, 33, d.online ? C_OK : C_BAD);

  card(6, 56, W - 12, 130);
  detailRow(18, 68, W - 36, IC_CPU, "CPU", d.cpu, d.cpu, false);
  detailRow(18, 90, W - 36, IC_RAM, "RAM", d.mem, d.mem, false);
  detailRow(18, 112, W - 36, IC_TEMP, "Temp", d.temp, d.temp + 10, true);

  int y = 136;
  if (d.nProb) {
    icon(tft, IC_WARN, 18, y, C_BAD);
    char p[40]; strlcpy(p, d.prob, sizeof p);
    text(p, 32, y, C_BAD, 1, TL_DATUM, CARD);
    y += 14;
  }
  Hist* h = histFor(d.name);
  int sh = 180 - y - 12;
  text("CPU, last 16 min", 18, y, FAINT, 1, TL_DATUM, CARD);
  text("Temp, last 16 min", 166, y, FAINT, 1, TL_DATUM, CARD);
  if (h) {
    spark(18, y + 12, 136, sh, h->cpu, h->n, 0, -1, levelColor(d.cpu), CARD);
    if (d.temp >= 0) spark(166, y + 12, 136, sh, h->temp, h->n, 0, -1, levelColor(d.temp + 10), CARD);
  }
}

void drawServer(const Snap& s) {
  sortServers(s);
  if (!s.nDev) { text("No servers reported", W / 2, 100, DIM, 2, TC_DATUM); return; }
  if (detailName[0]) {
    for (int i = 0; i < s.nDev; i++)
      if (!strcmp(s.dev[i].name, detailName)) { drawServerDetail(s, s.dev[i]); return; }
    detailName[0] = 0;  // that server is gone: fall back to the grid
  }
  drawServerGrid(s);
}

// Taps inside the Server tab. Returns true when the page needs a redraw.
bool serverTap(int x, int y) {
  if (detailName[0]) {
    if (y < 56 && x < 110) { detailName[0] = 0; return true; }
    return false;
  }
  int pages = pageCount();
  if (y < 48 && pages > 1) {
    if (x < 80) { serverPage = (serverPage + pages - 1) % pages; return true; }
    if (x > 240) { serverPage = (serverPage + 1) % pages; return true; }
    return false;
  }
  for (int k = 0; k < PER_PAGE; k++) {
    int tx = TILE_X[k % 2], ty = TILE_Y[k / 2], idx = serverPage * PER_PAGE + k;
    if (idx < nOrder && x >= tx - 3 && x < tx + TILE_W + 3 && y >= ty - 3 && y < ty + TILE_H + 3) {
      xSemaphoreTake(snapLock, portMAX_DELAY);
      strlcpy(detailName, snap.dev[order[idx]].name, sizeof detailName);
      xSemaphoreGive(snapLock);
      return true;
    }
  }
  return false;
}

void drawAds(const Snap& s) {
  if (!s.hasAds) { text("No ad blocker data", W / 2, 100, DIM, 2, TC_DATUM); return; }
  float pct = s.queries ? 100.0f * s.blocked / s.queries : 0;
  int cx = 72, cy = 108;
  tft.fillSmoothCircle(cx, cy, 44, RING_IN, BG);
  tft.drawSmoothArc(cx, cy, 58, 47, 0, 360, LINE, BG, false);
  int deg = (int)(360 * clampf(pct, 0, 100) / 100.0f);
  if (s.adsOn && deg > 2) {  // angle 0 is at the bottom; start the arc at the top (180)
    if (deg <= 180) tft.drawSmoothArc(cx, cy, 58, 47, 180, 180 + deg, ACC, BG, true);
    else { tft.drawSmoothArc(cx, cy, 58, 47, 180, 360, ACC, BG, false); tft.drawSmoothArc(cx, cy, 58, 47, 0, deg - 180, ACC, BG, true); }
  }
  icon(tft, IC_SHIELD, cx - 8, cy - 34, s.adsOn ? ACC : C_WARN, 2);
  char b[24];
  if (s.adsOn) { snprintf(b, sizeof b, "%.1f", pct); value(b, "%", cx + 24, cy - 11, FG, RING_IN); }
  else text("off", cx, cy - 11, C_WARN, 1, TC_DATUM, RING_IN, 4);
  text("blocked", cx, cy + 19, DIM, 1, TC_DATUM, RING_IN);

  card(146, 30, 166, 100);
  commas(b, sizeof b, s.blocked);
  text(b, 158, 42, FG, 1, TL_DATUM, CARD, 4);
  text("ads and trackers", 158, 72, DIM, 1, TL_DATUM, CARD);
  text("stopped today", 158, 84, DIM, 1, TL_DATUM, CARD);
  char q[16]; commas(q, sizeof q, s.queries);
  snprintf(b, sizeof b, "of %s lookups", q);
  text(b, 158, 106, FAINT, 1, TL_DATUM, CARD);
  pill(229, 148, s.adsOn ? "Blocking on" : "Blocking off", s.adsOn ? C_OK : C_WARN, s.adsOn ? OK_BG : WARN_BG);
}

void drawAlerts(const Snap& s) {
  if (!s.nProb) {
    text("Nothing wrong", 116, 62, FG, 2);
    char b[32];
    icon(tft, IC_GLOBE, 116, 90, s.ms >= 0 ? C_OK : C_BAD);
    if (s.ms >= 0) snprintf(b, sizeof b, "Internet %d ms", s.ms); else strcpy(b, "Internet down");
    text(b, 130, 90, DIM, 1);
    icon(tft, IC_SERVER, 116, 106, C_OK);
    snprintf(b, sizeof b, "%d device%s reporting", s.nDev, s.nDev == 1 ? "" : "s");
    text(b, 130, 106, DIM, 1);
    if (s.hasPower) {
      icon(tft, IC_BOLT, 116, 122, C_WARN);
      snprintf(b, sizeof b, "Using %d W now", s.watts);
      text(b, 130, 122, DIM, 1);
    }
    return;
  }
  for (int i = 0; i < s.nProb; i++) {
    int y = TOP_H + 6 + i * 44;
    card(6, y, W - 12, 38);
    tft.fillRect(6, y + 6, 3, 26, C_BAD);
    icon(tft, IC_WARN, 16, y + 9, C_BAD);
    char line[23];
    strlcpy(line, s.prob[i], sizeof line);
    text(line, 30, y + 6, FG, 2, TL_DATUM, CARD);
    if (strlen(s.prob[i]) > 22) text(s.prob[i] + 22, 30, y + 25, DIM, 1, TL_DATUM, CARD);
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
  text(fix, W / 2, 204, FAINT, 1, TC_DATUM);
}

void drawPetForTab() {
  if (!dataShown) drawPet(SPR_SLEEP, (W - 128) / 2, TOP_H + 12, 2);
  else if (tab == T_HOME) drawPet(moodSprite(), 6, TOP_H + 8, 2);
  else if (tab == T_ALERTS && nProbShown == 0) drawPet(moodSprite(), 30, 62, 1);
}

void drawAll() {
  Snap s;
  xSemaphoreTake(snapLock, portMAX_DELAY);
  s = snap;
  drawnVersion = snapVersion;
  xSemaphoreGive(snapLock);
  dataShown = s.valid;
  nProbShown = s.nProb;

  mood = !s.valid ? M_SLEEP : s.nProb ? M_SAD : M_RUN;
  float cpu = avgCpu(s);
  runEveryMs = (uint32_t)max(60.0f, 200.0f - cpu / 100.0f * 140.0f);
  setLed(!s.valid || s.stale ? ACC : s.nProb ? C_BAD : (s.hasAds && !s.adsOn) ? C_WARN : C_OK);

  recordHistory(s);
  drawTopBar(s);
  if (!s.valid) { drawOffline(s); drawPetForTab(); return; }
  bgFill(0, TOP_H, W, NAV_Y - TOP_H);
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
  mood = M_RUN;
  drawPet(SPR_RUN0, (W - 128) / 2, 26, 2);
  text("Pringle", W / 2, 166, FG, 3, TC_DATUM);
  text("waking up the homelab panel", W / 2, 198, DIM, 1, TC_DATUM);
  mood = M_SLEEP;

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
      if (t == T_SERVER && tab == T_SERVER && detailName[0]) { detailName[0] = 0; pageDirty = true; }  // tap Server again: back to the grid
      if (t != tab) { tab = t; pageDirty = true; }
    } else if (tab == T_SERVER && dataShown && serverTap(x, y)) {
      pageDirty = true;
    }
  }

  if (snapVersion != drawnVersion || pageDirty) { pageDirty = false; drawAll(); }

  struct tm t;
  if (now - lastClockAt > 5000) {
    lastClockAt = now;
    if (getLocalTime(&t, 0) && t.tm_min != clockMinute) pageDirty = true;
  }

  // Running: frame speed follows CPU. Worried or asleep: Pringle holds still, the weather keeps moving.
  uint32_t step = mood == M_RUN ? runEveryMs * 2 : 280;
  if (now - lastFrameAt > step) {
    lastFrameAt = now;
    tick++;
    if (mood == M_RUN) frame = (frame + 1) % 4;
    drawPetForTab();
  }
  delay(5);
}
