// pool.ino - Pool Temp settings screen, history graph, and Govee Open API
// integration. Thermometers are detected by their "sensorTemperature"
// capability; any unavailable reading shows "--". History globals are declared
// in cyd-horizon.ino so they're visible everywhere.

// ---- Govee Open API: Pool Temp integration ----

// Govee rate limits: per-day X-RateLimit-* (10000/day per key) and per-minute
// API-RateLimit-* plus Retry-After on 429; *-Reset is a UTC epoch. Headers must
// be collectHeaders()'d - HTTPClient discards everything else.
static const char* kGoveeRlKeys[] = {
  "X-RateLimit-Remaining", "X-RateLimit-Reset",
  "API-RateLimit-Remaining", "API-RateLimit-Reset", "Retry-After"
};
static const int kGoveeRlKeyCount = sizeof(kGoveeRlKeys) / sizeof(kGoveeRlKeys[0]);

// Park all Govee requests until the server's reset deadline; jitter
// desynchronizes boards sharing one API key (a shared 429 hands every board
// the same reset epoch, so they'd resume in one burst without it).
static void noteGoveeRateLimit(HTTPClient& http) {
  long nowSec = (long)time(nullptr);
  if (nowSec < 1600000000L) nowSec = (long)(millis() / 1000UL);  // pre-NTP fallback
  unsigned long until = (unsigned long)nowSec + 300UL;   // headerless fallback
  String day = http.header("X-RateLimit-Reset");
  if (day.length()) until = max(until, (unsigned long)day.toInt() + esp_random() % 300UL);
  String perMin = http.header("API-RateLimit-Reset");
  if (perMin.length()) until = max(until, (unsigned long)perMin.toInt() + esp_random() % 30UL);
  String retry = http.header("Retry-After");
  if (retry.length()) until = max(until, (unsigned long)(nowSec + retry.toInt() + (long)(esp_random() % 15UL)));
  g_goveeRateReset = until;
  prefs.begin("flight", false); prefs.putULong("goveerl", until); prefs.end();
  if (isDevBuild()) Serial.printf("[net] govee rate limited until epoch %lu\n", until);
}

// True while inside a stored Govee rate-limit window; arms g_poolRetryAt so a
// suppressed fetch retries at the deadline (the 5-min cadence outlives a
// touch-wake). With an unsynced clock it just re-asks in 60s.
bool goveeRateLimited() {
  if (!g_goveeRateReset || (unsigned long)time(nullptr) >= g_goveeRateReset) return false;
  if (!g_poolRetryAt) {
    unsigned long nowSec = (unsigned long)time(nullptr);
    g_poolRetryAt = millis() + 1000UL
        + (nowSec >= 1600000000UL ? min(g_goveeRateReset - nowSec, 172800UL) * 1000UL
                                  : 60000UL);
    if (isDevBuild()) {
      Serial.printf("[net] govee suppressed until epoch %lu, retry armed\n",
                    g_goveeRateReset);
    }
  }
  return true;
}

// A successful response lifts the deadline (persisted too, so the next
// deep-sleep wake doesn't think it's still limited).
static void clearGoveeRateLimit() {
  if (!g_goveeRateReset) return;
  g_goveeRateReset = 0;
  g_poolRetryAt = 0;   // window lifted early -> no pending retry needed
  prefs.begin("flight", false); prefs.putULong("goveerl", 0); prefs.end();
}

// A 200 reporting an exhausted bucket parks further requests preemptively so
// we don't burn the extra 429 to learn the same deadline.
static void checkGoveeRemaining(HTTPClient& http) {
  String dayRem = http.header("X-RateLimit-Remaining");
  String minRem = http.header("API-RateLimit-Remaining");
  if ((dayRem.length() && dayRem.toInt() <= 0) || (minRem.length() && minRem.toInt() <= 0)) {
    noteGoveeRateLimit(http);
  }
}

