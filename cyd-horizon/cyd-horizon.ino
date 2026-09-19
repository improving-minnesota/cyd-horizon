// cyd-horizon: standalone dashboard for CYD-family boards (see DEVELOPER.md;
// pins in tft_setup.h). Landscape, 320x240 logical (480x320 on E32R40T).
// Credentials are provisioned to NVS over serial - none compiled in.

#include <WiFi.h>
#include <HTTPClient.h>
#include "http_body.h"
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <malloc.h>  // malloc_usable_size
// Logos load at runtime from a LittleFS partition (logos.ino); declared here
// so Arduino's alphabetical .ino concat can't hide them from other files.
struct RuntimeLogo { char icao[4]; uint16_t* data; int w; int h; };
bool logosInit();
const RuntimeLogo* findAirlineLogo(const char* callsign);
// Same hoisting workaround: ntfStepAt()'s generated prototype would precede this struct.
struct NtfStep { uint16_t freq; uint16_t ms; uint8_t rgb; };
static const NtfStep* ntfStepAt(int preset, unsigned long now);
void logoRelease(const RuntimeLogo* logo);
// Forward-declare Plane so Arduino's auto-generated function prototypes (which
// are inserted before the sketch body) can reference drawFlightInfo(Plane& p).
struct Plane;
// Same hoisting workaround for plotSeries()' pending-bucket param (t==0 = none).
struct PendingRollup { unsigned long t; float avg, lo, hi; };

// LED blink colors for overhead-flight notifications.
enum BlinkColor { BLINK_NONE, BLINK_RED, BLINK_GREEN, BLINK_BLUE, BLINK_YELLOW, BLINK_WHITE };

// Serial NVS provisioning (wifi_config.ino). Defined here so it precedes
// wifi_config.ino in the .ino concat; #ifndef lets -D override.
#ifndef ENABLE_SERIAL_PROVISION
#define ENABLE_SERIAL_PROVISION 0
#endif

// Local HTTP OTA is only for explicitly enabled dev builds. Release builds
// retain the normal verified HTTPS OTA path and reject serial-triggered HTTP.
#ifndef ENABLE_LOCAL_OTA
#define ENABLE_LOCAL_OTA 0
#endif

#ifndef BUILD_NUM
#define BUILD_NUM 0
#endif

// Pool temp via Govee API, logged to flash for the history graphs; an
// unlisted/failed device shows "--" + red border rather than crashing.
#define POOL_FEATURE 1

// Deep-sleep wake interval while inside the sleep window. The device wakes
// briefly to log the pool temperature, then returns to low-power deep sleep.
#define SLEEP_POOL_INTERVAL_US (5ULL * 60ULL * 1000000ULL)   // 5 minutes

// Daily update scan scheduling: jittered so fleets waking together don't hit
// GitHub in lockstep, and kept clear of alarm firings (scheduler in loop()).
#define AUTOSCAN_JITTER_S       3600
#define AUTOSCAN_ALARM_QUIET_S  3600

// ---- Touch-wake from deep sleep ----
// VERIFIED on this unit: touch IRQ = GPIO 36 (active-low, EXT0, input-only);
// enterDeepSleep() must hold touch CS low during sleep for the wake to fire.
#define TOUCH_IRQ_ENABLED 1
#define TOUCH_IRQ_PIN     36
#define TOUCH_CS_PIN      33

#ifdef CYD_E32R40T
// ---- E32R40T 4" variant: XPT2046 shares the TFT's HSPI bus ----
#define TOUCH_SPI  HSPI
#define TOUCH_MOSI 13
#define TOUCH_MISO 12
#define TOUCH_CLK  14
#else
// ---- XPT2046 touch on VSPI (separate bus from the TFT's HSPI) ----
// getTouch() only works on the display bus, so we drive the XPT2046 directly.
#define TOUCH_SPI VSPI
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25
#endif

// Print raw + mapped touch coordinates on every press (and calibration params
// at boot) to diagnose touch/panel calibration. 0 removes the prints.
#define TOUCH_DEBUG 0

// ---------------- CONFIG (edit these) ----------------
// OpenSky bbox is computed from g_lat/g_lon at runtime.
// APP_VERSION: CI/dev pass -DAPP_VERSION=<ver>-dev; this literal is only the
// flag-less fallback. A "-dev" build never auto-updates (g_autoUpdate).
#define STRINGIZE_INNER(x) #x
#define STRINGIZE(x) STRINGIZE_INNER(x)
#ifndef APP_VERSION
  #define APP_VERSION "0.0.0-dev"
  const char* const kVersion = APP_VERSION;
  #define kVersionStr "0.0.0-dev"
#else
  const char* const kVersion = STRINGIZE(APP_VERSION);
  #define kVersionStr STRINGIZE(APP_VERSION)
#endif
// Version marker for ota_push.py's OTA screen label; the boot print keeps it linked.
const char kBuildTag[] = "CYD_TAG=" kVersionStr ", Build " STRINGIZE(BUILD_NUM);
bool isDevBuild() { return strstr(kVersion, "-dev") != NULL; }
// User-Agent sent on every network call so servers can identify the client,
// e.g. "cyd-horizon/v1.8.0" (dev builds carry the "-dev" suffix).
String appUserAgent() { return "cyd-horizon/v" + String(kVersion); }
// ------------------------------------------------------

// ---- Display wrapper: logical UI, optionally scaled to the panel ----
// Logical DISP_W x 240 -> panel: identity on the 2.8", x4/3 onto 480x320 on
// CYD_E32R40T (DEVELOPER.md "Board variants"). Rect sizes scale by EDGES
// (S(x+w)-S(x)) so truncation can't leave 1px seams; raw panel uses `lcd`.
#ifdef CYD_E32R40T
  #define DISP_W   360
  #define SCALEX(v)   ((v) * 4 / 3)
  #define SCALEY(v)   ((v) * 4 / 3)
  #define UNSCALEX(v) ((v) * 3 / 4)
  #define UNSCALEY(v) ((v) * 3 / 4)
#else
  #define DISP_W   320
  #define SCALEX(v)   (v)
  #define SCALEY(v)   (v)
  #define UNSCALEX(v) (v)
  #define UNSCALEY(v) (v)
#endif
// RX(): x position anchored to the right edge (keeps its right margin on
// wider UIs). CX: horizontal centre. Both are identity on the 2.8" build.
#define RX(v) ((v) + (DISP_W - 320))
#define CX    (DISP_W / 2)
// FONT_AUX: secondary text - classic F1 on the 2.8", smooth FreeSans F2 on the 4".
#ifdef CYD_E32R40T
  #define FONT_AUX 2
#else
  #define FONT_AUX 1
#endif

class UiTft {
public:
  UiTft(TFT_eSPI& d) : lcd(d) {}
  TFT_eSPI& lcd;

  // ---- raw panel ops (unscaled) ----
  void init(uint8_t tc = 0)          { lcd.init(tc); }
  void setRotation(uint8_t r)        { lcd.setRotation(r); }
  // Callers pass logical coords (the help-text view), so scale like drawing.
  void setViewport(int32_t x, int32_t y, int32_t w, int32_t h, bool d = true) { lcd.setViewport(SCALEX(x), SCALEY(y), SCALEX(w), SCALEY(h), d); }
  void resetViewport()               { lcd.resetViewport(); }
  template<typename... A> void writecommand(A... a) { lcd.writecommand(a...); }

  // ---- text state ----
  void setTextFont(uint8_t f)        { applyFont(f); }
  void setTextSize(uint8_t s)        { curSize = s; lcd.setTextSize(s); }
  void setTextColor(uint16_t c)      { lcd.setTextColor(c); }
  void setTextColor(uint16_t c, uint16_t bg) { lcd.setTextColor(c, bg); }
  void setTextDatum(uint8_t d)       { lcd.setTextDatum(d); }
  // Font metrics come back in panel pixels - scale down so layout math stays
  // in logical coordinates.
  int16_t textWidth(const char* s)             { return UNSCALEX(lcd.textWidth(s)); }
  int16_t textWidth(const String& s)           { return UNSCALEX(lcd.textWidth(s)); }
  int16_t textWidth(const char* s, uint8_t f)  { applyFont(f); return UNSCALEX(lcd.textWidth(s, fontArg(f))); }
  int16_t textWidth(const String& s, uint8_t f){ applyFont(f); return UNSCALEX(lcd.textWidth(s, fontArg(f))); }
  int16_t getCursorX() { return UNSCALEX(lcd.getCursorX()); }
  int16_t getCursorY() { return UNSCALEY(lcd.getCursorY() - curAsc * curSize); }
  // Full cell height of the active font, in logical px (0 for classic fonts).
  int16_t fontHeight() { return UNSCALEY((curAsc + curDesc) * curSize); }

  // ---- text drawing ----
  // setCursor's y is "top of text"; FreeFonts' cursor is the baseline - shift by ascent.
  void setCursor(int16_t x, int16_t y)                { lcd.setCursor(SCALEX(x), SCALEY(y) + curAsc * curSize); }
  void setCursor(int16_t x, int16_t y, uint8_t f)     { applyFont(f); setCursor(x, y); }
  int16_t drawString(const char* s, int32_t x, int32_t y)          { return lcd.drawString(s, SCALEX(x), SCALEY(y)); }
  int16_t drawString(const char* s, int32_t x, int32_t y, uint8_t f) { applyFont(f); return lcd.drawString(s, SCALEX(x), SCALEY(y), fontArg(f)); }
  int16_t drawString(const String& s, int32_t x, int32_t y)        { return lcd.drawString(s, SCALEX(x), SCALEY(y)); }
  int16_t drawString(const String& s, int32_t x, int32_t y, uint8_t f) { applyFont(f); return lcd.drawString(s, SCALEX(x), SCALEY(y), fontArg(f)); }
  int16_t drawCentreString(const char* s, int32_t x, int32_t y, uint8_t f) { applyFont(f); return lcd.drawCentreString(s, SCALEX(x), SCALEY(y), fontArg(f)); }
  int16_t drawCentreString(const String& s, int32_t x, int32_t y, uint8_t f) { applyFont(f); return lcd.drawCentreString(s, SCALEX(x), SCALEY(y), fontArg(f)); }
  int16_t drawRightString(const char* s, int32_t x, int32_t y, uint8_t f)  { applyFont(f); return lcd.drawRightString(s, SCALEX(x), SCALEY(y), fontArg(f)); }
  int16_t drawRightString(const String& s, int32_t x, int32_t y, uint8_t f)  { applyFont(f); return lcd.drawRightString(s, SCALEX(x), SCALEY(y), fontArg(f)); }

  template<typename T> size_t print(const T& v) { return lcd.print(v); }
  template<typename T, typename F> size_t print(const T& v, F f) { return lcd.print(v, f); }
  template<typename T> size_t println(const T& v) { return lcd.println(v); }
  size_t println() { return lcd.println(); }
  template<typename... A> size_t printf(const char* fmt, A... a) { return lcd.printf(fmt, a...); }

  // ---- geometry (scaled by edges so adjacent rects tile without seams) ----
  void fillScreen(uint32_t c) { lcd.fillScreen(c); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { lcd.fillRect(SCALEX(x), SCALEY(y), SCALEX(x + w) - SCALEX(x), SCALEY(y + h) - SCALEY(y), c); }
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { lcd.drawRect(SCALEX(x), SCALEY(y), SCALEX(x + w) - SCALEX(x), SCALEY(y + h) - SCALEY(y), c); }
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c) { lcd.fillRoundRect(SCALEX(x), SCALEY(y), SCALEX(x + w) - SCALEX(x), SCALEY(y + h) - SCALEY(y), SCALEX(r), c); }
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c) { lcd.drawRoundRect(SCALEX(x), SCALEY(y), SCALEX(x + w) - SCALEX(x), SCALEY(y + h) - SCALEY(y), SCALEX(r), c); }
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t c) { lcd.drawFastHLine(SCALEX(x), SCALEY(y), SCALEX(x + w) - SCALEX(x), c); }
  void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t c) { lcd.drawFastVLine(SCALEX(x), SCALEY(y), SCALEY(y + h) - SCALEY(y), c); }
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) { lcd.drawLine(SCALEX(x0), SCALEY(y0), SCALEX(x1), SCALEY(y1), c); }
  void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t c) { lcd.fillCircle(SCALEX(x), SCALEY(y), SCALEY(r), c); }
  void drawCircle(int32_t x, int32_t y, int32_t r, uint32_t c) { lcd.drawCircle(SCALEX(x), SCALEY(y), SCALEY(r), c); }
  void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c) { lcd.fillTriangle(SCALEX(x0), SCALEY(y0), SCALEX(x1), SCALEY(y1), SCALEX(x2), SCALEY(y2), c); }
  void drawTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c) { lcd.drawTriangle(SCALEX(x0), SCALEY(y0), SCALEX(x1), SCALEY(y1), SCALEX(x2), SCALEY(y2), c); }
  void drawPixel(int32_t x, int32_t y, uint32_t c) { lcd.drawPixel(SCALEX(x), SCALEY(y), c); }

private:
  // Active font extents for baseline compensation (0 for classic fonts).
  uint8_t curAsc = 0;
  uint8_t curDesc = 0;
  uint8_t curSize = 1;

  void applyFont(uint8_t f) {
#ifdef CYD_E32R40T
    const GFXfont* g = nullptr;
    uint8_t asc = 0, desc = 0;
    switch (f) {
      // F1 stays classic 8px: the only tier that fits the header's 3 credit rows.
      case 2: g = &FreeSans9pt7b;      asc = 13; desc = 5; break;  // body text
      case 4: g = &FreeSansBold18pt7b; asc = 25; desc = 8; break;  // big values
      case 6: g = &FreeSansBold18pt7b; asc = 25; desc = 8; break;  // header clock
      default: break;
    }
    if (g) { lcd.setFreeFont(g); curAsc = asc; curDesc = desc; return; }
#endif
    lcd.setTextFont(f);
    curAsc = 0;
    curDesc = 0;
  }
  // TFT_eSPI draws a FreeFont when the font arg is 1 and a gfxFont is active.
  uint8_t fontArg(uint8_t f) { return curAsc ? 1 : f; }
};

TFT_eSPI lcd = TFT_eSPI();
UiTft tft(lcd);
Preferences prefs;

// Caps one JSON parse's heap use so a huge response fails cleanly instead of
// exhausting the heap; frees are credited so frees mid-parse don't trip it.
class BoundedAllocator : public ArduinoJson::Allocator {
 public:
  explicit BoundedAllocator(size_t maxBytes) : limit_(maxBytes), used_(0) {}

  void* allocate(size_t size) override {
    if (used_ + size > limit_) return nullptr;
    void* p = malloc(size);
    if (p) used_ += malloc_usable_size(p);
    return p;
  }

  void deallocate(void* p) override {
    if (!p) return;
    size_t had = malloc_usable_size(p);
    used_ = (used_ > had) ? (used_ - had) : 0;
    free(p);
  }

  void* reallocate(void* p, size_t newSize) override {
    size_t had = p ? malloc_usable_size(p) : 0;
    // Enforce the cap on growth too, or a shrink-then-grow sequence escapes it entirely.
    if (newSize > had && (used_ - had) + newSize > limit_) return nullptr;
    void* q = realloc(p, newSize);
    if (!q) return nullptr;              // p is still valid and still accounted for
    used_ = (used_ > had) ? (used_ - had) : 0;
    used_ += malloc_usable_size(q);
    return q;
  }

  size_t used() const { return used_; }  // for heap diagnostics

 private:
  size_t limit_, used_;
};

struct Plane {
  char callsign[9];
  char icao24[7];   // transponder hex ID, used for route lookups
  float distMi;
  int   altFt;
  int   spdKt;
  float hdgDeg;   // true track (deg), 0-359 clockwise from north; -1 if unknown
  float dxMi;     // east  offset in miles
  float dyMi;     // north offset in miles
  unsigned long lastStepMs;   // millis() of the last known position / dead-reckon step
  int  lastPx;    // on-screen pixel of the last drawn blip
  int  lastPy;
  bool blipOn;    // true while a blip for this plane is on screen (to erase later)
};

const int MAXP = 6;
Plane planes[MAXP];
int planeCount = 0;

// Snapshot of the last overhead flight so the "N aircraft" tap can recall it
// after it leaves the radius (valid=false until the first one).
struct FlightSnap {
  bool  valid;            // false until at least one overhead flight is recorded
  char  callsign[9];
  char  icao24[7];
  int   altFt;
  int   spdKt;
  float hdgDeg;           // true track (deg), -1 if unknown
  float distMi;
  float dxMi;             // east offset (for the radar blip)
  float dyMi;             // north offset (for the radar blip)
  // Raw route sources, remembered so the recall detail screen can recompute the
  // chosen route even after the live globals have moved on to a new aircraft.
  char  adsbOrigin[6];
  char  adsbDest[6];
  char  adsbOriginCity[24];
  char  adsbDestCity[24];
  char  openOrigin[6];
  char  openDest[6];
  // Chosen route for the live view and the recall screen.
  char  origin[12];       // route display codes ("" = unknown) — "ICAO", or
  char  dest[12];         // "ICAO | IATA" when the adsb.lol route provides IATA
  char  originCity[24];   // city for the chosen route source
  char  destCity[24];
  bool  routeFetched;
  unsigned long tickMs;   // millis() of the last known position / dead-reckon step
};
FlightSnap g_lastFlight;
const unsigned long SCREEN_IDLE_TIMEOUT_MS = 120000UL; // 2 minutes
unsigned long g_screenIdleUntil = 0;       // non-dashboard screen auto-return deadline

// Flight-detail radar has its own blip state; g_radarShown gates dead-reckoning
// to a screen actually showing a radar (else we'd paint onto the idle dashboard).
bool g_radarShown = false;
int  g_flightLastPx, g_flightLastPy;
bool g_flightBlipOn = false;

// Settings (persisted)
float g_radiusMi = 3.5;
int   g_ceilingFt = 15000;
int   g_pollSec = 30;
bool  g_trackEnabled = true;   // flight tracking on/off
bool  g_blinkForFlight = true; // blink the LED when an overhead flight is found
// Units: Imperial/Metric/Aviation. Distances store as miles, altitudes feet;
// helpers convert at display. Switching keeps the number and reinterprets it.
#define UNITS_IMPERIAL 0
#define UNITS_METRIC   1
#define UNITS_AVIATION 2
int g_units = UNITS_IMPERIAL;
float       distConv()  { return g_units == UNITS_METRIC ? 1.60934f : (g_units == UNITS_AVIATION ? 0.868976f : 1.0f); }  // miles -> display
const char* distUnit()  { return g_units == UNITS_METRIC ? "km" : (g_units == UNITS_AVIATION ? "nm" : "mi"); }
float       altConv()   { return g_units == UNITS_METRIC ? 0.3048f : 1.0f; }  // feet -> display
const char* altUnit()   { return g_units == UNITS_METRIC ? "m" : "ft"; }
const char* unitsName() { return g_units == UNITS_METRIC ? "Metric" : (g_units == UNITS_AVIATION ? "Aviation" : "Imperial"); }
// Temps stored as F; converted at display so stored history survives unit switches.
float tempDisp(float f) { return g_units == UNITS_IMPERIAL ? f : (f - 32.0f) * 5.0f / 9.0f; }
char  tempUnit()        { return g_units == UNITS_IMPERIAL ? 'F' : 'C'; }
bool  g_clock24 = false;       // false = 12-hour clock with AM/PM, true = 24-hour
bool  g_showTimer = false;     // show/update the dashboard countdown bar (Flight Tracker)
bool  g_showIata = true;       // show IATA airport codes in route display when ADSB.lol has them
bool  g_autoUpdate = true;     // auto-check/install firmware updates once/day (General)
unsigned long g_lastScanDay = 0; // epoch day of last auto-update scan (0 = never)
time_t g_autoScanAt = 0;         // epoch the pending daily scan may run (0 = none)
bool   g_autoScanForced = false; // scan sits at a least-bad slot inside an alarm window

// OTA / firmware update state
int    g_updateState = 0;      // 0 idle, 1 checking, 2 available, 3 none, 4 error
String g_updateLatest;         // latest version label when an update is available
String g_updateAsset;          // download URL when an update is available
String g_updateDigest;         // "sha256:<hex>" of the available asset ("" if absent)
bool   g_otaActive = false;    // loop() should run the pending OTA
String g_otaVersion, g_otaUrl; // pending OTA target
String g_otaHost;             // local OTA / replay host (serial provisioned)
String g_otaSha256;            // digest of the pending OTA target
bool   g_rollbackMarked = false; // OTA rollback safeguard applied once post-boot
TaskHandle_t g_otaTask = NULL;   // dedicated task running performOTA; created once at boot (see setup())
TaskHandle_t g_netTask = NULL;     // net task, for stack high-water logging
volatile bool g_otaRunning = false; // OTA task owns the display; loop() yields
bool   g_otaFromAbout = false; // Install tapped on About — hold that page (no idle return)
bool   g_upgraded = false;     // this boot runs a different version than last boot (NVS "lastver")

#define NET_TASK_STACK_BYTES 12288
// Auto-update status shown at the bottom-left of the dashboard (reuses the
// idle screen's status line, see drawAutoUpdateStatus()).
int    g_autoUpdStatus = 0;       // 0 none, 1 scanning, 2 no updates, 3 updating, 4 check failed
unsigned long g_autoUpdStatusUntil = 0; // millis() deadline to keep showing the transient status
int   g_ftPage = 0;            // Flight Tracker settings page (0-2)
float g_lat = 0.0f;           // location; loaded from NVS, or guessed from IP on first boot
float g_lon = 0.0f;
int   g_creditsRemaining = 0; // OpenSky X-Rate-Limit-Remaining
bool  g_creditsKnown = false; // false until the first successful fetch
bool  g_creditsExhausted = false;  // true when at/near the daily radar-polling credit limit
int   g_flightsCredits = -1;  // /flights/* bucket remaining (-1 = not seen yet)
int   g_tracksCredits  = -1;  // /tracks/* bucket remaining (-1 = not seen yet)
unsigned long g_nextRadarMs = 0;  // earliest millis() to retry the /states/* bucket after a 429
unsigned long g_nextRouteMs = 0;  // earliest millis() to retry the /flights/* bucket after a 429
unsigned long g_nextTrackMs = 0;  // earliest millis() to retry the /tracks/* bucket after a 429
const int LOW_CREDIT_THRESHOLD = 0;         // "at the limit"
const unsigned long CREDIT_RECOVERY_MS = 15UL * 60UL * 1000UL;  // fallback retry cadence while exhausted
// After this many consecutive 401s (each retried with a fresh token), drop to
// the CREDIT_RECOVERY_MS cadence instead of hammering rejected credentials.
const int AUTH_401_BACKOFF_AFTER = 5;
const unsigned long ADSB_RETRY_MS = 60UL * 1000UL;  // vrs-standing-data 429/transport retry

// OpenSky auth state for the red border indicator
enum AuthState { AUTH_OK = 0, AUTH_ANON, AUTH_BAD };
int g_authState = AUTH_ANON;   // defaults to anonymous (blank creds)
bool g_authChecked = false;    // true after the first OpenSky fetch attempt
// Set on an OpenSky TLS handshake/cert failure (negative HTTPClient code) -
// turns it into a hard error, not a silent fallback to anonymous.
bool g_osHandshakeFailed = false;

// Sleep Mode settings (persisted)
bool g_sleepOn = true;
int  g_sleepStartH = 22, g_sleepStartM = 0;   // default 10:00 PM
int  g_sleepEndH   = 8,  g_sleepEndM   = 0;   // default 8:00 AM
int  g_wakeMin     = 5;                         // wake duration after a touch

// Sleep runtime state
bool g_displayOff = false;
unsigned long wakeUntil = 0;                  // 0 = not in a user-triggered wake
// True once configTime()/setupNTP() has run in THIS boot, so the timezone
// offset is applied to getLocalTime(). Set by setupNTP() (weather.ino).
bool g_timeReady = false;

