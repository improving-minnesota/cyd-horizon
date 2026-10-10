// flight_details.ino - origin/destination + flight details view. Route/track
// fetch once per overhead stint; the recall tap redisplays g_lastFlight.
// Codes show "ICAO | IATA" when available; comparisons always use ICAO.

// ---- Airport ICAO -> city lookup (major US + some intl). Fallback = raw code.
struct Airport { const char* icao; const char* city; };
static const Airport kAirports[] = {
  {"KDFW", "Dallas-Ft Worth"}, {"KDAL", "Dallas"},      {"KHOU", "Houston"},
  {"KIAH", "Houston"},         {"KAUS", "Austin"},      {"KSAT", "San Antonio"},
  {"KELP", "El Paso"},         {"KCRP", "Corpus Christi"},
  {"KATL", "Atlanta"},         {"KLAX", "Los Angeles"}, {"KJFK", "New York"},
  {"KLGA", "New York"},        {"KEWR", "Newark"},      {"KORD", "Chicago"},
  {"KMDW", "Chicago"},         {"KSEA", "Seattle"},     {"KSFO", "San Francisco"},
  {"KOAK", "Oakland"},         {"KSJC", "San Jose"},    {"KDEN", "Denver"},
  {"KPHX", "Phoenix"},         {"KPHL", "Philadelphia"},{"KBOS", "Boston"},
  {"KMIA", "Miami"},           {"KMCO", "Orlando"},     {"KTPA", "Tampa"},
  {"KLAS", "Las Vegas"},       {"KMSP", "Minneapolis"}, {"KDTW", "Detroit"},
  {"KCLT", "Charlotte"},       {"KSTL", "St Louis"},    {"KPIT", "Pittsburgh"},
  {"KIND", "Indianapolis"},    {"KCLE", "Cleveland"},   {"KCVG", "Cincinnati"},
  {"KMEM", "Memphis"},         {"KBNA", "Nashville"},   {"KJAX", "Jacksonville"},
  {"KRDU", "Raleigh"},          {"KABQ", "Albuquerque"},
  {"KSLC", "Salt Lake City"},  {"KPDX", "Portland"},    {"KSAN", "San Diego"},
  {"KDCA", "Washington"},      {"KIAD", "Washington"},  {"KBWI", "Baltimore"},
};
static const int kNumAirports = sizeof(kAirports) / sizeof(kAirports[0]);

String airportCity(const char* icao) {
  if (!icao || icao[0] == 0) return "--";
  for (int i = 0; i < kNumAirports; i++) {
    if (strcmp(kAirports[i].icao, icao) == 0) return kAirports[i].city;
  }
  return "--";   // unknown -> no city (don't echo the code and look like a route)
}