// Select the currently-highlighted thermometer and persist it.
void selectGoveeDevice() {
  if (g_goveeCount == 0) { g_poolValid = false; return; }
  GoveeDev &g = g_goveeDevs[g_goveeSel];
  if (g_poolDeviceId != g.id) {   // device actually changed
    g_poolValid = false;          // don't show the previous device's temp
    g_poolTemp = 0;
  }
  g_poolDeviceId = g.id;
  g_poolModel = g.model;
  g_poolName = g.name;
  prefs.begin("flight", false);
  prefs.putString("poolid", g_poolDeviceId);
  prefs.putString("poolmodel", g_poolModel);
  prefs.putString("poolname", g_poolName);
  prefs.end();
  if (g_poolEnabled) netWantPool = true;   // fetch the newly-selected device's temp async
}

// Fetch the account's thermometers into g_goveeDevs. Response: data[] of
// {sku, device, deviceName, type, capabilities}; thermometers have type
// "devices.types.thermometer" / a "sensorTemperature" capability.
bool fetchGoveeDevices() {
  if (WiFi.status() != WL_CONNECTED || g_goveeKey.length() == 0 || goveeRateLimited()) { g_goveeAuthBad = false; return false; }
  // Govee's Open API is Amazon-signed (Amazon Root CA 1 is in the global store).
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  http.collectHeaders(kGoveeRlKeys, kGoveeRlKeyCount);
  const char* devHdrs[] = { "Govee-API-Key", g_goveeKey.c_str(), nullptr };
  int code = httpsRequestRetry(http, sec, "https://openapi.api.govee.com/router/api/v1/user/devices",
                               HTTPS_METHOD_GET, "", devHdrs);
  g_goveeAuthBad = (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_FORBIDDEN);
  if (code == HTTP_CODE_TOO_MANY_REQUESTS) { noteGoveeRateLimit(http); http.end(); return false; }
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  clearGoveeRateLimit();
  checkGoveeRemaining(http);
  BoundedAllocator devicesAlloc(16384);
  JsonDocument doc(&devicesAlloc);
  HttpBodyStream body(http);
  DeserializationError parseErr = deserializeJson(doc, body);
  bool bodyComplete = body.complete() || body.drain();
  http.end();
  if (parseErr || !bodyComplete) return false;
  g_goveeCount = 0;
  JsonArray devs = doc["data"];   // array of devices
  for (JsonObject d : devs) {
    if (g_goveeCount >= MAX_GOVEE) break;
    bool isThermo = (String(d["type"] | "").indexOf("thermometer") >= 0);
    JsonArray caps = d["capabilities"];
    for (JsonObject c : caps) {
      if (String(c["instance"] | "") == "sensorTemperature") isThermo = true;
    }
    if (!isThermo) continue;
    GoveeDev &g = g_goveeDevs[g_goveeCount];
    String id = d["device"] | "", sku = d["sku"] | "", name = d["deviceName"] | "";
    strncpy(g.id, id.c_str(), 63); g.id[63] = 0;
    strncpy(g.model, sku.c_str(), 15); g.model[15] = 0;
    strncpy(g.name, name.c_str(), 39); g.name[39] = 0;
    g_goveeCount++;
  }
  g_goveeSel = 0;
  selectGoveeDevice();
  return g_goveeCount > 0;
}

