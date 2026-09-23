// settings.ino - Settings screen + sub-screens (Flight Tracker, Sleep Mode,
// Reset, Help, color picker) and their drawing helpers. Shares globals/helpers
// declared in cyd-horizon.ino.

// The Settings screen is a pure navigation list - it holds no settings itself;
// each row jumps to a dedicated sub-page. Settings live on those pages.

void drawSettings() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings");

  // Back button (top-right)
  backBtn("Back");

  // 2-column grid, alphabetical: About, Calibrate Touch, Flight Tracker,
  // General, Help, Location, Network, Pool Temp, Reset, Sleep Mode. Reset sits
  // bottom-left drawn red (destructive); handleTouch() mirrors this layout.
  const char* items[] = { "About", "Calibrate Touch", "Flight Tracker", "General",
                          "Help", "Location", "Network", "Pool Temp",
                          "Reset", "Sleep Mode" };
  const int n = 10, rowH = 34, step = 38;
  const int colW = (DISP_W - 30) / 2;        // 10px margins + 10px column gap
  const int colX[2] = { 10, 10 + colW + 10 };
  const int y0 = 42;                 // first row y
  for (int i = 0; i < n; i++) {
    int row = i / 2, col = i % 2;
    int y = y0 + row * step;
    bool rst = strcmp(items[i], "Reset") == 0;   // Reset -> danger color
    if (rst) tft.fillRoundRect(colX[col], y, colW, rowH, 6, dangerCol());
    else     themeBtn(colX[col], y, colW, rowH, 6);
    tft.setTextColor(rst ? btnFg(dangerCol()) : btnFg(btnCol()), rst ? dangerCol() : btnCol());
    tft.setTextFont(2);
    tft.setCursor(colX[col] + 8, y + (rowH - 16) / 2);
    tft.print(items[i]);
  }
}

// ---- About page ----
void drawAbout() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > About");

  backBtn("Back");

  // Body (compact, to leave room for the upgrade section below)
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 40);
  tft.print("cyd-horizon (Cheap Yellow Display)");

  tft.setTextFont(1);
  tft.setCursor(8, 62);
  tft.print("Developer: Paul Hassinger");

  tft.setCursor(8, 76);
  tft.print("Email: paul.hassinger (at) improving.com");

  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setCursor(8, 92);
  tft.print("github.com/improving-minnesota/cyd-horizon");

  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 112);
  tft.print("Version: v");
  tft.print(kVersion);
#if BUILD_NUM != 0
  tft.print("  Build: ");
  tft.print(BUILD_NUM);
#endif

  // Divider above the upgrade section
  tft.drawFastHLine(8, 130, DISP_W - 16, TFT_DARKGREY);

  // Upgrade section
  tft.setTextFont(2);
  if (g_updateState == 2) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 140);
    tft.print("Upgrade Available:");
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.setCursor(8, 164);
    tft.print("(v");
    tft.print(g_updateLatest);
    tft.print(")");
    // Install button (right)
    themeBtn(RX(218), 140, 76, 26, 5);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setCursor(RX(236), 147);
    tft.print("Install");
  } else if (g_updateState == 3) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 140);
    tft.print("No Updates Available");
  } else if (g_updateState == 4) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(8, 140);
    tft.print("Update Check Failed");
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 140);
    tft.print("Checking for updates...");
  }
}

void handleAboutTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; g_otaFromAbout = false; dirty = true; return; }
  // Install button (only shown when an update is available)
  if (g_updateState == 2 && inRect(x, y, RX(218), 140, RX(294), 166)) {
    g_otaVersion = g_updateLatest;
    g_otaUrl = g_updateAsset;
    g_otaSha256 = g_updateDigest;
    g_otaActive = true;
    g_otaFromAbout = true;   // keep the About result on screen after a failed OTA
    return;
  }
}

// ---- Help page (scrollable) ----
// Shows the end-user guide on the device. Text is pre-wrapped to fit and
// scrolls vertically with a scrollbar on the right.
#define HELP_TOP    36
#define HELP_BOT    222
#define HELP_X      8
#define HELP_W      (DISP_W - 34)
#define HELP_SB_X   (DISP_W - 16)
#define HELP_SB_W   10
#define HELP_LINEH  9