// ---- Airline lookup (ICAO 3-letter callsign prefix -> name + brand color) ----
struct Airline { const char* code; const char* name; uint16_t color; };
static const Airline kAirlines[] = {
  {"AAL", "American Airlines", TFT_BLUE},
  {"DAL", "Delta Air Lines",   TFT_RED},
  {"UAL", "United Airlines",   TFT_NAVY},
  {"SWA", "Southwest Airlines",TFT_RED},
  {"SCX", "Sun Country Airlines",TFT_RED},
  {"FDX", "FedEx Express",     TFT_PURPLE},
  {"UPS", "UPS Airlines",      TFT_MAROON},
  {"JBU", "JetBlue",           TFT_BLUE},
  {"ASA", "Alaska Airlines",   TFT_DARKCYAN},
  {"AAY", "Allegiant Air",     TFT_RED},
  {"FFT", "Frontier Airlines", TFT_DARKGREEN},
  {"NKS", "Spirit Airlines",   TFT_ORANGE},
  {"SKW", "SkyWest Airlines",  TFT_NAVY},
  {"ENY", "Envoy Air",         TFT_BLUE},
  {"RPA", "Republic Airways",  TFT_NAVY},
  {"EJA", "NetJets",           TFT_DARKGREY},
  {"BAW", "British Airways",   TFT_BLUE},
  {"AFR", "Air France",        TFT_NAVY},
  {"DLH", "Lufthansa",         TFT_YELLOW},
  {"KLM", "KLM Royal Dutch",   TFT_BLUE},
  {"UAE", "Emirates",          TFT_RED},
  {"QTR", "Qatar Airways",     TFT_PURPLE},
  {"ACA", "Air Canada",        TFT_RED},
  {"ANA", "All Nippon Airways",TFT_RED},
  {"EIN", "Aer Lingus",        TFT_GREEN},
  {"AMX", "Aeromexico",        TFT_NAVY},
  {"CCA", "Air China",         TFT_RED},
  {"AXM", "AirAsia",           TFT_RED},
  {"CPA", "Cathay Pacific",    TFT_GREEN},
  {"CES", "China Eastern",     TFT_RED},
  {"CSN", "China Southern",    TFT_BLUE},
  {"CMP", "Copa Airlines",     TFT_RED},
  {"ETD", "Etihad Airways",    TFT_YELLOW},
  {"EWG", "Eurowings",         TFT_RED},
  {"FIN", "Finnair",           TFT_NAVY},
  {"CHH", "Hainan Airlines",   TFT_RED},
  {"HAL", "Hawaiian Airlines", TFT_PURPLE},
  {"IBE", "Iberia",            TFT_RED},
  {"ICE", "Icelandair",        TFT_NAVY},
  {"IGO", "IndiGo",            TFT_BLUE},
  {"JAL", "Japan Airlines",    TFT_RED},
  {"KAL", "Korean Air",        TFT_BLUE},
  {"NAX", "Norwegian",         TFT_RED},
  {"QFA", "Qantas Airways",    TFT_RED},
  {"RYR", "Ryanair",           TFT_YELLOW},
  {"SAS", "Scandinavian Airlines", TFT_NAVY},
  {"SIA", "Singapore Airlines",TFT_YELLOW},
  {"SWR", "Swiss International",TFT_RED},
  {"AVA", "Avianca",           TFT_RED},
  {"TAM", "TAM (LATAM)",       TFT_RED},
  {"TAP", "TAP Air Portugal",  TFT_RED},
  {"THY", "Turkish Airlines",  TFT_RED},
  {"VIR", "Virgin Atlantic",   TFT_RED},
  {"VLG", "Vueling",           TFT_RED},
  {"WJA", "WestJet",           TFT_RED},
  {"WZZ", "Wizz Air",          TFT_MAGENTA},
  {"EZY", "easyJet",           TFT_ORANGE},
};
static const int kNumAirlines = sizeof(kAirlines) / sizeof(kAirlines[0]);

// Returns true if the airline is known, filling `name` and `color`.
bool airlineInfo(const char* callsign, String& name, uint16_t& color) {
  if (!callsign || callsign[0] == 0) return false;
  char code[4] = {};
  for (int i = 0; i < 3 && callsign[i]; i++) {
    code[i] = (char)toupper((unsigned char)callsign[i]);
  }
  for (int i = 0; i < kNumAirlines; i++) {
    if (strncmp(code, kAirlines[i].code, sizeof code - 1) == 0) {
      name = kAirlines[i].name;
      color = kAirlines[i].color;
      return true;
    }
  }
  return false;
}

// findAirlineLogo() lives in logos.ino (LittleFS partition, single reusable buffer).



