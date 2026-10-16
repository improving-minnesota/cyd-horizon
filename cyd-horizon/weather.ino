// weather.ino - NTP clock, Open-Meteo weather, and idle/weather screen.

#include <time.h>

// Light purple for the pool row. TFT_PURPLE (0x780F) is too dark to read on
// black; this is rgb(230,190,255) - bright enough to sit alongside the other
// rows while keeping an unmistakable purple cast.
#define TFT_LAVENDER 0xE5FF

// Weather state (global across tabs)
float g_temp = 0.0f, g_humidity = 0.0f, g_feels = 0.0f;
char  g_sunrise[6] = "--:--", g_sunset[6] = "--:--";
// 8 days: the 7-day grid leaves the 4th cell of its second row empty, so day 8
// fills it (see drawIdle()).
float g_high[8] = {0}, g_low[8] = {0};
int   g_rain[8] = {0}, g_wcode[8] = {0};
int   g_wcode_cur = 0;     // current WMO weather code (for the idle icon)
bool  g_weatherValid = false;
// Wind at 10 m, stored in knots (the API is asked for knots) so it shares
// spdDisp()/spdUnit() with flight speeds. g_windDir is the direction the wind
// comes FROM (meteorological convention); g_windValid is false until a fetch
// carries the field, so the idle rows can show "--" instead of 0.
float g_windKt = 0.0f, g_gustKt = 0.0f, g_windDir = 0.0f;
bool  g_windValid = false;

extern unsigned long g_lastWeather;

// NTP / clock helpers
void setupNTP() {
  // US Central (UTC-6, DST +1)
  configTime(-6 * 3600, 3600, "pool.ntp.org", "time.nist.gov");
  // After this call getLocalTime() applies the local offset; before it the
  // clock reads UTC, which can land inside the sleep window post-OTA.
  g_timeReady = true;
}

String fmtClock() {
  struct tm t;
  // Explicit 0ms timeout - called every ~1s from the main loop, and
  // getLocalTime()'s default 5s timeout would block touch + drawing.
  if (!getLocalTime(&t, 0)) return "--:--";
  char b[12];
  strftime(b, sizeof b, g_clock24 ? "%H:%M" : "%I:%M", &t);
  return String(b);
}

String fmtDate() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return "";
  char b[22];
  strftime(b, sizeof b, "%a %b %d", &t);
  return String(b);
}

// True when `hm` holds a real "HH:MM". The unpopulated placeholder is "--:--",
// which atoi() silently reads as 00:00 - that printed as "12:00A" and left
// isDayNow() stuck in night.
static bool isHm(const char* hm) {
  return hm && hm[0] >= '0' && hm[0] <= '9' && hm[1] >= '0' && hm[1] <= '9' &&
         hm[3] >= '0' && hm[3] <= '9' && hm[4] >= '0' && hm[4] <= '9';
}

// "HH:MM" (24h) for display: 12-hour gets a one-letter marker ("7:47A"),
// 24-hour stays as-is. Used for sunrise/sunset.
String fmtHm12(const char* hm) {
  if (!isHm(hm)) return "--";
  int h = atoi(hm);
  int m = atoi(hm + 3);
  char b[8];
  if (g_clock24) {
    snprintf(b, sizeof b, "%d:%02d", h, m);
  } else {
    int h12 = h % 12; if (h12 == 0) h12 = 12;
    snprintf(b, sizeof b, "%d:%02d%c", h12, m, (h < 12) ? 'A' : 'P');
  }
  return String(b);
}

// current day/night based on local time vs sunrise/sunset
bool isDayNow() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return true;
  if (!isHm(g_sunrise) || !isHm(g_sunset)) return true;   // sun times not in yet
  int now = t.tm_hour * 60 + t.tm_min;
  int sr = atoi(g_sunrise) * 60 + atoi(g_sunrise + 3);
  int ss = atoi(g_sunset)  * 60 + atoi(g_sunset + 3);
  return now >= sr && now < ss;
}

