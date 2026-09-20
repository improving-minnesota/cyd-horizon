// ota.ino - firmware OTA from the project's GitHub releases: query latest,
// compare semver, download + flash to the inactive slot, reboot. Verified TLS
// with a time-gated insecure fallback; integrity pinned by the asset's SHA-256
// digest. See DEVELOPER.md "OTA updates".

#include <NetworkClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"

// Per-host trust stores: GitHub API via kSectigoUSERTrustEccRootCAs, release
// assets via kIsrgRootCAs (cyd-horizon.ino).

#define OTA_REPO    "improving-minnesota/cyd-horizon"
// Per-board release asset. Firmware predating board-named assets can't OTA
// (bare "cyd-horizon.ino.bin" is no longer published) - USB update only.
#ifdef CYD_E32R40T
#define OTA_ASSET   "cyd-horizon-e32r40t.ino.bin"
#else
#define OTA_ASSET   "cyd-horizon-2432s028r.ino.bin"
#endif
#define OTA_API_URL "https://api.github.com/repos/" OTA_REPO "/releases/latest"
// Hard cap on the whole body download (~1.3 MB image): healthy transfers finish
// in tens of seconds; this still tolerates very slow links without hanging.
#define OTA_DL_DEADLINE_MS (5UL * 60 * 1000)

// ---- semver helpers -------------------------------------------------------
int compareVersions(const String& a, const String& b) {
  int ai = 0, bi = 0;
  while (ai < (int)a.length() || bi < (int)b.length()) {
    int av = 0, bv = 0;
    while (ai < (int)a.length() && a[ai] != '.') av = av * 10 + (a[ai++] - '0');
    ai++;  // skip '.'
    while (bi < (int)b.length() && b[bi] != '.') bv = bv * 10 + (b[bi++] - '0');
    bi++;
    if (av != bv) return av > bv ? 1 : -1;
  }
  return 0;
}
String stripV(const String& s) { return (s.length() && s[0] == 'v') ? s.substring(1) : s; }
// Strip a non-numeric suffix ("-dev") before comparing - it would parse as digits.
String bareVersion(const String& s) {
  int i = 0;
  while (i < (int)s.length() && (isDigit(s[i]) || s[i] == '.')) i++;
  return s.substring(0, i);
}
bool isNewerThanRunning(const String& tagVersion) {
  String tag = stripV(tagVersion);
  int c = compareVersions(tag, bareVersion(kVersion));
  if (c > 0) return true;
  // A release is an upgrade over a -dev build of the same version.
  return c == 0 && strstr(kVersion, "-dev") != NULL;
}

// ---- GitHub latest-release fetch -----------------------------------------
bool fetchLatestRelease(String& versionOut, String& assetUrlOut, String& sha256Out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  // Retry a transient TLS connect/GET (same class of failure as performOTA, so
  // an update check that drops after prolonged uptime doesn't fail immediately).
  NetworkClientSecure sec;
  HTTPClient http;
  http.setConnectTimeout(5000);   // bound the TCP connect/TLS handshake, not just the read
  http.setTimeout(10000);
  // Shared verified-TLS helper; allowInsecure only falls back to setInsecure()
  // past OTA_CA_EXPIRY so a root rotation can't block updates.
  const char* ghHdrs[] = { "Accept", "application/vnd.github+json", nullptr };
  int code = httpsRequestRetry(http, sec, OTA_API_URL, HTTPS_METHOD_GET, "", ghHdrs, /*allowInsecure=*/true);
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  JsonDocument filter;
  filter["tag_name"] = true;
  JsonObject assetFilter = filter["assets"].add<JsonObject>();
  assetFilter["name"] = true;
  assetFilter["browser_download_url"] = true;
  assetFilter["digest"] = true;
  BoundedAllocator releaseAlloc(4096);
  JsonDocument doc(&releaseAlloc);
  HttpBodyStream body(http);
  DeserializationError parseErr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  bool bodyComplete = body.complete() || body.drain();
  http.end();
  if (parseErr || !bodyComplete) return false;
  const char* tag = doc["tag_name"] | "";
  if (!tag || !tag[0]) return false;
  String url, digest;
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    if (String((const char*)(a["name"] | "")) == OTA_ASSET) {
      url = (const char*)(a["browser_download_url"] | "");
      digest = (const char*)(a["digest"] | "");   // "sha256:<hex>"; "" if GitHub omitted it
      break;
    }
  }
  if (url.length() == 0) return false;
  versionOut = String(tag);
  assetUrlOut = url;
  sha256Out = digest;
  return true;
}