static const char* const kHelpLines[] = {
  "cyd-horizon",
  "-----------",
  "Live flight tracker &",
  "weather station for Cheap",
  "Yellow Display (CYD)",
  "boards:",
  "  - 2.8\" 2432S028R",
  "  - 4\" E32R40T",
  "Once it's set up it runs",
  "on your WiFi - no",
  "computer needed. Made for",
  "this board only (wiring is",
  "CYD-specific).",
  "",
  "  - Live ADS-B flight radar",
  "  - Live weather & forecasts",
  "  - Pool temperature monitor",
  "  - 50 alarm & melody presets",
  "  - Standalone WiFi; auto-OTA",
  "",
  "FEATURES",
  "Clock: big time and date.",
  "Weather: temp, feels-like,",
  "humidity, sunrise/sunset,",
  "and a 7-day forecast.",
  "Flights (optional, on):",
  "  live aircraft with a",
  "  radar, callsign, planned",
  "  route (ADSB.lol, OpenSky",
  "  fallback; shown as ICAO |",
  "  IATA when IATA is known),",
  "  airline logo.",
  "The LED blinks blue for each",
  "overhead flight, yellow when",
  "both origin/destination are your",
  "home airport (ICAO), green when",
  "arriving, red when departing.",
  "Yellow/green/red stay lit while",
  "the live flight is displayed. A",
  "watched callsign - the value",
  "matches any part of a real",
  "callsign (* = all) -",
  "alerts with its",
  "Callsign Notify pattern on the",
  "LED + speaker while shown - the",
  "same 50 presets as alarms, from",
  "Simple's soft beep to melodies",
  "like Charge. The LED also glows",
  "red for a critical error (No WiFi,",
  "invalid creds, exhausted credits,",
  "or unavailable data) or yellow",
  "for OpenSky anonymous, matching",
  "the home-screen border.",
  "  Any settings or graph screen",
  "  returns to the main dashboard",
  "  after 2 minutes of inactivity.",
  "  Tap the aircraft count",
  "  (e.g. '6 aircraft') on the",
  "  idle screen for the last",
  "  flight's details.",
  "  With Flight Tracker on, the",
  "  header shows remaining",
  "  OpenSky credits:",
  "  CRP = radar polling,",
  "  CRL = route lookup,",
  "  CFT = flight tracking.",
  "  If radar-polling credits",
  "  run out, polling backs off",
  "  to a 15-min recovery check",
  "  until they refill (daily).",
  "  Colors: grey healthy,",
  "  yellow below 500 (or ? if",
  "  not fetched yet), pink",
  "  below 50. Tap for details.",
  "  A dotted line shows the",
  "  plane's past ground track.",
  "  Route & track data come",
  "  from third-party feeds",
  "  (ADSB.lol, OpenSky) and",
  "  may be stale or wrong.",
  "Govee pool temp (opt, off):",
  "  thermometer temp plus a",
  "  history graph with low /",
  "  average / high.",
  "  Tap the Pool reading to",
  "  open its history graph.",
  "Weather temp history: the",
  "  current temperature is",
  "  logged to flash every 10",
  "  min and graphed with low /",
  "  average / high. Always on.",
  "  Tap the big temperature on",
  "  the idle screen to open it.",
  "Sleep: deep-sleeps overnight",
  "and wakes on touch. A short",
  "chime plays on power-on (not",
  "  on deep-sleep wake); a",
  "  chime also plays after a",
  "  firmware update.",
  "Alarms: tap the header clock.",
  "  Up to 6 alarms with weekday",
  "  masks (all off = one-time)",
  "  and a Notify pattern: 50",
  "  LED + sound alerts: blinks,",
  "  chimes, sirens, sweeps, Morse,",
  "  and tunes (Nokia, Tetris,",
  "  Zelda, Charge, Two Bits).",
  "  Sound needs an external",
  "  speaker on the JST port;",
  "  without one the LED flashes",
  "  only. A bell icon shows beside",
  "  the clock when any alarm is on.",
  "  On fire: Dismiss or Snooze",
  "  (5 min). An alarm snoozes",
  "  up to 3x then dismisses",
  "  for the day. Alarms fire",
  "  on time during Sleep Mode",
  "  and survive power loss.",
  "",
  "GETTING STARTED",
  "Upload a compiled image to",
  "the device first - see",
  "DEVELOPER.md for build and",
  "flash instructions.",
  "Fresh device / after Factory",
  "  Reset: it calibrates touch,",
  "  then asks for your WiFi.",
  "To redo either later:",
  "  Calibrate: Settings ->",
  "    Calibrate Touch, or",
  "    hold the screen 10 sec.",
  "  Network: Settings ->",
  "    Network.",
  "Weather and flights work",
  "out of the box. Adding your",
  "OpenSky credentials raises",
  "the rate limit and removes",
  "the yellow 'anonymous'",
  "warning. Optional",
  "credentials below.",
  "Airline logos (optional):",
  "  see DEVELOPER.md ->",
  "  'Airline logos'.",
  "",
  "TYPING",
  "On a keyboard, tap inside",
  "the text field to place the",
  "cursor, then type or delete",
  "in the middle of a value.",
  "Shift cycles: lowercase ->",
  "Shift (one capital letter) ->",
  "CAPS (capitals until tapped",
  "again) -> lowercase.",
  "",
  "GETTING CREDENTIALS",
  "WiFi: from your router - the",
  "   network name and password.",
  "OpenSky (flights, optional):",
  "   free account at",
  "   opensky-network.org -> My",
  "   OpenSky -> Account; making",
  "   an API client downloads a",
  "   file with your client ID",
  "   and secret.",
  "Govee (pool, optional): free",
  "   developer account at",
  "   developer.govee.com;",
  "   generate an API key.",
  "",
  "SETTINGS GUIDE (defaults)",
  "About: version, author, update.",
  "Calibrate Touch: if taps land",
  "   in the wrong spot, rerun it.",
  "Flight Tracker (on): radius",
  "   3.5 (mi/km/nm: how far",
  "   away to look), ceiling",
  "   15000 (ft/m: ignore planes",
  "   above this), poll 30s,",
  "   timer bar on/off (off),",
  "   Home Airport (ICAO),",
  "   Watch Callsign (ICAO) +",
  "   its Notify pattern (Radar);",
  "   matches any part of a",
  "   callsign (* = every",
  "   flight); Show IATA",
  "   Airports (on); LED",
  "   blink per flight (on).",
  "General: auto-update (on),",
  "   theme color picker (blue)",
  "   - also colors buttons;",
  "   units Imperial/Metric/",
  "   Aviation (also weather F/C);",
  "   clock 12h or 24h;",
  "   Notify Volume: levels",
  "   1-10 (default 10)",
  "   for all speaker sounds.",
  "Location: your coordinates;",
  "   IP guess on first boot.",
  "   Search Address keeps your",
  "   last search so you can fix it.",
  "Network: WiFi network plus",
  "   IP Setup - DHCP or a",
  "   static IP, mask, gateway,",
  "   DNS and hostname. The",
  "   list shows 'Scanning'",
  "   while it searches.",
  "Pool Temp: off; add Govee key,",
  "   pick thermometer.",
  "   (off = no data collected)",
  "Reset: Settings clears",
  "   settings & credentials",
  "   (keeps touch calibration).",
  "   Network clears WiFi, IP",
  "   & hostname only.",
  "   Graph Data clears pool &",
  "   weather temp history.",
  "   Factory Reset clears ALL",
  "   incl. touch cal - airline",
  "   logos must be regenerated",
  "   & reloaded from a dev",
  "   computer. Restart reboots;",
  "   Cancel keeps all.",
  "Sleep Mode: on; 10 PM - 8 AM,",
  "   wake 5 min.",
  "",
  "TROUBLESHOOTING",
  "Screen not accurate? Press and",
  "hold anywhere 10 sec to",
  "recalibrate touch.",
  "",
  "Border colors: red = critical",
  "(no WiFi, bad OpenSky creds,",
  "radar credits exhausted, pool",
  "unavailable); yellow = OpenSky",
  "anonymous (flights still work).",
  "Wrong weather or no flights?",
  "   fix your location under",
  "   Settings -> Location.",
  "Route/track looks wrong? They",
  "   come from third-party feeds",
  "   and may be stale or wrong.",
  "New WiFi network? Re-enter it",
  "   under Settings -> Network.",
  "",
  "UPDATES",
  "Auto-Update (General) checks",
  "once per day for release",
  "builds. About shows a newer",
  "version and Install updates now.",
  "Do not power off during an",
  "update. A failed update rolls",
  "back to the previous version.",
};
static const int kNumHelpLines = sizeof(kHelpLines) / sizeof(kHelpLines[0]);