// Fetch the selected device's temperature. POST /device/state with
// {requestId, payload:{sku, device}}; temp is in payload.capabilities[] where
// instance=="sensorTemperature".
bool fetchGoveeTemp() {
  if (!g_poolEnabled || WiFi.status() != WL_CONNECTED || g_goveeKey.length() == 0
      || g_poolDeviceId.length() == 0) {
    g_poolValid = false;
    g_goveeAuthBad = false;
    return false;
  }
  // Inside a Govee rate-limit window the server already told us to wait - keep
  // the last reading instead of burning a guaranteed-failure request.
  if (goveeRateLimited()) return false;
  String requestBody = "{\"requestId\":\"pool-1\",\"payload\":{\"sku\":\"" + g_poolModel
                       + "\",\"device\":\"" + g_poolDeviceId + "\"}}";
  // Verified TLS via the global trust store (Amazon Root CA 1).
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  http.collectHeaders(kGoveeRlKeys, kGoveeRlKeyCount);
  const char* tempHdrs[] = { "Govee-API-Key", g_goveeKey.c_str(), "Content-Type", "application/json", nullptr };
  int code = httpsRequestRetry(http, sec, "https://openapi.api.govee.com/router/api/v1/device/state",
                               HTTPS_METHOD_POST, requestBody, tempHdrs);
  g_goveeAuthBad = (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_FORBIDDEN);
  if (code == HTTP_CODE_TOO_MANY_REQUESTS) { noteGoveeRateLimit(http); g_poolValid = false; http.end(); return false; }
  if (code != HTTP_CODE_OK) { g_poolValid = false; http.end(); return false; }
  clearGoveeRateLimit();
  checkGoveeRemaining(http);
  BoundedAllocator tempAlloc(4096);
  JsonDocument doc(&tempAlloc);
  HttpBodyStream body(http);
  DeserializationError parseErr = deserializeJson(doc, body);
  bool bodyComplete = body.complete() || body.drain();
  http.end();
  if (parseErr || !bodyComplete) { g_poolValid = false; return false; }
  bool found = false;
  JsonArray caps = doc["payload"]["capabilities"];
  for (JsonObject c : caps) {
    if (String(c["instance"] | "") == "sensorTemperature") {
      g_poolTemp = c["state"]["value"] | 0.0f;
      found = true;
      break;
    }
  }
  if (!found) { g_poolValid = false; return false; }
  g_poolValid = true;
  g_lastPool = millis();

  // record in the RAM ring buffer and (if available) persistent flash log
  poolLog(g_poolTemp, (unsigned long)time(nullptr));

  return true;
}

// Pool temps are stored in the unit Govee reported (g_poolUnit, normally F);
// normalize to F before converting to the display unit (see tempDisp()).
float poolDisp(float v) {
  return tempDisp(g_poolUnit == 'C' ? v * 9.0f / 5.0f + 32.0f : v);
}

// ---- Pool Temp settings screen ----
void drawPool() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Pool Temp");

  backBtn("Back");

  // Enabled toggle
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 40);
  tft.print("Enabled");
  tft.setTextColor(g_poolEnabled ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(150, 40);
  tft.print(g_poolEnabled ? "ON" : "OFF");
  themeBtn(RX(230), 36, 82, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(250), 43);
  tft.print("Toggle");

  // Govee API key
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 76);
  tft.print("Govee API Key");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextFont(1);
  tft.setCursor(8, 94);
  tft.print(g_goveeKey.length() ? g_goveeKey.substring(0, 34) : "(none)");
  themeBtn(RX(250), 72, 62, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(256), 79);
  tft.print("Edit");

  // Fetch devices
  themeBtn(10, 116, DISP_W - 20, 28, 6);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(2);
  tft.setCursor(20, 123);
  tft.print("Fetch Devices");

  // selected device + prev/next
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 160);
  tft.print("Device");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(80, 160);
  if (g_goveeCount > 0) tft.print(g_goveeDevs[g_goveeSel].name);
  else if (g_poolName.length() > 0) tft.print(g_poolName);
  else tft.print("none");
  if (g_goveeCount > 1) {
    themeBtn(RX(250), 156, 28, 24, 5);
    tft.setTextColor(btnFg(btnCol()), btnCol()); tft.setTextFont(2);
    tft.setCursor(RX(258), 163); tft.print("<");
    themeBtn(RX(284), 156, 28, 24, 5);
    tft.setCursor(RX(292), 163); tft.print(">");
  }

  // current temperature
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 200);
  tft.print("Current");
  tft.setTextColor(g_poolValid ? TFT_GREEN : TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(4);   // font 3 is unused in TFT_eSPI and renders nothing
  tft.setCursor(90, 196);
  if (g_poolValid) tft.printf("%.1f %c", poolDisp(g_poolTemp), tempUnit());
  else tft.print("--");
  tft.setTextSize(1);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(1);
  tft.setCursor(8, 232);
  tft.print("Key: developer.govee.com. Show -- if unavailable.");
}

void handlePoolTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; dirty = true; return; }
  if (inRect(x, y, RX(230), 36, RX(312), 60)) {  // Enabled toggle
    g_poolEnabled = !g_poolEnabled;
    prefs.begin("flight", false); prefs.putBool("poolen", g_poolEnabled); prefs.end();
    // On: kick an immediate fetch. Off: drop the stale reading so "Current"
    // shows -- (capture is stopped everywhere, so nothing refreshes it).
    if (g_poolEnabled) {
      if (g_poolDeviceId.length() > 0) netWantPool = true;
    } else {
      g_poolValid = false;
    }
    dirty = true;
    return;
  }
  if (inRect(x, y, RX(250), 72, RX(312), 96)) { g_screen = SCR_WIFI; g_wifiSub = 9; dirty = true; return; }  // edit key
  if (inRect(x, y, 10, 116, DISP_W - 10, 144)) { netWantPoolDevices = true; dirty = true; return; }  // fetch devices
  if (g_goveeCount > 1) {
    if (inRect(x, y, RX(250), 156, RX(278), 180)) { g_goveeSel = (g_goveeSel - 1 + g_goveeCount) % g_goveeCount; selectGoveeDevice(); dirty = true; return; }
    if (inRect(x, y, RX(284), 156, RX(312), 180)) { g_goveeSel = (g_goveeSel + 1) % g_goveeCount; selectGoveeDevice(); dirty = true; return; }
  }
}

// ---- Pool Temp history graph ----

// Window (seconds) for the current pool graph timeframe
unsigned long poolWindowSec() {
  switch (g_poolTF) {
    case TF_DAY:   return 86400UL;
    case TF_MONTH: return 2592000UL;
    case TF_YEAR:  return 31536000UL;
    default:       return 604800UL;  // TF_WEEK
  }
}

// Day/Week = raw tier, Month = hourly, Year = daily (each view spans further
// than one raw buffer). mins/maxs = per-bucket lo/hi (nullptr on raw); pend =
// the in-progress bucket (t==0 = none).
void poolSeriesForTF(unsigned long** times, float** temps,
                     float** mins, float** maxs, int* count,
                     PendingRollup* pend) {
  pend->t = 0;
  switch (g_poolTF) {
    case TF_MONTH:
      *times = g_poolHourTime; *temps = g_poolHourTemp;
      *mins = g_poolHourMin; *maxs = g_poolHourMax;
      *count = g_poolHourCount;
      if (g_curHourN > 0) {
        pend->t = (unsigned long)g_curHourBucket * 3600UL;
        pend->avg = g_curHourSum / g_curHourN;
        pend->lo = g_curHourMin; pend->hi = g_curHourMax;
      }
      break;
    case TF_YEAR:
      *times = g_poolDayTime; *temps = g_poolDayTemp;
      *mins = g_poolDayMin; *maxs = g_poolDayMax;
      *count = g_poolDayCount;
      if (g_curDayN > 0) {
        pend->t = (unsigned long)g_curDayBucket * 86400UL;
        pend->avg = g_curDaySum / g_curDayN;
        pend->lo = g_curDayMin; pend->hi = g_curDayMax;
      }
      break;
    default:
      *times = g_poolLogTime; *temps = g_poolLogTemp;
      *mins = nullptr; *maxs = nullptr;
      *count = g_poolLogCount; break;
  }
}