// weekday name for today + offset
const char* wdName(int offset) {
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    static const char* nms[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return nms[offset % 7];
  }
  int w = (t.tm_wday + offset) % 7;
  static const char* nms[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  return nms[w];
}

// Open-Meteo 429 handling: free tier limits per client IP (600/min, 5000/hr,
// 10000/day, 1 concurrent) shared across a NAT. Each 429's "reason" names the
// bucket; all reset on their UTC boundary. Jitter avoids lockstep retries.
static void noteWxRateLimit(HTTPClient& http) {
  long nowSec = (long)time(nullptr);
  if (nowSec < 1600000000L) nowSec = (long)(millis() / 1000UL);  // pre-NTP fallback
  String reason = http.getString();   // small {"error":true,"reason":"..."} body
  unsigned long until;
  bool retrySoon = false;
  if (reason.indexOf("Minutely") >= 0) {
    until = (unsigned long)(nowSec - nowSec % 60 + 60) + esp_random() % 15;
    retrySoon = true;
  } else if (reason.indexOf("Hourly") >= 0) {
    until = (unsigned long)(nowSec - nowSec % 3600 + 3600) + esp_random() % 120;
  } else if (reason.indexOf("Daily") >= 0) {
    until = (unsigned long)(nowSec - nowSec % 86400 + 86400) + esp_random() % 600;
  } else if (reason.indexOf("concurrent") >= 0) {
    // Transient queue overflow - the slot frees in seconds, so retry well
    // inside the normal cadence.
    until = (unsigned long)nowSec + 20 + esp_random() % 25;
    retrySoon = true;
  } else {
    // Unknown reason: exponential backoff (30s, 1m, 2m, ... capped at 15m);
    // a persistent streak is treated like the daily cap.
    unsigned long backoff = min(30UL << min(g_wx429Streak, 9), 900UL);
    until = (unsigned long)nowSec + backoff + esp_random() % 30;
    retrySoon = backoff < WEATHER_REFRESH_MS / 1000UL;
    if (g_wx429Streak >= 5) {
      until = (unsigned long)(nowSec - nowSec % 86400 + 86400) + esp_random() % 600;
      retrySoon = false;
    }
  }
  g_wx429Streak++;
  g_wxNextEpoch = until;
  // +1s cushion so the retry can't land a rounding-second inside the window.
  if (retrySoon) g_wxRetryAt = millis() + (until - (unsigned long)nowSec) * 1000UL + 1000UL;
  prefs.begin("flight", false); prefs.putULong("wxrl", until); prefs.end();
  if (isDevBuild()) {
    Serial.printf("[net] meteo 429 streak=%d until epoch %lu reason=%s\n",
                  g_wx429Streak, until, reason.c_str());
  }
}

// Open-Meteo weather fetch (free, no API key)
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  // Inside a stored 429 window: keep the last data instead of churning a
  // guaranteed failure, and arm a retry for when the window lifts (the 10-min
  // cadence outlives a touch-wake). Unsynced clock: re-ask; SNTP self-corrects.
  if (g_wxNextEpoch && (unsigned long)time(nullptr) < g_wxNextEpoch) {
    if (!g_wxRetryAt && time(nullptr) >= 1600000000L) {
      unsigned long dtSec = g_wxNextEpoch - (unsigned long)time(nullptr);
      g_wxRetryAt = millis() + min(dtSec, 172800UL) * 1000UL + 1000UL;
      if (isDevBuild()) {
        Serial.printf("[net] meteo suppressed until epoch %lu, retry armed\n",
                      g_wxNextEpoch);
      }
    }
    return;
  }

  // 448: the request is ~382 chars locally but up to 384 with a full-length
  // westward longitude, so the old 320 buffer no longer fits - snprintf would
  // silently truncate the URL and the fetch would fail.
  char url[448];
  snprintf(url, sizeof url,
    "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
    "wind_speed_10m,wind_direction_10m,wind_gusts_10m"
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code,sunrise,sunset"
    "&forecast_days=8&temperature_unit=fahrenheit&wind_speed_unit=kn&timezone=auto",
    g_lat, g_lon);

  // open-meteo is Let's Encrypt (ISRG root), same verified bundle as OpenSky.
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  int code = httpsRequestRetry(http, sec, url, HTTPS_METHOD_GET, "", nullptr);
  if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
    noteWxRateLimit(http);
    g_weatherDataFailed = true;
    http.end();
    return;
  }
  if (code != HTTP_CODE_OK) {
    g_weatherDataFailed = true;
    http.end();
    return;
  }
  BoundedAllocator weatherAlloc(8192);
  JsonDocument doc(&weatherAlloc);
  HttpBodyStream body(http);
  DeserializationError parseErr = deserializeJson(doc, body);
  bool bodyComplete = body.complete() || body.drain();
  http.end();
  if (parseErr || !bodyComplete) {
    g_weatherDataFailed = true;
    return;
  }

  JsonObject cur = doc["current"];
  g_temp = cur["temperature_2m"] | 0.0f;
  g_humidity = cur["relative_humidity_2m"] | 0.0f;
  g_feels = cur["apparent_temperature"] | g_temp;   // falls back to actual temp
  g_wcode_cur = cur["weather_code"] | g_wcode_cur;  // keep last known on absence
  // -1 sentinel: a body without the wind fields (or a partial one) leaves the
  // rows showing "--" rather than a bogus 0. In the "current" block
  // wind_speed_10m is an instantaneous value while wind_gusts_10m is the
  // preceding hour's maximum, so "Gusts" reads as the worst recent gust.
  g_windKt  = cur["wind_speed_10m"] | -1.0f;
  g_gustKt  = cur["wind_gusts_10m"] | -1.0f;
  g_windDir = cur["wind_direction_10m"] | 0.0f;
  g_windValid = (g_windKt >= 0.0f);
  if (isDevBuild()) {
    Serial.printf("[net] wx wind=%.0fkt dir=%.0f gust=%.0fkt valid=%d url=%uch\n",
                  g_windKt, g_windDir, g_gustKt, (int)g_windValid, (unsigned)strlen(url));
  }

  JsonObject daily = doc["daily"];
  JsonArray hmax = daily["temperature_2m_max"];
  JsonArray hmin = daily["temperature_2m_min"];
  JsonArray rain = daily["precipitation_probability_max"];
  JsonArray wc   = daily["weather_code"];
  JsonArray sr   = daily["sunrise"];
  JsonArray ss   = daily["sunset"];
  for (int i = 0; i < 8; i++) {
    g_high[i]   = hmax[i] | 0.0f;
    g_low[i]    = hmin[i] | 0.0f;
    g_rain[i]   = rain[i] | 0;
    g_wcode[i]  = wc[i] | 0;
  }
  // ISO "2026-09-01T06:42" -> copy "06:42"
  if (sr && !sr[0].isNull()) {
    const char* s = sr[0];
    if (strlen(s) >= 16) { strncpy(g_sunrise, s + 11, 5); g_sunrise[5] = 0; }
  }
  if (ss && !ss[0].isNull()) {
    const char* s = ss[0];
    if (strlen(s) >= 16) { strncpy(g_sunset, s + 11, 5); g_sunset[5] = 0; }
  }
  g_weatherValid = true;
  g_weatherDataFailed = false;
  g_lastWeather = millis();
  g_wx429Streak = 0;
  g_wxRetryAt = 0;   // a success from any path cancels a pending armed retry
  if (g_wxNextEpoch) {
    g_wxNextEpoch = 0;
    prefs.begin("flight", false); prefs.putULong("wxrl", 0); prefs.end();
  }

  // record in the RAM ring buffer and (if available) persistent flash log
  weatherLog(g_temp, (unsigned long)time(nullptr));
}