void drawHelp() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Help");
  backBtn("Back");

  const int viewH = HELP_BOT - HELP_TOP;
  const int contentH = kNumHelpLines * HELP_LINEH;
  int maxScroll = contentH - viewH;
  if (maxScroll < 0) maxScroll = 0;
  if (g_helpScroll > maxScroll) g_helpScroll = maxScroll;
  if (g_helpScroll < 0) g_helpScroll = 0;

  // Text clipped to the scroll area (absolute coords preserved).
  tft.setViewport(HELP_X, HELP_TOP, HELP_W, viewH, false);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(1);
  for (int i = 0; i < kNumHelpLines; i++) {
    int y = i * HELP_LINEH - g_helpScroll;
    if (y < -HELP_LINEH || y > viewH) continue;
    tft.setCursor(HELP_X, HELP_TOP + y);
    tft.print(kHelpLines[i]);
  }
  tft.resetViewport();

  // Scrollbar (right side).
  tft.fillRect(HELP_SB_X, HELP_TOP, HELP_SB_W, viewH, TFT_DARKGREY);
  int thumbH = (int)((long)viewH * viewH / contentH);
  if (thumbH < 16) thumbH = 16;
  if (thumbH > viewH) thumbH = viewH;
  int thumbY = HELP_TOP + (maxScroll > 0 ? ((viewH - thumbH) * g_helpScroll) / maxScroll : 0);
  tft.fillRoundRect(HELP_SB_X, thumbY, HELP_SB_W, thumbH, 3, TFT_LIGHTGREY);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(1);
  tft.setCursor(8, HELP_BOT + 2);
  tft.print("Tap right bar above/below the thumb to scroll.");
}

void handleHelpTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; dirty = true; return; }  // Back

  const int viewH = HELP_BOT - HELP_TOP;
  const int contentH = kNumHelpLines * HELP_LINEH;
  int maxScroll = contentH - viewH;
  if (maxScroll < 0) maxScroll = 0;

  // Tapping the scrollbar track: above the thumb scrolls up, below scrolls down.
  if (x >= HELP_SB_X && x <= HELP_SB_X + HELP_SB_W && y >= HELP_TOP && y <= HELP_BOT) {
    int thumbH = (int)((long)viewH * viewH / contentH);
    if (thumbH < 16) thumbH = 16;
    if (thumbH > viewH) thumbH = viewH;
    int thumbY = HELP_TOP + (maxScroll > 0 ? ((viewH - thumbH) * g_helpScroll) / maxScroll : 0);
    if (y < thumbY) g_helpScroll -= (int)(viewH * 0.6);
    else if (y > thumbY + thumbH) g_helpScroll += (int)(viewH * 0.6);
    if (g_helpScroll < 0) g_helpScroll = 0;
    if (g_helpScroll > maxScroll) g_helpScroll = maxScroll;
    dirty = true;
  }
}

// ---- General page ----
// ---- Clock Color picker (SCR_COLORPICK) ----
// Tap-only palette: hue row, shades of the picked hue (pale->pure->dark), and
// a greyscale ramp - any color in <=2 taps, live header-band preview. Stored
// as RGB565 in NVS "clkcol" and drives every theme-colored button.
int g_pickH = 210;    // 0-359
int g_pickS = 255;    // 0-255
int g_pickV = 220;    // 0-255

// HSV -> RGB565.
uint16_t hsv565(int h, int s, int v) {
  h = ((h % 360) + 360) % 360;
  int reg = h / 60;
  int f = (h % 60) * 255 / 60;
  int p = v * (255 - s) / 255;
  int q = v * (255 - s * f / 255) / 255;
  int t = v * (255 - s * (255 - f) / 255) / 255;
  int r, g, b;
  switch (reg) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Seed the picker's axes from the current RGB565 theme color; a pure grey has
// no hue, so keep the last one (else the hue marker jumps to red).
void colorPickEnter() {
  int h, s; colorHS(g_clockCol, h, s);
  if (s) g_pickH = h;
  g_pickS = s;
  g_pickV = max(((g_clockCol >> 11) & 31) * 255 / 31,
             max(((g_clockCol >> 5) & 63) * 255 / 63,
                 (g_clockCol & 31) * 255 / 31));
}

#define PICK_X0 16
#define PICK_W (DISP_W - 32)
#define PICK_COLS 12
#define PICK_CW (PICK_W / PICK_COLS)   // 24 px cells
#define PICK_RH 30
#define PICK_GAP 8
#define PICK_HUE_Y 84
#define PICK_SHADE_Y (PICK_HUE_Y + PICK_RH + PICK_GAP)
#define PICK_GREY_Y (PICK_SHADE_Y + PICK_RH + PICK_GAP)

// Shade swatch i of the picked hue: first half ramps pale -> pure,
// second half pure -> near black.
uint16_t pickShade(int i) {
  int s = i < PICK_COLS / 2 ? 40 + i * 43 : 255;
  int v = i < PICK_COLS / 2 ? 255 : 255 - (i - PICK_COLS / 2 + 1) * 37;
  return hsv565(g_pickH, s, v);
}

// Greyscale swatch i: white -> black.
uint16_t pickGrey(int i) {
  return hsv565(0, 0, 255 - i * 255 / (PICK_COLS - 1));
}

// Selection ring: black outer + white inner reads on any swatch color.
void pickMark(int i, int y) {
  tft.drawRect(PICK_X0 + i * PICK_CW - 2, y - 2, PICK_CW + 2, PICK_RH + 4, TFT_BLACK);
  tft.drawRect(PICK_X0 + i * PICK_CW - 1, y - 1, PICK_CW,     PICK_RH + 2, TFT_WHITE);
}

void drawColorPick() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Clock Color");

  backBtn("Back");

  // Live preview: the picked color drawn as the dashboard header band, with
  // text in the same adaptive color the real header uses.
  uint16_t pc = hsv565(g_pickH, g_pickS, g_pickV);
  uint16_t pfg = btnFg(pc);
  tft.fillRect(8, 36, DISP_W - 16, 34, pc);
  tft.setTextColor(pfg, pc);
  tft.setTextFont(2);
  tft.setCursor(14, 47);
  tft.print("Sun Sep 13");
#ifdef CYD_E32R40T
  tft.setTextFont(6);          // preview the real header clock font
  tft.setCursor(150, 40);
#else
  tft.setTextSize(2);
  tft.setCursor(150, 38);
#endif
  tft.print("04:23");
  tft.setTextSize(1);
  tft.setTextFont(1);
  tft.setCursor(RX(236), 42);
  tft.print("PM");

  // Hue row: 12 full-saturation hues; the picked hue's cell is ringed.
  for (int i = 0; i < PICK_COLS; i++)
    tft.fillRect(PICK_X0 + i * PICK_CW, PICK_HUE_Y, PICK_CW - 2, PICK_RH,
                 hsv565(i * 360 / PICK_COLS, 255, 255));
  pickMark(constrain(g_pickH * PICK_COLS / 360, 0, PICK_COLS - 1), PICK_HUE_Y);

  // Shade row (of the picked hue) and greyscale row; the cell matching the
  // current pick gets the ring.
  for (int i = 0; i < PICK_COLS; i++) {
    uint16_t sc = pickShade(i), gc = pickGrey(i);
    tft.fillRect(PICK_X0 + i * PICK_CW, PICK_SHADE_Y, PICK_CW - 2, PICK_RH, sc);
    tft.fillRect(PICK_X0 + i * PICK_CW, PICK_GREY_Y,  PICK_CW - 2, PICK_RH, gc);
    if (sc == pc) pickMark(i, PICK_SHADE_Y);
    if (gc == pc) pickMark(i, PICK_GREY_Y);
  }

  // Hex readout + hint.
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(PICK_X0, PICK_GREY_Y + PICK_RH + 10);
  tft.printf("0x%04X  rgb(%d,%d,%d)", pc,
             (pc >> 11 & 31) * 255 / 31, (pc >> 5 & 63) * 255 / 63, (pc & 31) * 255 / 31);
  tft.setCursor(PICK_X0, PICK_GREY_Y + PICK_RH + 24);
  tft.print("Tap a hue, then a shade or grey.");
}

void handleColorPickTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_GENERAL; dirty = true; return; }
  if (!inRect(x, y, PICK_X0, PICK_HUE_Y - 4, PICK_X0 + PICK_W - 1, PICK_GREY_Y + PICK_RH + 4)) return;
  int i = constrain((int)(x - PICK_X0) * PICK_COLS / PICK_W, 0, PICK_COLS - 1);
  if (inRect(x, y, PICK_X0, PICK_HUE_Y - 4, PICK_X0 + PICK_W - 1, PICK_HUE_Y + PICK_RH + 4)) {
    g_pickH = i * 360 / PICK_COLS;
    if (!g_pickS) { g_pickS = 255; g_pickV = 255; }   // grey has no hue - jump to full color
  } else if (inRect(x, y, PICK_X0, PICK_SHADE_Y - 4, PICK_X0 + PICK_W - 1, PICK_SHADE_Y + PICK_RH + 4)) {
    if (i < PICK_COLS / 2) { g_pickS = 40 + i * 43; g_pickV = 255; }
    else                   { g_pickS = 255; g_pickV = 255 - (i - PICK_COLS / 2 + 1) * 37; }
  } else if (inRect(x, y, PICK_X0, PICK_GREY_Y - 4, PICK_X0 + PICK_W - 1, PICK_GREY_Y + PICK_RH + 4)) {
    g_pickS = 0;
    g_pickV = 255 - i * 255 / (PICK_COLS - 1);
  } else return;
  g_clockCol = hsv565(g_pickH, g_pickS, g_pickV);
  prefs.begin("flight", false); prefs.putUInt("clkcol", g_clockCol); prefs.end();
  dirty = true;
}

void drawGeneral() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > General");

  backBtn("Back");

  // Clock Color: label + swatch of the current color (tap opens the picker);
  // the pick is also the theme color for ordinary buttons.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 40);
  tft.print("Clock Color");
  uint16_t cc = g_clockCol;   // raw picked color (not the button-adjusted one)
  tft.fillRoundRect(RX(190), 36, 122, 24, 5, cc);
  tft.drawRoundRect(RX(189), 35, 124, 26, 5, TFT_WHITE);

  // Auto-Update toggle
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 72);
  tft.print("Auto-Update");
  tft.setTextColor(g_autoUpdate ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(150, 72);
  tft.print(g_autoUpdate ? "ON" : "OFF");
  themeBtn(RX(230), 68, 82, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(250), 75);
  tft.print("Toggle");

  // Notify Volume: stepper over kNtfVolLevels (shown 1-10) driving all speaker
  // sounds via LEDC duty; stepping plays a test beep (notifyTestBeep).
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 104);
  tft.print("Notify Volume");
  char vb[8];
  snprintf(vb, sizeof vb, "%d", ntfVolIdx(g_notifyVol) + 1);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.drawCentreString(vb, RX(200), 104, 2);
  adjPair(TADJ_MX, 100);

  // Units cycle: Imperial / Metric / Aviation. Drives flight distances,
  // speeds and altitudes plus weather/pool temperatures (F or C).
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 136);
  tft.print("Units");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setCursor(150, 136);
  tft.print(unitsName());
  themeBtn(RX(230), 132, 82, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(250), 139);
  tft.print("Toggle");

  // Clock format: 12h (AM/PM marker) or 24h.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 168);
  tft.print("Clock");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setCursor(150, 168);
  tft.print(g_clock24 ? "24h" : "12h");
  themeBtn(RX(230), 164, 82, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(250), 171);
  tft.print("Toggle");

  tft.setTextFont(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(8, 196);
  tft.print("Auto-update checks & installs daily.");
  tft.setCursor(8, 206);
  tft.print("Units also set weather/pool F or C.");
  tft.setCursor(8, 216);
  tft.print("24h clock hides the AM/PM marker.");
}

void handleGeneralTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; dirty = true; return; }
  if (inRect(x, y, RX(230), 68, RX(312), 92)) {  // Auto-Update toggle
    g_autoUpdate = !g_autoUpdate;
    prefs.begin("flight", false); prefs.putBool("autoupd", g_autoUpdate); prefs.end();
    // Turning auto-update ON clears the last-scan date so it can try again today.
    if (g_autoUpdate) {
      g_lastScanDay = 0;
      prefs.begin("flight", false); prefs.putULong("lastscan", 0); prefs.end();
      // Scan promptly rather than waiting out the daily jitter; the alarm
      // quiet window still applies when it fires.
      g_autoScanAt = time(nullptr);
      g_autoScanForced = false;
    }
    dirty = true;
    return;
  }
  // Clock Color: tap the swatch -> swatch-row color picker
  if (inRect(x, y, RX(180), 32, RX(316), 64)) {
    colorPickEnter();
    g_screen = SCR_COLORPICK;
    dirty = true;
    return;
  }
  int nv = adjPairHit(x, y, TADJ_MX, 100);   // Notify Volume stepper
  if (nv) {
    notifyVolStep(nv);   // cycles the fixed levels, wrapping
    saveInt("ntfvol", g_notifyVol);
    notifyTestBeep();   // play a short beep at the new level
    dirty = true;
    return;
  }
  if (inRect(x, y, RX(230), 132, RX(312), 156)) {  // Units cycle: Imperial/Metric/Aviation
    // Keep the displayed numbers and reinterpret them in the new unit
    // (3.5 mi -> 3.5 km), re-storing them in miles / feet.
    float rDisp = g_radiusMi * distConv();
    float cDisp = g_ceilingFt * altConv();
    g_units = (g_units + 1) % 3;
    g_radiusMi  = constrain(rDisp, 1.0f, 10.0f) / distConv();
    g_ceilingFt = (int)roundf(constrain(cDisp, 3000.0f, 99000.0f) / altConv());
    prefs.begin("flight", false);
    prefs.putInt("units", g_units);
    prefs.putFloat("radius", g_radiusMi);
    prefs.putInt("ceiling", g_ceilingFt);
    prefs.end();
    dirty = true;
    return;
  }
  if (inRect(x, y, RX(230), 164, RX(312), 188)) {  // Clock 12h/24h toggle
    g_clock24 = !g_clock24;
    prefs.begin("flight", false); prefs.putBool("clock24", g_clock24); prefs.end();
    dirty = true;
    return;
  }
}

// ---- Location page ----
void drawLocation() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Location");

  backBtn("Back");

  // current coordinates + edit button
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 44);
  tft.print("Lat, Lon");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextFont(1);
  tft.setCursor(8, 62);
  tft.printf("%.4f, %.4f", g_lat, g_lon);
  themeBtn(RX(240), 40, 72, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(254), 47);
  tft.print("Set");

  // Search by address
  themeBtn(10, 96, DISP_W - 20, 34, 6);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(2);
  tft.setCursor(20, 104);
  tft.print("Search Address");

  // Find by IP
  themeBtn(10, 140, DISP_W - 20, 34, 6);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setCursor(20, 148);
  tft.print("Find by IP");

  // Confirmation / error from the last address search
  tft.setTextFont(1);
  if (g_lastPlace.length()) {
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.setCursor(8, 184);
    tft.print("Location:");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 196);
    tft.print(g_lastPlace.substring(0, 38));
  } else if (String(lastErr) == "no match") {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(8, 184);
    tft.print("No match for that address.");
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 196);
    tft.print("Search again or set lat/lon.");
  } else {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 190);
    tft.print("Find by IP guesses location from");
    tft.setCursor(8, 200);
    tft.print("the network the device is on.");
  }
}

// Flight Tracker settings screen
void drawFtracker() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Flight Tracker");

  backBtn("Back");

  if (g_ftPage == 0) {
    // Page 1: Enabled + sliders (Units moved to General)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(8, 40);
    tft.print("Enabled");
    tft.setTextColor(g_trackEnabled ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(150, 40);
    tft.print(g_trackEnabled ? "ON" : "OFF");
    themeBtn(RX(230), 36, 82, 24, 5);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(FONT_AUX);
    tft.setCursor(RX(250), 43);
    tft.print("Toggle");

    // Radius and ceiling are shown and edited in the selected unit
    // (mi/km/nm, ft/m) but stored in miles / feet. Units live under General.
    char rlbl[20], clbl[20];
    snprintf(rlbl, sizeof rlbl, "Radius (%s)", distUnit());
    snprintf(clbl, sizeof clbl, "Ceiling (%s)", altUnit());
    drawSlider(76, rlbl, g_radiusMi * distConv(), 1.0f, 10.0f, 0.5f, 1);
    drawSlider(124, clbl, g_ceilingFt * altConv(), 3000, 99000, 1000, 0);
    drawSlider(172, "Poll (s)", (float)g_pollSec, 10, 300, 10, 0);
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 102); tft.print("how far away to look");
    tft.setCursor(8, 150); tft.print("ignore planes above this");
    tft.setCursor(8, 198); tft.print("how often to check for planes");
  } else if (g_ftPage == 1) {
    // Page 2: toggles + OpenSky credentials button
    // Blink for Flight toggle
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(8, 40);
    tft.print("Blink for Flight");
    tft.setTextColor(g_blinkForFlight ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(150, 40);
    tft.print(g_blinkForFlight ? "ON" : "OFF");
    themeBtn(RX(230), 36, 82, 24, 5);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(FONT_AUX);
    tft.setCursor(RX(250), 43);
    tft.print("Toggle");

    // Enable Timer toggle
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(8, 76);
    tft.print("Enable Timer");
    tft.setTextColor(g_showTimer ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(150, 76);
    tft.print(g_showTimer ? "ON" : "OFF");
    themeBtn(RX(230), 72, 82, 24, 5);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(FONT_AUX);
    tft.setCursor(RX(250), 79);
    tft.print("Toggle");

    // Show IATA Airports toggle
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(8, 112);
    tft.print("Show IATA Airports");
    tft.setTextColor(g_showIata ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(150, 112);
    tft.print(g_showIata ? "ON" : "OFF");
    themeBtn(RX(230), 108, 82, 24, 5);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(FONT_AUX);
    tft.setCursor(RX(250), 115);
    tft.print("Toggle");

    // OpenSky credentials entry
    themeBtn(10, 150, DISP_W - 20, 24, 6);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(FONT_AUX);
    tft.drawCentreString("OpenSky Credentials", CX, 155, FONT_AUX);
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 184);
    tft.print("Client ID/secret for a higher");
    tft.setCursor(8, 195);
    tft.print("OpenSky rate limit.");
  } else {
    // Page 3: Home-Airport route setting + Watch Callsign
    drawEditRow(48, "Home Airport (ICAO)", g_homeAirport.length() ? g_homeAirport : "--");
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 76);
    tft.print("Used to blink red/green for");
    tft.setCursor(8, 87);
    tft.print("departures/arrivals. Empty = off.");

    drawEditRow(124, "Watch Callsign (ICAO)", g_watchCallsign.length() ? g_watchCallsign : "--");
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 152);
    tft.print("Matches any part; * = every flight.");
    tft.setCursor(8, 163);
    tft.print("Alerts while its flight is shown.");

    // Callsign Notify preset: same LED + speaker patterns as alarm
    // Notify. Works even when "Blink for Flight" is off.
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 184);
    tft.print("Callsign Notify");
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.drawCentreString(alarmPresetName(g_watchNotify), RX(205), 184, 2);
    adjPair(TADJ_MX, 178);
  }

  // Pager (bottom): left/right arrows + page indicator
  tft.setTextFont(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(CX - 20, 222);
  tft.printf("Page %d/3", g_ftPage + 1);
  if (g_ftPage > 0) {
    themeBtn(12, 214, 44, 26, 6);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(2);
    tft.setCursor(27, 220);
    tft.print("<");
  }
  if (g_ftPage < 2) {
    themeBtn(RX(264), 214, 44, 26, 6);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setTextFont(2);
    tft.setCursor(RX(279), 220);
    tft.print(">");
  }
}

void handleFtrackerTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; dirty = true; return; }
  // Pager arrows
  if (inRect(x, y, 12, 214, 56, 240)) { if (g_ftPage > 0) { g_ftPage--; dirty = true; } return; }
  if (inRect(x, y, RX(264), 214, RX(308), 240)) { if (g_ftPage < 2) { g_ftPage++; dirty = true; } return; }

  if (g_ftPage == 0) {
    if (inRect(x, y, RX(230), 36, RX(312), 60)) {  // Enabled toggle
      g_trackEnabled = !g_trackEnabled;
      prefs.begin("flight", false); prefs.putBool("track", g_trackEnabled); prefs.end();
      dirty = true;
      return;
    }
    // Radius / Ceiling / Poll sliders. Radius and ceiling step in the
    // displayed unit and are stored in miles / feet.
    if      (rowMinus(x, y, 76))  { g_radiusMi  = constrain(g_radiusMi * distConv() - 0.5f, 1.0f, 10.0f) / distConv(); saveFloat("radius", g_radiusMi); }
    else if (rowPlus(x, y, 76))   { g_radiusMi  = constrain(g_radiusMi * distConv() + 0.5f, 1.0f, 10.0f) / distConv(); saveFloat("radius", g_radiusMi); }
    else if (rowMinus(x, y, 124)) { g_ceilingFt = (int)roundf(constrain(g_ceilingFt * altConv() - 1000.0f, 3000.0f, 99000.0f) / altConv()); saveInt("ceiling", g_ceilingFt); }
    else if (rowPlus(x, y, 124))  { g_ceilingFt = (int)roundf(constrain(g_ceilingFt * altConv() + 1000.0f, 3000.0f, 99000.0f) / altConv()); saveInt("ceiling", g_ceilingFt); }
    else if (rowMinus(x, y, 172)) { g_pollSec   = constrain(g_pollSec - 10, 10, 300);         saveInt("poll", g_pollSec); }
    else if (rowPlus(x, y, 172))  { g_pollSec   = constrain(g_pollSec + 10, 10, 300);         saveInt("poll", g_pollSec); }
    return;
  }

  if (g_ftPage == 1) {
    // Page 2: toggles + OpenSky credentials button
    if (inRect(x, y, RX(230), 36, RX(312), 60)) {  // Blink for Flight toggle
      g_blinkForFlight = !g_blinkForFlight;
      prefs.begin("flight", false); prefs.putBool("blinkf", g_blinkForFlight); prefs.end();
      dirty = true;
      return;
    }
    if (inRect(x, y, RX(230), 72, RX(312), 96)) {  // Enable Timer toggle
      g_showTimer = !g_showTimer;
      prefs.begin("flight", false); prefs.putBool("timer", g_showTimer); prefs.end();
      dirty = true;
      return;
    }
    if (inRect(x, y, RX(230), 108, RX(312), 132)) {  // Show IATA toggle
      g_showIata = !g_showIata;
      prefs.begin("flight", false); prefs.putBool("showiata", g_showIata); prefs.end();
      dirty = true;
      return;
    }
    if (inRect(x, y, 10, 150, DISP_W - 10, 174)) {  // OpenSky credentials
      g_screen = SCR_WIFI;
      g_wifiSub = 3;
      dirty = true;
      return;
    }
    return;
  }

  // Page 3: edit rows + callsign notification preset stepper
  int np = adjPairHit(x, y, TADJ_MX, 178);
  if (np) {
    g_watchNotify = alarmPresetStep(g_watchNotify, np);   // alphabetical order
    previewAlarmLed(g_watchNotify);   // play the picked pattern once on LED + speaker
    saveInt("watchntf", g_watchNotify);
    dirty = true;
    return;
  }
  if (inRect(x, y, RX(270), 48, RX(312), 72)) {  // Edit home airport
    g_screen = SCR_WIFI;
    g_wifiSub = 11;
    dirty = true;
    return;
  }
  if (inRect(x, y, RX(270), 124, RX(312), 148)) {  // Edit watch callsign
    g_screen = SCR_WIFI;
    g_wifiSub = 13;
    dirty = true;
    return;
  }
}

