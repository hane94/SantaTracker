// =====================================================================
//  Santa Tracker keyring  -  ESP32-C3 1.44" ST7735 (Spotpear)
//  Screen 1 (live tracking) + Screen 4 (Santa is near) + falling snow
//
//  Board : ESP32C3 Dev Module, USB CDC On Boot: Enabled
//  Lib   : "Adafruit ST7735 and ST7789 Library" (+ Adafruit GFX)
//  Files : santa_tracker.ino + route_google.h + font_silk_bold.h +
//          setup_page.h (all in the same folder)
//
//  How it works
//   - Santa's route = Google Santa Tracker route (418 stops), stored in
//     route_google.h. Takeoff Dec 24 10:00 UTC every year.
//   - WiFi (phone hotspot / home) is used only to get the exact time (NTP).
//   - Touch on the back of the case:
//       asleep : double tap -> on   (WAKE_MODE 1: hold HOLD_MS instead)
//       awake  : 1 tap -> tracking <-> world map, 2 taps -> off, 4 taps -> SETUP MODE
//   - SETUP MODE: the keyring makes its own WiFi "SantaTracker-Setup".
//     Join it with a phone -> a settings page opens (WiFi, city, timings).
//     Opens by itself the first time (nothing saved yet).
//   - Screen turns off after the set time, then deep sleep.
//   - On Dec 24 the keyring wakes up by itself before Santa reaches the
//     home city and shows the alert screen.
//
//  Wiring (touch module): GND->GND, VCC->IO7, I/O->IO1
// =====================================================================

#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"
#include "route_google.h"
#include "font_silk_bold.h"   // Silkscreen Bold pixel font (OFL)
#include "setup_page.h"       // web page for setup mode
#include "kfont.h"            // Korean pixel fonts (Galmuri, OFL)
#include "map_land.h"         // world map for the map screen

// ------------------------------ SETTINGS ------------------------------
// WiFi, home city, screen time and alert time are set on the phone in
// SETUP MODE (tap the back 4 times quickly, or it opens by itself the first
// time). These are only the defaults before anything is saved.
const char* DEFAULT_CITY = "SEOUL";
const int   DEFAULT_ON_S = 60;      // screen on time (seconds)
const int   DEFAULT_ALERT_MIN = 30; // alert starts this many minutes before
const char* SETUP_AP = "SantaTracker-Setup";
const uint32_t SETUP_TIMEOUT_MS = 5 * 60 * 1000;   // leave setup after 5 min

const int WAKE_MODE = 2;            // 1 = touch & hold, 2 = double tap
const uint32_t HOLD_MS     = 3000;  // WAKE_MODE 1: hold time
const uint32_t TAP_GAP_MS  = 700;   // max gap between taps of one series
const uint32_t ALERT_ON_MS = 60000; // screen on time for the automatic alert
const uint32_t SYNC_EVERY_S = 6 * 3600;  // re-sync time over WiFi
const int AFTER_VISIT_MIN = 10;   // "Santa was here" screen after he leaves (minutes)

// DEMO mode is chosen on the setup page (pretend clock, no WiFi, no sleep):
//   1 = full demo (developer mode): the clock is set to 1 minute before
//       takeoff on Dec 24 and then runs at REAL speed. Everything behaves like
//       the real night (sleep, taps, waking up for the alert / arrival).
//   2 = tap demo: each tap shows the next screen
//       (ready -> live -> map -> alert -> arrived -> left), the keyring stays on
// ----------------------------------------------------------------------

#define TFT_SCLK 3
#define TFT_MOSI 4
#define TFT_CS   2
#define TFT_DC   0
#define TFT_RST  5
#define TOUCH_SIG 1
#define TOUCH_VCC 7

// 0xRRGGBB -> RGB565
#define C(rgb) ((uint16_t)((((rgb) >> 8) & 0xF800) | (((rgb) >> 5) & 0x07E0) | (((rgb) >> 3) & 0x001F)))

// types first (Arduino puts function prototypes near the top)
struct Flake { float x, y, vy, ph; uint8_t s; uint16_t c; };
struct R8    { int8_t x, y, w, h; uint16_t c; };
struct Trip  { int phase; int cur; int next; time_t nextAt; time_t homeAt; uint64_t gifts; };
// Trip.phase: 0 = before takeoff, 1 = flying, 2 = landed

// The LCD keeps its picture and settings while the ESP32 sleeps (its RESET
// line is held high). On wake we just reconnect to it instead of running the
// full init, because any reset makes this panel flash white.
class SantaTFT : public Adafruit_ST7735 {
public:
  SantaTFT(SPIClass* s, int8_t cs, int8_t dc) : Adafruit_ST7735(s, cs, dc, -1) {}
  void resume(uint32_t freq) {         // same state as initR(144GREENTAB) + setRotation(2)
    initSPI(freq);
    _colstart = 2; _rowstart = 1; _xstart = 2; _ystart = 1;
    _width = 128; _height = 128; rotation = 2;
  }
};
SantaTFT tft(&SPI, TFT_CS, TFT_DC);     // RESET pin is driven by our own code
GFXcanvas16 cv(128, 128);   // draw into memory, then push -> no flicker

const uint16_t NAVY = C(0x0B1530), BOX = C(0x16234A), WHITE = C(0xF4F7FB),
               GREY = C(0x9FB0D0), GOLD = C(0xF5C451), RED = C(0xE5484D),
               GREEN = C(0x3FB27F), BROWN = C(0xC98B4A), LRED = C(0xF45B5B),
               SKIN = C(0xF8D5B8), ARED = C(0xC8242B), DRED = C(0x7A0E12),
               INK = C(0x141414), PURE = C(0xFFFFFF);

// --------------------------- STATE / RTC ------------------------------
RTC_DATA_ATTR time_t lastSync = 0;
RTC_DATA_ATTR bool lcdKept = false;   // LCD stayed powered & configured during sleep
RTC_DATA_ATTR bool demoRunning = false; // full demo: pretend clock already set
RTC_DATA_ATTR int64_t demoOffset = 0;   // full demo: pretend time - system time
volatile bool sntpDone = false;
volatile int wifiState = 0;     // 0 idle, 1 working, 2 ok, 3 failed
uint32_t demoMillis0 = 0;
time_t demoBase = 0;
bool screenReady = false;
int homeIdx = -1;
uint32_t awakeUntil = 0;

// saved settings (flash, survive power loss)
struct Config { String s1, p1, s2, p2, city, lang; int onS, alertMin, demo; } cfg;
Preferences prefs;
void loadConfig() {
  prefs.begin("santa", true);
  cfg.s1 = prefs.getString("s1", ""); cfg.p1 = prefs.getString("p1", "");
  cfg.s2 = prefs.getString("s2", ""); cfg.p2 = prefs.getString("p2", "");
  cfg.city = prefs.getString("city", DEFAULT_CITY);
  cfg.onS = prefs.getInt("on", DEFAULT_ON_S);
  cfg.alertMin = prefs.getInt("al", DEFAULT_ALERT_MIN);
  cfg.demo = prefs.getInt("demo", 0);
  if (cfg.demo < 0 || cfg.demo > 2) cfg.demo = 0;   // old demo numbers
  cfg.lang = prefs.getString("lang", "en");
  String build = prefs.getString("build", "");
  prefs.end();
  // freshly uploaded firmware -> start with demo OFF (WiFi/city/language are kept)
  const char* thisBuild = __DATE__ " " __TIME__;
  if (build != thisBuild) {
    cfg.demo = 0;
    prefs.begin("santa", false);
    prefs.putInt("demo", 0); prefs.putString("build", thisBuild);
    prefs.end();
  }
}
void saveConfig() {
  prefs.begin("santa", false);
  prefs.putString("s1", cfg.s1); prefs.putString("p1", cfg.p1);
  prefs.putString("s2", cfg.s2); prefs.putString("p2", cfg.p2);
  prefs.putString("city", cfg.city);
  prefs.putInt("on", cfg.onS); prefs.putInt("al", cfg.alertMin);
  prefs.putInt("demo", cfg.demo); prefs.putString("lang", cfg.lang);
  prefs.end();
}
// screen on time; 0 = never turns off by itself (2 taps still turn it off)
uint32_t awakeFor(uint32_t ms) { return ms == 0 ? UINT32_MAX : millis() + ms; }
uint32_t onTimeMs() { return (uint32_t)cfg.onS * 1000; }
int homeTzMin() { return homeIdx >= 0 ? ROUTE[homeIdx].tz : 540; }
bool KO() { return cfg.lang == "ko"; }      // screen language (set on the setup page)