// Pool Temp / Govee integration (persisted + runtime)
String g_goveeKey = "";       // Govee Open API key
String g_poolDeviceId = "";   // selected thermometer device id (MAC)
String g_poolModel = "";      // selected device model, e.g. "H5075"
String g_poolName = "";       // selected device name
struct GoveeDev { char id[64]; char model[16]; char name[40]; };
#define MAX_GOVEE 10
GoveeDev g_goveeDevs[MAX_GOVEE];
int g_goveeCount = 0;         // how many thermometers found on the account
int g_goveeSel = 0;           // selected index
float g_poolTemp = 0;         // latest temperature (F)
char  g_poolUnit = 'F';       // unit of the fetched value
bool  g_poolValid = false;    // true when a temp was fetched successfully
bool  g_goveeAuthBad = false; // Govee API rejected the key (401/403) -> invalid creds
bool  g_poolEnabled = false;  // Pool Temp feature on/off (defaults off)
unsigned long g_lastPool = 0;
const unsigned long POOL_REFRESH_MS = 300UL * 1000UL;  // 5 min (respect Govee API limits)
// UTC epoch of the Govee rate-limit window end (NVS "goveerl"); persisted so
// deep-sleep wakes don't re-hit a long daily-limit window. 0 = not limited.
unsigned long g_goveeRateReset = 0;
// Millis deadline to retry when that window lifts - without it a suppressed
// fetch waits out the 5-min cadence, longer than a touch-wake stays awake.
unsigned long g_poolRetryAt = 0;

// Pool temp history tiers (raw/hourly/daily) give Day/Week/Month/Year real
// ranges; rollups store per-bucket lo/hi (DEVELOPER.md "Temperature history").
#define MAX_POOL_LOG 2048
float g_poolLogTemp[MAX_POOL_LOG];
unsigned long g_poolLogTime[MAX_POOL_LOG];
int g_poolLogNext = 0;
int g_poolLogCount = 0;

#define MAX_POOL_HOUR 800
float g_poolHourTemp[MAX_POOL_HOUR];
float g_poolHourMin[MAX_POOL_HOUR];
float g_poolHourMax[MAX_POOL_HOUR];
unsigned long g_poolHourTime[MAX_POOL_HOUR];
int g_poolHourNext = 0;
int g_poolHourCount = 0;
long g_curHourBucket = -1;   // epoch/3600 of the in-progress hour
float g_curHourSum = 0;
float g_curHourMin = 0;
float g_curHourMax = 0;
int   g_curHourN = 0;

#define MAX_POOL_DAY 400
float g_poolDayTemp[MAX_POOL_DAY];
float g_poolDayMin[MAX_POOL_DAY];
float g_poolDayMax[MAX_POOL_DAY];
unsigned long g_poolDayTime[MAX_POOL_DAY];
int g_poolDayNext = 0;
int g_poolDayCount = 0;
long g_curDayBucket = -1;    // epoch/86400 of the in-progress day
float g_curDaySum = 0;
float g_curDayMin = 0;
float g_curDayMax = 0;
int   g_curDayN = 0;

// Pool graph screen state
enum PoolTF { TF_DAY, TF_WEEK, TF_MONTH, TF_YEAR };
int g_poolTF = TF_WEEK;

// Weather temp history mirroring pool (10-min samples, so roughly half the
// raw samples for the same span; see weatherfs.ino/poolfs.ino). Always on.
#define MAX_WX_LOG 1200
float g_wxLogTemp[MAX_WX_LOG];
unsigned long g_wxLogTime[MAX_WX_LOG];
int g_wxLogNext = 0;
int g_wxLogCount = 0;

#define MAX_WX_HOUR 720
float g_wxHourTemp[MAX_WX_HOUR];
float g_wxHourMin[MAX_WX_HOUR];
float g_wxHourMax[MAX_WX_HOUR];
unsigned long g_wxHourTime[MAX_WX_HOUR];
int g_wxHourNext = 0;
int g_wxHourCount = 0;
long g_wxHourBucket = -1;   // epoch/3600 of the in-progress hour
float g_wxHourSum = 0;
float g_wxCurHourMin = 0;   // "Cur" prefix: g_wxHourMin/Max are the ring arrays
float g_wxCurHourMax = 0;
int   g_wxHourN = 0;

#define MAX_WX_DAY 365
float g_wxDayTemp[MAX_WX_DAY];
float g_wxDayMin[MAX_WX_DAY];
float g_wxDayMax[MAX_WX_DAY];
unsigned long g_wxDayTime[MAX_WX_DAY];
int g_wxDayNext = 0;
int g_wxDayCount = 0;
long g_wxDayBucket = -1;    // epoch/86400 of the in-progress day
float g_wxDaySum = 0;
float g_wxCurDayMin = 0;
float g_wxCurDayMax = 0;
int   g_wxDayN = 0;

// Weather graph screen state
enum WxTF { WX_DAY, WX_WEEK, WX_MONTH, WX_YEAR };
int g_wxTF = WX_WEEK;

String g_savedSsid = "";   // WiFi loaded from NVS
String g_savedPass = "";
// Network addressing (Settings -> Network -> IP Setup). g_ipDhcp keeps today's
// DHCP behavior; when false the g_static* strings are applied via WiFi.config().
bool   g_ipDhcp    = true;
String g_staticIp   = "";
String g_staticMask = "";
String g_staticGw   = "";
String g_staticDns  = "";
String g_hostname   = "";   // STA hostname (default "cyd-horizon", see loadNetCfg)
bool   g_netCfgDirty = false;  // set on IP-page edits; applied via reconnect on exit
String g_osClientId = "";       // OpenSky OAuth2 client id (blank = anonymous)
String g_osClientSecret = "";   // OpenSky OAuth2 client secret
String g_osToken = "";          // cached OpenSky bearer token
unsigned long g_osTokenExpiry = 0;  // millis() at which g_osToken expires
bool   g_osTokenValid = false;
// Consecutive radar 401s on a fresh token: drives AUTH_BAD and, past
// AUTH_401_BACKOFF_AFTER, the slow retry cadence. Reset on success/cred change.
int    g_auth401Streak = 0;

// True when the last radar/weather poll actually failed (not just empty).
// Drives the "OpenSky Data Unavailable" / "Weather Data Unavailable" labels.
bool   g_radarDataFailed = false;
bool   g_weatherDataFailed = false;
String g_addrSearch = "";  // address search buffer for location geocoding
String g_lastPlace = "";   // human-readable place name from the last successful geocode
// Address-search result code shown on the search status screen (wifiSub 12):
// "empty", "no-match", or a friendly human-readable error message ("" = none).
String g_addrErr = "";
String g_latLonStr = "";   // "lat,lon" edit buffer for the Location page
String g_sleepStartStr = "2200";  // HHMM for editing in settings
String g_sleepEndStr   = "0800";

enum Screen { SCR_DASH, SCR_SETTINGS, SCR_GENERAL, SCR_ABOUT, SCR_HELP, SCR_WIFI, SCR_RESET, SCR_SLEEP, SCR_FTRACKER, SCR_POOL, SCR_POOLGRAPH, SCR_WXGRAPH, SCR_LOCATION, SCR_CALIB, SCR_FLIGHTDETAIL, SCR_CREDITS, SCR_ALARMS, SCR_ALARMFIRE, SCR_COLORPICK };
Screen g_screen = SCR_DASH;
Screen g_creditsReturn = SCR_DASH;   // screen to return to from the OpenSky Credits page
int g_helpScroll = 0;   // Help page vertical scroll offset (px)
int g_resetConfirm = 0;  // Reset screen sub-state: 0=choose, 1=Factory, 2=Settings, 3=Graph Data, 4=Restart

extern int g_wifiSub;   // defined in wifi_config.ino
// Alarm state lives in alarms.ino (alphabetically first of the secondary
// files); these externs make it visible here in the main file.
extern bool   g_alarmFiring;
extern bool   g_alarmDelConfirm;
extern int    g_alarmIdx;
extern int    g_alarmCount;

// ---- Touch calibration state (see calibration.ino) ----
// Defined here because this file's touch code precedes calibration.ino in the concat.
enum CalState { CAL_NONE, CAL_LOADING, CAL_TARGET, CAL_DONE };
CalState g_calState = CAL_NONE;
// Factory defaults measured on this unit: rawY 592..3678 -> dispX 0..320,
// rawX 379..3396 -> dispY 0..240. On-device calibration (NVS) overrides them.
int  g_calScaleX = 9646;   // (3678-592)*1000/320
long g_calOffX   = 592;    // rawY offset
int  g_calScaleY = 12571;  // (3396-379)*1000/240
long g_calOffY   = 379;    // rawX offset
unsigned long g_calLongPressStart = 0;  // millis() when a press began (any screen)

volatile bool dirty = true; // force a redraw across loop/net tasks
bool connected = false;
char lastErr[40] = "connecting...";

unsigned long lastPoll = 0;
// millis() of the last completed flight fetch - the countdown bar references
// this so it empties exactly when the fresh flight appears (not at schedule time).
unsigned long g_lastData = 0;
unsigned long lastClockDraw = 0;
unsigned long g_lastWeather = 0;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
// Open-Meteo 429 backoff: g_wxNextEpoch is a UTC deadline (NVS "wxrl", survives
// deep sleep); g_wxRetryAt is a millis retry for transient cases.
int  g_wx429Streak = 0;
unsigned long g_wxNextEpoch = 0;
unsigned long g_wxRetryAt = 0;
extern bool g_weatherValid;   // declared in weather.ino

// First fetch of the boot: jittered so a fleet doesn't hit the APIs in
// lockstep; waits for SNTP time (verified TLS fails on future-dated certs),
// then retries at a per-boot random 55-65s until BOOT_FAIL_GRACE_MS.
#define BOOT_FETCH_JITTER_MS 30000UL
#define BOOT_TIME_WAIT_MS    10000UL
#define BOOT_FAIL_GRACE_MS   300000UL
// In-window logger wakes get a smaller spread - same-phase boards share the
// 5-min wake tick.
#define SLEEP_WAKE_JITTER_MS 30000UL

// First WiFi connect: start NTP and kick the jittered first load instead of
// waiting on poll timers (setup() couldn't fetch when WiFi is slow).
bool g_wifiConnectedOnce = false;   // true once WiFi has been up since boot
unsigned long g_firstConnectAt = 0; // millis of that first connect
unsigned long g_bootFetchAt = 0;    // millis deadline for the first data fetch
unsigned long g_bootFailRetryMs = 0; // per-boot random 55-65s absent-result retry
bool g_bootFetched = false;         // true once the jittered boot fetch was requested
bool g_firstBoot = false;           // true on first boot (no saved location yet)

// First-boot wizard: calibration if none saved, then WiFi creds; BOOT_DONE =
// normal boot. setup() picks the starting stage.
enum BootStage { BOOT_NONE, BOOT_CALIB, BOOT_WIFI, BOOT_DONE };
BootStage g_bootStage = BOOT_NONE;

// Non-blocking WiFi reconnect state
bool wifiTrying = false;
unsigned long wifiTryStart = 0;

// User dismissed the overhead view via the countdown bar; cleared on the next new overhead flight.
bool g_suppressFlight = false;

// Queued LED blink. Performed in loop() AFTER the flight view is drawn, so the
// user sees the data first and then the blink notification.
bool g_pendingBlink = false;
BlinkColor g_blinkColor = BLINK_BLUE;
BlinkColor g_routeHoldColor = BLINK_NONE;  // route color to keep lit while flight details are shown

// Route for the overhead flight, auto-fetched once per plane and cached until
// the overhead identity changes (flight_details.ino).
String g_routeOrigin = "";      // "KDAL"
String g_routeDest   = "";      // e.g. "KJFK"
bool   g_routeFetched = false;  // true once we've tried (success or not)
volatile bool g_routeBusy = false;  // true while a route fetch is in flight (cross-task)

// adsb.lol planned route is the primary display; OpenSky shows only when its
// actual route differs. IATA codes are display-only - comparisons use ICAO.
String g_adsbRouteOrigin = "";
String g_adsbRouteDest   = "";
String g_adsbOriginIata  = "";      // 3-letter IATA codes for display ("" = none)
String g_adsbDestIata    = "";
String g_adsbOriginCity  = "";
String g_adsbDestCity    = "";
bool   g_adsbRouteFetched = false;
volatile bool g_adsbRouteBusy = false;
unsigned long g_nextAdsbMs = 0; // back-off after a 429 or transport failure

// Ground track for the tracked plane (OpenSky /tracks, once per plane): draws
// the past path and dead-reckons the blip. Points are (dx,dy) miles rel. observer.
#define MAX_TRACK_PTS 64
struct TrackPoint { float dxMi, dyMi; };
TrackPoint g_trackPts[MAX_TRACK_PTS];
volatile int g_trackCount = 0;       // number of valid track points
float g_trackBearingDeg = -1.0f;     // latest track true-track bearing (-1 = none)
bool  g_trackFetched = false;        // tried once for the current plane
volatile bool g_trackBusy = false;   // cross-task guard
portMUX_TYPE g_trackMux = portMUX_INITIALIZER_UNLOCKED;

String g_homeAirport = "";      // home airport: drives the incoming/outgoing LED blink (empty = none)
String g_watchCallsign = "";    // callsign fragment to track preferentially + notify for (substring match) while its flight details are shown
int    g_watchNotify   = 0;     // its "Callsign Notify" preset: index into the shared alarm patterns (NVS "watchntf")
#define NTF_VOL_MIN 1           // Notify Volume floor: never fully inaudible
// Notify Volume is fixed levels; NVS "ntfvol" stores the percent so old values
// snap to the nearest level. UI shows 1-10.
const int kNtfVolLevels[] = { 1, 2, 3, 4, 5, 15, 25, 50, 75, 100 };
#define NTF_VOL_LEVELS ((int)(sizeof(kNtfVolLevels) / sizeof(kNtfVolLevels[0])))
int    g_notifyVol     = 100;   // one of kNtfVolLevels - loudness for all notification sounds (NVS "ntfvol")

// Index of the level nearest a stored percent.
static int ntfVolIdx(int vol) {
  int best = 0;
  for (int i = 1; i < NTF_VOL_LEVELS; i++)
    if (abs(kNtfVolLevels[i] - vol) < abs(kNtfVolLevels[best] - vol)) best = i;
  return best;
}
// g_notifyVol one step in dir (±1), wrapping through kNtfVolLevels.
void notifyVolStep(int dir) {
  g_notifyVol = kNtfVolLevels[(ntfVolIdx(g_notifyVol) + NTF_VOL_LEVELS + dir) % NTF_VOL_LEVELS];
}

// ---- small helpers ----
float hav(float lat1, float lon1, float lat2, float lon2) {
  const float R = 3958.8f; // miles
  float p1 = lat1 * PI / 180.0f, p2 = lat2 * PI / 180.0f;
  float dp = (lat2 - lat1) * PI / 180.0f;
  float dl = (lon2 - lon1) * PI / 180.0f;
  float a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 2 * R * asin(sqrt(a));
}

void sortPlanes() {
  // insertion sort by distance (ascending)
  for (int i = 1; i < planeCount; i++) {
    Plane tmp = planes[i];
    int j = i - 1;
    while (j >= 0 && planes[j].distMi > tmp.distMi) { planes[j + 1] = planes[j]; j--; }
    planes[j + 1] = tmp;
  }
}

// ---- WiFi ----
// Hostname must be set before WiFi.mode(WIFI_STA): arduino-esp32 only writes
// it to the esp_netif when creating the STA interface.
void setNetHostname() {
  if (g_hostname.length() > 0) WiFi.setHostname(g_hostname.c_str());
}

// Apply saved static-IP settings (after WiFi.mode, before WiFi.begin);
// invalid/incomplete settings fall back to DHCP.
void applyNetConfig() {
  if (g_ipDhcp) return;
  IPAddress ip, mask, gw, dns;
  if (!ip.fromString(g_staticIp.c_str()) ||
      !mask.fromString(g_staticMask.c_str()) ||
      !gw.fromString(g_staticGw.c_str())) return;
  if (g_staticDns.length() == 0 || !dns.fromString(g_staticDns.c_str()))
    dns = gw;   // home routers normally answer DNS on the gateway address
  WiFi.config(ip, gw, mask, dns);
}