// Reset screen. Two steps: choose what to reset (Factory / Settings / Graph Data / Network / Restart / Cancel),
// then a confirmation prompt before anything is wiped and the device reboots.
void drawReset() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, TFT_MAROON);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Reset");

  if (g_resetConfirm != 0) {
    // Confirmation prompt for the chosen reset.
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(10, 64);
    tft.print("Are you sure?");
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    int ty = 96;
    if (g_resetConfirm == 1) {          // Factory Reset: full wipe incl. calibration
      tft.setCursor(10, ty); tft.print("Factory Reset: settings,"); ty += 12;
      tft.setCursor(10, ty); tft.print("files, credentials,"); ty += 12;
      tft.setCursor(10, ty); tft.print("logos, calibration, and"); ty += 12;
      tft.setCursor(10, ty); tft.print("pool/weather history"); ty += 12;
      tft.setCursor(10, ty); tft.print("will be cleared."); ty += 12;
      tft.setCursor(10, ty); tft.print("The device will reboot."); ty += 12;
    } else if (g_resetConfirm == 2) {   // Settings: settings & credentials only
      tft.setCursor(10, ty); tft.print("Settings: settings and"); ty += 12;
      tft.setCursor(10, ty); tft.print("credentials will be cleared."); ty += 12;
      tft.setCursor(10, ty); tft.print("The device will reboot."); ty += 12;
    } else if (g_resetConfirm == 3) {   // Graph Data: history only
      tft.setCursor(10, ty); tft.print("Graph Data: pool &"); ty += 12;
      tft.setCursor(10, ty); tft.print("weather temperature history"); ty += 12;
      tft.setCursor(10, ty); tft.print("will be deleted."); ty += 12;
      tft.setCursor(10, ty); tft.print("The device will reboot."); ty += 12;
    } else if (g_resetConfirm == 5) {   // Network: WiFi creds + IP config only
      tft.setCursor(10, ty); tft.print("Network: WiFi credentials,"); ty += 12;
      tft.setCursor(10, ty); tft.print("IP settings and hostname"); ty += 12;
      tft.setCursor(10, ty); tft.print("will be cleared; all other"); ty += 12;
      tft.setCursor(10, ty); tft.print("settings are kept."); ty += 12;
      tft.setCursor(10, ty); tft.print("The device will reboot."); ty += 12;
    } else {                            // Restart: no data change
      tft.setCursor(10, ty); tft.print("Restart the device?"); ty += 12;
      tft.setCursor(10, ty); tft.print("No settings will be cleared."); ty += 12;
    }
    tft.setCursor(10, ty);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("This cannot be undone.");
    // Yes / No
    tft.fillRoundRect(30, 180, 110, 34, 6, dangerCol());
    tft.setTextColor(btnFg(dangerCol()), dangerCol());
    tft.setTextFont(2);
    tft.setCursor(70, 189);
    tft.print("Yes");
    themeBtn(CX + 20, 180, 110, 34, 6);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.setCursor(CX + 62, 189);
    tft.print("No");
    return;
  }

  // Choose what to reset: descriptions on the left, action buttons stacked on
  // the right so the destructive options can't be confused with Cancel.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(10, 40);
  tft.print("Choose what to reset:");
  tft.setTextFont(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  int ly = 64;
  tft.setCursor(10, ly); tft.print("Factory Reset: all"); ly += 10;
  tft.setCursor(10, ly); tft.print("  settings, files,"); ly += 10;
  tft.setCursor(10, ly); tft.print("  logos & touch cal."); ly += 12;
  tft.setCursor(10, ly); tft.print("Graph Data: pool &"); ly += 10;
  tft.setCursor(10, ly); tft.print("  weather history."); ly += 12;
  tft.setCursor(10, ly); tft.print("Settings: settings &"); ly += 10;
  tft.setCursor(10, ly); tft.print("  credentials."); ly += 12;
  tft.setCursor(10, ly); tft.print("Network: WiFi creds,"); ly += 10;
  tft.setCursor(10, ly); tft.print("  IP & hostname."); ly += 12;
  tft.setCursor(10, ly); tft.print("Cancel: back without"); ly += 10;
  tft.setCursor(10, ly); tft.print("  changing anything."); ly += 12;
  tft.setCursor(10, ly); tft.print("Restart: reboots the"); ly += 10;
  tft.setCursor(10, ly); tft.print("  device cleanly."); ly += 12;

  // Right column: Factory Reset / Graph Data / Settings / Network stacked
  // top-right and spaced apart.  Bottom row has Restart (left) and Cancel (right).
  tft.setTextFont(2);
  tft.fillRoundRect(RX(172), 36, 140, 30, 6, dangerCol());
  tft.setTextColor(btnFg(dangerCol()), dangerCol());
  tft.drawCentreString("Factory Reset", RX(242), 43, 2);

  tft.fillRoundRect(RX(172), 80, 140, 30, 6, TFT_OLIVE);
  tft.setTextColor(TFT_WHITE, TFT_OLIVE);
  tft.drawCentreString("Graph Data", RX(242), 87, 2);

  tft.fillRoundRect(RX(172), 124, 140, 30, 6, TFT_ORANGE);
  tft.setTextColor(TFT_WHITE, TFT_ORANGE);
  tft.drawCentreString("Settings", RX(242), 131, 2);

  tft.fillRoundRect(RX(172), 168, 140, 30, 6, TFT_PURPLE);
  tft.setTextColor(TFT_WHITE, TFT_PURPLE);
  tft.drawCentreString("Network", RX(242), 175, 2);

  tft.fillRoundRect(10, 210, 140, 26, 6, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawCentreString("Restart", 80, 216, 2);

  themeBtn(RX(172), 210, 140, 26, 6);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.drawCentreString("Cancel", RX(242), 216, 2);
}

// Sleep Mode settings screen
void drawSleep() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.print("Settings > Sleep Mode");

  backBtn("Back");

  // Enabled toggle
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 44);
  tft.print("Enabled");
  tft.setTextColor(g_sleepOn ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(150, 44);
  tft.print(g_sleepOn ? "ON" : "OFF");
  themeBtn(RX(230), 40, 82, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(250), 47);
  tft.print("Toggle");

  drawTimeAdj(84, "Start", g_sleepStartH, g_sleepStartM);
  drawTimeAdj(124, "End", g_sleepEndH, g_sleepEndM);

  // Wake Min: label + value + [▼][▲] stepper (±1, range 1-120)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 172);
  tft.print("Wake Min");
  char wb[8];
  snprintf(wb, sizeof wb, "%d", g_wakeMin);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.drawCentreString(wb, RX(200), 172, 2);
  adjPair(TADJ_MX, 164);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(1);
  tft.setCursor(8, 214);
#if TOUCH_IRQ_ENABLED
  tft.print("Deep sleep; pool temp keeps logging. Touch");
  tft.setCursor(8, 224);
  tft.print("wakes it for the wake duration, then sleeps.");
#else
  tft.print("Deep sleep; pool temp keeps logging. Ends at");
  tft.setCursor(8, 224);
  tft.print("the scheduled time (touch-wake needs wiring).");
#endif
}