// Simple 24x24 weather icon; day=true for sun, false for moon.
void drawWeatherIcon(int x, int y, int code, bool day) {
  bool rain  = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  bool snow  = (code >= 71 && code <= 77) || (code >= 85 && code <= 86);
  bool storm = (code >= 95);
  bool cloud = (code >= 1);

  // sun or moon background
  if (day) {
    tft.fillCircle(x + 12, y + 12, 5, TFT_YELLOW);
    for (int a = 0; a < 8; a++) {
      float ang = a * PI / 4.0f;
      int x1 = x + 12 + (int)(6 * cos(ang));
      int y1 = y + 12 + (int)(6 * sin(ang));
      int x2 = x + 12 + (int)(10 * cos(ang));
      int y2 = y + 12 + (int)(10 * sin(ang));
      tft.drawLine(x1, y1, x2, y2, TFT_YELLOW);
    }
  } else {
    tft.fillCircle(x + 12, y + 12, 5, TFT_LIGHTGREY);
    tft.fillCircle(x + 15, y + 12, 5, TFT_BLACK); // crescent cutout
  }

  // cloud overlay
  if (cloud) {
    tft.fillCircle(x + 8,  y + 16, 4, TFT_DARKGREY);
    tft.fillCircle(x + 14, y + 13, 5, TFT_DARKGREY);
    tft.fillCircle(x + 18, y + 16, 4, TFT_DARKGREY);
  }

  if (rain) {
    tft.drawLine(x + 8,  y + 21, x + 7,  y + 23, TFT_CYAN);
    tft.drawLine(x + 14, y + 21, x + 13, y + 23, TFT_CYAN);
    tft.drawLine(x + 20, y + 21, x + 19, y + 23, TFT_CYAN);
  } else if (snow) {
    tft.fillCircle(x + 8,  y + 22, 1, TFT_WHITE);
    tft.fillCircle(x + 14, y + 22, 1, TFT_WHITE);
    tft.fillCircle(x + 20, y + 22, 1, TFT_WHITE);
  } else if (storm) {
    tft.drawLine(x + 12, y + 20, x + 8, y + 24, TFT_YELLOW);
  }
}