bool tryConnect(const char* ssid, const char* pass) {
  setNetHostname();
  WiFi.mode(WIFI_STA);
  applyNetConfig();
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---- OpenSky OAuth2 client-credentials auth (Basic auth removed March 2026) ----
const char* kOsTokenUrl = "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
const int   kOsTokenLifetimeMs = 30 * 60 * 1000;   // OpenSky tokens expire after ~30 min

// Per-host trust stores: the smallest bundle covering each cert chain (fewer
// roots parsed per handshake). No fallback bundle - unmapped hosts fail TLS.

// Govee: Amazon Root CA 1
static const char* const kAmazonRootCAs =
  // [9] Amazon Root CA 1
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
  "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
  "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
  "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
  "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
  "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
  "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
  "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
  "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
  "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
  "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
  "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
  "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
  "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
  "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
  "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
  "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
  "rqXRfboQnoZsG4q5WTP468SQvvG5\n"
  "-----END CERTIFICATE-----\n";


// GitHub API: Sectigo / USERTrust ECC root
static const char* const kSectigoUSERTrustEccRootCAs =
  // [1] USERTrust ECC Certification Authority
  "-----BEGIN CERTIFICATE-----\n"
  "MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL\n"
  "MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl\n"
  "eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT\n"
  "JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx\n"
  "MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT\n"
  "Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg\n"
  "VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm\n"
  "aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo\n"
  "I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng\n"
  "o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G\n"
  "A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD\n"
  "VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB\n"
  "zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW\n"
  "RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=\n"
  "-----END CERTIFICATE-----\n";
// OpenSky, open-meteo, Nominatim: Let's Encrypt / ISRG hierarchy
static const char* const kIsrgRootCAs =
  // [2] ISRG Root X1
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
  "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
  "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
  "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
  "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
  "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
  "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
  "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
  "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
  "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
  "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
  "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
  "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
  "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
  "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
  "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
  "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
  "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
  "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
  "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
  "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
  "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
  "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
  "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
  "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
  "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
  "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
  "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
  "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
  "-----END CERTIFICATE-----\n"
  //
  // [3] Root YR
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n"
  "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
  "HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n"
  "BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n"
  "sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n"
  "YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n"
  "JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n"
  "ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n"
  "M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n"
  "vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n"
  "tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n"
  "Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n"
  "6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n"
  "IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n"
  "AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
  "DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n"
  "713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n"
  "LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n"
  "AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n"
  "i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n"
  "dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n"
  "06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n"
  "41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n"
  "EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n"
  "awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n"
  "To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n"
  "sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n"
  "-----END CERTIFICATE-----\n"
  //
  // [4] Root YE
  "-----BEGIN CERTIFICATE-----\n"
  "MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL\n"
  "MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN\n"
  "MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG\n"
  "A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi\n"
  "A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X\n"
  "yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8\n"
  "7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O\n"
  "BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9\n"
  "LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m\n"
  "onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47\n"
  "-----END CERTIFICATE-----\n"
  //
  // [5] Root YR
  "-----BEGIN CERTIFICATE-----\n"
  "MIIF9DCCA9ygAwIBAgIRAPJLbRf52a18scn+p4eCaZ8wDQYJKoZIhvcNAQELBQAw\n"
  "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
  "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMjYwNTEzMDAwMDAw\n"
  "WhcNMzIwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsGA1UEChMESVNSRzEQ\n"
  "MA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIB\n"
  "ANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9sGNiB0BD1fcOxbSUQCJI\n"
  "M1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGgYbSQ4OpzI+DG8SGuTlcE\n"
  "873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBNJAY+OKfX/FUvYKuhjT+n\n"
  "o49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBznZqvbNPLMXMLFxCb3WTfr\n"
  "JBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904M+faKx8hnLCpJ15ZqaEg\n"
  "cNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawTvSZuVvlbRrAlLxIB6pwM\n"
  "BjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEatnMdmDT5BqnKC92bd0Eh\n"
  "M1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lzYal+9zTg7C5DALyVOeG/\n"
  "CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU6H1qGg3DgTOuskf8eahT\n"
  "MiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9IWhH4YZKh3WnJEIt+oQv\n"
  "lYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/BAgMBAAGjgeswgegwDgYD\n"
  "VR0PAQH/BAQDAgEGMBMGA1UdJQQMMAoGCCsGAQUFBwMBMA8GA1UdEwEB/wQFMAMB\n"
  "Af8wHQYDVR0OBBYEFN7nW2DQIm1AKH0/DQH+pLVStFGUMB8GA1UdIwQYMBaAFHm0\n"
  "WeZ7tuXkAXOACIjIGlj26ZtuMDIGCCsGAQUFBwEBBCYwJDAiBggrBgEFBQcwAoYW\n"
  "aHR0cDovL3gxLmkubGVuY3Iub3JnLzATBgNVHSAEDDAKMAgGBmeBDAECATAnBgNV\n"
  "HR8EIDAeMBygGqAYhhZodHRwOi8veDEuYy5sZW5jci5vcmcvMA0GCSqGSIb3DQEB\n"
  "CwUAA4ICAQA8spSI95KKfn2W6GMmDpHBJSPaLbsS3W93cijJCRCYAc1fsJgL1FIL\n"
  "7C0C9ecPOdcwB2fi0Dk2p94j9iTJCxmt5CFSKLRWwnXT2MMSXexVxqoVB79BdWPx\n"
  "VXETkVme/qYSAuKVHh5Ps+5BixgmwS1JkjSAc+MfrUbNssVEEnH0aEiAh+rotXAV\n"
  "JSP/Ye7LJPEwD9DWG72vVWbhAcuOf5OLjz57Ctk7MgQHynZ7+PlHJtajroCaIbtC\n"
  "r6tcZZaAwUQm+jQyeWdV+2hv9deOYFmKeQyjjcSrN5Nadrw+L9DZJLbA1HqeNvLh\n"
  "BgqpP0fvJq2N6EtD574N6eMI7uMsJTnji2UDz9el5XLSv9fqJMuDQtYVb2oTNoKp\n"
  "oUqhxPVC0aq4eG5MESaIdn8b5ZGSSeAJLMHXljEdlNza+ncfkviXk1POLnnFdvx8\n"
  "/gk6M374WbLWFXw8N141B/Rl/tINGfl1TxOIiqtiMYkL02RSGb1kq34BL9NPP27z\n"
  "RGMuHGnzS3hFIrRTfKxrzUZ9RzQWzEG3K6fJ3r2nqSltkeytis9DIBoFY9VmVyjL\n"
  "M71DMi+y1+TRSJVClEMwvA4yL++7q9XZx5r5wBRWB4kQTKH5qyoZnDw7iiuh1lID\n"
  "yDFx8r7i9vIJU5HS3moZLkYWAOilMaV9N56A9Bgb6dNcHkvg3NoaYA==\n"
  "-----END CERTIFICATE-----\n"
  //
  // [6] Root YE
  "-----BEGIN CERTIFICATE-----\n"
  "MIICpjCCAiugAwIBAgIRAIchZfw0tuX7qK3Vs3BftTowCgYIKoZIzj0EAwMwTzEL\n"
  "MAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2VhcmNo\n"
  "IEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDIwHhcNMjYwNTEzMDAwMDAwWhcN\n"
  "MzIwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsGA1UEChMESVNSRzEQMA4G\n"
  "A1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAiA2IABDwS/6vhrcVqcbBo\n"
  "+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7XyFjWsPYhIPt/wzOqxTd2\n"
  "b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW87j77GaOB6zCB6DAOBgNV\n"
  "HQ8BAf8EBAMCAQYwEwYDVR0lBAwwCgYIKwYBBQUHAwEwDwYDVR0TAQH/BAUwAwEB\n"
  "/zAdBgNVHQ4EFgQUo8gmWo6hTNA1Y/ybI8g6rlbzT1YwHwYDVR0jBBgwFoAUfEKW\n"
  "rt5LSDv6kviejM9ti6lyN5UwMgYIKwYBBQUHAQEEJjAkMCIGCCsGAQUFBzAChhZo\n"
  "dHRwOi8veDIuaS5sZW5jci5vcmcvMBMGA1UdIAQMMAowCAYGZ4EMAQIBMCcGA1Ud\n"
  "HwQgMB4wHKAaoBiGFmh0dHA6Ly94Mi5jLmxlbmNyLm9yZy8wCgYIKoZIzj0EAwMD\n"
  "aQAwZgIxAMU19WCtmxVND8UHBZRoma49Z7jPs64Dma0eTu1OChVbB/2J7GV3nvYK\n"
  "Ax54uk1G9QIxAO0miLVJu8PLNiXXXkiE/gsK3CTRTF/aeo4bMX42Zw40csRU6AC2\n"
  "6hSW1/IWaas6dg==\n"
  "-----END CERTIFICATE-----\n"
  //
  // [7] ISRG Root X2
  "-----BEGIN CERTIFICATE-----\n"
  "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
  "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
  "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
  "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
  "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
  "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
  "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
  "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
  "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
  "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
  "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
  "/q4AaOeMSQ+2b1tbFfLn\n"
  "-----END CERTIFICATE-----\n"
  //
  // [8] YR1
  "-----BEGIN CERTIFICATE-----\n"
  "MIIE2zCCAsOgAwIBAgIRAKICU/FfJpHAXcHOE7m8yk4wDQYJKoZIhvcNAQELBQAw\n"
  "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
  "HhcNMjUwOTAzMDAwMDAwWhcNMjgwOTAyMjM1OTU5WjAzMQswCQYDVQQGEwJVUzEW\n"
  "MBQGA1UEChMNTGV0J3MgRW5jcnlwdDEMMAoGA1UEAxMDWVIxMIIBIjANBgkqhkiG\n"
  "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAoVi8X2xCYgMXvJxNPKp/oF13UMgmPABB07VC\n"
  "LNDtoXmt9luEZNJSBV10VyT1Pz6LD8Zq1d2gc43WNl1AdRrj4sEnazbOiz0nPpmG\n"
  "Bp2hui49oZtDIY6wdKeZAi5BbNU20CH6RSBBMLSQ9cXrH8dxdv4PAJ45ssGML68U\n"
  "SE3BsjC2a6cAN9L5CgXVIQi5tfNiTPoFZZ3S0OlXqLmmtdV95udWAb5b6e/F49Di\n"
  "CsH0Y00Ag72BVIb1hzynmKe+X0mERBTtsb3BwmpV9ipeBjMLoR/D9cHxHQCWoi5l\n"
  "TmXwY015J5rGelz1nZjJuxc2kioaX29XJBnhMkP531rSdG5uMwIDAQABo4HuMIHr\n"
  "MA4GA1UdDwEB/wQEAwIBhjATBgNVHSUEDDAKBggrBgEFBQcDATASBgNVHRMBAf8E\n"
  "CDAGAQH/AgEAMB0GA1UdDgQWBBQfLzW+RhSCzUCxrnksVXj699Ro+zAfBgNVHSME\n"
  "GDAWgBTe51tg0CJtQCh9Pw0B/qS1UrRRlDAyBggrBgEFBQcBAQQmMCQwIgYIKwYB\n"
  "BQUHMAKGFmh0dHA6Ly95ci5pLmxlbmNyLm9yZy8wEwYDVR0gBAwwCjAIBgZngQwB\n"
  "AgEwJwYDVR0fBCAwHjAcoBqgGIYWaHR0cDovL3lyLmMubGVuY3Iub3JnLzANBgkq\n"
  "hkiG9w0BAQsFAAOCAgEA0+zvMq3kHig1ddTmmm+RibTr9/RpX7k4buanMMRqbV/y\n"
  "IvP82zAHN3mvaw+cASuVsdpd0ikjhr4hnhJQLQOzOp2ccKrsdGOAgo0vddeISFAq\n"
  "EWEV4lmUM3vFF796up+bSgmJ1u6RupDCMxDgF8M3eLvGuj6L0lu3zkQ0KuQLnKxL\n"
  "tB0oQqn1Idg5CuuGpMvQzk29Pa3D/qHurc0EIM9SxukQuJqq63lxsYyRQFU8yMBO\n"
  "hq1w5LbfaWNRrz1uklOfI/pYkAb2E2MTZrAMQkBIE2S8Jt1F8gRc96o/xOsrgvSk\n"
  "a84AisX6xq1lz1Z7jGvrnXc4TMcjxZTjiTaihcYI1JIXZiLtEMSCa5l3cu8YWd6z\n"
  "dLRQlqRdclVjuQfNHawRJ6GWlkK0QJosivTKwdBw3KxEtzGo8yMHERbsy57gP1UX\n"
  "HOMcmZYQC0gtyR3SxfenIM/MxC3Ia2Ypab/kQ/CTnlIn2KQ5JUC6NYrGCbhFN9bp\n"
  "5lKJStEwCUnLpntcrXk5XVDCNv/5RyWpRThkGOV7GetKkQ0qAY8hCzWK6oqnAhDZ\n"
  "cjlYVdWfqOw3DIOX6EDNBgAqHarRVxyF9QZdOaXSyPJ0ueD2BYJEBgaCGQ8rAaU/\n"
  "Qc123V5LTXDZW4CcsPBDyhy4v+c8hClAyw/IkJlfBqxB9D+/wvIMHgECZ4ptP6o=\n"
  "-----END CERTIFICATE-----\n";


// Earliest bundled-root expiry (ISRG Root X1 = 2035-06; conservative). Past it
// OTA may fall back to setInsecure() so a root rotation can't block updates.
#define OTA_CA_EXPIRY 2051222400UL

// Smallest trust store covering the host; unmapped hosts fail TLS (no fallback
// bundle). OTA may still go insecure past OTA_CA_EXPIRY.
// adsb.lol / vrs-standing-data: GlobalSign ECC Root CA - R4 + WE1 intermediate.
static const char* const kGlobalSignEccRootCAs =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIB4TCCAYegAwIBAgIRKjikHJYKBN5CsiilC+g0mAIwCgYIKoZIzj0EAwIwUDEk\n"
  "MCIGA1UECxMbR2xvYmFsU2lnbiBFQ0MgUm9vdCBDQSAtIFI0MRMwEQYDVQQKEwpH\n"
  "bG9iYWxTaWduMRMwEQYDVQQDEwpHbG9iYWxTaWduMB4XDTEyMTExMzAwMDAwMFoX\n"
  "DTM4MDExOTAzMTQwN1owUDEkMCIGA1UECxMbR2xvYmFsU2lnbiBFQ0MgUm9vdCBD\n"
  "QSAtIFI0MRMwEQYDVQQKEwpHbG9iYWxTaWduMRMwEQYDVQQDEwpHbG9iYWxTaWdu\n"
  "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEuMZ5049sJQ6fLjkZHAOkrprlOQcJ\n"
  "FspjsbmG+IpXwVfOQvpzofdlQv8ewQCybnMO/8ch5RikqtlxP6jUuc6MHaNCMEAw\n"
  "DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFFSwe61F\n"
  "uOJAf/sKbvu+M8k8o4TVMAoGCCqGSM49BAMCA0gAMEUCIQDckqGgE6bPA7DmxCGX\n"
  "kPoUVy0D7O48027KqGx2vKLeuwIgJ6iFJzWbVsaj8kfSt24bAgAXqmemFZHe+pTs\n"
  "ewv4n4Q=\n"
  "-----END CERTIFICATE-----\n" \
  "-----BEGIN CERTIFICATE-----\n"
  "MIICjjCCAjOgAwIBAgIQf/NXaJvCTjAtkOGKQb0OHzAKBggqhkjOPQQDAjBQMSQw\n"
  "IgYDVQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkds\n"
  "b2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMjMxMjEzMDkwMDAwWhcN\n"
  "MjkwMjIwMTQwMDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRy\n"
  "dXN0IFNlcnZpY2VzMQwwCgYDVQQDEwNXRTEwWTATBgcqhkjOPQIBBggqhkjOPQMB\n"
  "BwNCAARvzTr+Z1dHTCEDhUDCR127WEcPQMFcF4XGGTfn1XzthkubgdnXGhOlCgP4\n"
  "mMTG6J7/EFmPLCaY9eYmJbsPAvpWo4IBAjCB/zAOBgNVHQ8BAf8EBAMCAYYwHQYD\n"
  "VR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMBIGA1UdEwEB/wQIMAYBAf8CAQAw\n"
  "HQYDVR0OBBYEFJB3kjVnxP+ozKnme9mAeXvMk/k4MB8GA1UdIwQYMBaAFFSwe61F\n"
  "uOJAf/sKbvu+M8k8o4TVMDYGCCsGAQUFBwEBBCowKDAmBggrBgEFBQcwAoYaaHR0\n"
  "cDovL2kucGtpLmdvb2cvZ3NyNC5jcnQwLQYDVR0fBCYwJDAioCCgHoYcaHR0cDov\n"
  "L2MucGtpLmdvb2cvci9nc3I0LmNybDATBgNVHSAEDDAKMAgGBmeBDAECATAKBggq\n"
  "hkjOPQQDAgNJADBGAiEAokJL0LgR6SOLR02WWxccAq3ndXp4EMRveXMUVUxMWSMC\n"
  "IQDspFWa3fj7nLgouSdkcPy1SdOR2AGm9OQWs7veyXsBwA==\n"
  "-----END CERTIFICATE-----\n";

static bool hostSuffixMatches(const char* host, size_t len, const char* suffix) {
  size_t slen = strlen(suffix);
  if (len < slen) return false;
  const char* p = host + len - slen;
  for (size_t i = 0; i < slen; ++i) {
    char a = p[i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}
static const char* trustStoreForUrl(const char* url) {
  const char* p = strstr(url, "://");
  const char* host = p ? p + 3 : url;
  const char* end = host;
  while (*end && *end != '/' && *end != ':' && *end != '?') ++end;
  size_t len = end - host;
  if (hostSuffixMatches(host, len, "opensky-network.org"))      return kIsrgRootCAs;
  if (hostSuffixMatches(host, len, "open-meteo.com"))           return kIsrgRootCAs;
  if (hostSuffixMatches(host, len, "openstreetmap.org"))        return kIsrgRootCAs;
  if (hostSuffixMatches(host, len, "govee.com"))                return kAmazonRootCAs;
  // GitHub API is Sectigo/USERTrust-signed; GitHub release assets are
  // served from *.githubusercontent.com and use Let's Encrypt / ISRG.
  if (hostSuffixMatches(host, len, "github.com"))               return kSectigoUSERTrustEccRootCAs;
  if (hostSuffixMatches(host, len, "githubusercontent.com"))    return kIsrgRootCAs;
  if (hostSuffixMatches(host, len, "vrs-standing-data.adsb.lol")) return kGlobalSignEccRootCAs;
  return nullptr;  // unmapped host: fail TLS setup cleanly
}

// Attach the host's smallest root bundle and start a verified request.
// allowInsecure (OTA only) may setInsecure() past OTA_CA_EXPIRY - but never as
// a retry after a validation failure, which would let a MITM defeat it.
bool httpsBegin(HTTPClient& http, NetworkClientSecure& sec, const char* url, bool allowInsecure) {
  const char* trust = trustStoreForUrl(url);
  if (!trust) return false;  // unmapped host: no bundle to validate with
  if (allowInsecure && time(nullptr) >= (time_t)OTA_CA_EXPIRY) sec.setInsecure();
  else sec.setCACert(trust);
  return http.begin(sec, url);
}

// Dev-only heap/stack/location diagnostic. No-op when the build is not a -dev version.
void logHeapDiag(const char* why) {
  if (!isDevBuild()) return;
  size_t freeHeap = ESP.getFreeHeap();
  size_t maxAlloc = ESP.getMaxAllocHeap();
  size_t minFree  = ESP.getMinFreeHeap();
  size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t intMax = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t netStackUsed = 0;
  if (g_netTask) {
    size_t highWaterWords = (size_t)uxTaskGetStackHighWaterMark(g_netTask);
    netStackUsed = NET_TASK_STACK_BYTES - highWaterWords * sizeof(StackType_t);
  }
  size_t psramTotal = ESP.getPsramSize();
  size_t psramFree  = ESP.getFreePsram();
  Serial.printf("[heap] %s free=%u maxalloc=%u intfree=%u intmax=%u minfree=%u netStack=%uB psram=%u/%u lat=%.4f lon=%.4f r=%.1f\n",
                why, (unsigned)freeHeap, (unsigned)maxAlloc, (unsigned)intFree,
                (unsigned)intMax, (unsigned)minFree, (unsigned)netStackUsed,
                (unsigned)psramFree, (unsigned)psramTotal, g_lat, g_lon, g_radiusMi);
}

// How many connection attempts (and ms between them) a retrying HTTPS request
// makes before giving up on a transport-layer failure. Mirrors the OTA retry.
#define HTTPS_RETRY_ATTEMPTS 3
#define HTTPS_RETRY_DELAY_MS 1500
// Own HTTP-method constants: some cores scope HTTPClient's HTTP_METHOD_* enums to the class.
#define HTTPS_METHOD_GET 0
#define HTTPS_METHOD_POST 1

// Verified-TLS request with transport retries: fresh connects can drop after
// long uptime, so retry the whole begin()+request; any real HTTP reply ends it.
// `headers` is nullptr-terminated - HTTPClient clears addHeader() on begin().
int httpsRequestRetry(HTTPClient& http, NetworkClientSecure& sec, const char* url,
                      int method, const String& body, const char* const* headers,
                      bool allowInsecure) {
  // HTTP/1.0 keeps the body unchunked so callers can stream-parse without
  // buffering; keep-alive is moot since we reconnect per attempt.
  http.useHTTP10(true);
  // Persistent per-client: setUserAgent() survives begin()/end(), unlike
  // addHeader(), so setting it here covers every request this helper makes.
  http.setUserAgent(appUserAgent());
  int code = -1;
  for (int attempt = 1; attempt <= HTTPS_RETRY_ATTEMPTS; attempt++) {
    http.end();                // release any previous attempt's connection
    sec.stop();                // close the TLS socket cleanly
    if (attempt > 1) delay(HTTPS_RETRY_DELAY_MS);
    if (isDevBuild()) {
      Serial.printf("[tls] pre attempt=%d intfree=%u intmax=%u\n", attempt,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!httpsBegin(http, sec, url, allowInsecure)) continue;   // connect failed -> retry
    if (headers) {
      for (int i = 0; headers[i] && headers[i + 1]; i += 2) {
        if (headers[i + 1][0]) http.addHeader(headers[i], headers[i + 1]);
      }
    }
    code = (method == HTTPS_METHOD_POST) ? http.POST(body) : http.GET();
    if (code >= 0) break;      // server responded (non-200): don't retry
  }
  if (code < 0 && isDevBuild()) {
    char tlsErr[128] = {};
    int mbedErr = sec.lastError(tlsErr, sizeof tlsErr);
    Serial.printf("[tls] fail code=%d mbedtls=%d %s\n", code, mbedErr, tlsErr);
  }
  return code;
}

// Percent-encode a string for an application/x-www-form-urlencoded body.
String urlEncode(const String& s) {
  String out;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof buf, "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// Route to display: adsb.lol planned, or OpenSky when its actual airports
// differ. Returns "ICAO | IATA" codes (when provided + Show IATA) + cities.
void getRouteDisplay(String& origin, String& originCity, String& dest, String& destCity, bool& hasData) {
  origin = ""; originCity = ""; dest = ""; destCity = ""; hasData = false;
  String adsbO = (g_adsbRouteBusy || g_adsbRouteOrigin.length() == 0) ? "" : g_adsbRouteOrigin;
  String adsbD = (g_adsbRouteBusy || g_adsbRouteDest.length() == 0) ? "" : g_adsbRouteDest;
  String adsbOC = (g_adsbRouteBusy || g_adsbOriginCity.length() == 0) ? "" : g_adsbOriginCity;
  String adsbDC = (g_adsbRouteBusy || g_adsbDestCity.length() == 0) ? "" : g_adsbDestCity;
  String openO = (g_routeBusy || g_routeOrigin.length() == 0) ? "" : g_routeOrigin;
  String openD = (g_routeBusy || g_routeDest.length() == 0) ? "" : g_routeDest;
  bool openDiffers = (openO.length() && openO != adsbO) || (openD.length() && openD != adsbD);
  if (openDiffers) {
    origin = openO; dest = openD;
    originCity = airportCity(openO.c_str());
    destCity = airportCity(openD.c_str());
  } else if (adsbO.length() || adsbD.length()) {
    origin = adsbO; dest = adsbD;
    originCity = adsbOC.length() ? adsbOC : airportCity(origin.c_str());
    destCity = adsbDC.length() ? adsbDC : airportCity(dest.c_str());
    // Show "ICAO | IATA" when enabled and adsb.lol provided an IATA code (the
    // ICAO codes above were already used for the diff check and city lookup).
    if (g_showIata) {
      if (origin.length() && g_adsbOriginIata.length()) origin += " | " + g_adsbOriginIata;
      if (dest.length()   && g_adsbDestIata.length())   dest   += " | " + g_adsbDestIata;
    }
  } else if (openO.length() || openD.length()) {
    origin = openO; dest = openD;
    originCity = airportCity(openO.c_str());
    destCity = airportCity(openD.c_str());
  }
  hasData = (origin.length() || dest.length() || g_adsbRouteFetched || g_routeFetched);
}

// Ensure g_osToken holds a valid bearer token (fetch when needed). false = no
// client configured (go anonymous) or rejected creds (callers flag AUTH_BAD).
bool openskyEnsureToken() {
  g_osHandshakeFailed = false;   // fresh for each call (cached-token path keeps it false)
  if (g_osTokenValid && g_osToken.length() > 0 && millis() < g_osTokenExpiry) {
    return true;
  }
  g_osToken = "";
  g_osTokenValid = false;
  if (g_osClientId.length() == 0 || g_osClientSecret.length() == 0) {
    return false;  // no client configured -> anonymous
  }
  // Verified TLS, no insecure fallback: a handshake failure is a hard error,
  // not a downgrade to anonymous. 400/401 = bad creds (AUTH_BAD); transport/
  // 429/5xx = transient (g_osHandshakeFailed keeps the last auth state).
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  String body = "grant_type=client_credentials&client_id=" + urlEncode(g_osClientId) +
                "&client_secret=" + urlEncode(g_osClientSecret);
  const char* tokenHdrs[] = { "Content-Type", "application/x-www-form-urlencoded", nullptr };
  int code = httpsRequestRetry(http, sec, kOsTokenUrl, HTTPS_METHOD_POST, body, tokenHdrs, false);
  if (isDevBuild()) Serial.printf("[net] OpenSky token response code=%d\n", code);
  if (code < 0) { g_osHandshakeFailed = true; return false; }  // TLS/transport failure on all attempts
  if (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_BAD_REQUEST) return false;  // genuine bad creds
  if (code != HTTP_CODE_OK) { g_osHandshakeFailed = true; return false; }  // transient server error (429/5xx)
  BoundedAllocator tokenAlloc(8192);
  JsonDocument doc(&tokenAlloc);
  HttpBodyStream tokenBody(http);
  DeserializationError parseErr = deserializeJson(doc, tokenBody);
  bool bodyComplete = tokenBody.complete() || tokenBody.drain();
  http.end();
  if (parseErr) {
    if (isDevBuild()) Serial.printf("[net] OpenSky token parse error=%s bytes=%u\n",
                                   parseErr.c_str(), (unsigned)tokenBody.bytesRead());
    g_osHandshakeFailed = true;
    return false;
  }
  if (!bodyComplete && isDevBuild()) {
    Serial.printf("[net] OpenSky token body framing incomplete bytes=%u len=%ld stalled=%d\n",
                  (unsigned)tokenBody.bytesRead(), tokenBody.contentLength(),
                  (int)tokenBody.stalled());
  }
  const char* tok = doc["access_token"];
  if (!tok || !tok[0]) { g_osHandshakeFailed = true; return false; }
  g_osToken = tok;
  int expiresIn = doc["expires_in"] | (kOsTokenLifetimeMs / 1000);
  int margin = (expiresIn > 30) ? 30 : 5;   // refresh shortly before expiry
  g_osTokenExpiry = millis() + (unsigned long)(expiresIn - margin) * 1000UL;
  g_osTokenValid = true;
  return true;
}

// ---- Async network task ----
// Synchronous HTTP can block for seconds; fetches run on the other core so the
// loop (touch + drawing) stays responsive. One fetch at a time.
volatile bool netWantFlights      = false;
volatile bool netWantWeather      = false;
volatile bool netWantLocation     = false;
volatile bool netWantPool         = false;   // refresh the current pool temp
volatile bool netWantPoolDevices  = false;   // list Govee thermometers
volatile bool netWantUpdateCheck  = false;   // query GitHub for the latest release
volatile bool netWantAutoScan     = false;   // daily auto-update scan (runs off the loop task)
volatile bool netBusy             = false;   // a fetch is currently running
volatile bool netUpdated          = false;   // set when a fetch finishes

void netTask(void* p) {
  for (;;) {
    vTaskDelay(30 / portTICK_PERIOD_MS);
    if (WiFi.status() != WL_CONNECTED) continue;
    // Pause net fetches during OTA: concurrent lwIP use can trip a FreeRTOS assert.
    if (g_otaRunning) { vTaskDelay(30); continue; }
    // User-initiated update check gets priority so the About page responds
    // quickly even while background fetches (flights/weather/pool) are queued.
    if (netWantUpdateCheck) { netWantUpdateCheck=false; netBusy=true; checkForUpdate(); netBusy=false; netUpdated=true; }
    else if (netWantLocation) { netWantLocation = false;   netBusy = true; fetchIpLocation();    netBusy = false; netUpdated = true; }
    else if (netWantFlights)  { netWantFlights  = false;   netBusy = true; fetchFlights();       netBusy = false; netUpdated = true; g_lastData = millis(); }
    else if (netWantWeather)  { netWantWeather  = false;   netBusy = true; fetchWeather();       netBusy = false; netUpdated = true; }
    else if (netWantPoolDevices){ netWantPoolDevices = false; netBusy = true; fetchGoveeDevices(); netBusy = false; netUpdated = true; }
    else if (netWantPool)    { netWantPool = false;       netBusy = true; fetchGoveeTemp();     netBusy = false; netUpdated = true; }
    else if (netWantAutoScan)   { netWantAutoScan=false;   netBusy=true; autoScanOnce();     netBusy=false; netUpdated=true; }
  }
}

// ---- OpenSky fetch + parse ----
void fetchFlights() {
  planeCount = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (g_savedSsid.length() && !tryConnect(g_savedSsid.c_str(), g_savedPass.c_str())) {
      snprintf(lastErr, sizeof lastErr, "no wifi"); dirty = true; return;
    }
    if (WiFi.status() != WL_CONNECTED) { snprintf(lastErr, sizeof lastErr, "no wifi"); dirty = true; return; }
    connected = true;
  }
  // Scope so sec/http (and their TLS buffers) are freed before route/track TLS.
  {
  NetworkClientSecure sec;
  HTTPClient http;
  char osurl[240];
  // Bound the query to what the radar draws (2x radius, min 8 mi); lon widened
  // by 1/cos(lat) so the box is a true circle.
  const float bboxMi = max(g_radiusMi * 2.0f, 8.0f);
  const float dLat = bboxMi / 69.0f;                        // ~1 deg lat ~ 69 mi
  const float dLon = dLat / cosf(g_lat * PI / 180.0f);
  snprintf(osurl, sizeof osurl,
    "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
    g_lat - dLat, g_lon - dLon, g_lat + dLat, g_lon + dLon);
  if (isDevBuild()) {
    Serial.printf("[net] flights query lat=%.5f lon=%.5f bbox=%.1fmi url=%s\n",
                  g_lat, g_lon, bboxMi, osurl);
  }
  http.setTimeout(5000);
  // Whitelist the rate-limit headers - HTTPClient discards all but a built-in set otherwise.
  const char* hdrKeys[] = { "X-Rate-Limit-Remaining", "X-Rate-Limit-Retry-After-Seconds" };
  http.collectHeaders(hdrKeys, 2);
  // OAuth2 creds raise the rate limit (4000 vs 400/day). A token-exchange
  // transport failure is transient, not bad creds - keep the last auth state.
  bool authed = openskyEnsureToken();
  if (isDevBuild()) {
    Serial.printf("[net] OpenSky auth client=%s token=%s handshake=%d\n",
                  g_osClientId.length() ? "set" : "blank",
                  authed ? "set" : "none", (int)g_osHandshakeFailed);
  }
  String authHdr;
  if (authed) authHdr = "Bearer " + g_osToken;
  const char* flightHdrs[] = { "Authorization", authHdr.c_str(), nullptr };
  int code = httpsRequestRetry(http, sec, osurl, HTTPS_METHOD_GET, "", flightHdrs, false);
  g_authChecked = true;   // we've made a real OpenSky attempt; auth state is now meaningful
  // Negative code = transport/TLS failure (transient), not bad creds - keep
  // the last auth state; the next poll recovers.
  if (code < 0) {
    g_osHandshakeFailed = true;
    g_radarDataFailed = true;
    snprintf(lastErr, sizeof lastErr, "tls fail %d", code);
    http.end();
    dirty = true;
    return;
  }
  if (code == HTTP_CODE_UNAUTHORIZED && authed) {
    // Token rejected: likely stale (~30 min expiry) - mint a fresh one and
    // retry now instead of waiting a whole poll cycle.
    http.end();
    g_osTokenValid = false;
    g_osToken = "";
    if (openskyEnsureToken()) {
      authHdr = "Bearer " + g_osToken;
      const char* retryHdrs[] = { "Authorization", authHdr.c_str(), nullptr };
      http.setTimeout(5000);
      http.collectHeaders(hdrKeys, 2);
      code = httpsRequestRetry(http, sec, osurl, HTTPS_METHOD_GET, "", retryHdrs, false);
      if (isDevBuild()) Serial.printf("[net] 401 retried with fresh token code=%d\n", code);
    } else {
      // Couldn't mint a new token - openskyEnsureToken() already classified
      // transient vs bad-creds; fall through with the 401.
      code = HTTP_CODE_UNAUTHORIZED;
    }
  }
  if (code == HTTP_CODE_UNAUTHORIZED) {
    // 401 with a fresh token = genuinely rejected creds for a configured
    // client (anonymous gets no claim); self-clears on the next success.
    if (g_osClientId.length() > 0 && !g_osHandshakeFailed) {
      if (g_auth401Streak < 1000) g_auth401Streak++;
      g_authState = AUTH_BAD;
      // Past the streak limit, back off to the slow recovery cadence instead of
      // retrying every poll interval.
      if (g_auth401Streak >= AUTH_401_BACKOFF_AFTER) {
        g_nextRadarMs = millis() + CREDIT_RECOVERY_MS;
        if (isDevBuild())
          Serial.printf("[net] 401 streak=%d, backing off radar polling to %lus\n",
                        g_auth401Streak, CREDIT_RECOVERY_MS / 1000UL);
      }
    }
    g_osTokenValid = false;
    snprintf(lastErr, sizeof lastErr, "401");
    http.end();
    dirty = true;
    return;
  }
  if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
    // Rate limited: treat as exhausted and back off per X-Rate-Limit-Retry-After-Seconds.
    g_creditsKnown = true;
    g_creditsRemaining = 0;
    g_creditsExhausted = true;
    String retry = http.header("X-Rate-Limit-Retry-After-Seconds");
    unsigned long waitMs = retry.length() ? (unsigned long)retry.toInt() * 1000UL : CREDIT_RECOVERY_MS;
    g_nextRadarMs = millis() + waitMs;
    if (isDevBuild() && retry.length()) Serial.printf("[net] 429 radar retry after %lus\n", (unsigned long)retry.toInt());
    snprintf(lastErr, sizeof lastErr, "429 no credits");
    g_radarDataFailed = true;
    http.end();
    dirty = true;
    return;
  }
  if (code != HTTP_CODE_OK) {
    snprintf(lastErr, sizeof lastErr, "http %d", code);
    g_radarDataFailed = true;
    http.end();
    dirty = true;
    return;
  }
  // Auth state reflects what OpenSky accepted: rejected exchange + configured
  // client = AUTH_BAD; a transport failure keeps the last known state.
  if (!g_osHandshakeFailed) {
    if (g_osClientId.length() > 0) g_authState = (authed ? AUTH_OK : AUTH_BAD);
    else g_authState = AUTH_ANON;
  }
  // A successful poll clears any 401 streak (and its backoff), so a temporary
  // server-side rejection can't leave a permanent "Invalid Creds" error.
  g_auth401Streak = 0;
  // Capture remaining credits from the rate-limit header (collected via
  // http.collectHeaders() above).
  String rem = http.header("X-Rate-Limit-Remaining");
  if (rem.length()) {
    g_creditsRemaining = rem.toInt();
    g_creditsKnown = true;
    g_creditsExhausted = (g_creditsRemaining <= LOW_CREDIT_THRESHOLD);
  }
  // Stream-parse states[] one row at a time - buffering it all would eat the
  // contiguous heap the next TLS handshake needs (we only keep MAXP planes).
  HttpBodyStream body(http);
  BoundedAllocator rowAlloc(4096);   // one state row; belt-and-suspenders cap
  JsonDocument st(&rowAlloc);
  if (!seekArray(body, "\"states\"")) {
    if (body.peek() == 'n') {
      // "states":null = empty bbox - a valid empty result, not a JSON error.
      bool ok = body.drain();
      http.end();
      if (isDevBuild()) Serial.printf("[net] flights states null len=%u ok=%d\n", (unsigned)body.bytesRead(), (int)ok);
      snprintf(lastErr, sizeof lastErr, "%d aircraft", planeCount);
      dirty = true;
      return;   // planeCount is already 0, no error
    }
    snprintf(lastErr, sizeof lastErr, "json no states");
    Serial.printf("[net] flights json no states len=%u\n", (unsigned)body.bytesRead());
    g_radarDataFailed = true;
    body.drain();
    http.end();
    dirty = true;
    return;
  }
  for (;;) {
    if (!nextElement(body, st)) break;
    if (st.isNull()) continue;
    if (planeCount >= MAXP) continue;   // keep draining the stream so complete() is meaningful

    JsonVariant csV = st[1];
    JsonVariant lonV = st[5];
    JsonVariant latV = st[6];
    JsonVariant altV = st[7];
    JsonVariant ogV = st[8];
    JsonVariant velV = st[9];
    JsonVariant hdgV = st[10];   // true_track (deg), null if not reported

    if (csV.isNull() || lonV.isNull() || latV.isNull() || ogV.isNull()) continue;
    bool onGround = ogV.as<bool>();
    if (onGround) continue;

    float lat = latV.as<float>();
    float lon = lonV.as<float>();
    float altM = altV.isNull() ? 0.0f : altV.as<float>();

    // altitude ceiling filter
    if (g_ceilingFt > 0 && altM * 3.28084f > g_ceilingFt) continue;

    // rough radar range: keep planes within the fetched bbox (2x radius, min 8 mi)
    float distMi = hav(g_lat, g_lon, lat, lon);
    if (distMi > bboxMi) continue;

    Plane &p = planes[planeCount++];
    JsonVariant icaoV = st[0];
    strncpy(p.icao24, icaoV.isNull() ? "" : icaoV.as<const char*>(), 6);
    p.icao24[6] = 0;
    strncpy(p.callsign, csV.as<const char*>(), 8);
    p.callsign[8] = 0;
    p.distMi = distMi;
    p.altFt = (int)round(altM * 3.28084f);
    p.spdKt = velV.isNull() ? 0 : (int)round(velV.as<float>() * 1.94384f);
    p.hdgDeg = hdgV.isNull() ? -1.0f : hdgV.as<float>();
    p.lastStepMs = millis();   // dead-reckoning starts fresh from real data
    // dx = east miles, dy = north miles
    float dlat = (lat - g_lat);
    float dlon = (lon - g_lon) * cos(g_lat * PI / 180.0f);
    p.dyMi = dlat * 69.0f;
    p.dxMi = dlon * 69.0f;
  }
  bool bodyOk = body.drain();
  http.end();
  if (!bodyOk) {
    snprintf(lastErr, sizeof lastErr, "json short");
    Serial.printf("[net] flights json short len=%u/%ld%s\n", (unsigned)body.bytesRead(),
                  body.contentLength(), body.stalled() ? " stalled" : "");
    g_radarDataFailed = true;
    dirty = true;
    return;
  }
  }  // end scoped fetch block (frees states sec/http + TLS context before route/track handshake)
  sortPlanes();
  g_radarDataFailed = false;
  if (isDevBuild()) Serial.printf("[net] flights ok planes=%d free=%u\n", planeCount, (unsigned)ESP.getFreeHeap());
  // A watched callsign that would draw is promoted to planes[0] (substring
  // match + distance sort = nearest wins) so downstream tracks it.
  int watchIdx = -1;
  if (g_watchCallsign.length() > 0) {
    for (int i = 0; i < planeCount; i++) {
      if (isWatchedCallsign(planes[i].callsign) && blipOnScreen(planes[i])) {
        watchIdx = i;
        break;
      }
    }
  }
  if (watchIdx > 0) {
    Plane t = planes[0];
    planes[0] = planes[watchIdx];
    planes[watchIdx] = t;
  }
  bool watchShown = (watchIdx >= 0);
  // A new overhead flight re-shows the view even after a dismiss; an identity
  // change resets the route cache (it belongs to the previous plane).
  static char lastOverheadIcao[7] = "";
  static char lastBlinkIcao[7] = "";    // so we re-blink when the color changes
  static BlinkColor lastBlinkColor = BLINK_NONE;
  if (planeCount > 0 && (planes[0].distMi <= g_radiusMi || watchShown)) {
    g_suppressFlight = false;
    // Snapshot the overhead flight every poll so the "N aircraft" tap can
    // recall it later, current up to when it left.
    g_lastFlight.valid = true;
    strncpy(g_lastFlight.callsign, planes[0].callsign, 8); g_lastFlight.callsign[8] = 0;
    strncpy(g_lastFlight.icao24, planes[0].icao24, 6);     g_lastFlight.icao24[6]   = 0;
    g_lastFlight.altFt  = planes[0].altFt;
    g_lastFlight.spdKt  = planes[0].spdKt;
    g_lastFlight.hdgDeg = planes[0].hdgDeg;
    g_lastFlight.distMi = planes[0].distMi;
    g_lastFlight.dxMi   = planes[0].dxMi;
    g_lastFlight.dyMi   = planes[0].dyMi;
    g_lastFlight.tickMs = planes[0].lastStepMs;
    if (!g_adsbRouteBusy && !g_routeBusy) {
      String o, oc, d, dc;
      bool hasData;
      getRouteDisplay(o, oc, d, dc, hasData);
      g_lastFlight.origin[0] = 0; strncpy(g_lastFlight.origin, o.c_str(), sizeof(g_lastFlight.origin) - 1); g_lastFlight.origin[sizeof(g_lastFlight.origin) - 1] = 0;
      g_lastFlight.dest[0]   = 0; strncpy(g_lastFlight.dest,   d.c_str(), sizeof(g_lastFlight.dest)   - 1); g_lastFlight.dest[sizeof(g_lastFlight.dest)   - 1]   = 0;
      g_lastFlight.originCity[0] = 0; strncpy(g_lastFlight.originCity, oc.c_str(), sizeof(g_lastFlight.originCity) - 1); g_lastFlight.originCity[sizeof(g_lastFlight.originCity) - 1] = 0;
      g_lastFlight.destCity[0]   = 0; strncpy(g_lastFlight.destCity,   dc.c_str(), sizeof(g_lastFlight.destCity)   - 1); g_lastFlight.destCity[sizeof(g_lastFlight.destCity)   - 1] = 0;
      g_lastFlight.routeFetched = hasData;
      // Remember the raw sources so the recall screen can recompute the same
      // choice later, even after the live globals are reset for a new aircraft.
      g_lastFlight.adsbOrigin[0] = 0; strncpy(g_lastFlight.adsbOrigin, g_adsbRouteOrigin.c_str(), 5); g_lastFlight.adsbOrigin[5] = 0;
      g_lastFlight.adsbDest[0]   = 0; strncpy(g_lastFlight.adsbDest,   g_adsbRouteDest.c_str(),   5); g_lastFlight.adsbDest[5]   = 0;
      g_lastFlight.adsbOriginCity[0] = 0; strncpy(g_lastFlight.adsbOriginCity, g_adsbOriginCity.c_str(), sizeof(g_lastFlight.adsbOriginCity) - 1); g_lastFlight.adsbOriginCity[sizeof(g_lastFlight.adsbOriginCity) - 1] = 0;
      g_lastFlight.adsbDestCity[0]   = 0; strncpy(g_lastFlight.adsbDestCity,   g_adsbDestCity.c_str(),   sizeof(g_lastFlight.adsbDestCity)   - 1); g_lastFlight.adsbDestCity[sizeof(g_lastFlight.adsbDestCity)   - 1]   = 0;
      g_lastFlight.openOrigin[0] = 0; strncpy(g_lastFlight.openOrigin, g_routeOrigin.c_str(), 5); g_lastFlight.openOrigin[5] = 0;
      g_lastFlight.openDest[0]   = 0; strncpy(g_lastFlight.openDest,   g_routeDest.c_str(),   5); g_lastFlight.openDest[5]   = 0;
    }
    if (strncmp(planes[0].icao24, lastOverheadIcao, 6) != 0) {
      strncpy(lastOverheadIcao, planes[0].icao24, 6);
      lastOverheadIcao[6] = 0;
      g_routeFetched = false;
      g_routeOrigin = "";
      g_routeDest = "";
      g_adsbRouteFetched = false;
      g_adsbRouteOrigin = "";
      g_adsbRouteDest = "";
      g_adsbOriginIata = "";
      g_adsbDestIata = "";
      g_adsbOriginCity = "";
      g_adsbDestCity = "";
      g_trackFetched = false;
      portENTER_CRITICAL(&g_trackMux);
      g_trackCount = 0;
      g_trackBearingDeg = -1.0f;
      portEXIT_CRITICAL(&g_trackMux);
      lastBlinkIcao[0] = 0;
      lastBlinkColor = BLINK_NONE;
    }
    // Auto-fetch route + ground track once per overhead plane; a failed track
    // leaves g_trackCount=0 (no line; dead-reckoning uses heading/speed).
    unsigned long nowMs = millis();
    if (!g_routeFetched && (long)(nowMs - g_nextRouteMs) >= 0) fetchRoute(planes[0].icao24);
    // A watched flight's track refetches every poll so path + blip follow its
    // real trajectory.
    if ((!g_trackFetched || watchShown) && (long)(nowMs - g_nextTrackMs) >= 0) fetchTrack(planes[0].icao24);
    if (!g_adsbRouteFetched && (long)(nowMs - g_nextAdsbMs) >= 0) fetchAdsbRoute(planes[0].callsign);
    // LED from best route data (OpenSky preferred, adsb.lol fills gaps):
    // yellow home / green arrival / red departure / blue. Watched callsigns
    // skip this - loop() runs their preset instead.
    bool watchMatch = isWatchedCallsign(planes[0].callsign);
    BlinkColor color = watchMatch ? BLINK_NONE : computeBlinkColor();
    if (strncmp(planes[0].icao24, lastBlinkIcao, 6) != 0) {
      strncpy(lastBlinkIcao, planes[0].icao24, 6);
      lastBlinkIcao[6] = 0;
      lastBlinkColor = color;
      g_blinkColor = color;
      g_pendingBlink = !watchMatch;
    } else if (!watchMatch && color != lastBlinkColor) {
      lastBlinkColor = color;
      g_blinkColor = color;
      g_pendingBlink = true;
    }
  } else {
    lastOverheadIcao[0] = 0;
    lastBlinkIcao[0] = 0;
    lastBlinkColor = BLINK_NONE;
  }
  snprintf(lastErr, sizeof lastErr, "%d aircraft", planeCount);
  dirty = true;
}

// ---- IP geolocation for default location (free, no key: ip-api.com) ----
void fetchIpLocation() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://ip-api.com/json/");  // plain HTTP is allowed by this endpoint
  http.setUserAgent(appUserAgent());
  http.setTimeout(5000);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    BoundedAllocator locationAlloc(2048);
    JsonDocument doc(&locationAlloc);
    HttpBodyStream body(http);
    DeserializationError parseErr = deserializeJson(doc, body);
    bool bodyComplete = body.complete() || body.drain();
    if (!parseErr && bodyComplete) {
      g_lat = doc["lat"] | g_lat;
      g_lon = doc["lon"] | g_lon;
      if (g_lat != 0.0f || g_lon != 0.0f) {
        saveFloat("lat", g_lat);
        saveFloat("lon", g_lon);
      }
      fetchWeather();   // refresh weather immediately for the new location
    }
  }
  http.end();
}

// ---- Address geocoding (free, no key: Nominatim / OpenStreetMap) ----
// true on match (updates g_lat/g_lon); lastErr gets a short code on failure.
bool geocodeAddress() {
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(lastErr, sizeof lastErr, "geo 0");   // no connection
    return false;
  }
  String url = "https://nominatim.openstreetmap.org/search?format=json&limit=1&q="
             + urlEncode(g_addrSearch);

  // Nominatim is Let's Encrypt signed, verified against the same ISRG roots.
  // Scope so sec/http (and their TLS buffers) are freed before fetchWeather().
  {
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  // User-Agent is set centrally in httpsRequestRetry (appUserAgent()).
  int code = httpsRequestRetry(http, sec, url.c_str(), HTTPS_METHOD_GET, "", nullptr, false);
  if (code != HTTP_CODE_OK) {
    http.end();
    snprintf(lastErr, sizeof lastErr, "geo %d", code);
    return false;
  }
  BoundedAllocator geoAlloc(8192);
  JsonDocument doc(&geoAlloc);
  HttpBodyStream body(http);
  DeserializationError parseErr = deserializeJson(doc, body);
  bool bodyComplete = body.complete() || body.drain();
  http.end();
  if (parseErr || !bodyComplete) {
    snprintf(lastErr, sizeof lastErr, "geo json");
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() == 0) {
    snprintf(lastErr, sizeof lastErr, "no match");
    g_lastPlace = "";
    return false;
  }
  g_lat = atof(arr[0]["lat"] | "0");
  g_lon = atof(arr[0]["lon"] | "0");
  // Keep a short human-readable confirmation (city/state/country or the full
  // display name) so the user can verify the search resolved correctly.
  const char* disp = arr[0]["display_name"] | "";
  g_lastPlace = disp;
  if (g_lastPlace.length() > 40) g_lastPlace = g_lastPlace.substring(0, 40);
  saveFloat("lat", g_lat);
  saveFloat("lon", g_lon);
  }  // end geocode scoped block (frees sec/http + TLS buffers before fetchWeather)
  fetchWeather();   // refresh weather immediately for the new location
  snprintf(lastErr, sizeof lastErr, "ok");
  return true;
}