void drawEditRow(int y, const char* label, const String& value) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, y);
  tft.print(label);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setCursor(RX(210), y + 5);
  tft.print(value);
  themeBtn(RX(270), y, 42, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(280), y + 6);
  tft.print("Edit");
}

// ---- shared time-of-day editor ----
// One ~30px row (allow ~40px): label, hour stepper, tappable "H:MM AM/PM"
// (opens the HHMM keyboard), minute stepper; hour wraps AM/PM. Used by the
// Alarms editor and Sleep Mode start/end. TADJ_* geometry lives in
// cyd-horizon.ino (alarms.ino concats before this file).

// One horizontal [▼][▲] stepper pair: ▼ at (x,y), ▲ TADJ_BW+4 to its right;
// 24px horizontal targets hit easier than stacked arrows.
void adjPair(int x, int y) {
  uint16_t fg = btnFg(btnCol());
  themeBtn(x, y, TADJ_BW, 24, 5);
  int c1 = x + TADJ_BW / 2;
  tft.fillTriangle(c1, y + 15, c1 - 4, y + 9, c1 + 4, y + 9, fg);
  themeBtn(x + TADJ_BW + 4, y, TADJ_BW, 24, 5);
  int c2 = x + TADJ_BW + 4 + TADJ_BW / 2;
  tft.fillTriangle(c2, y + 9, c2 - 4, y + 15, c2 + 4, y + 15, fg);
}

// -1 for [▼] (left), +1 for [▲] (right), 0 for a miss.
int adjPairHit(uint16_t x, uint16_t y, int colX, int rowY) {
  if (inRect(x, y, colX, rowY, colX + TADJ_BW - 1, rowY + 23)) return -1;
  if (inRect(x, y, colX + TADJ_BW + 4, rowY, colX + 2 * TADJ_BW + 3, rowY + 23)) return 1;
  return 0;
}

void drawTimeAdj(int y, const char* label, int h24, int m) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, y + 6);
  tft.print(label);
  adjPair(TADJ_HX, y);                  // hour stepper, left of the time
  int hDisp = g_clock24 ? h24 : (h24 % 12 ? h24 % 12 : 12);
  char tb[8];
  snprintf(tb, sizeof tb, "%d:%02d", hDisp, m);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.drawCentreString(tb, (TADJ_HX + 2 * TADJ_BW + 4 + TADJ_MX - 4) / 2, y + 3, 4);
  // Stacked AM/PM indicator like the header's (12h only): active meridiem
  // bright in its slot (AM top), other dimmed; flips at hour wrap.
  if (!g_clock24) {
    tft.setTextFont(1);
    tft.setTextColor(h24 < 12 ? TFT_GREENYELLOW : TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(RX(230), y + 2);
    tft.print("AM");
    tft.setTextColor(h24 < 12 ? TFT_DARKGREY : TFT_GREENYELLOW, TFT_BLACK);
    tft.setCursor(RX(230), y + 18);
    tft.print("PM");
  }
  adjPair(TADJ_MX, y);                  // minute stepper, right of the time
}

// Hit-test a drawTimeAdj row: 1=hour+ 2=hour- 3=min+ 4=min-, 7=the time text
// itself (opens the manual HHMM keyboard), 0=miss.
int timeAdjHit(uint16_t x, uint16_t y, int rowY) {
  int h = adjPairHit(x, y, TADJ_HX, rowY);
  if (h) return h > 0 ? 1 : 2;
  int m = adjPairHit(x, y, TADJ_MX, rowY);
  if (m) return m > 0 ? 3 : 4;
  if (inRect(x, y, TADJ_HX + 2 * TADJ_BW + 6, rowY, TADJ_MX - 4, rowY + 28)) return 7;
  return 0;
}

// Apply a timeAdjHit to the sleep start (which=0) or end (which=1); the
// time-text tap opens the HHMM keyboard (subs 6/7).
void applySleepTimeAdj(int which, int hit) {
  int& H = which ? g_sleepEndH : g_sleepStartH;
  int& M = which ? g_sleepEndM : g_sleepStartM;
  switch (hit) {
    case 1: H = (H + 1) % 24; break;    // hour wrap flips AM/PM on its own
    case 2: H = (H + 23) % 24; break;
    case 3: M = (M + 1) % 60; break;
    case 4: M = (M + 59) % 60; break;
    case 7: g_wifiSub = which ? 7 : 6; g_screen = SCR_WIFI; return;
  }
  // Keep the HHMM edit buffers in sync and persist.
  char b[8];
  snprintf(b, sizeof b, "%02d%02d", H, M);
  prefs.begin("flight", false);
  if (which == 0) { g_sleepStartStr = b; prefs.putInt("sleepsH", H); prefs.putInt("sleepsM", M); }
  else            { g_sleepEndStr   = b; prefs.putInt("sleepeH", H); prefs.putInt("sleepeM", M); }
  prefs.end();
}

void handleSleepTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_screen = SCR_SETTINGS; dirty = true; return; }
  if (inRect(x, y, RX(230), 40, RX(312), 64)) {  // Toggle
    g_sleepOn = !g_sleepOn;
    prefs.begin("flight", false); prefs.putBool("sleepon", g_sleepOn); prefs.end();
    dirty = true;
    return;
  }
  int hit = timeAdjHit(x, y, 84);
  int which = 0;
  if (!hit) { hit = timeAdjHit(x, y, 124); which = 1; }
  if (hit) { applySleepTimeAdj(which, hit); dirty = true; return; }
  int wh = adjPairHit(x, y, TADJ_MX, 164);
  if (wh) { g_wakeMin = constrain(g_wakeMin + wh, 1, 120); saveInt("wake", g_wakeMin); dirty = true; return; }
}

// type 1 = float step, type 0 = int
void drawSlider(int y, const char* label, float val, float vmin, float vmax, float step, int type) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, y);
  tft.print(label);

  // label value [▼][▲]   (value follows the label so unit suffixes fit)
  adjPair(RX(246), y);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8 + tft.textWidth(label, 2) + 14, y + 5);
  if (type) tft.printf("%.1f", val);
  else      tft.print((int)roundf(val));
  (void)vmin; (void)vmax; (void)step;
}