// Fetch route via OpenSky /flights/aircraft into g_routeOrigin/Dest (ICAO);
// cached per plane to save credits.
void fetchRoute(const char* icao24) {
  g_routeBusy = true;   // guard g_routeOrigin/g_routeDest while we write them
  g_routeOrigin = "";
  g_routeDest = "";
  // No WiFi is transient; leave g_routeFetched false so we retry once connected.
  if (WiFi.status() != WL_CONNECTED) { g_routeBusy = false; return; }
  // The flights/aircraft endpoint requires begin/end (Unix seconds) to return a
  // flight. The plane is overhead right now, so search the last few hours.
  time_t now = time(nullptr);
  time_t begin = now - 4 * 3600;   // 4 hours back
  time_t end = now + 60;           // a little into the future
  String url = String("https://opensky-network.org/api/flights/aircraft?icao24=") + icao24
               + "&begin=" + String((long)begin) + "&end=" + String((long)end);
  // Verified TLS against the same ISRG roots used by the main OpenSky calls.
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  // Capture the /flights/* bucket's remaining credits and 429 retry deadline.
  const char* hdrKeys[] = { "X-Rate-Limit-Remaining", "X-Rate-Limit-Retry-After-Seconds" };
  http.collectHeaders(hdrKeys, 2);
  String authHdr;
  if (openskyEnsureToken()) authHdr = "Bearer " + g_osToken;
  const char* routeHdrs[] = { "Authorization", authHdr.c_str(), nullptr };
  int code = httpsRequestRetry(http, sec, url.c_str(), HTTPS_METHOD_GET, "", routeHdrs);
  String rem = http.header("X-Rate-Limit-Remaining");
  if (rem.length()) g_flightsCredits = rem.toInt();
  bool parsedOk = false;
  if (code == HTTP_CODE_OK) {
    HttpBodyStream body(http);
    BoundedAllocator routeAlloc(2048);
    JsonDocument doc(&routeAlloc);
    int first = body.read();
    while (first == ' ' || first == '\r' || first == '\n' || first == '\t') first = body.read();
    if (first == '[') {
      // The route endpoint returns an array; only its first object is needed.
      if (nextElement(body, doc)) {
        JsonObject flight = doc.as<JsonObject>();
        if (!flight.isNull()) {
          const char* dep = flight["estDepartureAirport"] | "";
          const char* dst = flight["estArrivalAirport"]   | "";
          g_routeOrigin = dep;
          g_routeDest   = dst;
        }
      }
      parsedOk = body.drain();
    } else if (first == 'n') {
      parsedOk = body.drain();  // valid empty/null response
    }
  }
  http.end();
  // Mark fetched on TLS error/429/bad framing too - don't keep burning credits
  // on the same plane.
  if (code < 0 || code == HTTP_CODE_TOO_MANY_REQUESTS || (code == HTTP_CODE_OK && !parsedOk)) {
    g_routeFetched = true;
    if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
      g_flightsCredits = 0;
      String retry = http.header("X-Rate-Limit-Retry-After-Seconds");
      unsigned long waitMs = retry.length() ? (unsigned long)retry.toInt() * 1000UL : CREDIT_RECOVERY_MS;
      g_nextRouteMs = millis() + waitMs;
      if (isDevBuild() && retry.length()) Serial.printf("[net] 429 route retry after %lus\n", (unsigned long)retry.toInt());
    }
  }
  if (code >= 0 && parsedOk) g_routeFetched = true;
  if (code == HTTP_CODE_OK) {
    if (isDevBuild()) Serial.printf("[net] route ok origin=%s dest=%s free=%u\n", g_routeOrigin.c_str(), g_routeDest.c_str(), (unsigned)ESP.getFreeHeap());
  }
  g_routeBusy = false;
}