// Govee Open API (Pool Temp): selectGoveeDevice(), fetchGoveeDevices(),
// fetchGoveeTemp() live in pool.ino.

// ---- drawing ----
// Header: date, credits, and a larger clock; the band is widened to fit it.
#define HEADER_H 36
// Shared time-of-day editor geometry (drawTimeAdj in settings.ino; defined
// here since alarms.ino concats first). Hour step wraps AM/PM - no meridiem control.
#define TADJ_HX 80      // hour [▼▲] pair x ([▲] is TADJ_BW+4 to its right)
#define TADJ_MX RX(252) // minute / value [▼▲] pair x (right-anchored)
#define TADJ_BW 30    // stepper button width (buttons are 24px tall)
// Header color: picked clock color (NVS "clkcol"), or red/maroon while a
// critical issue shows; non-critical warnings keep the normal color.
#define DEFAULT_CLOCK_COL TFT_BLUE
uint16_t g_clockCol = DEFAULT_CLOCK_COL;

// Perceived luminance (~0-255) of an RGB565 color.
int colorLum(uint16_t c) {
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return (r * 8 * 299 + g * 4 * 587 + b * 8 * 114) / 1000;
}

// Text color on top of a filled RGB565 color (the picked theme color on
// buttons/headers): black when the fill is light, white when dark.
uint16_t btnFg(uint16_t bg) {
  return colorLum(bg) > 140 ? TFT_BLACK : TFT_WHITE;
}

// Button color: theme nudged darker/lighter so buttons stand out from the
// header. Lightening blends toward white - scaling a saturated channel would clamp.
uint16_t btnCol() {
  int r = (g_clockCol >> 11) & 31, g = (g_clockCol >> 5) & 63, b = g_clockCol & 31;
  if (colorLum(g_clockCol) > 140) {
    r = r * 13 / 16; g = g * 13 / 16; b = b * 13 / 16;              // darker
  } else {
    r += (31 - r) / 4; g += (63 - g) / 4; b += (31 - b) / 4;        // toward white
  }
  return (r << 11) | (g << 5) | b;
}

// Themed button; white outline when the fill is near-black (all channels low -
// luminance alone would flag plainly-visible dark blues/reds).
void themeBtn(int x, int y, int w, int h, int r) {
  uint16_t c = btnCol();
  tft.fillRoundRect(x, y, w, h, r, c);
  int cr = (c >> 11) & 31, cg = (c >> 5) & 63, cb = c & 31;
  if (max(cr * 8, max(cg * 4, cb * 8)) < 64)
    tft.drawRoundRect(x, y, w, h, r, TFT_WHITE);
}

// Header Back/close button, top-right of every sub-screen. Ordinary button
// color — it navigates, it isn't destructive.
void backBtn(const char* label) {
  themeBtn(RX(265), 4, 50, 20, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(2);
  tft.drawCentreString(label, RX(290), 6, 2);
}

// Per-channel blend of RGB565 `a` toward `b` by num/den.
uint16_t mix565(uint16_t a, uint16_t b, int num, int den) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return ((ar + (br - ar) * num / den) << 11)
       | ((ag + (bg - ag) * num / den) << 5)
       |  (ab + (bb - ab) * num / den);
}

// Graph colors from the theme: data line = theme pushed 75% toward white/black
// (contrasts the plot fill); avg line = complement (orange if hue-less grey).
uint16_t graphBgCol() { return btnCol(); }
uint16_t graphLineCol() {
  return mix565(g_clockCol, colorLum(g_clockCol) > 140 ? TFT_BLACK : TFT_WHITE, 3, 4);
}
uint16_t graphAvgCol() {
  int h, s; colorHS(g_clockCol, h, s);
  uint16_t c = (s >= 60) ? hsv565(h + 180, s, 255) : TFT_ORANGE;
  return mix565(c, colorLum(g_clockCol) > 140 ? TFT_BLACK : TFT_WHITE, 3, 4);
}

// Hue (0-359) and saturation (0-255) of an RGB565 color. Hue is meaningless
// when s is ~0 (greys, black, white).
void colorHS(uint16_t c, int& h, int& s) {
  int r = ((c >> 11) & 31) * 255 / 31;
  int g = ((c >> 5) & 63) * 255 / 63;
  int b = (c & 31) * 255 / 31;
  int mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
  s = mx ? d * 255 / mx : 0;
  if (!d) { h = 0; return; }
  if (mx == r)      h = ((60 * (g - b) / d) + 360) % 360;
  else if (mx == g) h = ((60 * (b - r) / d) + 480) % 360;
  else              h = ((60 * (r - g) / d) + 600) % 360;
}

// Destructive buttons are red - yellow when the theme is near-red so they
// still contrast the header band.
uint16_t dangerCol() {
  int h, s; colorHS(g_clockCol, h, s);
  if (s >= 80 && min(h, 360 - h) <= 25) return TFT_YELLOW;
  return TFT_RED;
}

// Unselected toggles: dark grey, or white on a mid-grey theme so states can't be confused.
uint16_t disabledCol() {
  int h, s; colorHS(g_clockCol, h, s);
  int lum = colorLum(g_clockCol);
  if (s < 60 && lum > 70 && lum < 190) return TFT_WHITE;
  return TFT_DARKGREY;
}