// Widen [lo,hi] with a tier's in-window extremes (stored units). mins/maxs
// null means a raw tier - each sample is its own lo/hi.
void widenRange(const unsigned long* times, const float* temps,
                const float* mins, const float* maxs, int count,
                unsigned long t0, unsigned long nowSec, float& lo, float& hi) {
  bool hasRange = (mins != nullptr && maxs != nullptr);
  for (int i = 0; i < count; i++) {
    if (times[i] < t0 || times[i] > nowSec) continue;
    float l = hasRange ? mins[i] : temps[i];
    float h = hasRange ? maxs[i] : temps[i];
    if (l < lo) lo = l;
    if (h > hi) hi = h;
  }
}

// Plot one series + avg/scale labels; outputs the window's data low/high.
bool plotSeries(unsigned long* times, float* temps, float* mins, float* maxs,
                int count, const PendingRollup* pend, float exLo, float exHi,
                unsigned long t0, unsigned long nowSec, unsigned long win,
                int gx, int gy, int gw, int gh,
                float& dataMin, float& dataMax,
                float (*toDisp)(float)) {
  bool hasRange = (mins != nullptr && maxs != nullptr);
  float vmin = 1e9f, vmax = -1e9f;
  float sum = 0.0f;
  int cnt = 0;
  for (int i = 0; i < count; i++) {
    unsigned long t = times[i];
    if (t < t0 || t > nowSec) continue;
    float v = temps[i];
    float lo = hasRange ? mins[i] : v;
    float hi = hasRange ? maxs[i] : v;
    if (lo < vmin) vmin = lo;
    if (hi > vmax) vmax = hi;
    sum += v;
    cnt++;
  }
  // Only ever widens the range: the flushed buckets' lo/hi stay the basis.
  if (pend && pend->t >= t0 && pend->t <= nowSec) {
    if (pend->lo < vmin) vmin = pend->lo;
    if (pend->hi > vmax) vmax = pend->hi;
    sum += pend->avg;
    cnt++;
  }
  if (exLo <= exHi) {
    if (exLo < vmin) vmin = exLo;
    if (exHi > vmax) vmax = exHi;
  }
  if (cnt == 0) return false;

  dataMin = toDisp(vmin);   // actual data low for this timeframe, display units
  dataMax = toDisp(vmax);   // actual data high for this timeframe, display units
  float avg = sum / cnt;   // average temp for this timeframe

  if (vmax - vmin < 1.0f) { vmin -= 1.0f; vmax += 1.0f; }   // pad only for y-scale
  int prevX = -1, prevY = -1;
  unsigned long prevT = 0;
  for (int i = 0; i < count; i++) {
    unsigned long t = times[i];
    if (t < t0 || t > nowSec) continue;
    float v = temps[i];
    int px = gx + (int)((double)(t - t0) * gw / (double)win);
    int py = gy + gh - (int)((v - vmin) * gh / (vmax - vmin));
    px = constrain(px, gx, gx + gw);
    py = constrain(py, gy, gy + gh);
    // The RAM rings store the newest run first, so index order jumps back in
    // time at the wrap point - break the line there instead of drawing a
    // full-width segment from the newest sample back to the oldest.
    if (prevX >= 0 && t >= prevT) tft.drawLine(prevX, prevY, px, py, graphLineCol());
    prevX = px; prevY = py; prevT = t;
  }

  // Dotted horizontal line at the average temperature, with an "avg" label just
  // above it, near the left edge of the graph.
  int avgY = gy + gh - (int)((avg - vmin) * gh / (vmax - vmin));
  avgY = constrain(avgY, gy, gy + gh);
  for (int x = gx; x <= gx + gw; x += 6) {
    int w = (x + 3 <= gx + gw) ? 3 : (gx + gw - x);
    if (w > 0) tft.drawFastHLine(x, avgY, w, graphAvgCol());
  }
  char abuf[16];
  snprintf(abuf, sizeof abuf, "avg %.1f", toDisp(avg));
  tft.setTextColor(graphAvgCol(), graphBgCol());
  tft.setTextFont(1);
  tft.setCursor(gx + 2, avgY - 8);
  tft.print(abuf);

  // Padded y-axis scale labels (max top-right, min bottom-right), drawn inside
  // the chart corners - no room above the graph (timeframe buttons sit there).
  tft.setTextColor(btnFg(graphBgCol()), graphBgCol());
  char sbuf[16];
  snprintf(sbuf, sizeof sbuf, "%.0f", toDisp(vmax));
  tft.drawRightString(sbuf, gx + gw, gy + 2, 1);        // padded max
  snprintf(sbuf, sizeof sbuf, "%.0f", toDisp(vmin));
  tft.drawRightString(sbuf, gx + gw, gy + gh - 9, 1);   // padded min
  return true;
}