// ---- Wind display ----
// Open-Meteo reports the direction the wind comes FROM (the meteorological
// convention, same as "an NE wind"). The arrow points downwind instead - the
// direction the air is travelling toward - which is what a glance expects; the
// letter keeps the "from" reading unambiguous.
const char* windDir8(float fromDeg) {
  static const char* d[8] = {"N","NE","E","SE","S","SW","W","NW"};
  return d[((int)lroundf(fromDeg / 45.0f)) & 7];
}

// Wind tiers, knots (mph in comments). Red is the NWS Wind Advisory criteria
// (sustained 31 mph / gusts 46 mph); the lower tiers are backyard-tuned, since
// an advisory-level warning arrives later than a patio owner wants to know.
const float WIND_YEL_KT = 13.0f, WIND_ORG_KT = 22.0f, WIND_RED_KT = 27.0f;  // 15 / 25 / 31 mph
const float GUST_YEL_KT = 17.0f, GUST_ORG_KT = 30.0f, GUST_RED_KT = 40.0f;  // 20 / 35 / 46 mph

uint16_t windCol(float kt, bool gust) {
  float y = gust ? GUST_YEL_KT : WIND_YEL_KT;
  float o = gust ? GUST_ORG_KT : WIND_ORG_KT;
  float r = gust ? GUST_RED_KT : WIND_RED_KT;
  return kt >= r ? TFT_RED : (kt >= o ? TFT_ORANGE : (kt >= y ? TFT_YELLOW : TFT_GREEN));
}

// Arrowhead + shaft pointing along the downwind bearing. `r` is the half-length
// (the arrow spans 2r) and the head is sized from it.
void drawWindArrow(int cx, int cy, int r, float fromDeg, uint16_t col) {
  float a = (fromDeg + 180.0f) * DEG_TO_RAD;
  float dx = sinf(a), dy = -cosf(a);   // screen unit vector (y grows downward)
  float px = -dy, py = dx;             // perpendicular
  int tipX  = cx + (int)lroundf(r * dx),        tipY  = cy + (int)lroundf(r * dy);
  int tailX = cx - (int)lroundf(r * dx),        tailY = cy - (int)lroundf(r * dy);
  float hl = r * 0.8f, hw = r * 0.45f;          // head length / half-width
  int baseX = tipX - (int)lroundf(hl * dx),     baseY = tipY - (int)lroundf(hl * dy);
  tft.drawLine(tailX, tailY, baseX, baseY, col);
  tft.fillTriangle(tipX, tipY,
                   baseX + (int)lroundf(hw * px), baseY + (int)lroundf(hw * py),
                   baseX - (int)lroundf(hw * px), baseY - (int)lroundf(hw * py), col);
}