// Credit tier color (header + Credits screen): pink <50, yellow <500 or
// unobserved, else grey.
uint16_t creditTierColor(int value, bool known) {
  if (!known)    return TFT_YELLOW;
  if (value < 50)  return TFT_PINK;
  if (value < 500) return TFT_YELLOW;
  return TFT_LIGHTGREY;
}

// One header credit bucket: label + tier-colored value ("?" if unobserved),
// FONT1 so the three stacked rows fit the 36px band.
void drawHeaderCredit(int x, int y, const char* label, int value, bool known,
                      uint16_t bg) {
  tft.setTextColor(btnFg(bg), bg);
  tft.setCursor(x, y);
  tft.print(label);
  tft.setTextColor(creditTierColor(value, known), bg);
  tft.setCursor(x + 26, y);
  if (known) tft.printf("%d", value);
  else tft.print("?");
}

void drawHeaderBand() {
  bool err = (dashboardCriticalLabel() != nullptr);
  uint16_t bg = err ? TFT_MAROON : (isDevBuild() ? TFT_DARKGREY : g_clockCol);
  tft.fillRect(0, 0, DISP_W, HEADER_H, bg);
  // date (left) + credits (right) in FONT2; text follows the band color so a
  // light picked color still gets readable black text.
  tft.setTextColor(btnFg(bg), bg);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setCursor(4, 11);
  tft.print(fmtDate());
  // Three OpenSky credit buckets (CRP radar / CRL routes / CFT tracks) stacked
  // right, inside the header's credits tap zone. Only while tracking is on.
  if (g_trackEnabled) {
    tft.setTextFont(1);
    tft.setTextSize(1);
    drawHeaderCredit(RX(202), 4,  "CRP:", g_creditsRemaining, g_creditsKnown,        bg);
    drawHeaderCredit(RX(202), 14, "CRL:", g_flightsCredits,   g_flightsCredits >= 0, bg);
    drawHeaderCredit(RX(202), 24, "CFT:", g_tracksCredits,    g_tracksCredits >= 0,  bg);
  }
  // Bigger clock: FONT2 doubled on the 2.8"; F6 -> FreeSansBold18 on the 4".
  // AM/PM marker in the bottom-right slot (12h only); alarm bell takes the top.
  String clk = fmtClock();
#ifdef CYD_E32R40T
  tft.setTextFont(6);
  tft.setTextSize(1);
  // Centre the clock+marker block horizontally and the font box vertically.
  const int clkX = CX - tft.textWidth(clk) / 2 - 8;
  const int clkY = (HEADER_H - tft.fontHeight()) / 2;
#else
  tft.setTextFont(2);
  tft.setTextSize(2);
  const int clkX = 96;
  const int clkY = 1;
#endif
  tft.setTextColor(btnFg(bg), bg);
  tft.setCursor(clkX, clkY);
  int clkEndX = clkX + tft.textWidth(clk);
  tft.print(clk);
  struct tm ct;
  if (getLocalTime(&ct, 0)) {
    int sx = clkEndX + 4;
    tft.setTextFont(1);
    tft.setTextSize(1);
    // The bell takes the top slot and the AM/PM marker the bottom slot -
    // the bell sits too low against the header's bottom edge otherwise.
    if (anyAlarmEnabled()) drawAlarmBell(sx, 5, bg);
    if (!g_clock24) {
      tft.setCursor(sx, 24);
      tft.print(ct.tm_hour < 12 ? "AM" : "PM");
    }
    tft.setTextFont(2);
  }
  tft.setTextSize(1);
}

// One row of the OpenSky Credits screen: a bucket label + its last-known
// remaining balance in the same tier colors as the header readouts.
void drawCreditsRow(int y, const char* label, int remaining, bool known) {
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, y);
  tft.print(label);
  tft.setTextColor(creditTierColor(remaining, known), TFT_BLACK);
  tft.setCursor(8, y + 20);
  if (known) tft.printf("%d", remaining);
  else tft.print("--");
}

// OpenSky Credits screen: the three daily credit buckets with last-known
// balances (tap the header credits to open; Back returns).
void drawCredits() {
  tft.fillScreen(TFT_BLACK);
  uint16_t bg = g_clockCol;
  tft.fillRect(0, 0, DISP_W, HEADER_H, bg);
  tft.setTextColor(btnFg(bg), bg);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setCursor(4, 11);
  tft.print("OpenSky Credits");
  backBtn("Back");

  int y = 48;
  drawCreditsRow(y,     "Radar Polling",   g_creditsRemaining, g_creditsKnown);
  drawCreditsRow(y+48,  "Route Lookup",    g_flightsCredits,   g_flightsCredits >= 0);
  drawCreditsRow(y+96,  "Flight Tracking", g_tracksCredits,    g_tracksCredits >= 0);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(1);
  tft.setCursor(8, y + 158);
  tft.print("Last-known remaining per bucket.");
  tft.setCursor(8, y + 170);
  tft.print("Grey ok, yellow < 500, pink < 50.");
}

// 1s incremental update: redraw only the header clock + countdown bar to avoid
// flicker; full redraws still happen when the underlying data changes.
void updateDashboard() {
  // Incremental 1-second update for the dashboard and the flight-detail page;
  // both are the only screens with a header band.
  if (g_screen != SCR_DASH && g_screen != SCR_FLIGHTDETAIL) return;
  // The header background color depends on WiFi; if it flipped, a full redraw is
  // cleaner than patching (the border color changes too).
  static bool lastConnected = false;
  if (lastConnected != connected) {
    lastConnected = connected;
    dirty = true;
    return;
  }
  // Redraw the header only when the clock/credits actually change - every-second
  // full redraws flickered.
  static String lastClock;
  static int lastC = -1, lastF = -1, lastT = -1;
  String curClock = fmtClock();
  int c = g_creditsKnown ? g_creditsRemaining : -1;
  if (curClock != lastClock || c != lastC || g_flightsCredits != lastF || g_tracksCredits != lastT) {
    lastClock = curClock;
    lastC = c; lastF = g_flightsCredits; lastT = g_tracksCredits;
    drawHeaderBand();
    // The header redraw covers Back - restore it only when a radar/flight view
    // is up; `overhead` can pass early and would draw Back onto the idle dash.
    bool onDetail = (g_screen == SCR_FLIGHTDETAIL);
    if (g_radarShown || onDetail) drawFlightBackButton();
  }
  if (g_screen == SCR_DASH && g_showTimer && g_trackEnabled) drawCountdownBar(); // updates the bar in place
  // The snooze countdown owns the status line while pending; the transient
  // auto-update status stays hidden for those few minutes.
  if (snoozePending()) drawSnoozeStatus(); else drawAutoUpdateStatus();
  // Dead-reckon the radar blips forward and redraw them in place.
  stepRadar();
}

// Auto-update status line, bottom-left; each status persists a few seconds
// (g_autoUpdStatusUntil) so the outcome is readable.
void drawAutoUpdateStatus() {
  if (g_autoUpdStatus == 0) return;
  // Every status expires so none can get stuck on screen.
  if ((long)(millis() - g_autoUpdStatusUntil) > 0) { g_autoUpdStatus = 0; return; }
  const char* msg = "";
  uint16_t col = TFT_LIGHTGREY;
  switch (g_autoUpdStatus) {
    case 1: msg = "Update: Scanning...";  col = TFT_YELLOW; break;
    case 2: msg = "Update: No Updates"; col = TFT_LIGHTGREY; break;
    case 3: msg = "Update: Updating..."; col = TFT_GREEN; break;
    case 4: msg = "Update: Check Failed"; col = TFT_RED; break;
    default: return;
  }
  // Reuse the bottom-left status line (lastErr). Stop above the status
  // border's bottom frame rows (y=238/239) so a live border isn't erased.
  const int x = 8, y = 224, w = 180, h = (FONT_AUX == 2) ? 14 : 12;
  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.setTextFont(FONT_AUX);
  tft.setTextColor(col, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(msg);
}

// Pending-snooze countdown over the bottom-left status line; re-renders only
// on a minute change (other draws own the line, so no erase is needed).
void drawSnoozeStatus() {
  char msg[40];
  if (!snoozeStatusText(msg, sizeof msg)) return;
  // Cache keyed to the pending refire: a different snooze forces a redraw
  // even if the minute text matches the previous one's.
  static time_t drawnFor = 0;
  static char last[40] = "";
  time_t ref = snoozeRefireAt();
  if (ref == drawnFor && strcmp(msg, last) == 0) return;
  drawnFor = ref;
  strncpy(last, msg, sizeof last);
  const int x = 8, y = 224, w = 180, h = (FONT_AUX == 2) ? 14 : 12;
  tft.fillRect(x, y, w, h, TFT_BLACK);
  tft.setTextFont(FONT_AUX);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(msg);
}

// True when a flight shows live: tracked plane in radius, or watched callsign
// on-screen. Uses live dx/dy so dead-reckoning off drops it like the next poll.
bool flightOverhead() {
  return planeCount > 0 &&
         (planes[0].distMi <= g_radiusMi ||
          (isWatchedCallsign(planes[0].callsign) && blipOnScreen(planes[0])));
}

void drawDashboard() {
  tft.fillScreen(TFT_BLACK);

  drawHeaderBand();

  // right-side countdown bar (only when tracking is enabled and the timer is shown)
  if (g_showTimer && g_trackEnabled) drawCountdownBar();

  // settings cog under the countdown bar
  drawCog();

  // main content: flight overhead or idle weather. Honor g_suppressFlight so a
  // dismissed flight stays dismissed until a new overhead flight is found.
  bool overhead = g_trackEnabled && !g_suppressFlight && flightOverhead();
  if (overhead) {
    // Route details drawn inline once auto-fetched; the recall tap only
    // redisplays the snapshot.
    drawFlightInfo(planes[0]);
    drawRadar();
  } else {
    drawIdle();
  }
  // Record whether a radar is actually on screen so the per-second
  // dead-reckoning update only moves blips here (never on the idle view).
  g_radarShown = overhead;

  // Red border while anonymous or when OpenSky credentials are rejected
  drawAuthBorder();
}

// Critical-issue label: No WiFi > bad creds > no credits > data unavailable >
// pool unavailable. Anonymous OpenSky is a warning, not critical (below).
const char* dashboardCriticalLabel() {
  if (WiFi.status() != WL_CONNECTED) return "No WIFI";
  if (g_trackEnabled) {
    if (g_authState == AUTH_BAD) return "Invalid OpenSky Creds";
    if (g_creditsKnown && g_creditsExhausted) return "No Flight Credits";
    if (g_radarDataFailed) return "OpenSky Data Unavailable";
  }
  if (g_wxNextEpoch && (unsigned long)time(nullptr) < g_wxNextEpoch) return "Weather Rate Limited";
  if (g_weatherDataFailed) return "Weather Data Unavailable";
#if POOL_FEATURE
  if (g_poolEnabled && goveeRateLimited()) return "Govee Rate Limited";
  if (g_poolEnabled && !g_poolValid) return g_goveeAuthBad ? "Invalid Govee Creds" : "Pool Temp Data Unavailable";
#endif
  return nullptr;
}

// Non-critical warning (yellow border, normal clock): anonymous OpenSky - only
// after the first fetch determined auth state, else it'd show at startup.
const char* dashboardWarningLabel() {
  if (g_trackEnabled && g_authChecked && g_authState == AUTH_ANON) return "ANONYMOUS";
  return nullptr;
}

// Status border around the screen showing the current issue/warning. Critical
// issues are red; the anonymous warning is yellow.
void drawAuthBorder() {
  const char* crit = dashboardCriticalLabel();
  const char* warn = dashboardWarningLabel();
  if (crit) drawStatusBorder(crit, TFT_RED);
  else if (warn) drawStatusBorder(warn, TFT_YELLOW);
}

void drawStatusBorder(const char* label, uint16_t col) {
  // 2px frame around the screen
  tft.drawRect(0, 0, DISP_W, 240, col);
  tft.drawRect(1, 1, DISP_W - 2, 238, col);

  // tag on the bottom-right corner of the frame, sized to the label. The 4"
  // uses the smoother F2 which needs a taller tag.
  tft.setTextFont(FONT_AUX);
  int tagW = tft.textWidth(label) + 8;
  int tagH = (FONT_AUX == 2) ? 16 : 8;
  int tagY = 240 - tagH;
  int tagX = DISP_W - 4 - tagW;
  tft.fillRect(tagX, tagY, tagW, tagH, col);
  // Black text is legible on the yellow anonymous-warning border; white on red.
  tft.setTextColor((col == TFT_YELLOW) ? TFT_BLACK : TFT_WHITE, col);
  tft.setCursor(tagX + 4, tagY + 1);
  tft.print(label);
}

void drawCountdownBar() {
  int x = RX(303), w = 14, topY = HEADER_H + 2, botY = 200;
  int h = botY - topY;
  unsigned long now = millis();
  // Bar references last-shown data (g_lastData) so it empties exactly when the
  // next fetch's flight appears.
  unsigned long ref = g_lastData ? g_lastData : lastPoll;
  unsigned long elapsed = (now >= ref) ? (now - ref) : 0;
  unsigned long period = (unsigned long)g_pollSec * 1000UL;
  float frac = 1.0f;
  if (period > 0) frac = 1.0f - (float)elapsed / (float)period;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  int fill = (int)(h * frac);
  tft.fillRect(x, topY, w, h, TFT_DARKGREY);              // track
  uint16_t col = TFT_GREEN;
  if (frac < 0.33f) col = TFT_RED;
  else if (frac < 0.66f) col = TFT_YELLOW;
  tft.fillRect(x, botY - fill, w, fill, col);             // fill
  tft.drawRect(x, topY, w, h, TFT_WHITE);                 // border
}

void drawCog() {
  int cx = RX(310), cy = 215, r = 9;
  tft.fillCircle(cx, cy, r, TFT_DARKGREY);
  for (int i = 0; i < 6; i++) {
    float a = i * PI / 3.0f;
    int x1 = cx + (int)(r * 0.6f * cos(a));
    int y1 = cy + (int)(r * 0.6f * sin(a));
    int x2 = cx + (int)(r * 1.3f * cos(a));
    int y2 = cy + (int)(r * 1.3f * sin(a));
    tft.drawLine(x1, y1, x2, y2, TFT_LIGHTGREY);
  }
  tft.fillCircle(cx, cy, 4, TFT_BLACK);
}

// ---- LED blink notifications ----
// RGB LED pins (active-low). Red is GPIO 22 on this board, not GPIO 4 (panel
// reset) - other CYD revisions differ, so this is hard-coded for this unit.
#define CYD_LED_RED   22
#define CYD_LED_GREEN 16
#define CYD_LED_BLUE  17

// Pick the best route field for the LED. OpenSky is live data, so it wins if
// it has a non-empty value; otherwise fall back to the ADSB.lol planned route.
static String ledRouteField(const String& openVal, const String& adsbVal) {
  return openVal.length() ? openVal : adsbVal;
}

// LED color for the overhead flight: OpenSky preferred per field, adsb.lol
// fills gaps; yellow same-home, green arrival, red departure, else blue.
static BlinkColor computeBlinkColor() {
  if (g_homeAirport.length() == 0) return BLINK_BLUE;
  String o = ledRouteField(g_routeOrigin, g_adsbRouteOrigin);
  String d = ledRouteField(g_routeDest,   g_adsbRouteDest);
  if (o.length() && d.length() && o == d && o == g_homeAirport) return BLINK_YELLOW;
  if (d == g_homeAirport) return BLINK_GREEN;
  if (o == g_homeAirport) return BLINK_RED;
  return BLINK_BLUE;
}

// True when the configured watch callsign is "*", the match-all wildcard.
static bool watchIsWildcard() {
  String b = g_watchCallsign; b.trim(); b.toUpperCase();
  return b == "*";
}

// Watch callsign = case-insensitive substring match ("DAL" matches DAL1234);
// a lone "*" matches every flight.
static bool isWatchedCallsign(const char* cs) {
  if (g_watchCallsign.length() == 0) return false;
  if (watchIsWildcard()) return true;
  String a = cs; a.trim(); a.toUpperCase();
  String b = g_watchCallsign; b.trim(); b.toUpperCase();
  return a.indexOf(b) >= 0;
}

// Blink `pin` (active-low) `times` times, `ms` per phase.
void blinkLedPin(int pin, int times, int ms) {
  pinMode(pin, OUTPUT);
  for (int i = 0; i < times; i++) {
    digitalWrite(pin, LOW);   // ON (active-low)
    delay(ms);
    digitalWrite(pin, HIGH);  // OFF
    delay(ms);
  }
}

// Blink the LED for an overhead flight: white watch / yellow home / green
// arrival / red departure / blue. Route colors stay lit on the details view.
void blinkLed(BlinkColor color) {
  pinMode(CYD_LED_RED, OUTPUT);   digitalWrite(CYD_LED_RED, HIGH);
  pinMode(CYD_LED_GREEN, OUTPUT); digitalWrite(CYD_LED_GREEN, HIGH);
  pinMode(CYD_LED_BLUE, OUTPUT);  digitalWrite(CYD_LED_BLUE, HIGH);
  if (color == BLINK_RED) {
    blinkLedPin(CYD_LED_RED, 5, 240);
    digitalWrite(CYD_LED_RED, LOW);   // stay lit while the flight is displayed
  } else if (color == BLINK_GREEN) {
    blinkLedPin(CYD_LED_GREEN, 5, 240);
    digitalWrite(CYD_LED_GREEN, LOW); // stay lit while the flight is displayed
  } else if (color == BLINK_YELLOW) {
    for (int i = 0; i < 5; i++) {
      digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, LOW); digitalWrite(CYD_LED_BLUE, HIGH);
      delay(240);
      digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
      delay(240);
    }
    digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, LOW); digitalWrite(CYD_LED_BLUE, HIGH);  // stay lit
  } else if (color == BLINK_WHITE) {
    for (int i = 0; i < 5; i++) {
      digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, LOW); digitalWrite(CYD_LED_BLUE, LOW);
      delay(240);
      digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
      delay(240);
    }
    digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);  // white off after one sequence
  } else {
    blinkLedPin(CYD_LED_BLUE, 5, 240);
    digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
  }
}

// Watch-callsign alert lives in alarms.ino (updateWatchNotify) - it reuses the alarm presets.

// Route colors stay solid while the live details show; off when the flight
// leaves or the view is dismissed (including recalled details).
void updateRouteLed(bool active) {
  pinMode(CYD_LED_RED, OUTPUT);
  pinMode(CYD_LED_GREEN, OUTPUT);
  pinMode(CYD_LED_BLUE, OUTPUT);
  if (!active || g_routeHoldColor == BLINK_NONE) {
    digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
    if (!active) g_routeHoldColor = BLINK_NONE;
    return;
  }
  switch (g_routeHoldColor) {
    case BLINK_RED:
      digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
      break;
    case BLINK_GREEN:
      digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, LOW); digitalWrite(CYD_LED_BLUE, HIGH);
      break;
    case BLINK_YELLOW:
      digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, LOW); digitalWrite(CYD_LED_BLUE, HIGH);
      break;
    default:
      digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
      break;
  }
}

// No flight notification: the LED mirrors the status border (red critical /
// yellow anonymous warning).
void updateStatusLed() {
  pinMode(CYD_LED_RED, OUTPUT);
  pinMode(CYD_LED_GREEN, OUTPUT);
  pinMode(CYD_LED_BLUE, OUTPUT);
  const char* crit = dashboardCriticalLabel();
  const char* warn = dashboardWarningLabel();
  if (!crit && !warn) {
    digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
    return;
  }
  // Solid, matching the steady red/yellow status border on screen.
  if (crit) {
    digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
  } else {
    digitalWrite(CYD_LED_RED, LOW); digitalWrite(CYD_LED_GREEN, LOW); digitalWrite(CYD_LED_BLUE, HIGH);
  }
}

// ---- Flight details card ----
// Back button (upper-right) that dismisses the overhead flight back to idle.
void drawFlightBackButton() {
  backBtn("Back");
}

// Draw an RGB565 bitmap upscaled nearest-neighbor, skipping `transp` pixels;
// fillRect per pixel needs no scratch buffer (logos are pre-sized, scale=1).
void drawScaledBitmap(int x, int y, const uint16_t* data, int w, int h,
                      int scale, uint16_t transp) {
  for (int sy = 0; sy < h; sy++) {
    for (int sx = 0; sx < w; sx++) {
      uint16_t c = data[sy * w + sx];
      if (c == transp) continue;
      tft.fillRect(x + sx * scale, y + sy * scale, scale, scale, c);
    }
  }
}

void drawFlightInfo(Plane& p) {
  drawFlightBackButton();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(4);
  tft.setCursor(8, 40);
  tft.print(p.callsign);
  tft.setTextSize(1);
  tft.setTextFont(2);

  // Airline name below the callsign (from the ICAO code in the callsign).
  String alName; uint16_t alColor;
  bool hasAirline = airlineInfo(p.callsign, alName, alColor);
  (void)alColor;   // color was only used by the removed badge fallback
  if (hasAirline) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(8, 74);
    tft.print(alName.substring(0, 26));
  }

  tft.setCursor(8, 94);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  if (g_units == UNITS_METRIC) {
    tft.printf("%dm  %dkts  %.1fkm", (int)round(p.altFt * 0.3048f), p.spdKt, p.distMi * 1.60934f);
  } else if (g_units == UNITS_AVIATION) {
    tft.printf("%dft  %dkts  %.1fnm", p.altFt, p.spdKt, p.distMi * 0.868976f);
  } else {
    tft.printf("%dft  %dmph  %.1fmi", p.altFt, (int)round(p.spdKt * 1.15078f), p.distMi);
  }

  // Route: adsb.lol planned, OpenSky shown only when different; getRouteDisplay
  // guards the shared Strings against a fetch in flight.
  String origin, originCity, dest, destCity;
  bool hasData;
  getRouteDisplay(origin, originCity, dest, destCity, hasData);
  bool origKnown = (origin.length() > 0);
  bool destKnown = (dest.length() > 0);
  int y = 114;
  tft.setTextFont(2);
  if (origKnown) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(8, y); tft.print("Origin");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, y + 16); tft.print(origin);        // airport code
    tft.setCursor(8, y + 32); tft.print(originCity);    // city
    y += 54;
  }
  if (destKnown) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(8, y); tft.print("Destination");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, y + 16); tft.print(dest);           // airport code
    tft.setCursor(8, y + 32); tft.print(destCity);       // city
  } else if (hasData && !destKnown && origKnown) {
    // OpenSky leaves arrival unknown until landing; show "--" so the block
    // height matches a known endpoint and isn't read as origin==dest.
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(8, y); tft.print("Destination");
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, y + 16); tft.print("--");   // airport code placeholder
    tft.setCursor(8, y + 32); tft.print("--");   // city placeholder
  }
  // If neither side had route data, say so so it is clear the feature is there.
  if (hasData && !origKnown && !destKnown) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, y);
    tft.print("No route data");
  }

  // Airline logo top-right at native stored size (no upscale); nothing if absent.
  const RuntimeLogo* logo = findAirlineLogo(p.callsign);
  if (logo) {
    drawScaledBitmap(RX(226), 40, logo->data, logo->w, logo->h, 1, 0xF81F);
    logoRelease(logo);
  }
}

// ---- Radar (frame, blips, ground track, projection) ----
// Dashboard + detail radars share center/radius so blips can redraw in place.
static const int kRadarCX = RX(235), kRadarCY = 155, kRadarR = 48;

// Draw the static radar rings, crosshairs and range label.
void drawRadarFrame(int cx, int cy, int r) {
  tft.drawCircle(cx, cy, r, TFT_DARKGREY);
  tft.drawCircle(cx, cy, r / 2, TFT_DARKGREY);
  tft.drawLine(cx - r, cy, cx + r, cy, TFT_DARKGREY);
  tft.drawLine(cx, cy - r, cx, cy + r, TFT_DARKGREY);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(cx - 12, cy + r + 6);
  tft.printf("%d%s", (int)round(g_radiusMi * distConv()), distUnit());
}