// Fetch ground track via OpenSky /tracks into g_trackPts + g_trackBearingDeg
// for dead-reckoning; a failed/empty response leaves g_trackCount=0 (no line).
void fetchTrack(const char* icao24) {
  g_trackBusy = true;
  portENTER_CRITICAL(&g_trackMux);
  g_trackCount = 0;
  g_trackBearingDeg = -1.0f;
  portEXIT_CRITICAL(&g_trackMux);
  if (WiFi.status() != WL_CONNECTED) { g_trackFetched = false; g_trackBusy = false; return; }
  time_t now = time(nullptr);
  String url = String("https://opensky-network.org/api/tracks/all?icao24=") + icao24
               + "&time=" + String((long)now);
  NetworkClientSecure sec;
  HTTPClient http;
  http.setTimeout(5000);
  const char* hdrKeys[] = { "X-Rate-Limit-Remaining", "X-Rate-Limit-Retry-After-Seconds" };
  http.collectHeaders(hdrKeys, 2);
  String authHdr;
  if (openskyEnsureToken()) authHdr = "Bearer " + g_osToken;
  const char* trackHdrs[] = { "Authorization", authHdr.c_str(), nullptr };
  int code = httpsRequestRetry(http, sec, url.c_str(), HTTPS_METHOD_GET, "", trackHdrs);
  String rem = http.header("X-Rate-Limit-Remaining");
  if (rem.length()) g_tracksCredits = rem.toInt();
  bool parsedOk = false;
  if (code == HTTP_CODE_OK) {
    HttpBodyStream body(http);
    BoundedAllocator ptAlloc(1024);
    JsonDocument pt(&ptAlloc);
    if (!seekArray(body, "\"path\"")) {
      if (body.peek() == 'n' && body.drain()) {
        parsedOk = true;  // OpenSky uses path:null for a valid empty track.
        if (isDevBuild()) Serial.printf("[net] track path null len=%u\n", (unsigned)body.bytesRead());
      } else {
        if (isDevBuild()) Serial.printf("[net] track json no path len=%u\n", (unsigned)body.bytesRead());
      }
      portENTER_CRITICAL(&g_trackMux);
      g_trackCount = 0;
      portEXIT_CRITICAL(&g_trackMux);
    } else {
      const float maxRange = max(g_radiusMi * 2.0f, 8.0f);
      const float cosLat = cosf(g_lat * PI / 180.0f);
      TrackPoint ring[MAX_TRACK_PTS];
      int written = 0;
      int kept = 0;
      float bearing = -1.0f;
      bool haveBearing = false;
      for (;;) {
        if (!nextElement(body, pt)) break;
        if (!pt.is<JsonArray>() || pt.size() < 5) continue;
        float lat = pt[1].as<float>();
        float lon = pt[2].as<float>();
        float dlat = lat - g_lat;
        float dlon = (lon - g_lon) * cosLat;
        float dxMi = dlon * 69.0f;
        float dyMi = dlat * 69.0f;
        if (sqrtf(dxMi * dxMi + dyMi * dyMi) > maxRange) continue;
        ring[written].dxMi = dxMi;
        ring[written].dyMi = dyMi;
        written = (written + 1) % MAX_TRACK_PTS;
        if (kept < MAX_TRACK_PTS) kept++;
        // Keep the newest near point's true-track as the follow bearing.
        if (!pt[4].isNull()) { bearing = pt[4].as<float>(); haveBearing = true; }
      }
      bool bodyComplete = body.drain();
      if (bodyComplete) {
        int start = (written - kept + MAX_TRACK_PTS) % MAX_TRACK_PTS;
        portENTER_CRITICAL(&g_trackMux);
        for (int i = 0; i < kept; i++) {
          g_trackPts[i] = ring[(start + i) % MAX_TRACK_PTS];
        }
        if (haveBearing) g_trackBearingDeg = bearing;
        g_trackCount = kept;
        portEXIT_CRITICAL(&g_trackMux);
        parsedOk = true;
      } else {
        portENTER_CRITICAL(&g_trackMux);
        g_trackCount = 0;
        portEXIT_CRITICAL(&g_trackMux);
        if (isDevBuild()) Serial.printf("[net] track json short len=%u/%ld%s\n", (unsigned)body.bytesRead(), body.contentLength(), body.stalled() ? " stalled" : "");
        http.end();
        g_trackFetched = true;  // do not retry this plane for a truncated body
        g_trackBusy = false;
        return;
      }
    }
    if (isDevBuild()) Serial.printf("[net] track ok free=%u pts=%d\n", (unsigned)ESP.getFreeHeap(), (int)g_trackCount);
  }
  http.end();
  // Mark fetched on TLS error/429/bad framing too (a no-WiFi exit above stays
  // unfetched) - don't keep burning credits on the same plane.
  if (code < 0 || code == HTTP_CODE_TOO_MANY_REQUESTS || (code == HTTP_CODE_OK && !parsedOk)) {
    g_trackFetched = true;
    if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
      g_tracksCredits = 0;
      String retry = http.header("X-Rate-Limit-Retry-After-Seconds");
      unsigned long waitMs = retry.length() ? (unsigned long)retry.toInt() * 1000UL : CREDIT_RECOVERY_MS;
      g_nextTrackMs = millis() + waitMs;
      if (isDevBuild() && retry.length()) Serial.printf("[net] 429 track retry after %lus\n", (unsigned long)retry.toInt());
    }
  }
  if (code >= 0 && parsedOk) g_trackFetched = true;
  g_trackBusy = false;
}