// ------------------------------ TIME ----------------------------------
time_t utcOf(int year, int mon, int day, int h, int m) {
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = mon - 1; t.tm_mday = day;
  t.tm_hour = h; t.tm_min = m;
  return mktime(&t);            // TZ is UTC0, so this is UTC
}
bool timeValid() { return time(nullptr) > 1700000000; }
uint32_t demoSpeed = 1;
time_t demoEnd = 0;
// tap demo (2): one scene per tap, the clock runs normally inside each scene
const int SCENES = 6;            // ready, live, map, alert, arrived, left
const int MAP_SCENE = 2;
time_t sceneBase[SCENES], sceneMax[SCENES];
int scene = 0;
time_t nowUtc() {
  if (!cfg.demo) return time(nullptr);
  if (cfg.demo == 1) return time(nullptr) + (time_t)demoOffset;   // real speed, shifted
  time_t t = sceneBase[scene] + (millis() - demoMillis0) / 1000;  // tap demo
  return t > sceneMax[scene] ? sceneMax[scene] : t;
}
time_t takeoff(int year) { return utcOf(year, 12, 24, 10, 0); }
int seasonYear(time_t now) {
  struct tm t; gmtime_r(&now, &t);
  int y = t.tm_year + 1900;
  if (now > takeoff(y) + ROUTE_LANDING + 1800) y++;   // this year's trip is over
  return y;
}
time_t homeArrival(time_t now) {
  return homeIdx >= 0 ? takeoff(seasonYear(now)) + ROUTE[homeIdx].arr : 0;
}

void startDemo() {
  int y = timeValid() ? seasonYear(time(nullptr)) : 2026;
  time_t t0 = takeoff(y);
  time_t home = homeIdx >= 0 ? t0 + ROUTE[homeIdx].arr : t0 + 4 * 3600;
  time_t stay = homeIdx >= 0 ? ROUTE[homeIdx].stay : 60;
  time_t al = home - cfg.alertMin * 60, gone = home + stay, back = gone + AFTER_VISIT_MIN * 60;
  if (cfg.demo == 2) {                  // tap demo scenes: {start, latest}
    const time_t b[SCENES] = {t0 - 2 * 3600, al - 20 * 60, al - 20 * 60, home - 10 * 60, home, gone + 1};
    const time_t m[SCENES] = {t0 - 1, al - 1, al - 1, home - 1, gone - 1, back - 1};
    for (int i = 0; i < SCENES; i++) { sceneBase[i] = b[i]; sceneMax[i] = m[i]; }
    scene = 0;
  } else if (!demoRunning) {            // full demo: start 1 min before takeoff
    demoOffset = (int64_t)(t0 - 60) - (int64_t)time(nullptr);
    demoRunning = true;
  }
  demoMillis0 = millis();
}

// ------------------------------ SNOW ----------------------------------
Flake snow[34];
void initSnow() {
  for (auto& f : snow) {
    f.x = random(128); f.y = random(128);
    int layer = random(3);
    f.s  = layer == 2 ? 2 : 1;
    f.vy = 0.25f + layer * 0.25f + random(100) / 400.0f;
    f.c  = layer == 0 ? C(0x6F7FA8) : layer == 1 ? C(0xC8D2E6) : PURE;
    f.ph = random(628) / 100.0f;
  }
}
void stepSnow() {
  for (auto& f : snow) {
    f.y += f.vy; f.ph += 0.05f;
    if (f.y > 128) { f.y = -2; f.x = random(128); }
    cv.fillRect((int)(f.x + sinf(f.ph) * 1.5f), (int)f.y, f.s, f.s, f.c);
  }
}

// --------------------------- PIXEL ART --------------------------------
const R8 SLEIGH[] = {   // 32x16 grid
  {0,6,2,2,BROWN},{2,7,6,3,BROWN},{2,10,1,3,BROWN},{7,10,1,3,BROWN},{1,5,1,1,LRED},
  {8,9,6,1,GOLD},{14,5,14,6,RED},{12,11,18,1,GOLD},{12,12,1,1,GOLD},{29,10,2,1,GOLD},
  {19,1,5,4,RED},{20,2,3,2,SKIN},{19,4,5,2,WHITE},{23,0,2,2,WHITE},{25,2,3,3,GREEN}};
const R8 GIFT[] = {     // 8x8 grid
  {0,3,8,5,RED},{0,2,8,2,LRED},{3,2,2,6,GOLD},{1,0,2,2,GOLD},{5,0,2,2,GOLD}};
const R8 SANTA[] = {    // 24x24 grid
  {7,2,9,2,PURE},{8,0,8,2,DRED},{16,1,3,3,PURE},{7,4,10,5,SKIN},{9,6,2,1,INK},
  {13,6,2,1,INK},{6,8,12,2,PURE},{7,10,10,3,PURE},{9,13,6,2,PURE},{11,10,2,1,LRED},
  {5,13,4,7,DRED},{15,13,4,7,DRED},{9,15,6,5,DRED},{5,17,14,2,INK},{11,17,2,2,GOLD},
  {6,20,4,3,INK},{14,20,4,3,INK}};
void drawArt(const R8* a, int n, int ox, int oy, int sc) {
  for (int i = 0; i < n; i++)
    cv.fillRect(ox + a[i].x * sc, oy + a[i].y * sc, a[i].w * sc, a[i].h * sc, a[i].c);
}
#define art(A, X, Y, S) drawArt(A, sizeof(A) / sizeof(A[0]), X, Y, S)