// Draw a dashed (dotted) line segment, used for the ground track so it stays
// visually distinct from the solid radar rings.
void drawDottedLine(int x0, int y0, int x1, int y1, uint16_t col, int dash, int gap) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.5f) return;
  float ux = dx / len, uy = dy / len;
  float t = 0.0f;
  while (t < len) {
    float a = t, b = t + dash;
    if (b > len) b = len;
    tft.drawLine((int)(x0 + ux * a), (int)(y0 + uy * a),
                 (int)(x0 + ux * b), (int)(y0 + uy * b), col);
    t += dash + gap;
  }
}

// Which UI zones a dotted line is clipped against.
enum ClipZones {
  CLIP_DYN,       // dynamic: header/menu bar+clock, countdown bar
  CLIP_DYN_COG,   // + settings cog (flight track)
  CLIP_ALL        // + flight-info text, airline logo (projection)
};
// Forward decl (defined after kBlipR, which the keep-out rects reference).
void drawDottedLineSafe(int x0, int y0, int x1, int y1, uint16_t col, int dash, int gap, ClipZones zones);

// Draw the tracked flight's past ground-track polyline on a radar. Points are
// stored as (dx,dy) miles from the observer; off-screen dashes are clipped.
void drawTrackPolyline(int cx, int cy, float scale) {
  TrackPoint pts[MAX_TRACK_PTS];
  int count;
  portENTER_CRITICAL(&g_trackMux);
  count = g_trackCount;
  if (count > 0) memcpy(pts, g_trackPts, count * sizeof(TrackPoint));
  portEXIT_CRITICAL(&g_trackMux);
  if (count < 2) return;
  int prevX = cx + (int)(pts[0].dxMi * scale);
  int prevY = cy - (int)(pts[0].dyMi * scale);
  for (int i = 1; i < count; i++) {
    int x = cx + (int)(pts[i].dxMi * scale);
    int y = cy - (int)(pts[i].dyMi * scale);
    drawDottedLineSafe(prevX, prevY, x, y, TFT_CYAN, 3, 3, CLIP_DYN_COG);
    prevX = x; prevY = y;
  }
}

// Line to the screen edge from the track end (or along live heading); static,
// redrawn each frame so the blip doesn't leave a hole.
void drawTrackProjection(int cx, int cy, float scale, float planeDxMi, float planeDyMi, float planeHdgDeg) {
  TrackPoint lastPoint;
  int count;
  float bearing;
  portENTER_CRITICAL(&g_trackMux);
  count = g_trackCount;
  bearing = g_trackBearingDeg;
  if (count > 0) lastPoint = g_trackPts[count - 1];
  portEXIT_CRITICAL(&g_trackMux);
  float sxMi, syMi, dirDx, dirDy;
  if (count >= 1 && bearing >= 0.0f) {
    // Track available: start at its end and continue along its end bearing.
    sxMi = lastPoint.dxMi;
    syMi = lastPoint.dyMi;
    float rad = bearing * PI / 180.0f;
    dirDx = sinf(rad); dirDy = cosf(rad);   // east / north
  } else if (planeHdgDeg >= 0.0f) {
    // No track: project from the plane's current position along its heading.
    sxMi = planeDxMi; syMi = planeDyMi;
    float rad = planeHdgDeg * PI / 180.0f;
    dirDx = sinf(rad); dirDy = cosf(rad);
  } else {
    return;   // no track and no heading to project along
  }
  int sx = cx + (int)(sxMi * scale);
  int sy = cy - (int)(syMi * scale);
  float ux = dirDx * scale;    // east -> +x
  float uy = -dirDy * scale;   // north -> -y
  float um = sqrtf(ux * ux + uy * uy);
  if (um == 0.0f) return;
  ux /= um; uy /= um;
  // Extend the ray from the start point until it leaves the screen.
  float t = 1e9f;
  if (ux > 0.0001f) t = fminf(t, (DISP_W - 1 - sx) / ux);
  if (ux < -0.0001f) t = fminf(t, (0 - sx) / ux);
  if (uy > 0.0001f) t = fminf(t, (239 - sy) / uy);
  if (uy < -0.0001f) t = fminf(t, (0 - sy) / uy);
  int ex = sx + (int)(ux * t);
  int ey = sy + (int)(uy * t);
  // Same grey as the trail but sparser dots; clipped to safe (non-UI) areas.
  drawDottedLineSafe(sx, sy, ex, ey, TFT_LIGHTGREY, 1, 6, CLIP_ALL);
}

// Blip bounding radius for erase/overlap checks - a fixed circle covers the
// icon at any rotation; sized for the larger tracked-flight icon.
const int kBlipR = 11;
// Scale multipliers for the radar plane icons. The tracked flight is drawn
// bigger so it stands out from the other (smaller) planes.
const float kOtherScale   = 1.0f;   // non-tracked flights
const float kTrackedScale = 1.6f;   // tracked (overhead) flight

// ---- line clipping for the track/projection ----
struct Seg { int x0, y0, x1, y1; };

// Liang-Barsky: the parameter interval [t0,t1] of the segment that lies INSIDE
// rect [rx0..rx1]x[ry0..ry1]. Returns false if the segment never enters.
static bool segInsideRect(int x0, int y0, int x1, int y1,
                          int rx0, int ry0, int rx1, int ry1,
                          float& t0, float& t1) {
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  t0 = 0.0f; t1 = 1.0f;
  float p[4] = { -dx, dx, -dy, dy };
  float q[4] = { (float)x0 - rx0, (float)rx1 - x0, (float)y0 - ry0, (float)ry1 - y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0.0f) {
      if (q[i] < 0) return false;
    } else {
      float r = q[i] / p[i];
      if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else          { if (r < t0) return false; if (r < t1) t1 = r; }
    }
  }
  return true;
}

// Dotted line that skips the same UI keep-out zones blipBlocked() protects.
void drawDottedLineSafe(int x0, int y0, int x1, int y1, uint16_t col, int dash, int gap, ClipZones zones) {
  const int R = kBlipR;
  Seg rects[5]; int nRects = 0;
  // Always clip the header/menu and countdown bars - they redraw often, so a
  // line over them leaves artifacts.
  rects[nRects++] = { -50, -50, DISP_W + 50, 33 + R };              // header/menu bar + clock
  if (g_screen == SCR_DASH && g_showTimer && g_trackEnabled)
    rects[nRects++] = { RX(302) - R, 34 - R, RX(320) + R, 200 + R };   // countdown bar
  // The settings cog is clipped for the track and projection.
  if (g_screen == SCR_DASH && zones != CLIP_DYN)
    rects[nRects++] = { RX(296) - R, 200 - R, RX(320) + R, 213 + R };  // settings cog
  // Static zones (flight-info text, airline logo) are only clipped for the
  // projection; the track is allowed to pass over them.
  if (zones == CLIP_ALL) {
    rects[nRects++] = { -R, 34 - R, 171 + R, 208 + R };     // flight-info text
    rects[nRects++] = { RX(222) - R, 34 - R, RX(320) + R, 93 + R }; // airline logo
  }
  Seg cur[32]; int m = 1;                       // current segments (safe portions)
  cur[0] = { x0, y0, x1, y1 };
  for (int r = 0; r < nRects && m > 0; r++) {
    Seg next[32]; int k = 0;
    for (int i = 0; i < m; i++) {
      float t0, t1;
      if (!segInsideRect(cur[i].x0, cur[i].y0, cur[i].x1, cur[i].y1,
                         rects[r].x0, rects[r].y0, rects[r].x1, rects[r].y1, t0, t1)) {
        if (k < 32) next[k++] = cur[i];         // fully outside this rect: keep
      } else {
        if (t0 > 0.001f && k < 32) {            // keep [0,t0]
          next[k] = cur[i];
          next[k].x1 = (int)(cur[i].x0 + (cur[i].x1 - cur[i].x0) * t0);
          next[k].y1 = (int)(cur[i].y0 + (cur[i].y1 - cur[i].y0) * t0);
          k++;
        }
        if (t1 < 0.999f && k < 32) {            // keep [t1,1]
          next[k] = cur[i];
          next[k].x0 = (int)(cur[i].x0 + (cur[i].x1 - cur[i].x0) * t1);
          next[k].y0 = (int)(cur[i].y0 + (cur[i].y1 - cur[i].y0) * t1);
          k++;
        }
      }
    }
    m = k;
    for (int i = 0; i < m; i++) cur[i] = next[i];
  }
  for (int i = 0; i < m; i++) drawDottedLine(cur[i].x0, cur[i].y0, cur[i].x1, cur[i].y1, col, dash, gap);
}

// Would a blip at (px,py) overlap an on-screen object? Such blips are skipped
// rather than erased-through later; boxes are padded by kBlipR. skipLogo lets
// blipOnScreen() treat the airline-logo zone as showable - a watched flight in
// that corner still counts even though the blip itself can't draw there.
bool blipBlocked(int px, int py, bool skipLogo) {
  const int R = kBlipR;
  if (py <= 33 + R) return true;                                     // header band
  if (inRect(px, py, -R, 34 - R, 171 + R, 208 + R)) return true;     // flight-info text
  if (!skipLogo &&
      inRect(px, py, RX(222) - R, 34 - R, RX(320) + R, 93 + R)) return true; // airline logo
  if (g_screen == SCR_DASH) {
    // The countdown bar only occupies the right strip while the timer is shown;
    // when it's toggled off, that space is free for blips to draw in.
    if (g_showTimer && g_trackEnabled &&
        inRect(px, py, RX(302) - R, 34 - R, RX(320) + R, 200 + R)) return true;  // countdown bar
    // The settings cog is always drawn on the dashboard, so it stays protected.
    if (inRect(px, py, RX(296) - R, 200 - R, RX(320) + R, 213 + R)) return true; // settings cog
  }
  if (py >= 213) return true;                                        // bottom status/border strip
  return false;
}

// A single point offset from a blip's center, in the (forward, right) frame
// of its heading, converted to screen pixels.
void planePoint(float fx, float fy, float rx, float ry, float f, float r,
                int cx, int cy, int16_t& outX, int16_t& outY) {
  outX = cx + (int16_t)round(fx * f + rx * r);
  outY = cy + (int16_t)round(fy * f + ry * r);
}

// Plane silhouette along hdgDeg with a black outline (legible overlap);
// unknown heading (-1) falls back to a dot.
void drawPlaneIcon(int cx, int cy, float hdgDeg, uint16_t color, float scale) {
  if (hdgDeg < 0.0f) {
    int r = max(1, (int)round(3.0f * scale));
    tft.fillCircle(cx, cy, r, color);
    tft.drawCircle(cx, cy, r, TFT_BLACK);
    return;
  }
  float rad = hdgDeg * PI / 180.0f;
  // Forward (nose) and right-wing unit vectors in screen pixels; matches the
  // dead-reckoning convention (0=north/up, clockwise).
  float fx = sin(rad), fy = -cos(rad);
  float rx = cos(rad), ry = sin(rad);
  float s = scale;

  // Tail shares the wings' back edge (f = -1) so the shapes read as one silhouette.
  int16_t noseX, noseY, wingLX, wingLY, wingRX, wingRY;
  int16_t tailX, tailY, tailBaseLX, tailBaseLY, tailBaseRX, tailBaseRY;
  planePoint(fx, fy, rx, ry,  5.0f * s,  0.0f,      cx, cy, noseX, noseY);
  planePoint(fx, fy, rx, ry, -1.4f * s, -4.5f * s,  cx, cy, wingLX, wingLY);
  planePoint(fx, fy, rx, ry, -1.4f * s,  4.5f * s,  cx, cy, wingRX, wingRY);
  planePoint(fx, fy, rx, ry, -5.5f * s,  0.0f,      cx, cy, tailX, tailY);
  planePoint(fx, fy, rx, ry, -1.4f * s, -1.7f * s,  cx, cy, tailBaseLX, tailBaseLY);
  planePoint(fx, fy, rx, ry, -1.4f * s,  1.7f * s,  cx, cy, tailBaseRX, tailBaseRY);

  tft.fillTriangle(noseX, noseY, wingLX, wingLY, wingRX, wingRY, color);
  tft.fillTriangle(tailX, tailY, tailBaseLX, tailBaseLY, tailBaseRX, tailBaseRY, color);
  tft.drawTriangle(noseX, noseY, wingLX, wingLY, wingRX, wingRY, TFT_BLACK);
  tft.drawTriangle(tailX, tailY, tailBaseLX, tailBaseLY, tailBaseRX, tailBaseRY, TFT_BLACK);
}

// Draw one blip, clipped, recording its pixel (outPx/outPy) for a later erase.
// false (draws nothing) if off-screen or overlapping an object.
bool plotRadarBlip(int cx, int cy, float scale, float dxMi, float dyMi, float distMi,
                   int& outPx, int& outPy, uint16_t color, float hdgDeg, float glyphScale) {
  int px = cx + (int)(dxMi * scale);
  int py = cy - (int)(dyMi * scale);
  if (px < 0 || px > DISP_W - 1 || py < 0 || py > 239) return false;
  if (blipBlocked(px, py, false)) return false;
  drawPlaneIcon(px, py, hdgDeg, color, glyphScale);
  outPx = px; outPy = py;
  return true;
}

// Erase a previously drawn blip so its old pixel doesn't leave a trail. Only
// safe because blips are never drawn over on-screen objects.
void eraseRadarBlip(int px, int py) { tft.fillCircle(px, py, kBlipR, TFT_BLACK); }

// True when the blip would actually draw - fetchFlights() only promotes a
// watched callsign that lands clear of the keep-out zones (text column,
// header, timer/cog, status strip). The logo zone still counts: the flight is
// shown even while its blip sits behind the logo.
bool blipOnScreen(const Plane& p) {
  float scale = (float)kRadarR / g_radiusMi;
  int px = kRadarCX + (int)(p.dxMi * scale);
  int py = kRadarCY - (int)(p.dyMi * scale);
  return px >= 0 && px <= DISP_W - 1 && py >= 0 && py <= 239 &&
         !blipBlocked(px, py, true);
}

// The overhead flight (planes[0], whose details are on the left) is cyan so
// it's easy to pick out; the rest stay red (in range) / green (outside).
uint16_t blipColor(const Plane& p, int index) {
  return (index == 0) ? TFT_CYAN
         : ((p.distMi <= g_radiusMi) ? TFT_RED : TFT_GREEN);
}

// Tracked flight: larger cyan icon on top; not drawn once dead-reckoned off-screen.
void drawTrackedBlip(int cx, int cy, float scale, Plane& p) {
  int px = cx + (int)(p.dxMi * scale);
  int py = cy - (int)(p.dyMi * scale);
  if (px >= 0 && px <= DISP_W - 1 && py >= 0 && py <= 239) {
    p.blipOn = plotRadarBlip(cx, cy, scale, p.dxMi, p.dyMi, p.distMi,
                             p.lastPx, p.lastPy, TFT_CYAN, p.hdgDeg, kTrackedScale);
  } else {
    p.blipOn = false;
  }
}

// Full radar draw: frame + all blips, recording each pixel (screen is already
// black on a full redraw).
void drawRadar() {
  int cx = kRadarCX, cy = kRadarCY, r = kRadarR;
  drawRadarFrame(cx, cy, r);
  float scale = r / g_radiusMi;
  drawTrackPolyline(cx, cy, scale);
  if (planeCount > 0)
    drawTrackProjection(cx, cy, scale, planes[0].dxMi, planes[0].dyMi, planes[0].hdgDeg);
  // Draw other flights first, then the tracked flight last so its cyan dot is
  // always painted on top and can't be hidden by a neighbor drawn after it.
  for (int i = 1; i < planeCount; i++) {
    Plane &p = planes[i];
    p.blipOn = plotRadarBlip(cx, cy, scale, p.dxMi, p.dyMi, p.distMi,
                             p.lastPx, p.lastPy, blipColor(p, i), p.hdgDeg, kOtherScale);
  }
  if (planeCount > 0) drawTrackedBlip(cx, cy, scale, planes[0]);
  tft.fillCircle(cx, cy, 3, TFT_WHITE); // you
}

// Per-second in-place update: erase each blip's pixel, restore the frame, redraw
// at dead-reckoned positions (blips only ever cover black or the frame).
void drawRadarInPlace() {
  int cx = kRadarCX, cy = kRadarCY, r = kRadarR;
  float scale = r / g_radiusMi;
  for (int i = 0; i < planeCount; i++) {
    Plane &p = planes[i];
    if (p.blipOn) eraseRadarBlip(p.lastPx, p.lastPy);
  }
  drawRadarFrame(cx, cy, r);
  // Redraw the ground track after erasing blips so a moved blip doesn't leave a
  // black hole through the track, then draw blips on top.
  drawTrackPolyline(cx, cy, scale);
  if (planeCount > 0)
    drawTrackProjection(cx, cy, scale, planes[0].dxMi, planes[0].dyMi, planes[0].hdgDeg);
  // Draw other flights first, then the tracked flight last so its cyan dot is
  // always on top (see drawRadar).
  for (int i = 1; i < planeCount; i++) {
    Plane &p = planes[i];
    p.blipOn = plotRadarBlip(cx, cy, scale, p.dxMi, p.dyMi, p.distMi,
                             p.lastPx, p.lastPy, blipColor(p, i), p.hdgDeg, kOtherScale);
  }
  if (planeCount > 0) drawTrackedBlip(cx, cy, scale, planes[0]);
  tft.fillCircle(cx, cy, 3, TFT_WHITE); // you
}

// Full radar draw for the flight-detail page (single plane).
void drawFlightDetailRadar() {
  int cx = kRadarCX, cy = kRadarCY, r = kRadarR;
  drawRadarFrame(cx, cy, r);
  drawTrackPolyline(cx, cy, r / g_radiusMi);
  drawTrackProjection(cx, cy, r / g_radiusMi, g_lastFlight.dxMi, g_lastFlight.dyMi, g_lastFlight.hdgDeg);
  // Cyan so the flight whose details are shown is easy to pick out.
  g_flightBlipOn = plotRadarBlip(cx, cy, r / g_radiusMi,
                                 g_lastFlight.dxMi, g_lastFlight.dyMi, g_lastFlight.distMi,
                                 g_flightLastPx, g_flightLastPy, TFT_CYAN, g_lastFlight.hdgDeg,
                                 kTrackedScale);
  tft.fillCircle(cx, cy, 3, TFT_WHITE); // you
}

// In-place per-second update for the flight-detail radar blip.
void drawFlightDetailRadarInPlace() {
  int cx = kRadarCX, cy = kRadarCY, r = kRadarR;
  if (g_flightBlipOn) eraseRadarBlip(g_flightLastPx, g_flightLastPy);
  drawRadarFrame(cx, cy, r);
  drawTrackPolyline(cx, cy, r / g_radiusMi);
  drawTrackProjection(cx, cy, r / g_radiusMi, g_lastFlight.dxMi, g_lastFlight.dyMi, g_lastFlight.hdgDeg);
  g_flightBlipOn = plotRadarBlip(cx, cy, r / g_radiusMi,
                                 g_lastFlight.dxMi, g_lastFlight.dyMi, g_lastFlight.distMi,
                                 g_flightLastPx, g_flightLastPy, TFT_CYAN, g_lastFlight.hdgDeg,
                                 kTrackedScale);
  tft.fillCircle(cx, cy, 3, TFT_WHITE); // you
}

// Advance an offset by dtMs of straight-level flight; missing heading (-1) or
// zero speed means no extrapolation.
void deadReckonPosition(float& dxMi, float& dyMi, float& distMi,
                        float hdgDeg, int spdKt, unsigned long dtMs) {
  if (hdgDeg < 0.0f || spdKt <= 0 || dtMs == 0) return;
  float dtHr = dtMs / 3600000.0f;
  float spdMph = spdKt * 1.15078f;
  float rad = hdgDeg * PI / 180.0f;
  dyMi += spdMph * cos(rad) * dtHr;   // north component
  dxMi += spdMph * sin(rad) * dtHr;   // east  component
  distMi = sqrtf(dxMi * dxMi + dyMi * dyMi);
}

// Per-second dead-reckoning + in-place radar redraw; deltas capped so a long
// absence can't jump wildly (the next poll overwrites anyway).
void stepRadar() {
  unsigned long now = millis();
  for (int i = 0; i < planeCount; i++) {
    Plane &p = planes[i];
    unsigned long dt = now - p.lastStepMs;
    p.lastStepMs = now;
    if (dt > 2000) continue;          // gap too long: don't extrapolate
    // The tracked (overhead) flight dead-reckons along its real ground track's
    // bearing when we have it; everyone else uses the live heading.
    float hdg = p.hdgDeg;
    if (i == 0 && g_trackBearingDeg >= 0.0f) hdg = g_trackBearingDeg;
    deadReckonPosition(p.dxMi, p.dyMi, p.distMi, hdg, p.spdKt, dt);
  }
  if (g_lastFlight.valid) {
    unsigned long dt = now - g_lastFlight.tickMs;
    g_lastFlight.tickMs = now;
    if (dt <= 2000) {
      float hdg = g_lastFlight.hdgDeg;
      if (g_trackBearingDeg >= 0.0f) hdg = g_trackBearingDeg;
      deadReckonPosition(g_lastFlight.dxMi, g_lastFlight.dyMi, g_lastFlight.distMi,
                         hdg, g_lastFlight.spdKt, dt);
    }
  }

  // Move blips only on a screen actually showing a radar (g_radarShown) - else
  // we'd paint a phantom onto the idle dashboard before its full redraw.
  if (g_screen == SCR_FLIGHTDETAIL) {
    if (g_radarShown) drawFlightDetailRadarInPlace();
  } else if (g_screen == SCR_DASH) {
    if (g_radarShown) drawRadarInPlace();
  }
}

// Flight-recall page (tap "N aircraft"); mirrors the overhead view, placeholders
// if none seen. Auto-returns after ~30s (see loop()).
void drawFlightDetailPage() {
  tft.fillScreen(TFT_BLACK);
  drawHeaderBand();
  drawFlightBackButton();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(4);
  tft.setCursor(8, 40);
  if (g_lastFlight.valid) tft.print(g_lastFlight.callsign);
  else tft.print("-No Data-");
  tft.setTextSize(1);
  tft.setTextFont(2);

  if (g_lastFlight.valid) {
    String alName; uint16_t alColor;
    if (airlineInfo(g_lastFlight.callsign, alName, alColor)) {
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(8, 74);
      tft.print(alName.substring(0, 26));
    }

    tft.setCursor(8, 94);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    if (g_units == UNITS_METRIC) {
      tft.printf("%dm  %dkts  %.1fkm", (int)round(g_lastFlight.altFt * 0.3048f),
                 g_lastFlight.spdKt, g_lastFlight.distMi * 1.60934f);
    } else if (g_units == UNITS_AVIATION) {
      tft.printf("%dft  %dkts  %.1fnm", g_lastFlight.altFt,
                 g_lastFlight.spdKt, g_lastFlight.distMi * 0.868976f);
    } else {
      tft.printf("%dft  %dmph  %.1fmi", g_lastFlight.altFt,
                 (int)round(g_lastFlight.spdKt * 1.15078f), g_lastFlight.distMi);
    }

    // Origin/destination from the snapshot (same rules as drawFlightInfo).
    const char* origin = g_lastFlight.origin;
    const char* dest   = g_lastFlight.dest;
    const char* originCity = g_lastFlight.originCity;
    const char* destCity   = g_lastFlight.destCity;
    bool origKnown = (origin[0] != 0);
    bool destKnown = (dest[0] != 0);
    int y = 114;
    if (origKnown) {
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(8, y); tft.print("Origin");
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(8, y + 16); tft.print(origin);
      tft.setCursor(8, y + 32); tft.print(originCity[0] ? originCity : "--");
      y += 54;
    }
    if (destKnown) {
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(8, y); tft.print("Destination");
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(8, y + 16); tft.print(dest);
      tft.setCursor(8, y + 32); tft.print(destCity[0] ? destCity : "--");
    } else if (g_lastFlight.routeFetched && !destKnown && origKnown) {
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(8, y); tft.print("Destination");
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(8, y + 16); tft.print("--");
      tft.setCursor(8, y + 32); tft.print("--");
    }
    if (g_lastFlight.routeFetched && !origKnown && !destKnown) {
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(8, y); tft.print("No route data");
    }

    // Airline logo (top-right), if one exists.
    const RuntimeLogo* logo = findAirlineLogo(g_lastFlight.callsign);
    if (logo) {
      drawScaledBitmap(RX(226), 40, logo->data, logo->w, logo->h, 1, 0xF81F);
      logoRelease(logo);
    }
  } else {
    // No overhead flight recorded yet: show dashes for the fields.
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(8, 94);
    tft.print("--  --  --");   // alt / speed / distance
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 130);
    tft.print("No overhead flight recorded.");
  }

  // Radar showing the recalled plane's position (red = was within radius).
  if (g_lastFlight.valid) drawFlightDetailRadar();
  g_radarShown = g_lastFlight.valid;
}