// Populate the About-page update state from the GitHub API. Runs on the net
// task so it never blocks drawing.
void checkForUpdate() {
  String ver, url, digest;
  if (!fetchLatestRelease(ver, url, digest)) { g_updateState = 4; return; }   // error
  if (isNewerThanRunning(ver)) {
    g_updateState = 2; g_updateLatest = stripV(ver); g_updateAsset = url; g_updateDigest = digest;
  } else {
    g_updateState = 3;                                                 // none
  }
}

// ---- daily auto-update scan ----------------------------------------------
// Once per calendar day (NVS-tracked) after WiFi + NTP; a newer version with
// Auto-Update ON starts the OTA.
void maybeAutoUpdate() {
  time_t now = time(nullptr);
  unsigned long day = (unsigned long)(now / 86400UL);
  if (!g_autoUpdate) return;
  if (now < 1600000000L) return;                 // NTP not synced yet
  if (g_lastScanDay == day) return;              // already scanned today
  // Only announce "Scanning" once a check will really run; the deadline covers
  // the fetch timeouts so the status can't outlive the scan.
  g_autoUpdStatus = 1; g_autoUpdStatusUntil = millis() + 20000;   // Scanning...
  String ver, url, digest;
  if (!fetchLatestRelease(ver, url, digest)) {
    g_autoUpdStatus = 4; g_autoUpdStatusUntil = millis() + 4000;   // Check Failed
    return;                                      // transient; try next boot
  }
  g_lastScanDay = day;                           // only mark after a good scan
  prefs.begin("flight", false); prefs.putULong("lastscan", day); prefs.end();
  if (isNewerThanRunning(ver)) {
    g_autoUpdStatus = 3; g_autoUpdStatusUntil = millis() + 4000;   // Updating...
    g_otaVersion = stripV(ver);
    g_otaUrl = url;
    g_otaSha256 = digest;
    g_otaActive = true;                          // loop() performs the update
  } else {
    g_autoUpdStatus = 2; g_autoUpdStatusUntil = millis() + 4000;   // No Updates
  }
}

// Daily scan on the net task - the TLS fetch + JSON parse overflows loopTask.
// Waits briefly for NTP so the once-per-day clock is meaningful.
void autoScanOnce() {
  unsigned long t0 = millis();
  while (time(nullptr) < 1600000000L && millis() - t0 < 8000) delay(100);
  maybeAutoUpdate();
}

// ---- OTA execution --------------------------------------------------------
// Last-drawn bar fill, so drawOtaProgress() only paints the grown segment
// (full-bar redraws flickered).
static int s_otaFill = 0;

void drawOtaHeader(const String& version) {
  s_otaFill = 0;
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol); tft.setTextFont(2);
  tft.setCursor(8, 6); tft.print("Updating");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK); tft.setTextFont(2);
  tft.setCursor(8, 52); tft.print("Updating to v" + version + "...");
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setCursor(8, 84); tft.print("Do not power off device");
  tft.drawRect(10, 120, DISP_W - 20, 22, TFT_WHITE);
}

void drawOtaProgress(int total, size_t got) {
  if (total <= 0) return;
  int fill = (int)((long)got * (DISP_W - 24) / total); if (fill > DISP_W - 24) fill = DISP_W - 24;
  if (fill < s_otaFill) fill = s_otaFill;
  if (fill > s_otaFill) {
    tft.fillRect(12 + s_otaFill, 122, fill - s_otaFill, 18, TFT_GREEN);
    s_otaFill = fill;
  }
  char pct[16]; snprintf(pct, sizeof pct, "%d%%", (int)((long)got * 100 / total));
  tft.fillRect(CX - 80, 146, 160, 18, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextFont(2);
  tft.setCursor(CX - 40, 148); tft.print(pct);
}

void drawOtaRestart() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextFont(2);
  tft.setCursor(60, 110); tft.print("Restarting...");
}