// Idle screen shown when no plane is overhead.
// Draws below the header (y >= 30), leaving the right 20 px for the countdown bar.
void drawIdle() {
  bool day = isDayNow();

  // current conditions (left column). Content starts below the taller header.
  // Before the first fetch every reading is still 0, which reads as real data
  // (and the icon would be a default sun), so show "--" until one lands - in
  // grey, so a placeholder never wears the reading's own color.
  const int top = 42;
  tft.setTextColor(g_weatherValid ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(4);
  tft.setCursor(8, top);
  if (g_weatherValid) tft.printf("%d%c", (int)round(tempDisp(g_temp)), tempUnit());
  else tft.print("--");
  tft.setTextSize(1);
  // Current conditions icon, to the right of the big temperature (not below
  // the sunset). Refreshed each weather update. Held back until the first
  // fetch: a default "clear" icon would claim data we don't have yet.
  if (g_weatherValid) drawWeatherIcon(tft.getCursorX() + 6, top + 4, g_wcode_cur, isDayNow());
  // "Feels like" temperature (apparent_temperature), compact label "FL".
  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(8, top + 30);
  if (g_weatherValid) tft.printf("FL %d%c", (int)round(tempDisp(g_feels)), tempUnit());
  else tft.print("FL --");
  tft.setTextColor(g_weatherValid ? TFT_CYAN : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(8, top + 50);
  if (g_weatherValid) tft.printf("%d%% hum", (int)g_humidity);
  else tft.print("-- hum");

  tft.setTextColor(g_weatherValid ? TFT_YELLOW : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(8, top + 72);  tft.print("Sunr "); tft.print(fmtHm12(g_sunrise));
  tft.setTextColor(g_weatherValid ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(8, top + 90); tft.print("Suns "); tft.print(fmtHm12(g_sunset));

  // Wind + gusts. Fixed rows between sunset and Pool so their position doesn't
  // depend on the pool toggle. The arrow gets its own line, after the "Wind"
  // label: rotated inside a text row the arrow is either squeezed small or
  // reaches into the text (a rigid arrow is as wide as it is long once the wind
  // turns E/W), and labelling that line instead keeps the value rows at full
  // column width, which a 3-digit gust needs. Each reading takes its own tier
  // color, so a calm day with strong gusts reads green over yellow.
  tft.setTextFont(2);
  uint16_t wcol = g_windValid ? windCol(g_windKt, false) : TFT_LIGHTGREY;
  tft.setTextColor(wcol, TFT_BLACK);
  tft.setCursor(8, top + 108); tft.print("Wind");
  if (g_windValid) drawWindArrow(50, top + 116, 8, g_windDir, wcol);
  if (g_windValid) {
    tft.setCursor(8, top + 126);
    tft.printf("%s %d%s", windDir8(g_windDir), (int)round(spdDisp(g_windKt)), spdUnit());
    tft.setTextColor(windCol(g_gustKt, true), TFT_BLACK);
    tft.setCursor(8, top + 144);
    tft.printf("Gusts %d%s", (int)round(spdDisp(g_gustKt)), spdUnit());
  } else {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, top + 126); tft.print("--");
    tft.setCursor(8, top + 144); tft.print("Gusts --");
  }

  // Pool temp (Govee; only when enabled). Tapping opens the pool history
  // graph; same one-line layout as Sunr/Suns, below the wind rows. Lavender so
  // it can't be read as one of the wind tiers, and the whole row goes grey when
  // there is no reading, like every other placeholder.
#if POOL_FEATURE
  if (g_poolEnabled) {
    tft.setTextFont(2);
    tft.setTextColor(g_poolValid ? TFT_LAVENDER : TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, top + 162); tft.print("Pool ");
    if (g_poolValid) tft.printf("%.1f%c", poolDisp(g_poolTemp), tempUnit());
    else tft.print("--");
  }
#endif

  // 8-day forecast (right/bottom); columns spread into any extra panel
  // width but stay clear of the countdown-bar strip on the right edge.
  const int x0 = 110, y0 = top + 2, h = 92;
  const int w = (DISP_W - x0 - 16) / 4;   // 48 on the 2.8", 58 on the 4"
  for (int i = 0; i < 8; i++) {
    int col = i % 4;
    int row = i / 4;
    int x = x0 + col * w;
    int y = y0 + row * h;

    // day name (FONT_AUX: F2 on the 4" where 8px reads small, F1 on the 2.8")
    tft.setTextFont(FONT_AUX);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x, y);
    tft.print(wdName(i));

    // No fetch yet: 0/0/0% would read as real data and the icon would be a
    // default sun, so show "--" and skip this cell's values.
    if (!g_weatherValid) {
      tft.setTextFont(FONT_AUX);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setCursor(x, y + 44);
      tft.print("--");
      continue;
    }

    // icon
    drawWeatherIcon(x + 4, y + 16, g_wcode[i], i == 0 ? day : true);

    // high/low
    tft.setTextFont(FONT_AUX);
    tft.setTextColor(TFT_PINK, TFT_BLACK);
    tft.setCursor(x, y + 44);
    tft.printf("%d", (int)round(tempDisp(g_high[i])));
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(x + 28, y + 44);
    tft.printf("%d", (int)round(tempDisp(g_low[i])));

    // rain probability centered under the temps block (the widest element -
    // on the 4" layout the cell is wider than its left-anchored content)
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    char rbuf[8]; snprintf(rbuf, sizeof rbuf, "%d%%", g_rain[i]);
    char lobuf[8]; snprintf(lobuf, sizeof lobuf, "%d", (int)round(tempDisp(g_low[i])));
    int blockC = x + (28 + tft.textWidth(lobuf, FONT_AUX)) / 2;
    tft.drawCentreString(rbuf, blockC, y + 62, FONT_AUX);
  }

  // status line: a pending alarm snooze counts down here instead of the
  // aircraft count (flight polls are suppressed during a snooze = stale count)
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(FONT_AUX);
  tft.setCursor(8, 224);
  char sb[40];
  // lastErr is only ever set by the flights fetch, so with tracking off it
  // would sit on the initial "connecting..." forever - leave the line blank.
  if (snoozeStatusText(sb, sizeof sb)) tft.print(sb);
  else if (g_trackEnabled) tft.print(lastErr);
}

// ---- Weather temp history graph ----
// Mirrors the pool graph (pool.ino) for the Open-Meteo temperature logged to
// flash every ~10 min; opened by tapping the weather temp on the idle screen.

// Window (seconds) for the current weather graph timeframe
unsigned long wxWindowSec() {
  switch (g_wxTF) {
    case WX_DAY:   return 86400UL;
    case WX_MONTH: return 2592000UL;
    case WX_YEAR:  return 31536000UL;
    default:       return 604800UL;  // WX_WEEK
  }
}

// Weather twin of poolSeriesForTF (raw/hourly/daily tiers + pend).
void wxSeriesForTF(unsigned long** times, float** temps,
                   float** mins, float** maxs, int* count,
                   PendingRollup* pend) {
  pend->t = 0;
  switch (g_wxTF) {
    case WX_MONTH:
      *times = g_wxHourTime; *temps = g_wxHourTemp;
      *mins = g_wxHourMin; *maxs = g_wxHourMax;
      *count = g_wxHourCount;
      if (g_wxHourN > 0) {
        pend->t = (unsigned long)g_wxHourBucket * 3600UL;
        pend->avg = g_wxHourSum / g_wxHourN;
        pend->lo = g_wxCurHourMin; pend->hi = g_wxCurHourMax;
      }
      break;
    case WX_YEAR:
      *times = g_wxDayTime; *temps = g_wxDayTemp;
      *mins = g_wxDayMin; *maxs = g_wxDayMax;
      *count = g_wxDayCount;
      if (g_wxDayN > 0) {
        pend->t = (unsigned long)g_wxDayBucket * 86400UL;
        pend->avg = g_wxDaySum / g_wxDayN;
        pend->lo = g_wxCurDayMin; pend->hi = g_wxCurDayMax;
      }
      break;
    default:
      *times = g_wxLogTime; *temps = g_wxLogTemp;
      *mins = nullptr; *maxs = nullptr;
      *count = g_wxLogCount; break;
  }
}

// Weather series wrapper; the generic plotter is in pool.ino.
bool plotWeatherSeries(unsigned long* times, float* temps, float* mins, float* maxs,
                       int count, const PendingRollup* pend, float exLo, float exHi,
                       unsigned long t0, unsigned long nowSec, unsigned long win,
                       int gx, int gy, int gw, int gh,
                       float& dataMin, float& dataMax) {
  return plotSeries(times, temps, mins, maxs, count, pend, exLo, exHi,
                    t0, nowSec, win, gx, gy, gw, gh,
                    dataMin, dataMax, tempDisp);
}

// Weather temp history graph. Plots the samples we have logged for the
// selected timeframe. Auto-dismisses after 30s; any touch keeps it alive.
void drawWxGraph() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("History > Weather Temperature");
  backBtn("Back");

  // timeframe selector
  const char* labels[4] = {"Day", "Week", "Month", "Year"};
  int bx = 8;
  for (int i = 0; i < 4; i++) {
    bool sel = i == g_wxTF;
    uint16_t col = sel ? btnCol() : disabledCol();
    if (sel) themeBtn(bx, 34, 70, 22, 5);
    else tft.fillRoundRect(bx, 34, 70, 22, 5, col);
    tft.setTextColor(btnFg(col), col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(labels[i], bx + 35, 45, FONT_AUX);
    tft.setTextDatum(TL_DATUM);
    bx += 76 + (DISP_W - 320) / 4;
  }

  // graph area
  int gx = 10, gy = 66, gw = DISP_W - 20, gh = 140;
  uint16_t gbg = graphBgCol();
  tft.fillRect(gx, gy, gw, gh, gbg);
  tft.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, btnFg(gbg));

  unsigned long nowSec = (unsigned long)time(nullptr);

  // Time not synced yet (right after a boot/deep-sleep wake). Show an explicit
  // "waiting" message instead of "No data" (see drawPoolGraph()).
  if (nowSec < 1600000000UL) {
    tft.setTextColor(btnFg(gbg), gbg);
    tft.setTextFont(1);
    tft.setCursor(gx + 20, gy + gh / 2);
    tft.print("Waiting for time sync...");
    return;
  }

  unsigned long win = wxWindowSec();
  unsigned long t0 = (nowSec > win) ? (nowSec - win) : 0;

  unsigned long* times; float* temps; float* mins; float* maxs; int count;
  PendingRollup pend;
  wxSeriesForTF(&times, &temps, &mins, &maxs, &count, &pend);

  // Same widening as drawPoolGraph: Month <- raw; Year <- hourly + raw.
  float exLo = 1e9f, exHi = -1e9f;
  if (g_wxTF == WX_MONTH || g_wxTF == WX_YEAR)
    widenRange(g_wxLogTime, g_wxLogTemp, nullptr, nullptr,
               g_wxLogCount, t0, nowSec, exLo, exHi);
  if (g_wxTF == WX_YEAR)
    widenRange(g_wxHourTime, g_wxHourTemp, g_wxHourMin, g_wxHourMax,
               g_wxHourCount, t0, nowSec, exLo, exHi);

  float lo, hi;
  bool plotted = plotWeatherSeries(times, temps, mins, maxs, count, &pend, exLo, exHi, t0, nowSec, win, gx, gy, gw, gh, lo, hi);

  if (!plotted) {
    tft.setTextColor(btnFg(gbg), gbg);
    tft.setTextFont(1);
    tft.setCursor(gx + 20, gy + gh / 2);
    tft.print("No data in this period yet");
  } else {
    // Bottom strip: data low (left), current temp (center), data high (right)
    // for the timeframe; sits on the black screen, not the graph fill.
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(gx, gy + gh + 6);
    tft.printf("Lo %.1f", lo);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(gx + 110, gy + gh + 6);
    tft.printf("now %.1f%c", tempDisp(g_temp), tempUnit());
    char hbuf[16];
    snprintf(hbuf, sizeof hbuf, "Hi %.1f", hi);
    tft.drawRightString(hbuf, gx + gw, gy + gh + 6, 2);
  }
}

void handleWxGraphTouch(uint16_t x, uint16_t y) {
  // any touch keeps the screen alive for another 2 minutes
  g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;

  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_DASH; dirty = true; return; }  // close
  int bx = 8;
  for (int i = 0; i < 4; i++) {
    if (inRect(x, y, bx, 34, bx + 70, 56)) {
      if (g_wxTF != i) { g_wxTF = i; dirty = true; }
      return;
    }
    bx += 76 + (DISP_W - 320) / 4;
  }
  dirty = true;
}