// ------------------------------ TEXT ----------------------------------
void txt(const char* s, int x, int y, uint16_t c, int size) {
  cv.setTextSize(size); cv.setTextColor(c); cv.setCursor(x, y); cv.print(s);
}
void txtR(const char* s, int xr, int y, uint16_t c, int size) {   // right aligned
  txt(s, xr - (int)strlen(s) * 6 * size, y, c, size);
}
void txtC(const char* s, int y, uint16_t c, int size) {           // centered
  txt(s, 64 - (int)strlen(s) * 3 * size, y, c, size);
}
// Silkscreen Bold (chunky 8-bit font). y = top of the capitals.
// SILK_TRACK = letter spacing change in font pixels (-1 = a bit tighter).
const int SILK_TRACK = -1;
const GFXglyph& silkG(char ch) {
  if (ch < 0x20 || ch > 0x7E) ch = '?';
  return SilkBoldGlyphs[ch - 0x20];
}
int silkWidth(const char* s, int size) {   // ink width, first to last pixel
  if (!*s) return 0;
  int x = -silkG(s[0]).xOffset, right = 0;
  for (; *s; s++) {
    const GFXglyph& g = silkG(*s);
    if (g.width) right = x + g.xOffset + g.width;
    x += g.xAdvance + SILK_TRACK;
  }
  return right * size;
}
void silk(const char* s, int x, int y, uint16_t c, int size) {
  if (!*s) return;
  cv.setFont(&SilkBold); cv.setTextSize(size); cv.setTextColor(c);
  int cx = x - silkG(s[0]).xOffset * size;     // first pixel exactly at x
  char one[2] = {0, 0};
  for (; *s; s++) {
    one[0] = *s;
    cv.setCursor(cx, y + 5 * size);            // custom fonts use the baseline
    cv.print(one);
    cx += (silkG(*s).xAdvance + SILK_TRACK) * size;
  }
  cv.setFont(NULL);
}
void silkC(const char* s, int y, uint16_t c, int size) {
  silk(s, 64 - silkWidth(s, size) / 2, y, c, size);
}
// ---- Korean text (Galmuri pixel fonts, UTF-8). y = top of the line ----
const KGlyph* kFind(const KFont& f, uint16_t cp) {
  int lo = 0, hi = f.n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (f.g[m].cp == cp) return &f.g[m];
    if (f.g[m].cp < cp) lo = m + 1; else hi = m - 1;
  }
  return nullptr;
}
uint16_t utf8Next(const char*& s) {
  uint8_t c = *s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) { uint16_t v = (c & 0x1F) << 6; v |= (*s++ & 0x3F); return v; }
  if ((c & 0xF0) == 0xE0) { uint16_t v = (c & 0x0F) << 12; v |= (*s++ & 0x3F) << 6; v |= (*s++ & 0x3F); return v; }
  while ((*s & 0xC0) == 0x80) s++;          // 4-byte chars: skip
  return '?';
}
int kWidth(const char* s, const KFont& f, int sc) {
  int w = 0;
  while (*s) { const KGlyph* g = kFind(f, utf8Next(s)); w += g ? g->adv : 4; }
  return w * sc;
}
void kText(const char* s, int x, int y, uint16_t c, const KFont& f, int sc) {
  int base = y + f.ascent * sc;
  while (*s) {
    const KGlyph* g = kFind(f, utf8Next(s));
    if (!g) { x += 4 * sc; continue; }
    int w = g->w_h >> 8, h = g->w_h & 0xFF, bit = 0;
    const uint8_t* b = f.bmp + g->off;
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++, bit++)
        if (b[bit >> 3] & (0x80 >> (bit & 7)))
          cv.fillRect(x + (g->xo + xx) * sc, base + (g->yo + yy) * sc, sc, sc, c);
    x += g->adv * sc;
  }
}
void kTextR(const char* s, int xr, int y, uint16_t c, const KFont& f, int sc) { kText(s, xr - kWidth(s, f, sc), y, c, f, sc); }
void kTextC(const char* s, int y, uint16_t c, const KFont& f, int sc) { kText(s, 64 - kWidth(s, f, sc) / 2, y, c, f, sc); }
// copy as much of s as fits in maxW pixels ("..." added when cut)
void kFit(const char* s, int maxW, const KFont& f, char* out, int outSize) {
  if (kWidth(s, f, 1) <= maxW) { strncpy(out, s, outSize - 1); out[outSize - 1] = 0; return; }
  int dots = kWidth("..", f, 1), w = 0; const char* p = s;
  while (*p) {
    const char* q = p; const KGlyph* g = kFind(f, utf8Next(q));
    int a = g ? g->adv : 4;
    if (w + a + dots > maxW || (q - s) >= outSize - 3) break;
    w += a; p = q;
  }
  int n = p - s; memcpy(out, s, n); strcpy(out + n, "..");
}
// English UI message -> Korean
const char* ko(const char* en) {
  static const char* T[][2] = {
    {"SEARCHING", "와이파이"}, {"WIFI...", "찾는 중..."}, {"CONNECTING", "연결 중"},
    {"GETTING", "시간"}, {"TIME...", "맞추는 중..."}, {"NO TIME", "시간 서버"},
    {"NTP FAILED", "연결 실패"}, {"CAN'T CONNECT", "연결 실패"}, {"CHECK PASSWORD", "비밀번호를 확인하세요"},
    {"NO WIFI FOUND", "와이파이 없음"}, {"SETUP MODE", "설정 모드"},
    {"STARTING...", "시작 중..."}, {"SAVED!", "저장 완료!"}, {"RESTARTING...", "다시 시작 중..."},
    {"TAP 4X = SETUP", "4번 톡톡 = 설정"}};
  for (auto& r : T) if (!strcmp(en, r[0])) return r[1];
  return en;
}