// Fetch the planned route from adsb.lol (vrs-standing-data). The airport_codes
// fallback is ICAO-only - _iata spans multi-hop routes and can't pair legs.
void fetchAdsbRoute(const char* callsign) {
  g_adsbRouteBusy = true;
  g_adsbRouteOrigin = "";
  g_adsbRouteDest = "";
  g_adsbOriginCity = "";
  g_adsbDestCity = "";
  g_adsbOriginIata = "";
  g_adsbDestIata = "";
  if (WiFi.status() != WL_CONNECTED || !callsign || callsign[0] == 0) { g_adsbRouteBusy = false; return; }
  // Trim trailing whitespace and require at least 2 chars for the prefix directory.
  char cs[16];
  int n = 0;
  for (int i = 0; i < (int)sizeof(cs) - 1 && callsign[i]; i++) cs[n++] = callsign[i];
  while (n > 0 && (cs[n-1] == ' ' || cs[n-1] == '\t')) n--;
  cs[n] = 0;
  if (n < 2) { g_adsbRouteBusy = false; return; }
  String url = String("https://vrs-standing-data.adsb.lol/routes/") + String(cs).substring(0, 2) + "/" + cs + ".json";
  NetworkClientSecure sec;
  HTTPClient http;
  int code = httpsRequestRetry(http, sec, url.c_str(), HTTPS_METHOD_GET, "", nullptr);
  bool parsedOk = false;
  if (code == HTTP_CODE_OK) {
    HttpBodyStream body(http);  // adsb.lol sends Content-Length, not chunked
    JsonDocument filter;
    filter["airport_codes"] = true;
    JsonObject f = filter["_airports"].add<JsonObject>();
    f["icao"] = true;
    f["iata"] = true;
    f["location"] = true;
    f["countryiso2"] = true;
    BoundedAllocator adsbAlloc(2048);
    JsonDocument doc(&adsbAlloc);
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (!err) {
      JsonObject root = doc.as<JsonObject>();
      if (!root.isNull()) {
        JsonArray arr = root["_airports"];
        if (!arr.isNull() && arr.size() >= 1) {
          int n = arr.size();
          // Multi-leg routes list every hop; the current leg is the last two
          // airports. A single airport is destination-only, with no origin.
          if (n >= 2) {
            JsonObject a0 = arr[n - 2];
            JsonObject a1 = arr[n - 1];
            const char* oi = a0["icao"];
            const char* ot = a0["iata"];
            const char* oc = a0["location"];
            const char* oCountry = a0["countryiso2"];
            const char* di = a1["icao"];
            const char* dt = a1["iata"];
            const char* dc = a1["location"];
            const char* dCountry = a1["countryiso2"];
            if (oi && oi[0]) g_adsbRouteOrigin = oi;
            if (ot && ot[0]) g_adsbOriginIata = ot;
            if (oc && oc[0]) g_adsbOriginCity = oc;
            if (oCountry && oCountry[0] && g_adsbOriginCity.length()) g_adsbOriginCity += String(", ") + oCountry;
            if (di && di[0]) g_adsbRouteDest = di;
            if (dt && dt[0]) g_adsbDestIata = dt;
            if (dc && dc[0]) g_adsbDestCity = dc;
            if (dCountry && dCountry[0] && g_adsbDestCity.length()) g_adsbDestCity += String(", ") + dCountry;
          } else {  // n == 1
            JsonObject a1 = arr[0];
            const char* di = a1["icao"];
            const char* dt = a1["iata"];
            const char* dc = a1["location"];
            const char* dCountry = a1["countryiso2"];
            if (di && di[0]) g_adsbRouteDest = di;
            if (dt && dt[0]) g_adsbDestIata = dt;
            if (dc && dc[0]) g_adsbDestCity = dc;
            if (dCountry && dCountry[0] && g_adsbDestCity.length()) g_adsbDestCity += String(", ") + dCountry;
          }
          parsedOk = (g_adsbRouteOrigin.length() > 0 || g_adsbRouteDest.length() > 0);
        }
        if (!parsedOk) {
          const char* codes = root["airport_codes"];
          if (codes && codes[0]) {
            String s = codes;
            int last  = s.lastIndexOf('-');
            if (last < 0) {
              // One airport code = destination only.
              g_adsbRouteDest = s;
            } else {
              int prev = (last > 0) ? s.lastIndexOf('-', last - 1) : -1;
              g_adsbRouteOrigin = (prev >= 0) ? s.substring(prev + 1, last) : s.substring(0, last);
              g_adsbRouteDest   = s.substring(last + 1);
            }
            if (g_adsbRouteOrigin.length()) g_adsbOriginCity = airportCity(g_adsbRouteOrigin.c_str());
            if (g_adsbRouteDest.length())   g_adsbDestCity    = airportCity(g_adsbRouteDest.c_str());
            parsedOk = (g_adsbRouteOrigin.length() > 0 || g_adsbRouteDest.length() > 0);
          }
        }
      }
    } else if (isDevBuild()) {
      Serial.printf("[net] adsb parse err %s free=%u\n", err.c_str(), (unsigned)ESP.getFreeHeap());
    }
    parsedOk = body.drain() && parsedOk;
  }
  http.end();
  if (code < 0 || code == HTTP_CODE_TOO_MANY_REQUESTS) {
    g_nextAdsbMs = millis() + ADSB_RETRY_MS;
    if (isDevBuild()) Serial.printf("[net] adsb 429/fail code=%d, retry in %lus\n", code, ADSB_RETRY_MS/1000);
  } else if (code == HTTP_CODE_OK) {
    g_adsbRouteFetched = true;
    if (isDevBuild()) Serial.printf("[net] adsb ok origin=%s/%s (%s) dest=%s/%s (%s) code=%d free=%u\n",
                                     g_adsbRouteOrigin.c_str(), g_adsbOriginCity.c_str(), g_adsbOriginIata.c_str(),
                                     g_adsbRouteDest.c_str(), g_adsbDestCity.c_str(), g_adsbDestIata.c_str(),
                                     code, (unsigned)ESP.getFreeHeap());
  } else {
    // 404 or other HTTP error: mark fetched so we don't keep trying this callsign.
    g_adsbRouteFetched = true;
    if (isDevBuild()) Serial.printf("[net] adsb %d for %s\n", code, cs);
  }
  g_adsbRouteBusy = false;
}