// Pool temp series wrapper. Weather uses plotWeatherSeries below.
bool plotPoolSeries(unsigned long* times, float* temps, float* mins, float* maxs,
                    int count, const PendingRollup* pend, float exLo, float exHi,
                    unsigned long t0, unsigned long nowSec, unsigned long win,
                    int gx, int gy, int gw, int gh,
                    float& dataMin, float& dataMax) {
  return plotSeries(times, temps, mins, maxs, count, pend, exLo, exHi,
                    t0, nowSec, win, gx, gy, gw, gh,
                    dataMin, dataMax, poolDisp);
}

// Pool temp history graph. Plots the samples we have logged for the selected
// timeframe. Auto-dismisses after 30s; any touch keeps it alive.
void drawPoolGraph() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("History > Pool Temperature");
  backBtn("Back");

  // timeframe selector
  const char* labels[4] = {"Day", "Week", "Month", "Year"};
  int bx = 8;
  for (int i = 0; i < 4; i++) {
    bool sel = i == g_poolTF;
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

  // Time not synced yet: the window filter would compare every real sample
  // against nowSec==0 and reject them all, so show "waiting" not "No data".
  if (nowSec < 1600000000UL) {
    tft.setTextColor(btnFg(gbg), gbg);
    tft.setTextFont(1);
    tft.setCursor(gx + 20, gy + gh / 2);
    tft.print("Waiting for time sync...");
    return;
  }

  unsigned long win = poolWindowSec();
  unsigned long t0 = (nowSec > win) ? (nowSec - win) : 0;

  unsigned long* times; float* temps; float* mins; float* maxs; int count;
  PendingRollup pend;
  poolSeriesForTF(&times, &temps, &mins, &maxs, &count, &pend);

  // Finer tiers may hold extremes the plotted tier lacks. Month <- raw;
  // Year <- hourly + raw.
  float exLo = 1e9f, exHi = -1e9f;
  if (g_poolTF == TF_MONTH || g_poolTF == TF_YEAR)
    widenRange(g_poolLogTime, g_poolLogTemp, nullptr, nullptr,
               g_poolLogCount, t0, nowSec, exLo, exHi);
  if (g_poolTF == TF_YEAR)
    widenRange(g_poolHourTime, g_poolHourTemp, g_poolHourMin, g_poolHourMax,
               g_poolHourCount, t0, nowSec, exLo, exHi);

  float lo, hi;
  bool plotted = plotPoolSeries(times, temps, mins, maxs, count, &pend, exLo, exHi, t0, nowSec, win, gx, gy, gw, gh, lo, hi);

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
    tft.printf("now %.1f %c", poolDisp(g_poolTemp), tempUnit());
    char hbuf[16];
    snprintf(hbuf, sizeof hbuf, "Hi %.1f", hi);
    tft.drawRightString(hbuf, gx + gw, gy + gh + 6, 2);
  }
}

void handlePoolGraphTouch(uint16_t x, uint16_t y) {
  // any touch keeps the screen alive for another 2 minutes
  g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;

  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_DASH; dirty = true; return; }  // close
  int bx = 8;
  for (int i = 0; i < 4; i++) {
    if (inRect(x, y, bx, 34, bx + 70, 56)) {
      if (g_poolTF != i) { g_poolTF = i; dirty = true; }
      return;
    }
    bx += 76 + (DISP_W - 320) / 4;
  }
  dirty = true;
}