void fit(const char* s, int maxChars, char* out) {   // shorten with '.'
  int n = strlen(s);
  if (n <= maxChars) { strcpy(out, s); return; }
  memcpy(out, s, maxChars - 1); out[maxChars - 1] = '.'; out[maxChars] = 0;
}
void commas(uint64_t v, char* out) {
  char tmp[24]; int n = 0;
  do { tmp[n++] = '0' + v % 10; v /= 10; if (v && n % 4 == 3) tmp[n++] = ','; } while (v);
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = 0;
}
void etaStr(long s, char* out) {
  if (s < 0) s = 0;
  if (s >= 86400) sprintf(out, "%ldD %02ldH", s / 86400, (s % 86400) / 3600);
  else if (s >= 3600) sprintf(out, "%ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
  else sprintf(out, "%02ld:%02ld", s / 60, s % 60);
}

void etaStrKo(long s, char* out) {
  if (s < 0) s = 0;
  if (s >= 86400) sprintf(out, "D-%ld", (s + 86399) / 86400);
  else etaStr(s, out);
}

// ------------------------------ TRIP ----------------------------------
Trip getTrip(time_t now) {
  Trip t = {}; t.cur = -1; t.next = -1;
  int y = seasonYear(now);
  time_t t0 = takeoff(y);
  t.homeAt = homeIdx >= 0 ? t0 + ROUTE[homeIdx].arr : 0;
  if (now < t0) { t.phase = 0; t.nextAt = t0; return t; }
  if (now >= t0 + (time_t)ROUTE_LANDING) {
    t.phase = 2; t.nextAt = takeoff(y + 1);
    t.gifts = (uint64_t)ROUTE[N_ROUTE - 1].gifts2 * 2; return t;
  }
  t.phase = 1;
  uint32_t s = now - t0;
  int i = 0;                                   // first stop not reached yet
  while (i < N_ROUTE && ROUTE[i].arr <= s) i++;
  t.cur = i - 1;
  t.next = i < N_ROUTE ? i : -1;
  t.nextAt = t.next >= 0 ? t0 + ROUTE[t.next].arr : t0 + ROUTE_LANDING;
  // gifts: grow smoothly from the previous stop to the next one
  uint64_t g0 = t.cur >= 0 ? (uint64_t)ROUTE[t.cur].gifts2 * 2 : 0;
  uint64_t g1 = t.next >= 0 ? (uint64_t)ROUTE[t.next].gifts2 * 2 : g0;
  uint32_t a0 = t.cur >= 0 ? ROUTE[t.cur].arr : 0;
  uint32_t a1 = t.next >= 0 ? ROUTE[t.next].arr : ROUTE_LANDING;
  t.gifts = a1 > a0 ? g0 + (g1 - g0) * (s - a0) / (a1 - a0) : g0;
  return t;
}
// before Santa reaches the home city: red alert
bool inAlert(const Trip& t, time_t now) {
  return homeIdx >= 0 && now >= t.homeAt - cfg.alertMin * 60 && now < t.homeAt;
}
// from arrival until he leaves (+ a little while after): Merry Christmas screen
time_t homeDeparture(const Trip& t) { return t.homeAt + (homeIdx >= 0 ? ROUTE[homeIdx].stay : 60); }
bool inArrival(const Trip& t, time_t now) {
  return homeIdx >= 0 && now >= t.homeAt && now < homeDeparture(t) + AFTER_VISIT_MIN * 60;
}

// ----------------------------- SCREENS --------------------------------
// Before takeoff: Santa's workshop - Santa wraps gifts, the pile grows,
// snow falls outside the window, countdown to takeoff at the bottom.
const uint16_t WALL = C(0x1A2550), WOOD = C(0x8A5A32), WOODD = C(0x5A3A22),
               FLOOR = C(0x3A2618), BLUE = C(0x4A7BD0), FRAME = C(0x6B4A2E);
const uint16_t GIFT_COL[4] = {RED, GREEN, BLUE, GOLD};
void giftBox(int x, int y, int w, int h, int col, bool lid, bool ribbon, bool bow) {
  uint16_t body = GIFT_COL[col], rib = col == 3 ? RED : GOLD;
  cv.fillRect(x, y, w, h, body);
  if (lid) cv.fillRect(x - 1, y - 2, w + 2, 3, body == GOLD ? C(0xE0A93A) : LRED);
  if (ribbon) {
    cv.fillRect(x + w / 2 - 1, y - 2, 2, h + 2, rib);
    cv.fillRect(x, y + h / 2 - 1, w, 2, rib);
  }
  if (bow) {
    cv.fillRect(x + w / 2 - 4, y - 5, 3, 3, rib);
    cv.fillRect(x + w / 2 + 1, y - 5, 3, 3, rib);
    cv.fillRect(x + w / 2 - 1, y - 4, 2, 2, rib);
  }
}
void drawPacking(const Trip& t, time_t now) {
  const int PX[6] = {90, 103, 116, 96, 109, 102}, PY[6] = {76, 76, 76, 66, 66, 56};
  uint32_t ms = millis();
  uint32_t k = ms / 2800, ph = ms % 2800;          // gift number, phase in its cycle
  int col = k % 4;
  int pile = k % 7;                                // finished gifts on the pile (0..6)
  cv.fillScreen(WALL);
  // window with falling snow
  cv.fillRect(80, 20, 42, 30, FRAME);
  cv.fillRect(82, 22, 38, 26, NAVY);
  for (auto& f : snow) {
    f.y += f.vy * 0.6f; f.ph += 0.05f;
    if (f.y > 128) { f.y = -2; f.x = random(128); }
    int x = (int)(f.x + sinf(f.ph) * 1.5f), y = (int)f.y;
    int wx = 82 + (x * 38) / 128, wy = 22 + (y * 26) / 128;   // map into the window
    cv.fillRect(wx, wy, 1, 1, f.c);
  }
  cv.fillRect(100, 22, 2, 26, FRAME); cv.fillRect(82, 34, 38, 2, FRAME);
  // top bar
  bool blink = (ms / 500) % 2;
  if (blink) cv.fillRect(6, 7, 4, 4, GOLD);
  char buf[40];
  time_t lt = now + homeTzMin() * 60; struct tm tm; gmtime_r(&lt, &tm);
  sprintf(buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  if (KO()) { kText("준비중", 13, 4, WHITE, KS, 1); kTextR(buf, 123, 4, GREY, KS, 1); }
  else { txt("READY", 13, 6, WHITE, 1); txtR(buf, 122, 6, GREY, 1); }
  // floor + pile of finished gifts
  cv.fillRect(0, 86, 128, 6, FLOOR);
  for (int i = 0; i < pile && i < 6; i++) giftBox(PX[i], PY[i], 11, 9, (k - pile + i) % 4, true, true, false);
  // Santa behind the table
  art(SANTA, 4, 22, 2);
  // table
  cv.fillRect(2, 64, 84, 4, WOOD);
  cv.fillRect(2, 68, 84, 3, WOODD);
  cv.fillRect(6, 71, 4, 15, WOODD); cv.fillRect(78, 71, 4, 15, WOODD);
  // gift being wrapped
  int gx = 52, gy = 50;
  if (ph < 2400) {
    int stage = ph / 600;                          // 0 box, 1 lid, 2 ribbon, 3 bow
    giftBox(gx, gy, 18, 14, col, stage >= 1, stage >= 2, stage >= 3);
    if (stage == 3 && (ph / 150) % 2) {            // sparkles
      cv.fillRect(gx - 4, gy - 8, 2, 2, PURE); cv.fillRect(gx + 21, gy - 6, 2, 2, PURE);
      cv.fillRect(gx + 9, gy - 12, 2, 2, GOLD);
    }
  } else if (pile < 6) {                           // fly to the pile
    float p = (ph - 2400) / 400.0f;
    int tx = PX[pile], ty = PY[pile];
    int x = gx + (int)((tx - gx) * p), y = gy + (int)((ty - gy) * p) - (int)(18 * sinf(p * 3.1416f));
    giftBox(x, y, 11, 9, col, true, true, true);
  }
  // arms: busy hands
  int b = ((ms / 200) % 2) ? 1 : -1;
  cv.fillRect(38, 50, 12, 4, DRED);  cv.fillRect(48, 52 + b, 4, 4, SKIN);
  if (ph < 2400) { cv.fillRect(40, 44, 20, 4, DRED); cv.fillRect(58 + 3 * b, 43, 4, 4, SKIN); }
  // countdown to takeoff
  cv.fillRect(6, 94, 116, 30, BOX);
  long s = (long)(t.nextAt - now); if (s < 0) s = 0;
  if (KO()) {                                   // "출발까지 · 12/24 19:00" (local takeoff time)
    time_t tl = t.nextAt + homeTzMin() * 60; struct tm tk; gmtime_r(&tl, &tk);
    sprintf(buf, "출발까지 · %d/%d %02d:%02d", tk.tm_mon + 1, tk.tm_mday, tk.tm_hour, tk.tm_min);
    kTextC(buf, 95, GREY, KS, 1);
  } else txtC("TAKEOFF IN", 97, GREY, 1);
  if (s >= 86400 && KO()) sprintf(buf, "D-%ld", (s + 86399) / 86400);
  else if (s >= 86400) sprintf(buf, "%ldD %02ldH", s / 86400, (s % 86400) / 3600);
  else sprintf(buf, "%02ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
  silkC(buf, 108, GOLD, 2);
}

void drawMainKo(const Trip& t, time_t now) {
  char buf[64], buf2[64];
  bool blink = (millis() / 500) % 2;
  const char* st = t.phase == 0 ? "준비중" : t.phase == 1 ? "비행중" : "도착";
  uint16_t sc = t.phase == 0 ? GOLD : t.phase == 1 ? RED : GREEN;
  if (blink || t.phase != 1) cv.fillRect(6, 7, 4, 4, sc);
  kText(st, 13, 4, WHITE, KS, 1);
  time_t lt = now + homeTzMin() * 60; struct tm tm; gmtime_r(&lt, &tm);
  sprintf(buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  kTextR(buf, 123, 4, GREY, KS, 1);
  int bob = ((millis() / 600) % 2) ? -1 : 0;
  art(SLEIGH, 6, 36 + bob, 1);
  const char* city = t.cur >= 0 ? ROUTE[t.cur].cityKo : "북극";
  const char* reg  = t.cur >= 0 ? ROUTE[t.cur].regionKo : (t.phase == 0 ? "선물 포장 중" : "휴식 중");
  const int W = 80;                             // room right of the sleigh
  if (kWidth(city, KB, 1) <= W) {
    kText("지금 여기", 44, 26, GREY, KS, 1);
    kText(city, 44, 38, WHITE, KB, 1);
    kFit(reg, W, KS, buf, sizeof(buf)); kText(buf, 44, 53, GREY, KS, 1);
  } else {                                      // long name: 2 lines
    const char* p = city; const char* cut = nullptr; int w = 0;
    while (*p) {                                // last space that still fits
      const char* q = p; const KGlyph* g = kFind(KB, utf8Next(q));
      if (*p == ' ' && w <= W) cut = p;
      w += g ? g->adv : 4; if (w > W && !cut) { cut = p; break; }
      p = q;
    }
    if (!cut) cut = p;
    int n = cut - city; memcpy(buf2, city, n); buf2[n] = 0;
    kText("지금 여기", 44, 22, GREY, KS, 1);
    kText(buf2, 44, 33, WHITE, KB, 1);
    kFit(cut + (*cut == ' ' ? 1 : 0), W, KB, buf, sizeof(buf)); kText(buf, 44, 45, WHITE, KB, 1);
    kFit(reg, W, KS, buf, sizeof(buf)); kText(buf, 44, 59, GREY, KS, 1);
  }
  cv.fillRect(6, 74, 116, 26, BOX);
  kText("다음", 10, 76, GREY, KS, 1); kTextR("도착까지", 119, 76, GREY, KS, 1);
  char eta[24]; etaStrKo((long)(t.nextAt - now), eta);
  const char* nx = t.phase == 0 ? "출발" : t.phase == 2 ? "내년 출발"
                 : t.next >= 0 ? ROUTE[t.next].cityKo : "북극";
  kFit(nx, 108 - kWidth(eta, KR, 1) - 6, KR, buf, sizeof(buf));
  kText(buf, 10, 87, WHITE, KR, 1);
  kTextR(eta, 119, 87, GOLD, KR, 1);
  art(GIFT, 6, 110, 1);
  commas(t.gifts, buf); strcat(buf, "개");
  kText(buf, 18, 108, WHITE, KS, 1);
}

void drawMain(const Trip& t, time_t now) {
  cv.fillScreen(NAVY);
  stepSnow();
  if (KO()) { drawMainKo(t, now); return; }
  char buf[32];
  // top row: status + local time
  bool blink = (millis() / 500) % 2;
  const char* st = t.phase == 0 ? "READY" : t.phase == 1 ? "LIVE" : "DONE";
  uint16_t sc = t.phase == 0 ? GOLD : t.phase == 1 ? RED : GREEN;
  if (blink || t.phase != 1) cv.fillRect(6, 7, 4, 4, sc);
  txt(st, 13, 6, WHITE, 1);
  time_t lt = now + homeTzMin() * 60; struct tm tm; gmtime_r(&lt, &tm);
  sprintf(buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  txtR(buf, 122, 6, GREY, 1);
  // sleigh + current place
  int bob = ((millis() / 600) % 2) ? -1 : 0;
  art(SLEIGH, 6, 36 + bob, 1);
  const char* city = t.cur >= 0 ? ROUTE[t.cur].city : "N.POLE";
  const char* reg  = t.cur >= 0 ? ROUTE[t.cur].region : (t.phase == 0 ? "PACKING GIFTS" : "RESTING");
  txt(t.cur >= 0 ? "NOW OVER" : "NOW AT", 44, 26, GREY, 1);
  int n = strlen(city);
  if (silkWidth(city, 2) <= 78) silk(city, 44, 39, WHITE, 2);   // big chunky name
  else if (n <= 13) txt(city, 44, 42, WHITE, 1);
  else {                                        // long name: 2 lines
    char a[14], b[14]; int cut = 13;
    while (cut > 0 && city[cut] != ' ' && city[cut] != '-') cut--;
    if (cut == 0) cut = 13;
    memcpy(a, city, cut); a[cut] = 0;
    fit(city + cut + (city[cut] == ' ' ? 1 : 0), 13, b);
    txt(a, 44, 37, WHITE, 1); txt(b, 44, 46, WHITE, 1);
  }
  txt(reg, 44, 57, GREY, 1);
  // next stop box
  cv.fillRect(6, 74, 116, 26, BOX);
  txt("NEXT", 10, 78, GREY, 1); txtR("ETA", 118, 78, GREY, 1);
  char eta[16]; etaStr((long)(t.nextAt - now), eta);
  const char* nx = t.phase == 0 ? "TAKEOFF" : t.phase == 2 ? "NEXT YEAR"
                 : t.next >= 0 ? ROUTE[t.next].city : "N.POLE";
  int room = (108 - (int)strlen(eta) * 6 - 6) / 6;
  fit(nx, room, buf);
  txt(buf, 10, 89, WHITE, 1);
  txtR(eta, 118, 89, GOLD, 1);
  // gifts delivered
  art(GIFT, 6, 110, 1);
  commas(t.gifts, buf);
  txt(buf, 18, 111, WHITE, 1);
}

void drawAlert(const Trip& t, time_t now) {
  cv.fillScreen(ARED);
  stepSnow();
  bool on = (millis() / 400) % 2;
  cv.fillRect(33, 5, 62, 11, on ? PURE : C(0xE79A9D));
  int bob = ((millis() / 600) % 2) ? -2 : 0;
  art(SANTA, 40, 20 + bob, 2);
  if (KO()) {
    kTextC("! 알림 !", 5, DRED, KS, 1);
    kTextC("산타가", 69, PURE, KB, 1);
    kTextC("곧 와요!", 82, PURE, KB, 2);
    cv.fillRect(6, 108, 116, 15, DRED);
    long left = (long)(t.homeAt - now);
    char r[24];
    if (left > 0) sprintf(r, "~%ld분", (left + 59) / 60); else strcpy(r, "도착!");
    kText(left > 0 ? "어서 자요" : "호호호", 10, 110, PURE, KS, 1);
    kTextR(r, 119, 110, GOLD, KS, 1);
    return;
  }
  txtC("! ALERT !", 7, DRED, 1);
  silkC("SANTA IS", 74, PURE, 2);
  silkC("NEAR!", 90, PURE, 2);
  cv.fillRect(6, 108, 116, 14, DRED);
  long left = (long)(t.homeAt - now);
  char r[24];
  if (left > 0) sprintf(r, "~%ld MIN", (left + 59) / 60); else strcpy(r, "HERE!");
  txt(left > 0 ? "GO TO BED" : "HO HO HO", 10, 111, PURE, 1);
  txtR(r, 118, 111, GOLD, 1);
}

// Santa has landed on our roof: sleigh on the roof, gifts drop down the chimney
void drawArrived(const Trip& t, time_t now) {
  uint32_t ms = millis();
  bool here = now < homeDeparture(t);           // still here, or already left
  cv.fillScreen(NAVY);
  stepSnow();
  // stars
  const uint8_t SX[6] = {14, 40, 70, 96, 118, 58}, SY[6] = {52, 60, 50, 58, 48, 66};
  for (int i = 0; i < 6; i++) if ((ms / 300 + i) % 4) cv.fillRect(SX[i], SY[i], 1, 1, PURE);
  // house: wall + lit window
  cv.fillRect(8, 104, 112, 24, C(0x5A2A22));
  for (int y = 107; y < 128; y += 5) cv.drawFastHLine(8, y, 112, C(0x4A221C));
  cv.fillRect(24, 110, 14, 12, GOLD); cv.fillRect(30, 110, 2, 12, C(0x5A2A22)); cv.fillRect(24, 115, 14, 2, C(0x5A2A22));
  cv.fillRect(84, 110, 14, 12, GOLD); cv.fillRect(90, 110, 2, 12, C(0x5A2A22)); cv.fillRect(84, 115, 14, 2, C(0x5A2A22));
  // roof (flat ridge + slopes) with snow
  cv.fillTriangle(0, 104, 30, 84, 30, 104, DRED);
  cv.fillTriangle(128, 104, 98, 84, 98, 104, DRED);
  cv.fillRect(30, 84, 68, 20, DRED);
  cv.fillRect(28, 82, 72, 3, PURE);
  cv.drawLine(0, 103, 29, 83, PURE); cv.drawLine(128, 103, 99, 83, PURE);
  // chimney with smoke
  cv.fillRect(104, 76, 12, 16, C(0x8A3A2E)); cv.fillRect(102, 74, 16, 3, PURE);
  for (int i = 0; i < 3; i++) {
    int p = (ms / 40 + i * 25) % 75;
    cv.fillRect(108 + (p / 12) % 3, 70 - p / 3, 3 + p / 25, 3 + p / 25, C(0x9FB0D0));
  }
  if (here) {
    // sleigh on the ridge, gifts hop into the chimney
    int bob = ((ms / 500) % 2) ? -1 : 0;
    art(SLEIGH, 32, 58 + bob, 2);
    float p = (ms % 1400) / 1400.0f;
    int gx = 80 + (int)(26 * p), gy = 64 - (int)(18 * sinf(p * 3.1416f)) + (int)(10 * p);
    if (p < 0.9f) giftBox(gx, gy, 7, 6, (ms / 1400) % 4, true, true, false);
  } else {
    // gone: footprints in the snow and gifts left by the chimney
    for (int x = 40; x < 96; x += 8) cv.fillRect(x, 83, 2, 1, C(0xC8D2E6));
    giftBox(84, 76, 8, 7, 0, true, true, true);
    giftBox(93, 78, 7, 5, 1, true, true, false);
  }
  // text
  char buf[48];
  if (KO()) {
    kTextC(here ? "산타가 도착했어요!" : "산타가 다녀갔어요", 5, WHITE, KS, 1);
    kTextC("메리 크리스마스!", 22, GOLD, KB, 1);
    snprintf(buf, sizeof(buf), here ? "지금 %s에 있어요" : "%s에 선물 배달 완료", ROUTE[homeIdx].cityKo);
    kTextC(buf, 38, GREY, KS, 1);
  } else {
    txtC(here ? "SANTA IS HERE!" : "SANTA WAS HERE!", 6, WHITE, 1);
    silkC("MERRY", 18, GOLD, 2);
    silkC("CHRISTMAS!", 31, GOLD, 2);
    snprintf(buf, sizeof(buf), "IN %s", ROUTE[homeIdx].city);
    if (!here) snprintf(buf, sizeof(buf), "GIFTS DELIVERED");
    txtC(buf, 45, GREY, 1);
  }
}

// World map: gold dots = cities Santa has visited, faint dots = still to come,
// red dot = Santa, white square = home.
bool showMap = false;
const uint16_t OCEAN = C(0x0E1D45), LAND = C(0x2A4373), TODO = C(0x5F6F98);
const int MAP_Y = 18;
void mapXY(int lat100, int lng100, int& x, int& y) {
  x = (int)((lng100 / 100.0f + 180.0f) / 360.0f * MAP_W);
  y = MAP_Y + (int)((MAP_LAT0 - lat100 / 100.0f) / (MAP_LAT0 - MAP_LAT1) * MAP_H);
  x = constrain(x, 0, MAP_W - 1); y = constrain(y, MAP_Y, MAP_Y + MAP_H - 1);
}
void drawMap(const Trip& t, time_t now) {
  cv.fillScreen(NAVY);
  cv.fillRect(0, MAP_Y, MAP_W, MAP_H, OCEAN);
  for (int j = 0; j < MAP_H; j++)
    for (int i = 0; i < MAP_W; i++)
      if (MAP_LAND[j * (MAP_W / 8) + i / 8] & (0x80 >> (i & 7))) cv.drawPixel(i, MAP_Y + j, LAND);
  int visited = t.phase == 2 ? N_ROUTE : t.cur + 1;
  for (int i = 0; i < N_ROUTE; i++) {
    int x, y; mapXY(ROUTE[i].lat100, ROUTE[i].lng100, x, y);
    cv.drawPixel(x, y, i < visited ? GOLD : TODO);
  }
  int hx, hy;
  if (homeIdx >= 0) { mapXY(ROUTE[homeIdx].lat100, ROUTE[homeIdx].lng100, hx, hy); cv.drawRect(hx - 2, hy - 2, 5, 5, PURE); }
  int sx, sy;
  if (t.phase == 1 && t.cur >= 0) mapXY(ROUTE[t.cur].lat100, ROUTE[t.cur].lng100, sx, sy);
  else mapXY(8460, 16800, sx, sy);              // North Pole (Santa's Village)
  if ((millis() / 400) % 4) { cv.fillCircle(sx, sy, 3, RED); cv.drawCircle(sx, sy, 3, PURE); }
  // top: visited count + clock
  char buf[64], eta[24];
  time_t lt = now + homeTzMin() * 60; struct tm tm; gmtime_r(&lt, &tm);
  sprintf(buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
  cv.fillRect(6, 7, 4, 4, RED);
  const char* city = t.cur >= 0 && t.phase == 1 ? (KO() ? ROUTE[t.cur].cityKo : ROUTE[t.cur].city) : (KO() ? "북극" : "N.POLE");
  const char* nx = t.phase == 0 ? (KO() ? "출발" : "TAKEOFF") : t.phase == 2 ? (KO() ? "내년 출발" : "NEXT YEAR")
                 : t.next >= 0 ? (KO() ? ROUTE[t.next].cityKo : ROUTE[t.next].city) : (KO() ? "북극" : "N.POLE");
  char cnt[16]; sprintf(cnt, "%d/%d", visited, N_ROUTE);
  if (KO()) {
    kText("들른 곳", 13, 4, WHITE, KS, 1); kText(cnt, 13 + kWidth("들른 곳", KS, 1) + 4, 4, GOLD, KS, 1);
    kTextR(buf, 123, 4, GREY, KS, 1);
    kText("지금 여기", 6, 83, GREY, KS, 1);
    kFit(city, 70, KB, buf, sizeof(buf)); kText(buf, 52, 82, WHITE, KB, 1);
    cv.fillRect(6, 97, 116, 27, BOX);
    etaStrKo((long)(t.nextAt - now), eta);
    kText("다음", 10, 99, GREY, KS, 1);
    kFit(nx, 108 - 24 - kWidth(eta, KS, 1) - 4, KS, buf, sizeof(buf)); kText(buf, 34, 99, WHITE, KS, 1);
    kTextR(eta, 119, 99, GOLD, KS, 1);
    cv.fillCircle(12, 116, 2, RED); kText("산타", 17, 111, GREY, KS, 1);
    cv.drawRect(44, 114, 5, 5, PURE); kText("우리 집", 52, 111, GREY, KS, 1);
  } else {
    kText("STOPS", 13, 4, WHITE, KS, 1); kText(cnt, 13 + kWidth("STOPS ", KS, 1), 4, GOLD, KS, 1);
    kTextR(buf, 123, 4, GREY, KS, 1);              // narrow pixel font so it all fits
    txt("NOW OVER", 6, 85, GREY, 1);
    fit(city, 10, buf); txt(buf, 60, 85, WHITE, 1);
    cv.fillRect(6, 97, 116, 27, BOX);
    long left = (long)(t.nextAt - now);
    if (left >= 86400) sprintf(eta, "D-%ld", (left + 86399) / 86400); else etaStr(left, eta);
    txt("NEXT", 10, 100, GREY, 1);
    fit(nx, (108 - 30 - (int)strlen(eta) * 6) / 6, buf); txt(buf, 38, 100, WHITE, 1);
    txtR(eta, 119, 100, GOLD, 1);
    cv.fillCircle(12, 115, 2, RED); txt("SANTA", 17, 112, GREY, 1);
    cv.drawRect(52, 113, 5, 5, PURE); txt("HOME", 60, 112, GREY, 1);
  }
}

void drawMessage(const char* a, const char* b) {
  cv.fillScreen(NAVY);
  stepSnow();
  art(SLEIGH, 48, 34, 1);
  if (KO()) { kTextC(ko(a), 62, WHITE, KR, 1); kTextC(ko(b), 77, GREY, KS, 1); return; }
  txtC(a, 64, WHITE, 1);
  txtC(b, 78, GREY, 1);
}

void drawHold(float p) {
  cv.fillScreen(NAVY);
  stepSnow();
  if (KO()) kTextC("꾹 누르고 있어요", 40, WHITE, KB, 1);
  else txtC("HOLD...", 40, WHITE, 2);
  cv.drawRect(10, 80, 108, 8, GREY);
  cv.fillRect(12, 82, (int)(104 * p), 4, GOLD);
  art(SLEIGH, 10 + (int)(76 * p), 60, 1);
}

void push() { tft.drawRGBBitmap(0, 0, cv.getBuffer(), 128, 128); }

// ------------------------------ WIFI ----------------------------------
void onSntp(struct timeval* tv) { sntpDone = true; }
// Tries WiFi 1 then WiFi 2 from the saved settings.
// Only networks that are actually visible are tried.
char wifiMsg[2][24] = {"SEARCHING", "WIFI..."};
void setWifiMsg(const char* a, const char* b) {
  strncpy(wifiMsg[0], a, 23); strncpy(wifiMsg[1], b, 23);
}
bool tryConnect(const char* ssid, const char* pass, bool lowPower) {
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_STA);
  if (lowPower) WiFi.setTxPower(WIFI_POWER_8_5dBm);   // fixes some C3 boards
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(100);
  Serial.printf("  %s%s -> %s (status %d)\n", ssid, lowPower ? " [low tx]" : "",
                WiFi.status() == WL_CONNECTED ? "OK" : "fail", WiFi.status());
  return WiFi.status() == WL_CONNECTED;
}
void wifiTask(void* arg) {
  wifiState = 1;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true); delay(100);
  setWifiMsg("SEARCHING", "WIFI...");
  int n = WiFi.scanNetworks();
  Serial.printf("wifi scan: %d networks\n", n);
  for (int i = 0; i < n; i++)
    Serial.printf("  %-32s %4d dBm  ch%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));

  const String* SS[2] = {&cfg.s1, &cfg.s2};
  const String* PP[2] = {&cfg.p1, &cfg.p2};
  bool connected = false, anySeen = false;
  for (int k = 0; k < 2 && !connected; k++) {
    if (SS[k]->length() == 0) continue;
    bool seen = false;
    for (int i = 0; i < n; i++) if (WiFi.SSID(i) == *SS[k]) seen = true;
    if (!seen) { Serial.printf("  %s: not found\n", SS[k]->c_str()); continue; }
    anySeen = true;
    setWifiMsg("CONNECTING", SS[k]->c_str());
    connected = tryConnect(SS[k]->c_str(), PP[k]->c_str(), false) ||
                tryConnect(SS[k]->c_str(), PP[k]->c_str(), true);
  }
  WiFi.scanDelete();

  if (connected) {
    setWifiMsg("GETTING", "TIME...");
    sntpDone = false;
    sntp_set_time_sync_notification_cb(onSntp);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    uint32_t t0 = millis();
    while (!sntpDone && millis() - t0 < 10000) delay(50);
    if (sntpDone) { lastSync = time(nullptr); wifiState = 2; }
    else { setWifiMsg("NO TIME", "NTP FAILED"); wifiState = 3; }
  } else {
    if (anySeen) setWifiMsg("CAN'T CONNECT", "CHECK PASSWORD");
    else         setWifiMsg("NO WIFI FOUND", "");
    wifiState = 3;
  }
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);   // save battery
  Serial.printf("wifi/time: %s\n", wifiState == 2 ? "synced" : "failed");
  vTaskDelete(nullptr);
}

// ------------------------------ SLEEP ---------------------------------
bool wokeFromSleep = false;
void screenOn() {
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  if (wokeFromSleep && lcdKept) {
    tft.resume(27000000);        // no reset -> no white flash
  } else {
    pinMode(TFT_RST, OUTPUT);    // first power-on: full reset + init
    digitalWrite(TFT_RST, LOW); delay(20);
    digitalWrite(TFT_RST, HIGH); delay(150);
    tft.initR(INITR_144GREENTAB);
    tft.setRotation(2);          // upside down? use 0
    tft.setSPISpeed(27000000);
    tft.fillScreen(ST77XX_BLACK);
  }
  screenReady = true;
}
bool touched() { return digitalRead(TOUCH_SIG) == HIGH; }

void goToSleep() {
  if (cfg.demo == 2) return;     // tap demo never sleeps (4 taps -> setup to turn it off)
  // This panel turns WHITE when the LCD is switched off (its backlight has no
  // off switch). So: leave the LCD on showing black, and hold its RESET/CS
  // lines during deep sleep so it isn't reset (a reset also shows white).
  if (screenReady) { cv.fillScreen(0); push(); }
  pinMode(TFT_RST, OUTPUT); digitalWrite(TFT_RST, HIGH);
  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  gpio_hold_en((gpio_num_t)TFT_RST);
  gpio_hold_en((gpio_num_t)TFT_CS);
  lcdKept = true;
  while (touched()) delay(20);   // otherwise it wakes right away
  delay(100);
  gpio_hold_en((gpio_num_t)TOUCH_VCC);       // keep touch sensor powered
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_SIG, ESP_GPIO_WAKEUP_GPIO_HIGH);
  // wake up by itself for the alert, and again when Santa lands
  if ((timeValid() || cfg.demo == 1) && homeIdx >= 0) {
    time_t now = nowUtc();
    time_t arr = homeArrival(now), a = arr - cfg.alertMin * 60;
    time_t next = a > now ? a : (arr > now ? arr : 0);
    if (next) esp_sleep_enable_timer_wakeup((uint64_t)(next - now) * 1000000ULL);
  }
  Serial.println("sleep");
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------------------- SETUP MODE ------------------------------
WebServer web(80);
DNSServer dns;
bool setupSaved = false;
String scanJson;

void scanForSetup() {
  int n = WiFi.scanNetworks();
  scanJson = "[";
  int cnt = 0;
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    if (ss.length() == 0 || scanJson.indexOf("\"" + ss + "\"") >= 0) continue;   // skip hidden / duplicates
    ss.replace("\\", "\\\\"); ss.replace("\"", "\\\"");
    if (cnt++) scanJson += ",";
    scanJson += "\"" + ss + "\"";
  }
  scanJson += "]";
  WiFi.scanDelete();
}
String jsonStr(const String& v) {
  String o = v; o.replace("\\", "\\\\"); o.replace("\"", "\\\"");
  return "\"" + o + "\"";
}
void handleData() {
  String j;
  j.reserve(16000);
  j = "{\"s1\":" + jsonStr(cfg.s1) + ",\"s2\":" + jsonStr(cfg.s2) +
      ",\"city\":" + jsonStr(cfg.city) + ",\"on\":" + cfg.onS + ",\"al\":" + cfg.alertMin +
      ",\"demo\":" + cfg.demo + ",\"lang\":" + jsonStr(cfg.lang) +
      ",\"nets\":" + scanJson + ",\"cities\":[";
  for (int i = 0; i < N_ROUTE; i++) {
    if (i) j += ",";
    j += "[\""; j += ROUTE[i].city; j += "\",\""; j += ROUTE[i].region; j += "\",";
    j += ROUTE[i].arr; j += ","; j += ROUTE[i].tz; j += "]";
  }
  j += "]}";
  web.send(200, "application/json", j);
}
void handleSave() {
  // empty password + same WiFi name = keep the saved password
  String s1 = web.arg("s1"), p1 = web.arg("p1"), s2 = web.arg("s2"), p2 = web.arg("p2");
  if (p1.length() == 0 && s1 == cfg.s1) p1 = cfg.p1;
  if (p2.length() == 0 && s2 == cfg.s2) p2 = cfg.p2;
  cfg.s1 = s1; cfg.p1 = p1; cfg.s2 = s2; cfg.p2 = p2;
  if (web.hasArg("city")) cfg.city = web.arg("city");
  cfg.onS = constrain(web.arg("on").toInt(), 0, 600);   // 0 = always on
  cfg.alertMin = constrain(web.arg("al").toInt(), 5, 180);
  cfg.demo = constrain(web.arg("demo").toInt(), 0, 2);
  cfg.lang = web.arg("lang") == "ko" ? "ko" : "en";
  saveConfig();
  lastSync = 0;                                  // get the time again next boot
  demoRunning = false;                           // full demo starts over after saving
  web.send(200, "text/html", SAVED_HTML);
  setupSaved = true;
}
void drawSetup(uint32_t leftMs) {
  cv.fillScreen(NAVY);
  stepSnow();
  if (KO()) {
    kTextC("설정 모드", 10, GOLD, KB, 1);
    kText("1. 이 와이파이에 접속", 8, 32, GREY, KS, 1);
    kText("SantaTracker-Setup", 8, 44, WHITE, KS, 1);
    kText("2. 설정 페이지가 열려요", 8, 60, GREY, KS, 1);
    kText("(안 열리면 주소창에", 8, 72, GREY, KS, 1);
    kText("192.168.4.1)", 8, 84, WHITE, KS, 1);
    kText("2번 톡톡 = 취소", 8, 102, GREY, KS, 1);
    int w = (int)(116L * leftMs / SETUP_TIMEOUT_MS);
    cv.fillRect(6, 118, 116, 4, BOX); cv.fillRect(6, 118, w, 4, GOLD);
    return;
  }
  silkC("SETUP MODE", 8, GOLD, 2);
  txt("1. JOIN THIS WIFI", 8, 32, GREY, 1);
  txt("SantaTracker-Setup", 8, 44, WHITE, 1);
  txt("2. A PAGE OPENS", 8, 62, GREY, 1);
  txt("(or go to", 8, 74, GREY, 1);
  txt("192.168.4.1)", 8, 86, WHITE, 1);
  txt("TAP 2X = CANCEL", 8, 104, GREY, 1);
  int w = (int)(116L * leftMs / SETUP_TIMEOUT_MS);
  cv.fillRect(6, 118, 116, 4, BOX);
  cv.fillRect(6, 118, w, 4, GOLD);
}
void runSetupMode() {
  Serial.println("setup mode");
  drawMessage("SETUP MODE", "STARTING..."); push();
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP_STA);
  scanForSetup();
  WiFi.softAP(SETUP_AP);
  delay(200);
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);                        // every address -> this page
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html", SETUP_HTML); });
  web.on("/data", HTTP_GET, handleData);
  web.on("/scan", HTTP_GET, []() { scanForSetup(); web.send(200, "application/json", scanJson); });
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([ip]() {                        // phone "captive portal" checks
    web.sendHeader("Location", String("http://") + ip.toString() + "/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();

  uint32_t t0 = millis(), lastTap = 0; int taps = 0; bool was = touched();
  while (!setupSaved && millis() - t0 < SETUP_TIMEOUT_MS) {
    dns.processNextRequest();
    web.handleClient();
    bool tch = touched();
    if (tch && !was) { taps = (millis() - lastTap < TAP_GAP_MS) ? taps + 1 : 1; lastTap = millis(); }
    was = tch;
    if (taps >= 2 && millis() - lastTap > TAP_GAP_MS) break;     // cancel
    if (taps == 1 && millis() - lastTap > TAP_GAP_MS) taps = 0;
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 60) { drawSetup(SETUP_TIMEOUT_MS - (millis() - t0)); push(); lastDraw = millis(); }
    delay(5);
  }
  if (setupSaved) {
    drawMessage("SAVED!", "RESTARTING..."); push();
    uint32_t t1 = millis();
    while (millis() - t1 < 1500) { web.handleClient(); delay(5); }   // let the page load
  }
  web.stop(); dns.stop();
  WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
  if (!setupSaved) goToSleep();                  // cancelled / timed out
  ESP.restart();
}

// ------------------------------ SETUP ---------------------------------
void setup() {
  Serial.begin(115200);
  setenv("TZ", "UTC0", 1); tzset();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // touch sensor power (release the sleep hold after setting HIGH)
  pinMode(TOUCH_VCC, OUTPUT); digitalWrite(TOUCH_VCC, HIGH);
  gpio_hold_dis((gpio_num_t)TOUCH_VCC);
  pinMode(TOUCH_SIG, INPUT_PULLDOWN);
  // LCD lines were held high while asleep: keep them high while releasing,
  // otherwise RESET floats low and the panel resets (= white flash)
  pinMode(TFT_RST, OUTPUT); digitalWrite(TFT_RST, HIGH);
  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  gpio_hold_dis((gpio_num_t)TFT_RST);
  gpio_hold_dis((gpio_num_t)TFT_CS);
  wokeFromSleep = (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_TIMER);
  if (!wokeFromSleep) lcdKept = false;

  loadConfig();
  for (int i = 0; i < N_ROUTE; i++) if (cfg.city == ROUTE[i].city) homeIdx = i;
  initSnow();

  if (cause == ESP_SLEEP_WAKEUP_GPIO && WAKE_MODE == 2) {
    // first tap woke us: wait for release, then a 2nd tap within TAP_GAP_MS
    uint32_t t0 = millis();
    while (touched()) {                     // long press = not a tap (pocket)
      if (millis() - t0 > 1500) goToSleep();
      delay(10);
    }
    uint32_t t1 = millis(); bool second = false;
    while (millis() - t1 < TAP_GAP_MS) { if (touched()) { second = true; break; } delay(10); }
    if (!second) goToSleep();               // single tap -> back to sleep
    screenOn();
  } else if (cause == ESP_SLEEP_WAKEUP_GPIO && WAKE_MODE == 1 && HOLD_MS > 0) {
    uint32_t t0 = millis(); bool shown = false;
    while (true) {
      uint32_t el = millis() - t0;
      if (!touched()) goToSleep();          // let go too early -> back to sleep
      if (el >= HOLD_MS) break;
      if (el > 800) {                       // after a moment, show progress
        if (!shown) { screenOn(); shown = true; }
        drawHold((float)el / HOLD_MS); push();
      }
      delay(30);
    }
    if (!shown) screenOn();
  } else {
    screenOn();
    if (cause != ESP_SLEEP_WAKEUP_GPIO && cause != ESP_SLEEP_WAKEUP_TIMER)
      delay(800);                           // power-on: let the touch sensor calibrate
  }

  if (!cfg.demo && cfg.s1.length() == 0) runSetupMode();   // nothing saved yet -> setup

  if (cfg.demo != 1) demoRunning = false;
  if (cfg.demo == 2) {
    startDemo();
    awakeUntil = UINT32_MAX;
  } else if (cfg.demo == 1) {                 // developer mode: like the real night
    startDemo();
    awakeUntil = cause == ESP_SLEEP_WAKEUP_TIMER ? millis() + ALERT_ON_MS : awakeFor(onTimeMs());
  } else {
    awakeUntil = cause == ESP_SLEEP_WAKEUP_TIMER ? millis() + ALERT_ON_MS : awakeFor(onTimeMs());
    if (!timeValid() || time(nullptr) - lastSync > (time_t)SYNC_EVERY_S)
      xTaskCreate(wifiTask, "wifi", 8192, nullptr, 1, nullptr);
  }
  Serial.printf("wake cause %d, %d stops, home %s\n", cause, N_ROUTE,
                homeIdx >= 0 ? ROUTE[homeIdx].city : "NOT FOUND");
}

// ------------------------------ LOOP ----------------------------------
void loop() {
  // taps are counted as a series; the action happens when the series ends:
  // 1 tap = stay on longer, 2 taps = off, 4+ taps = setup mode
  static bool wasTouched = true;
  static uint32_t lastTap = 0;
  static int taps = 0;
  bool tch = touched();
  if (tch && !wasTouched) {
    taps = (lastTap && millis() - lastTap < TAP_GAP_MS) ? taps + 1 : 1;
    lastTap = millis();
  }
  wasTouched = tch;
  if (taps > 0 && !tch && millis() - lastTap > TAP_GAP_MS) {
    int n = taps; taps = 0;
    if (n >= 4) runSetupMode();
    else if (n == 2 && WAKE_MODE == 2) goToSleep();
    else if (n == 1 && cfg.demo == 2) { scene = (scene + 1) % SCENES; demoMillis0 = millis(); }
    else { showMap = !showMap; awakeUntil = awakeFor(onTimeMs()); }   // tracking <-> map
  }

  if (!cfg.demo && !timeValid()) {
    drawMessage(wifiMsg[0], wifiMsg[1]);
    if (wifiState == 3) {
      if (KO()) kTextC(ko("TAP 4X = SETUP"), 98, GOLD, KS, 1);
      else txtC("TAP 4X = SETUP", 100, GOLD, 1);
    }
    push();
    if (wifiState == 3 && millis() > awakeUntil) goToSleep();
    delay(30);
    return;
  }

  time_t now = nowUtc();
  Trip t = getTrip(now);
  bool special = inAlert(t, now) || inArrival(t, now) || showMap || (cfg.demo == 2 && scene == MAP_SCENE);
  if (inAlert(t, now)) drawAlert(t, now);
  else if (inArrival(t, now)) drawArrived(t, now);
  else if (showMap || (cfg.demo == 2 && scene == MAP_SCENE)) drawMap(t, now);
  else if (t.phase == 0) drawPacking(t, now);
  else drawMain(t, now);
  if (cfg.demo && !special) txt("DEMO", 52, 6, GOLD, 1);
  push();

  if (millis() > awakeUntil && wifiState != 1) goToSleep();
  delay(30);
}