void drawOtaError(const char* msg) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextFont(2);
  tft.setCursor(20, 100); tft.print("Update Failed");
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK); tft.setTextFont(1);
  tft.setCursor(20, 124); tft.print(msg);
  delay(3000);
}

// Terminal failure page, drawn only when the TLS arena can't be rebuilt after
// a failed OTA: no buttons, and the caller never returns - the device holds
// this until power cycled rather than resume polling on a broken heap.
void drawOtaRestartRequired() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextFont(2);
  tft.setCursor(20, 88); tft.print("Update Failed");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 118); tft.print("Restart or power cycle");
  tft.setCursor(20, 136); tft.print("the device to continue.");
}

// Download + flash on the dedicated task (TLS overflows loopTask). Owns the
// display; reboots on success.
static bool sha256Matches(const uint8_t hash[32], const String& expected) {
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(&hex[i * 2], 3, "%02x", hash[i]);
  hex[64] = 0;
  String exp = expected;
  if (exp.startsWith("sha256:")) exp = exp.substring(7);
  exp.trim();
  return exp.equalsIgnoreCase(hex);
}

bool performOTA(const String& url, const String& version, const String& expectedSha256) {
  // Fresh TLS connects can drop after long uptime, so retry the WHOLE download
  // (connect + GET + stream + checksum) - a mid-stream drop fails any stage.

  // Plain HTTP OTA is restricted to explicitly enabled development builds.
  bool useHttps = url.startsWith("https://");
  if (!useHttps && (!isDevBuild() || !ENABLE_LOCAL_OTA)) return false;
  const char* failReason = "Download failed";

  // Dev-build debug output for OTA diagnostics.
  bool dev = isDevBuild();
  if (dev) Serial.printf("[OTA] start url=%s\n", url.c_str());

  // Wait for WiFi first - a serial-triggered OTA can arrive during boot connect,
  // and HTTPClient returns -1 with no link.
  unsigned long wifiWaitStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiWaitStart) < 20000) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[OTA] update failed: No WiFi\n");
    drawOtaError("No WiFi");
    return false;
  }

  for (int attempt = 1; attempt <= HTTPS_RETRY_ATTEMPTS; attempt++) {
    drawOtaHeader(version);          // reset screen + progress bar each attempt
    if (attempt > 1) delay(HTTPS_RETRY_DELAY_MS);

    // Allocate the flash buffer BEFORE the request's TLS/lwIP churn: once the
    // ~1.3 MB body starts streaming into RX pbufs, the internal heap can squeeze
    // enough that Update.begin()'s 4 KB buffer alloc fails (returns err=0).
    // Unknown size is safe: got/total + the SHA-256 digest still bound the
    // transfer, and Update.end(true) finalizes a partial-length stream.
    if (!Update.begin(OTA_SIZE_UNKNOWN, U_FLASH)) {
      Serial.printf("[OTA] update failed: flash begin err=%d\n", Update.getError());
      drawOtaError("Flash failed");
      return false;
    }

    HTTPClient http;
    http.setUserAgent(appUserAgent());   // persistent across begin()/end()
    bool connected = false;
    WiFiClient plainClient;
    NetworkClientSecure sec;
    String requestUrl = url;
    int code = -1;

    // Follow redirects manually so each TLS connection gets the CA bundle for
    // its actual host (github.com -> release-assets host).
    for (int redirect = 0; redirect < 4; redirect++) {
      http.setConnectTimeout(5000);
      http.setTimeout(20000);
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      if (useHttps) {
        // Keep `sec` alive for the full request; httpsBegin selects the bundle
        // from the current request URL, including the redirected asset host.
        connected = httpsBegin(http, sec, requestUrl.c_str(), /*allowInsecure=*/true);
      } else {
        // Plain HTTP path (local dev server).
        connected = http.begin(plainClient, requestUrl.c_str());
      }
      if (redirect == 0 && dev) {
        Serial.printf("[OTA] attempt %d %s connect=%d\n", attempt,
                      useHttps ? "https" : "http", connected);
      }
      if (!connected) break;
      code = http.GET();
      if (code >= 300 && code < 400) {
        String nextUrl = http.getLocation();
        http.end();
        // http.end() skips client.stop() when the peer already closed the
        // socket (HTTPClient::disconnect early-outs on !connected()), which
        // leaks the whole TLS context into the next hop's handshake and
        // starves the arena. Tear down explicitly so each hop is truly serial.
        if (useHttps) sec.stop();
        if (dev) logHeapDiag("ota-hop");
        if (nextUrl.length() == 0) { code = -1; break; }
        requestUrl = nextUrl;
        continue;
      }
      break;
    }
    if (dev) Serial.printf("[OTA] attempt %d code=%d size=%d\n", attempt, code, http.getSize());
    if (!connected || code != HTTP_CODE_OK) { http.end(); if (useHttps) sec.stop(); Update.abort(); continue; }

    int total = http.getSize();
    bool haveTotal = (total > 0);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    uint8_t buf[4096];
    int lastPct = -1;
    bool done = false;
    unsigned long idleSince = millis();
    const unsigned long dlStart = idleSince;
    while (!done) {
      // Absolute deadline on top of the idle guard: a connection trickling a
      // few bytes/minute keeps resetting idleSince and stalls the update
      // indefinitely otherwise.
      if (millis() - dlStart > OTA_DL_DEADLINE_MS) { failReason = "Download timeout"; break; }
      if (stream->available() == 0) {
        // If we already got the full announced body, finish even if connection stays open.
        if (haveTotal && got >= (size_t)total) { done = true; break; }
        if (!http.connected()) { done = true; break; }
        // Timeout if no data arrives for too long (HTTP read timeout should catch this,
        // but a watchdog-free loop needs its own guard).
        if (millis() - idleSince > 60000UL) { failReason = "Download timeout"; break; }
        delay(1); continue;
      }
      idleSince = millis();
      int n = stream->readBytes(buf, min(sizeof buf, (size_t)stream->available()));
      if (n <= 0) { done = true; break; }
      mbedtls_sha256_update(&sha, buf, n);
      Update.write(buf, n); got += n;
      // No esp_task_wdt_reset() here: performOTA runs on its own task that is not
      // watchdog-subscribed, and the HTTP timeouts bound the download.
      int pct = haveTotal ? (int)((long)got * 100 / total) : -1;
      if (pct != lastPct && pct >= 0) { lastPct = pct; drawOtaProgress(total, got); }
      if (haveTotal && got >= (size_t)total) done = true;
    }
    if (dev) Serial.printf("[OTA] attempt %d got=%d total=%d\n", attempt, (int)got, total);
    http.end();
    if (useHttps) sec.stop();   // same peer-closed leak as the redirect hop

    if (haveTotal && got < (size_t)total) { Update.abort(); failReason = "Download failed"; continue; }   // dropped mid-stream -> retry

    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    // Enforce a supplied digest on every transport; local dev images have none.
    if (!expectedSha256.isEmpty() && !sha256Matches(hash, expectedSha256)) {
      Update.abort(); failReason = "Checksum mismatch"; continue;   // corrupted transfer -> retry
    }

    if (!Update.end(true)) {   // evenIfRemaining: _size came in as UNKNOWN
      Serial.printf("[OTA] update failed: flash end err=%d\n", Update.getError());
      drawOtaError("Flash failed"); return false;
    }
    drawOtaRestart();
    delay(500);
    ESP.restart();
    return true;
  }
  Serial.printf("[OTA] update failed: %s\n", failReason);   // one line for post-mortem diagnosis
  drawOtaError(failReason);
  return false;
}

// ---- rollback safeguard ---------------------------------------------------
// After the boot grace period: cancel pending rollback so the OTA'd slot stays.
void markAppValidBoot() {
  esp_ota_img_states_t st;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK) {
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }
}