// Settings screens + helpers (drawSettings, drawFtracker, handleFtrackerTouch,
// drawReset, drawSleep, drawEditRow, handleSleepTouch, drawSlider) in settings.ino.

// ---- touch ----
bool inRect(int x, int y, int x0, int y0, int x1, int y1) {
  return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

bool rowMinus(int x, int y, int rowY) { return inRect(x, y, RX(246), rowY, RX(276), rowY + 24); }
bool rowPlus(int x, int y, int rowY)  { return inRect(x, y, RX(280), rowY, RX(310), rowY + 24); }

void saveFloat(const char* key, float v) {
  prefs.begin("flight", false); prefs.putFloat(key, v); prefs.end();
  dirty = true;
}
void saveInt(const char* key, int v) {
  prefs.begin("flight", false); prefs.putInt(key, v); prefs.end();
  dirty = true;
}

// Read one XPT2046 channel over VSPI. Must wait ~200us after the command for
// the ADC to convert - reading early gives wildly unstable values.
static uint16_t xptChannel(uint8_t cmd, SPIClass& spi) {
  spi.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS_PIN, LOW);
  spi.transfer(cmd);                 // command byte
  delayMicroseconds(200);            // let the ADC convert
  uint16_t tmp = spi.transfer(0);    // first 8 data bits
  delayMicroseconds(200);
  tmp = (tmp << 5);
  tmp |= 0x1f & (spi.transfer(0) >> 3);   // last 8 data bits
  digitalWrite(TOUCH_CS_PIN, HIGH);
  spi.endTransaction();
  return tmp & 0x0FFF;
}

// Read XPT2046 touch mapped to logical coords, gated on the GPIO 36 IRQ - idle
// channel noise (esp. with WiFi) makes a value threshold give false touches.
bool touchReadXY(uint16_t& outX, uint16_t& outY, uint16_t* rawX = nullptr, uint16_t* rawY = nullptr) {
  static SPIClass tspi(TOUCH_SPI);
  static bool inited = false;
  if (!inited) {
    tspi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS_PIN);
    pinMode(TOUCH_CS_PIN, OUTPUT);
    digitalWrite(TOUCH_CS_PIN, HIGH);
    pinMode(TOUCH_IRQ_PIN, INPUT);   // input-only pin, no pull-up available
    inited = true;
  }

  if (digitalRead(TOUCH_IRQ_PIN) != LOW) return false;  // not touched

  uint16_t rx = xptChannel(0xD0, tspi);   // raw X
  uint16_t ry = xptChannel(0x90, tspi);   // raw Y
  if (rawX) *rawX = rx;
  if (rawY) *rawY = ry;

  // Map raw -> panel coords using the NVS-calibrated linear transform
  // (see calibration.ino). disp = (raw - offset) * 1000 / scale.
  long dx = ((long)ry - g_calOffX) * 1000L / g_calScaleX;
  long dy = ((long)rx - g_calOffY) * 1000L / g_calScaleY;
#ifdef CYD_E32R40T
  // E32R40T touch axes run inverted vs the panel - flip in panel space (the cal
  // fit is positive-scale only), then convert panel -> logical.
  dx = (479 - dx) * 3 / 4;
  dy = (319 - dy) * 3 / 4;
#endif
  outX = (uint16_t)constrain(dx, 0, DISP_W - 1);
  outY = (uint16_t)constrain(dy, 0, 239);

  return true;
}

void handleTouch() {
  uint16_t x, y, rx, ry;
  static bool prevPressed = false;

  bool pressed = touchReadXY(x, y, &rx, &ry);
#if TOUCH_DEBUG
  // Print once per tap (rising edge only), not every iteration while held.
  if (pressed && !prevPressed) Serial.printf("touch raw(%u,%u) map(%u,%u)\n", rx, ry, x, y);
#endif

  // While calibration is active, handleTouch defers entirely to calPoll() so it
  // doesn't consume or mis-handle calibration taps.
  if (g_calState != CAL_NONE) { prevPressed = pressed; return; }

  // Any touch resets the idle timeout for non-dashboard screens.
  if (pressed) g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;

  // Long-press (10s) on any screen enters touch calibration. Track the press in
  // real time so a held finger is caught before it's released.
  if (g_calState == CAL_NONE) {
    if (pressed && !prevPressed) {
      g_calLongPressStart = millis();          // press just began
    } else if (pressed && prevPressed) {
      // still held - check for the 10s threshold
      if ((long)(millis() - g_calLongPressStart) >= 10000L) {
        calBegin();
        return;
      }
    }
    // (released: prevPressed goes false; nothing to do here)
  }

  // Only act on the rising edge of a press (like a tap), not continuous holds.
  if (!(pressed && !prevPressed)) { prevPressed = pressed; return; }
  prevPressed = true;   // we've acted on this press; ignore until it's released

  // Draw a crosshair at the touch point so the user sees where the touch is
  // being registered. The screen redraws within ~1s, clearing it.
  tft.drawFastHLine(x - 8, y, 16, TFT_RED);
  tft.drawFastVLine(x, y - 8, 16, TFT_RED);

  if (g_screen == SCR_WIFI) { handleWifiTouch(x, y); return; }

  if (g_screen == SCR_GENERAL) { handleGeneralTouch(x, y); return; }

  if (g_screen == SCR_ABOUT) { handleAboutTouch(x, y); return; }

  if (g_screen == SCR_HELP) { handleHelpTouch(x, y); return; }

  if (g_screen == SCR_SLEEP) { handleSleepTouch(x, y); return; }

  if (g_screen == SCR_FTRACKER) { handleFtrackerTouch(x, y); return; }

  if (g_screen == SCR_POOL) { handlePoolTouch(x, y); return; }

  if (g_screen == SCR_POOLGRAPH) { handlePoolGraphTouch(x, y); return; }

  if (g_screen == SCR_WXGRAPH) { handleWxGraphTouch(x, y); return; }

  if (g_screen == SCR_COLORPICK) { handleColorPickTouch(x, y); return; }
  if (g_screen == SCR_ALARMS) { handleAlarmsTouch(x, y); return; }

  if (g_screen == SCR_ALARMFIRE) { handleAlarmFireTouch(x, y); return; }

  if (g_screen == SCR_CREDITS) {
    if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = g_creditsReturn; dirty = true; return; }  // Back
    return;
  }

  if (g_screen == SCR_RESET) {
    if (g_resetConfirm == 0) {
      // Step 1: choose what to reset. Buttons stacked top-right: Factory Reset,
      // Graph Data, Settings; Restart bottom-left, Cancel bottom-right.
      if (inRect(x, y, RX(172), 36, RX(312), 66)) { g_resetConfirm = 1; dirty = true; }          // Factory Reset
      else if (inRect(x, y, RX(172), 80, RX(312), 110)) { g_resetConfirm = 3; dirty = true; }    // Graph Data
      else if (inRect(x, y, RX(172), 124, RX(312), 154)) { g_resetConfirm = 2; dirty = true; }   // Settings
      else if (inRect(x, y, 10, 210, 150, 236)) { g_resetConfirm = 4; dirty = true; }    // Restart
      else if (inRect(x, y, RX(172), 210, RX(312), 236)) { g_resetConfirm = 0; g_screen = SCR_SETTINGS; dirty = true; }  // Cancel
      return;
    }
    // Step 2: confirmation prompt.
    if (inRect(x, y, 30, 180, 140, 214)) {  // Yes -> wipe + reboot
      // Reset scopes: (3) graph files only; (2) NVS minus touch calibration
      // (hardware-specific, same namespace); (1) also logos + calibration.
      if (g_resetConfirm == 4) {  // Restart only, no data wipe
        ESP.restart();
      }
      if (g_resetConfirm != 3) {
        prefs.begin("flight", false);
        prefs.clear();
        if (g_resetConfirm == 2) {
          prefs.putInt("calsx", g_calScaleX); prefs.putLong("calox", g_calOffX);
          prefs.putInt("calsy", g_calScaleY); prefs.putLong("caloy", g_calOffY);
        }
        prefs.end();
      }
      if (g_resetConfirm == 1 || g_resetConfirm == 3) { poolfsWipe(); weatherfsWipe(); }   // Factory/Graph Data delete pool + weather history files
      if (g_resetConfirm == 1) { logosWipe(); }   // Factory Reset also removes airline logos
      // On-screen feedback + a short pause so NVS/LittleFS flush before reboot.
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextFont(2);
      if (g_resetConfirm == 1) {                      // Factory Reset
        tft.drawCentreString("Performing Factory Reset...", CX, 108, 2);
      } else if (g_resetConfirm == 2) {               // Settings
        tft.drawCentreString("Resetting Stored", CX, 100, 2);
        tft.drawCentreString("Settings...", CX, 118, 2);
      } else {                                        // Graph Data
        tft.drawCentreString("Resetting Graph Data...", CX, 108, 2);
      }
      delay(1500);
      ESP.restart();
    } else if (inRect(x, y, CX + 20, 180, CX + 130, 214)) {  // No -> back to choose
      g_resetConfirm = 0;
      dirty = true;
    }
    return;
  }

  if (g_screen == SCR_SETTINGS) {
    // 2-column grid; geometry mirrors drawSettings() in settings.ino.
    const int n = 10, rowH = 34, step = 38;
    const int colW = (DISP_W - 30) / 2;          // mirrors drawSettings()
    const int colX[2] = { 10, 10 + colW + 10 };
    const int y0 = 42;
    if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_DASH; dirty = true; return; }   // Back
    int row = (y >= y0) ? (y - y0) / step : -1;
    int col = (x >= colX[1]) ? 1 : 0;
    int idx = (row < 0) ? -1 : row * 2 + col;   // flat index, same order as drawSettings
    if (idx < 0 || idx >= n) return;            // empty grid cell or out of range
    // Confirm the tap is actually inside this button (not a row/column gap).
    if (!inRect(x, y, colX[col], y0 + row * step, colX[col] + colW - 1, y0 + row * step + rowH - 1)) return;
    if (idx == 0) { g_screen = SCR_ABOUT; dirty = true; g_updateState = 1; netWantUpdateCheck = true; return; }  // About
    if (idx == 1) { calBegin(); return; }                                // Calibrate Touch
    if (idx == 2) { g_ftPage = 0; g_screen = SCR_FTRACKER; dirty = true; return; }   // Flight Tracker
    if (idx == 3) { g_screen = SCR_GENERAL; dirty = true; return; }      // General
    if (idx == 4) { g_screen = SCR_HELP; g_helpScroll = 0; dirty = true; return; }  // Help
    if (idx == 5) { g_screen = SCR_LOCATION; dirty = true; return; }     // Location
    if (idx == 6) { enterWifiScreen(); return; }                         // Network
    if (idx == 7) { g_screen = SCR_POOL; dirty = true; return; }         // Pool Temp
    if (idx == 8) { g_resetConfirm = 0; g_screen = SCR_RESET; dirty = true; return; }   // Reset
    if (idx == 9) { g_screen = SCR_SLEEP; dirty = true; return; }        // Sleep Mode
    return;
  }

  if (g_screen == SCR_LOCATION) {
    if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; dirty = true; return; }  // Back
    if (inRect(x, y, RX(240), 40, RX(312), 64)) {  // Set lat/lon
      char ll[32];
      snprintf(ll, sizeof ll, "%.6f,%.6f", g_lat, g_lon);
      g_latLonStr = ll;
      g_screen = SCR_WIFI;
      g_wifiSub = 10;
      dirty = true;
      return;
    }
    if (inRect(x, y, 10, 96, DISP_W - 10, 130)) {  // Search Address
      // Keep g_addrSearch so a failed/previous search can be edited rather
      // than retyped from scratch.
      g_screen = SCR_WIFI;
      g_wifiSub = 5;
      dirty = true;
      return;
    }
    if (inRect(x, y, 10, 140, DISP_W - 10, 174)) {  // Find by IP
      fetchIpLocation();
      saveFloat("lat", g_lat);
      saveFloat("lon", g_lon);
      dirty = true;
      return;
    }
    return;
  }

  if (g_screen == SCR_CALIB) {
    // Calibration runs via calPoll(); this screen is only shown during it.
    return;
  }

  if (g_screen == SCR_DASH) { // SCR_DASH
    bool overhead = g_trackEnabled && !g_suppressFlight && flightOverhead();

    // Header-credits tap opens the Credits screen (only drawn while tracking
    // is on). Zone avoids the flight Back button.
    if (g_trackEnabled && inRect(x, y, RX(194), 0, RX(264), 33)) { g_creditsReturn = SCR_DASH; g_screen = SCR_CREDITS; dirty = true; return; }

    // Clock/date tap opens Alarms; on the 4" the centered clock widens the zone
    // (RX keeps it clear of the credits zone).
    if (inRect(x, y, 0, 0, RX(190), HEADER_H - 1)) { g_screen = SCR_ALARMS; g_alarmIdx = constrain(g_alarmIdx, 0, g_alarmCount - 1); dirty = true; return; }

    // settings cog -> settings. Generous tap zone so it's easy to hit even with
    // a small touch-calibration offset (the cog itself is only ~28x24).
    if (inRect(x, y, RX(278), 184, DISP_W, 234)) { g_screen = SCR_SETTINGS; dirty = true; return; }

    // Back / countdown-bar tap dismisses the overhead view. Gate on
    // g_radarShown (view up), not `overhead` - dead-reckoning can clear it
    // early. The bar zone applies only while the timer is drawn.
    if (g_radarShown &&
        // Back zone runs to the screen edge so taps the calibration maps hot still register.
        ((inRect(x, y, RX(265), 4, DISP_W - 1, 24)) ||
         (g_showTimer && x >= RX(296) && y >= HEADER_H + 2 && y <= 200))) {  // countdown bar
      g_suppressFlight = true;
      dirty = true;
      return;
    }

    // tapping the pool icon (idle, pool enabled) opens the history graph
    if (!overhead && g_poolEnabled && inRect(x, y, 4, 146, 100, 170)) {
      g_screen = SCR_POOLGRAPH;
      g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;
      dirty = true;
      return;
    }

    // tapping the weather temperature (idle, always shown) opens the weather
    // temp history graph
    if (!overhead && inRect(x, y, 4, 38, 100, 72)) {
      g_screen = SCR_WXGRAPH;
      g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;
      dirty = true;
      return;
    }

    // "N aircraft" tap recalls the last overhead flight; disabled while a snooze
    // countdown owns the line (polls suppressed, so the recall would be stale).
    if (!overhead && !snoozePending() && inRect(x, y, 4, 216, 170, 236)) {
      g_screen = SCR_FLIGHTDETAIL;
      g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;
      dirty = true;
      return;
    }
  }

  if (g_screen == SCR_FLIGHTDETAIL) {
    // Any tap resets the 30s auto-return; Back returns to the dashboard;
    // credits tap opens the Credits screen (returning here).
    g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;
    if (g_trackEnabled && inRect(x, y, RX(194), 0, RX(264), 33)) { g_creditsReturn = SCR_FLIGHTDETAIL; g_screen = SCR_CREDITS; dirty = true; return; }
    if (inRect(x, y, 0, 0, RX(190), HEADER_H - 1)) { g_screen = SCR_ALARMS; g_alarmIdx = constrain(g_alarmIdx, 0, g_alarmCount - 1); dirty = true; return; }
    if (inRect(x, y, RX(265), 4, DISP_W - 1, 24)) { g_screen = SCR_DASH; dirty = true; return; }
    return;
  }
}

// ---- Program setup ----
void setup() {
  Serial.begin(115200);
  prefs.begin("flight", false);
#if ENABLE_SERIAL_PROVISION
  serialProvision();   // optional: import KEY=VALUE credentials from USB serial into NVS
#endif
  calLoad();           // load touch calibration parameters (defaults if none)
#if TOUCH_DEBUG
  Serial.printf("cal: scaleX=%d offX=%ld scaleY=%d offY=%ld\n",
                g_calScaleX, (long)g_calOffX, g_calScaleY, (long)g_calOffY);
#endif
  g_radiusMi = prefs.getFloat("radius", 3.5f);
  g_ceilingFt = prefs.getInt("ceiling", 15000);
  g_pollSec = prefs.getInt("poll", 30);
  g_savedSsid = prefs.getString("ssid", "");
  g_savedPass = prefs.getString("pass", "");
  loadNetCfg();   // ipdhcp/ipaddr/ipmask/ipgw/ipdns/hostname
  g_osClientId = prefs.getString("oscid", "");
  g_osClientSecret = prefs.getString("ocssec", "");
  // Board model on every boot (not dev-gated): scripts/detect_boards.py uses
  // it to map serial ports to board variants before OTA/flash updates.
#ifdef CYD_E32R40T
  Serial.println("[boot] board=e32r40t");
#else
  Serial.println("[boot] board=2432s028r");
#endif
  // Version on every build - detect_boards.py parses it (releases: build=0).
  Serial.printf("[boot] version=%s build=%d\n", kVersion, (int)BUILD_NUM);
  if (isDevBuild()) {
    Serial.printf("[boot] %s\n", kBuildTag);
    Serial.printf("[boot] OpenSky credentials clientId=%s clientSecret=%s\n",
                  g_osClientId.length() ? "set" : "blank",
                  g_osClientSecret.length() ? "set" : "blank");
  }
  g_lat = prefs.getFloat("lat", 0.0f);
  g_lon = prefs.getFloat("lon", 0.0f);
  if (isDevBuild()) {
    Serial.printf("[boot] NVS location lat=%.5f lon=%.5f\n", g_lat, g_lon);
  }
  g_sleepOn = prefs.getBool("sleepon", true);
  g_sleepStartH = prefs.getInt("sleepsH", 22);
  g_sleepStartM = prefs.getInt("sleepsM", 0);
  g_sleepEndH = prefs.getInt("sleepeH", 8);
  g_sleepEndM = prefs.getInt("sleepeM", 0);
  g_wakeMin = prefs.getInt("wake", 5);
  // Sync sleep-time edit buffers from NVS - they default to hardcoded strings
  // and would show stale "22:00"/"08:00" after every reboot.
  {
    char buf[8];
    snprintf(buf, sizeof buf, "%02d%02d", g_sleepStartH, g_sleepStartM);
    g_sleepStartStr = buf;
    snprintf(buf, sizeof buf, "%02d%02d", g_sleepEndH, g_sleepEndM);
    g_sleepEndStr = buf;
  }
  g_trackEnabled = prefs.getBool("track", true);
  g_blinkForFlight = prefs.getBool("blinkf", true);
  g_units = prefs.getInt("units", prefs.getBool("metric", false) ? UNITS_METRIC : UNITS_IMPERIAL);
  if (prefs.isKey("metric")) prefs.remove("metric");   // migrated to "units"
  g_clock24 = prefs.getBool("clock24", false);
  g_showTimer = prefs.getBool("timer", false);
  g_showIata = prefs.getBool("showiata", true);
  g_autoUpdate = prefs.getBool("autoupd", true);
  g_lastScanDay = prefs.getULong("lastscan", 0);
  // Dev builds never auto-update: force off for this boot, but don't persist -
  // the user's preference must survive into the next release build.
  if (isDevBuild() && g_autoUpdate) {
    g_autoUpdate = false;
  }
  g_clockCol = (uint16_t)prefs.getUInt("clkcol", DEFAULT_CLOCK_COL);
  g_homeAirport = prefs.getString("homeap", "");
  g_watchCallsign = prefs.getString("watchcs", "");
  // Callsign Notify defaults to "Radar" - looked up by name so it stays
  // correct even if preset indices shift in a later release.
  int watchNtfDef = 0;
  for (int i = 0; i < alarmPresetCount(); i++)
    if (strcmp(alarmPresetName(i), "Radar") == 0) { watchNtfDef = i; break; }
  g_watchNotify = constrain((int)prefs.getInt("watchntf", watchNtfDef), 0, alarmPresetCount() - 1);
  g_notifyVol = kNtfVolLevels[ntfVolIdx((int)prefs.getInt("ntfvol", 100))];
  g_goveeKey = prefs.getString("govee", "");
  g_poolDeviceId = prefs.getString("poolid", "");
  g_poolModel = prefs.getString("poolmodel", "");
  g_poolName = prefs.getString("poolname", "");
  g_poolEnabled = prefs.getBool("poolen", false);
  g_goveeRateReset = prefs.getULong("goveerl", 0);
  g_wxNextEpoch = prefs.getULong("wxrl", 0);
  if (isDevBuild() && (g_goveeRateReset || g_wxNextEpoch)) {
    Serial.printf("[boot] ratelimit wxrl=%lu goveerl=%lu\n",
                  g_wxNextEpoch, g_goveeRateReset);
  }
  // First boot = no saved location; the first data load guesses it from IP.
  g_firstBoot = (g_lat == 0.0f && g_lon == 0.0f);
  if (isDevBuild()) {
    Serial.printf("[boot] location state firstBoot=%d lat=%.5f lon=%.5f\n",
                  (int)g_firstBoot, g_lat, g_lon);
  }
  // First-boot wizard: calibrate if its NVS keys are absent, then WiFi creds.
  // Keys are read while the namespace is open - calLoad() must not re-open it.
  bool needCalib = !prefs.isKey("calsx");
  bool needWifi  = (g_savedSsid.length() == 0);
  if (needCalib)      g_bootStage = BOOT_CALIB;
  else if (needWifi)  g_bootStage = BOOT_WIFI;
  else                g_bootStage = BOOT_DONE;
  // First boot on a new version: play the 1-up jingle after the chime, then
  // stamp the version so it fires once.
  g_upgraded = (prefs.getString("lastver", "") != kVersion);
  if (g_upgraded) prefs.putString("lastver", kVersion);
  loadAlarms();   // reads the "alarms" blob while the namespace is still open
  prefs.end();

  poolfsInit();     // load persisted pool temp history from flash into RAM
  weatherfsInit();  // load persisted weather temp history from flash into RAM
  logosInit();    // mount the "logos" partition (may be absent -> run logo-less)

  // esp_sleep_get_wakeup_cause() survives soft resets with a stale value - only
  // trust it when esp_reset_reason() confirms a real deep-sleep wake.
  bool wokeFromDeepSleep = (esp_reset_reason() == ESP_RST_DEEPSLEEP);
  esp_sleep_wakeup_cause_t wakeCause = wokeFromDeepSleep
      ? esp_sleep_get_wakeup_cause() : ESP_SLEEP_WAKEUP_UNDEFINED;
#if TOUCH_IRQ_ENABLED
  // Release the touch-CS hold latched for the sleep wake - else the asserted
  // IRQ would re-fire EXT0 on every later deep-sleep attempt.
  gpio_hold_dis((gpio_num_t)TOUCH_CS_PIN);
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH);   // deselect the touch controller
  // Touch wake usually reports EXT0 but the cause register is unreliable -
  // also honor a still-asserted IRQ, else the device re-sleeps immediately.
  bool touchActiveNow = (digitalRead(TOUCH_IRQ_PIN) == LOW);
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0 || touchActiveNow) {
    wakeUntil = millis() + (unsigned long)g_wakeMin * 60000UL;
  }
#endif

  // Timer wake inside the sleep window: run the low-power pool logger; it
  // returns when the window ends or WiFi/time fails - then boot normally.
  bool alreadyAwake = false;
  if (g_sleepOn && wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    alreadyAwake = sleeperRun();
  }
  // A firing missed while asleep needs no special handling: the alarm's
  // nextFire is a past epoch now, so the first loop() checkAlarms() fires it.

  tft.init();
  tft.setRotation(1); // landscape 320x240 (480x320 panel on CYD_E32R40T)
  tft.fillScreen(TFT_BLACK);

  // Wizard: missing calibration -> calBegin() (calPoll() advances it); missing
  // WiFi creds -> straight to the WiFi screen.
  if (g_bootStage == BOOT_CALIB) {
    calBegin();
    // Calibration needs a responsive touch path before anything else.
    dirty = true;
  } else if (g_bootStage == BOOT_WIFI) {
    enterWifiScreen();
    dirty = true;
  }

  // Connect async and show the dashboard right away - boot isn't held by the
  // connect/fetch timeouts on a no-WiFi device.
  connected = (WiFi.status() == WL_CONNECTED);
  if (!alreadyAwake && !connected && g_savedSsid.length() > 0) {
    setNetHostname();
    WiFi.mode(WIFI_STA);
    applyNetConfig();
    WiFi.begin(g_savedSsid.c_str(), g_savedPass.c_str());   // non-blocking
    wifiTrying = true;   // tell loop() a connect is already in flight so it
    wifiTryStart = millis();  // doesn't call WiFi.begin() again mid-handshake
  }
  if (connected) {
    // NTP only after WiFi is up - configTime() before the link triggers the
    // lwip "Required to lock TCPIP core functionality" crash.
    setupNTP();
    delay(100);            // let SNTP get a moment to start cleanly
  }
  // First fetch runs on the net task at a jittered offset (fleet lockstep) -
  // boot no longer blocks on synchronous TLS.
  g_bootFetchAt = millis() + esp_random() % BOOT_FETCH_JITTER_MS;
  // Absent-result retry interval for the post-boot grace window: drawn once
  // per boot (55-65s) so boards that powered up together retry off-phase.
  g_bootFailRetryMs = 55000UL + esp_random() % 10001UL;
  if (isDevBuild()) {
    Serial.printf("[net] boot fetch in %lums\n",
                  (unsigned long)(g_bootFetchAt - millis()));
  }
  // Stay on the dashboard even without WiFi (setup reachable from Settings);
  // failed connects show "No WIFI" and retry.

  // Net task on the other core so blocking HTTP can't freeze the loop.
  // Stack: ~6 KB measured peak (mbedTLS + JSON), so 12 KB is ~2x headroom.
  xTaskCreatePinnedToCore(netTask, "net", NET_TASK_STACK_BYTES, NULL, 1, &g_netTask, 0);
  // OTA task pre-created at boot: a fresh task's first TLS connect was observed
  // to fail, and an on-demand stack drops below what mbedtls needs.
  xTaskCreate(otaTaskEntry, "ota", 12288, NULL, 1, &g_otaTask);

  // Boot chime + LED sweep ("device is on"), cold boots only - deep-sleep wakes
  // skip it; a first boot on a new version appends the 1-up jingle.
  if (!wokeFromDeepSleep) {
    playBootChime();
    if (g_upgraded) playUpgradeChime();
  }

  dirty = true;
}

// ---- Sleep / deep-sleep ----
// Deep sleep with a timer wake (pool logging) + touch-IRQ wake. Never returns.
void enterDeepSleep() {
#if TOUCH_IRQ_ENABLED
  // Keep the XPT2046 selected during deep sleep so its IRQ can assert LOW on
  // touch. Holding the CS line low lets GPIO 36 (the IRQ) wake us via EXT0.
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, LOW);
  gpio_hold_en((gpio_num_t)TOUCH_CS_PIN);
  delay(10);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_IRQ_PIN, 0);  // wake on LOW (touch)
#endif
  // Wake at the next alarm if sooner than the pool cadence so it fires on time;
  // nextFire is in NVS so the refire survives this sleep.
  uint64_t wakeUs = SLEEP_POOL_INTERVAL_US;
  time_t nextAlarm = nextAlarmAt();
  time_t epochNow = time(nullptr);
  if (nextAlarm > epochNow) {
    uint64_t alarmUs = (uint64_t)(nextAlarm - epochNow) * 1000000ULL;
    if (alarmUs < wakeUs) wakeUs = alarmUs;
  }
  esp_sleep_enable_timer_wakeup(wakeUs);
  saveRollupState();     // keep the in-progress pool hour/day rollups across this deep sleep
  saveWxRollupState();   // keep the in-progress weather hour/day rollups across this deep sleep
  esp_deep_sleep_start();
}

// Is the current local time inside the configured sleep window?
bool inSleepWindowNow() {
  if (!g_sleepOn) return false;
  // Only trust the clock after NTP sync - after a soft reset the RTC epoch
  // looks valid but the timezone isn't applied until configTime() runs.
  // sntp_get_sync_status() never completes on-device, so g_timeReady is the gate.
  if (!g_timeReady) return false;
  if (time(nullptr) < 1600000000L) return false;
  struct tm t;
  // 0ms timeout: called every loop iteration; getLocalTime()'s default 5s would
  // block the loop while time isn't synced.
  if (!getLocalTime(&t, 0)) return false;
  int cur = t.tm_hour * 60 + t.tm_min;
  int start = g_sleepStartH * 60 + g_sleepStartM;
  int end   = g_sleepEndH * 60 + g_sleepEndM;
  if (start < end) return (cur >= start && cur < end);
  return (cur >= start || cur < end);   // wraps past midnight
}

// Next sleep-window start after `after` (0 when Sleep Mode off) - bounds the
// daily update scan to the awake time remaining.
time_t nextSleepStart(time_t after) {
  if (!g_sleepOn) return 0;
  struct tm t;
  if (!getLocalTime(&t, 0)) return 0;
  for (int d = 0; d < 2; d++) {
    struct tm c = t;
    c.tm_mday += d;
    c.tm_hour = g_sleepStartH; c.tm_min = g_sleepStartM;
    c.tm_sec = 0; c.tm_isdst = -1;
    time_t cand = mktime(&c);
    if (cand > after) return cand;
  }
  return 0;
}

// Latest-resort scan slot: midpoint of the largest alarm-free gap in [from, horizon].
static time_t bestEffortSlot(time_t from, time_t horizon) {
  time_t best = from, segStart = from;
  long bestGap = -1;
  time_t occ = nextAlarmAfter(from - 1);
  while (occ && occ <= horizon) {
    if (occ - segStart > bestGap) {
      bestGap = occ - segStart;
      best = segStart + (occ - segStart) / 2;
    }
    segStart = occ + 1;
    occ = nextAlarmAfter(occ);
  }
  if (horizon - segStart > bestGap) best = segStart + (horizon - segStart) / 2;
  return best;
}

// Low-power pool logger for the sleep window: wake, sync time, log, sleep again.
// Returns true only when the window has ended (caller boots normally).
bool sleeperRun() {
  if (g_savedSsid.length() == 0) return false;
  // WiFi before NTP - configTime() pre-link triggers the lwip crash (see setup()).
  bool conn = tryConnect(g_savedSsid.c_str(), g_savedPass.c_str());
  if (!conn) return false;
  setupNTP();

  // wait for NTP time so the sleep-window check is valid
  unsigned long t0 = millis();
  while (time(nullptr) < 1600000000L && millis() - t0 < 8000) delay(100);

  while (true) {
    if (!inSleepWindowNow()) return true;    // window ended -> boot normally
    // An alarm whose time passed since the last 5-minute wake wins over the
    // sleep schedule: boot normally and let loop()'s checkAlarms() fire it.
    if (alarmDueNow()) return true;
    // Boards that powered up together (outage restore) share the same 5-min
    // wake phase; a short random pause keeps their API calls out of lockstep.
    delay(esp_random() % SLEEP_WAKE_JITTER_MS);
    if (g_poolEnabled && g_goveeKey.length() > 0 && g_poolDeviceId.length() > 0) {
      fetchGoveeTemp();                      // logs to flash via poolLog
    }
    fetchWeather();                          // logs weather temp to flash via weatherLog
    enterDeepSleep();                        // resets on next wake
  }
}

// Runs performOTA on the pre-created task (TLS overflows loopTask; a fresh
// task's first connect was seen to fail). Owns the display; reboots on success.
void otaTaskEntry(void*) {
  for (;;) {
    if (!g_otaRunning) { vTaskDelay(50 / portTICK_PERIOD_MS); continue; }
    // Draw "Updating" before any blocking network wait so the UI doesn't look frozen.
    drawOtaHeader(g_otaVersion);
    // Wait for in-flight net fetches: two tasks doing HTTP/lwIP at once can
    // trip a FreeRTOS xTaskPriorityDisinherit assert.
    while (netBusy) vTaskDelay(20);
    performOTA(g_otaUrl, g_otaVersion, g_otaSha256);
    // Only reached on failure (success reboots via ESP.restart()):
    g_otaRunning = false;
    g_screen = SCR_ABOUT;
    g_updateState = 4;   // Update Check Failed
    dirty = true;
  }
}

void loop() {
  unsigned long now = millis();

  // While an OTA runs on its dedicated task, yield so it can own the display
  // (no TFT contention from the loop).
  if (g_otaRunning) { delay(10); return; }

#if ENABLE_SERIAL_PROVISION
  handleSerialCommands();   // runtime OTA / command input (no-op if no serial)
#endif

#if TOUCH_DEBUG
  // Heartbeat: if the gap between prints grows large, loop() itself is
  // stalling somewhere (not just slow drawing/network). Logged at most once/sec.
  static unsigned long lastBeat = 0;
  if (now - lastBeat >= 1000) {
    unsigned long gap = (lastBeat == 0) ? 0 : (now - lastBeat);
    Serial.printf("loop alive t=%lu gap=%lums dirty=%d screen=%d netBusy=%d\n",
                  now, gap, dirty, (int)g_screen, netBusy);
    lastBeat = now;
  }
#endif

  // Fire a due alarm before anything can sleep the device - firing switches to
  // SCR_ALARMFIRE, which also gates the SCR_DASH checks below.
  checkAlarms();

  // --- Sleep Mode ---
  // Sleep only from the dashboard; a pending snooze doesn't block (NVS
  // nextFire survives); a ringing alarm can't be slept on (SCR_ALARMFIRE).
  bool asleep = false;
  if (g_screen == SCR_DASH) {
    // expire a user-triggered wake once the duration has elapsed
    if (wakeUntil != 0 && (long)(now - wakeUntil) >= 0) wakeUntil = 0;
    asleep = !g_otaActive && !g_otaRunning
             && inSleepWindowNow() && (wakeUntil == 0);
  }

  if (asleep) {
    // Blank the display, then deep sleep with a timer wake (pool logging) +
    // optional touch-IRQ wake.
    if (!g_displayOff) { tft.writecommand(0x28); g_displayOff = true; }  // ILI9341 off
    delay(150);
    enterDeepSleep();   // does not return; resets on next wake
  }

  // --- Awake path: ensure display is on ---
  if (g_displayOff) { tft.writecommand(0x29); g_displayOff = false; dirty = true; }

  // Hand a pending OTA (About Install / daily scan) to the OTA task via
  // g_otaRunning; the task owns the display while it runs.
  if (g_otaActive && !g_otaRunning) {
    g_otaActive = false;
    g_otaRunning = true;
    g_screenIdleUntil = 0;   // drop any armed idle return so it can't fire mid-OTA
    // Bail before the dirty-redraw: concurrent TFT/SPI draws with the OTA task
    // can hang the SPI bus (TFT_eSPI has no cross-task locking).
    return;
  }
  // OTA rollback safeguard: after a successful boot grace period, cancel any
  // pending rollback so a freshly-installed slot stays active.
  if (!g_rollbackMarked && now > 30000UL) { g_rollbackMarked = true; markAppValidBoot(); }

  handleTouch();
  calPoll();   // drive the touch-calibration state machine (no-op unless active)
  pollWifiScan();   // harvests/finishes an in-flight async WiFi scan

  // --- WiFi availability / non-blocking reconnect ---
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (!wifiUp && g_savedSsid.length() > 0) {
    if (!wifiTrying) {
      // kick off an asynchronous connect with the saved settings
      wifiTrying = true;
      wifiTryStart = now;
      setNetHostname();
      WiFi.mode(WIFI_STA);
      applyNetConfig();
      WiFi.begin(g_savedSsid.c_str(), g_savedPass.c_str());
    } else if (now - wifiTryStart > 25000) {
      // give up on this attempt and try again shortly
      WiFi.disconnect();
      wifiTrying = false;
      wifiTryStart = now;
    }
  } else {
    wifiTrying = false;   // connected or no creds saved
  }
  connected = wifiUp;

  // First WiFi-up: start NTP here (pre-link configTime() crashes lwip) and
  // redraw; the first data load fires on the jittered deadline below.
  if (wifiUp && !g_wifiConnectedOnce) {
    g_wifiConnectedOnce = true;
    g_firstConnectAt = now;
    setupNTP();
    delay(100);
    if (isDevBuild()) {
      Serial.printf("[net] ip=%s mask=%s gw=%s dns=%s dhcp=%d hostname=%s\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.subnetMask().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str(),
                    WiFi.dnsIP().toString().c_str(),
                    (int)g_ipDhcp,
                    g_hostname.c_str());
    }
    if (g_screen == SCR_DASH) dirty = true;
  }

  // First data load at a jittered offset (fleet lockstep), held until time is
  // synced (verified TLS fails on future-dated certs) up to a cap.
  if (wifiUp && !g_bootFetched && (long)(now - g_bootFetchAt) >= 0
      && (time(nullptr) >= 1600000000L || now - g_firstConnectAt >= BOOT_TIME_WAIT_MS)) {
    g_bootFetched = true;
    if (g_trackEnabled) { lastPoll = now; netWantFlights = true; }
    g_lastWeather = now;
    // No saved location (first boot): guess it from IP; fetchIpLocation()
    // chains into fetchWeather() once it has coordinates.
    if (g_firstBoot) netWantLocation = true;
    else netWantWeather = true;
#if POOL_FEATURE
    if (g_poolEnabled && g_goveeKey.length() > 0) {
      if (g_poolDeviceId.length() > 0) { g_lastPool = now; netWantPool = true; }
      else netWantPoolDevices = true;   // key set but no device: auto-pick one
    }
#endif
    dirty = true;
  }

  // --- Daily auto-update scan scheduling ---
  // Once/day at a random offset (fleet lockstep), never near an alarm or in the
  // sleep window (an OTA reboot would chime at night); runs on the net task.
  {
    time_t epoch = time(nullptr);
    if (g_autoUpdate && wifiUp && g_timeReady && epoch >= 1600000000L
        && !g_otaActive && !g_otaRunning && !inSleepWindowNow()) {
      unsigned long day = (unsigned long)(epoch / 86400UL);
      if (!g_autoScanAt && g_lastScanDay != day) {
        g_autoScanAt = epoch + (time_t)(esp_random() % AUTOSCAN_JITTER_S);
        if (isDevBuild())
          Serial.printf("[ota] auto scan scheduled in %lus\n",
                        (unsigned long)(g_autoScanAt - epoch));
      }
      if (g_autoScanAt && epoch >= g_autoScanAt) {
        if (g_alarmFiring) {
          // Never start an OTA over a ringing alarm; retry shortly. Kept
          // ahead of the forced check so even a least-bad slot waits.
          g_autoScanAt = epoch + 300;
        } else if (g_autoScanForced) {
          g_autoScanAt = 0; g_autoScanForced = false;
          netWantAutoScan = true;
        } else {
          time_t occ = nextAlarmAfter(epoch - AUTOSCAN_ALARM_QUIET_S);
          if (!occ || occ > epoch + AUTOSCAN_ALARM_QUIET_S) {
            g_autoScanAt = 0;
            netWantAutoScan = true;
          } else {
            time_t deferTo = occ + AUTOSCAN_ALARM_QUIET_S;
            // Don't defer past the next sleep entry - it would lose today's
            // only slot (scans never run inside the window).
            time_t horizon = g_sleepOn ? nextSleepStart(epoch) : 0;
            if (!horizon || deferTo <= horizon) {
              g_autoScanAt = deferTo;
            } else {
              g_autoScanAt = bestEffortSlot(epoch, horizon);
              g_autoScanForced = true;
            }
            if (isDevBuild())
              Serial.printf("[ota] auto scan deferred to epoch %lu%s\n",
                            (unsigned long)g_autoScanAt,
                            g_autoScanForced ? " (forced)" : "");
          }
        }
      }
    }
  }

  // --- Polling runs only while WiFi is up; otherwise updates are suspended ---
  if (g_screen == SCR_DASH && wifiUp) {
    // Radar poll cadence; while credits are exhausted, back off to a slow
    // recovery check (OpenSky resets daily) instead of hammering the API.
    if (g_trackEnabled && !snoozePending()) {   // no flight polls during a snooze
      bool wantFlights = false;
      // Both an exhausted credit bucket and a run of rejected tokens park the
      // next attempt in g_nextRadarMs; honor it instead of the normal cadence.
      if (g_creditsExhausted || g_auth401Streak >= AUTH_401_BACKOFF_AFTER) {
        if ((long)(now - g_nextRadarMs) >= 0) wantFlights = true;
      } else if (now - lastPoll >= (unsigned long)g_pollSec * 1000UL) {
        wantFlights = true;
      }
      if (wantFlights) {
        lastPoll = now;
        netWantFlights = true;
      }
    }
    // Weather poll cadence; an armed 429 retry fires sooner and suppresses the
    // cadence. Early-boot failures retry every g_bootFailRetryMs.
    if ((g_wxRetryAt == 0 &&
         (now - g_lastWeather >= WEATHER_REFRESH_MS ||
          (!g_weatherValid && now <= BOOT_FAIL_GRACE_MS && now - g_lastWeather >= g_bootFailRetryMs))) ||
        (g_wxRetryAt && (long)(now - g_wxRetryAt) >= 0)) {
      g_wxRetryAt = 0;
      g_lastWeather = now;
      netWantWeather = true;
      dirty = true;
    }
    // Periodic pool temp refresh (only when enabled and a Govee device is
    // selected); same armed-retry and post-boot grace pattern as weather.
#if POOL_FEATURE
    if (g_poolEnabled && g_poolDeviceId.length() > 0 &&
        ((g_poolRetryAt == 0 &&
          (now - g_lastPool >= POOL_REFRESH_MS ||
           (!g_poolValid && now <= BOOT_FAIL_GRACE_MS && now - g_lastPool >= g_bootFailRetryMs))) ||
         (g_poolRetryAt && (long)(now - g_poolRetryAt) >= 0))) {
      g_poolRetryAt = 0;
      g_lastPool = now;
      netWantPool = true;
      dirty = true;
    }
#endif
  }

  // A fetch finished on the network task; make sure the new data is drawn.
  if (netUpdated) {
    netUpdated = false;
    dirty = true;
  }

  // 1s refresh: header clock + countdown redrawn in place, radar blips dead-reckoned.
  if ((g_screen == SCR_DASH || g_screen == SCR_FLIGHTDETAIL) && now - lastClockDraw >= 1000) {
    lastClockDraw = now;
    updateDashboard();
  }

  // Auto-return to the dashboard after 2 min idle (any touch resets). Excludes
  // the boot wizard, pending/running OTA, and About after Install is tapped.
  if (g_calState == CAL_NONE && g_bootStage == BOOT_DONE && !g_otaActive && !g_otaRunning &&
      !(g_screen == SCR_ABOUT && g_otaFromAbout)) {
    if (g_screen == SCR_ALARMFIRE) {
      // An unanswered alarm auto-snoozes; the ALARM_MAX_SNOOZES cap turns the
      // next timeout into a dismiss for the day.
      if (g_screenIdleUntil == 0) g_screenIdleUntil = now + SCREEN_IDLE_TIMEOUT_MS;
      if (g_screenIdleUntil != 0 && (long)(now - g_screenIdleUntil) >= 0) alarmSnooze();
    } else if (g_screen != SCR_DASH) {
      if (g_screenIdleUntil == 0) g_screenIdleUntil = now + SCREEN_IDLE_TIMEOUT_MS;
      if (g_screenIdleUntil != 0 && (long)(now - g_screenIdleUntil) >= 0) {
        g_screen = SCR_DASH;
        g_helpScroll = 0;
        g_resetConfirm = 0;
        g_alarmDelConfirm = false;
        g_ftPage = 0;
        g_addrSearch = "";
        g_wifiSub = 0;
        g_otaFromAbout = false;
        g_screenIdleUntil = 0;
        dirty = true;
      }
    } else {
      g_screenIdleUntil = 0;
    }
  }

  if (dirty) {
    // Clear before drawing so a net-task or touch update that arrives during
    // the redraw remains pending instead of being lost by a trailing clear.
    dirty = false;
    if (g_calState != CAL_NONE) {
      // calibration draws its own screen (calDrawTarget / done message)
    } else if (g_screen == SCR_WIFI) drawWifiScreen();
    else if (g_screen == SCR_SETTINGS) drawSettings();
    else if (g_screen == SCR_GENERAL) drawGeneral();
    else if (g_screen == SCR_ABOUT) drawAbout();
    else if (g_screen == SCR_HELP) drawHelp();
    else if (g_screen == SCR_LOCATION) drawLocation();
    else if (g_screen == SCR_RESET) drawReset();
    else if (g_screen == SCR_SLEEP) drawSleep();
    else if (g_screen == SCR_FTRACKER) drawFtracker();
    else if (g_screen == SCR_POOL) drawPool();
    else if (g_screen == SCR_POOLGRAPH) drawPoolGraph();
    else if (g_screen == SCR_WXGRAPH) drawWxGraph();
    else if (g_screen == SCR_FLIGHTDETAIL) drawFlightDetailPage();
    else if (g_screen == SCR_CREDITS) drawCredits();
    else if (g_screen == SCR_ALARMS) drawAlarms();
    else if (g_screen == SCR_ALARMFIRE) drawAlarmFire();
    else if (g_screen == SCR_COLORPICK) drawColorPick();
    else drawDashboard();
  }

  // A firing alarm (or a notify-preset preview in the editor) owns the LED;
  // the flight/status LED logic below is skipped while it runs.
  updateAlarmLed(now);

  // Run a queued LED blink. The color was already decided in fetchFlights()
  // and re-checked on every poll, so we blink whenever the route state changes.
  BlinkColor blinked = BLINK_NONE;
  if (!alarmLedBusy()) {
  if (g_pendingBlink) {
    g_pendingBlink = false;
    blinked = g_blinkColor;
    if (g_blinkForFlight) blinkLed(g_blinkColor);
  }

  // Is a flight live on the dashboard (not the recall page), and is it the
  // watched one? The watch alert ignores g_blinkForFlight (route-color only).
  bool liveFlight = (g_blinkForFlight && g_screen == SCR_DASH && !g_suppressFlight &&
                     flightOverhead());
  bool watchActive = g_screen == SCR_DASH && !g_suppressFlight &&
                     flightOverhead() && isWatchedCallsign(planes[0].callsign);

  // A non-watch route blink that just finished is the color to hold solid while
  // the live flight remains on the dashboard.
  if (blinked != BLINK_NONE && blinked != BLINK_BLUE && blinked != BLINK_WHITE) {
    g_routeHoldColor = blinked;
  }

  // LED + speaker: the watch alert and route-hold color override the dashboard
  // status; all inside the alarm gate so a firing alarm keeps them.
  updateWatchNotify(now, watchActive, planeCount > 0 ? planes[0].icao24 : "");
  bool routeActive = liveFlight && !watchActive && g_routeHoldColor != BLINK_NONE;
  if (watchActive) {
    updateRouteLed(false);
  } else if (routeActive) {
    updateRouteLed(true);
  } else {
    g_routeHoldColor = BLINK_NONE;
    if (g_screen == SCR_DASH) updateStatusLed();
    else {
      digitalWrite(CYD_LED_RED, HIGH); digitalWrite(CYD_LED_GREEN, HIGH); digitalWrite(CYD_LED_BLUE, HIGH);
    }
  }
  }   // end !alarmLedBusy LED gate

  // Periodic heap/TLS/fetch diagnostic. No-op in release builds.
  static unsigned long lastHeapDiag = 0;
  if (now - lastHeapDiag >= 30000UL) {
    lastHeapDiag = now;
    logHeapDiag("tick");
  }
  delay(20);
}
