#include <strings.h>   // strcasecmp (preset sort)

// ---- Alarms ----
// Time-of-day alarms: weekday masks (empty = one-time) + LED/speaker preset;
// persisted nextFire drives firing/snooze (missed firings still go off).
// One NVS blob in "flight". See DEVELOPER.md "Alarms".
#define MAX_ALARMS 6
#define ALARM_SNOOZE_SEC (5 * 60)
#define ALARM_MAX_SNOOZES 3   // snoozes per firing episode; the next request dismisses

struct Alarm {
  uint8_t en;        // enabled
  uint8_t h;         // hour, 0-23
  uint8_t m;         // minute, 0-59
  uint8_t days;      // weekday bitmask, bit 0 = Sunday
  uint8_t preset;    // index into kAlarmPresets
  uint8_t snoozes;   // snoozes taken this firing episode
  uint8_t reserved;
  uint32_t nextFire; // epoch of next firing; 0 = recompute once time is known
};

Alarm g_alarms[MAX_ALARMS];
int   g_alarmCount = 1;
int   g_alarmIdx = 0;        // which alarm the editor screen shows
int   g_fireIdx = -1;        // which alarm is currently firing
bool  g_alarmFiring = false;
bool  g_alarmDelConfirm = false;   // editor is showing the delete confirmation
String g_alarmTimeStr = "0700";   // HHMM buffer for the manual keyboard editor
extern int g_wifiSub;             // defined in wifi_config.ino (concatenated last)

// ---- notification presets ----
// A preset is a looping (freq, ms, rgb) score driving LED + speaker; rgb bits
// 1/2/4 = R/G/B, active-low. NtfStep lives in cyd-horizon.ino for prototypes.

static const NtfStep kPatBlink[] = {   // white flash + beep ~1Hz
  { 988, 500, 7 }, { 0, 500, 0 },
};
static const NtfStep kPatRapid[] = {   // fast white strobe + high beep train
  { 1047, 150, 7 }, { 0, 150, 0 },
};
static const NtfStep kPatDouble[] = {  // two quick blinks/beeps, then a pause
  { 988, 120, 7 }, { 0, 120, 0 }, { 784, 120, 7 }, { 0, 1440, 0 },
};
static const NtfStep kPatColors[] = {  // red -> green -> blue, walking a C triad
  { 523, 350, 1 }, { 659, 350, 2 }, { 784, 350, 4 },
};
static const NtfStep kPatPulse[] = {   // brief flash + two-note chime ~1.6s
  { 880, 160, 7 }, { 0, 160, 0 }, { 659, 160, 7 }, { 0, 1120, 0 },
};
static const NtfStep kPatSimple[] = {  // one soft beep + short flash every ~2s
  { 784, 120, 7 }, { 0, 1880, 0 },
};
static const NtfStep kPatChime[] = {   // doorbell ding-dong (E5 -> C5)
  { 659, 220, 7 }, { 0, 70, 0 }, { 523, 360, 7 }, { 0, 1350, 0 },
};
static const NtfStep kPatSiren[] = {   // red/blue swap with a two-tone siren
  { 880, 320, 1 }, { 660, 320, 4 },
};
static const NtfStep kPatRadar[] = {   // green sonar ping + a fainter echo
  { 1568, 70, 2 }, { 0, 160, 0 }, { 1175, 60, 2 }, { 0, 2110, 0 },
};
static const NtfStep kPatMorse[] = {   // "CYD" in Morse: -.-.  -.--  -..
  { 988, 270, 7 }, { 0, 90, 0 }, { 988, 90, 7 }, { 0, 90, 0 },   // C
  { 988, 270, 7 }, { 0, 90, 0 }, { 988, 90, 7 }, { 0, 270, 0 },
  { 988, 270, 7 }, { 0, 90, 0 }, { 988, 90, 7 }, { 0, 90, 0 },   // Y
  { 988, 270, 7 }, { 0, 90, 0 }, { 988, 270, 7 }, { 0, 270, 0 },
  { 988, 270, 7 }, { 0, 90, 0 }, { 988, 90, 7 }, { 0, 90, 0 },   // D
  { 988, 90, 7 }, { 0, 1010, 0 },
};
static const NtfStep kPatCharge[] = {  // "Charge!" fanfare run + held note
  { 392, 140, 1 }, { 523, 140, 2 }, { 659, 140, 4 }, { 784, 190, 7 },
  { 0, 60, 0 }, { 659, 140, 7 }, { 784, 460, 7 }, { 0, 1060, 0 },
};
static const NtfStep kPatTwoBits[] = { // "shave and a haircut - two bits"
  { 262, 280, 7 }, { 196, 140, 2 }, { 196, 140, 4 }, { 220, 280, 1 },
  { 196, 280, 7 }, { 0, 280, 0 }, { 247, 280, 4 }, { 262, 440, 7 },
  { 0, 800, 0 },
};

// ---- added presets (appended; never reorder - indices are persisted) ----
static const NtfStep kPatStrobe[] = {  // 4x fast white strobe + click
  { 1568, 40, 7 }, { 0, 80, 0 }, { 1568, 40, 7 }, { 0, 80, 0 },
  { 1568, 40, 7 }, { 0, 80, 0 }, { 1568, 40, 7 }, { 0, 1440, 0 },
};
static const NtfStep kPatSlow[] = {    // slow gentle flash + soft C5 every 2s
  { 523, 400, 7 }, { 0, 1600, 0 },
};
static const NtfStep kPatHeart[] = {   // heartbeat lub-dub + double flash
  { 220, 110, 7 }, { 0, 130, 0 }, { 196, 140, 7 }, { 0, 1620, 0 },
};
static const NtfStep kPatTick[] = {    // metronome tick + pinprick flash, 1s
  { 1568, 25, 7 }, { 0, 975, 0 },
};
static const NtfStep kPatCountDn[] = { // 3-2-1 countdown, green "go" beep
  { 659, 120, 1 }, { 0, 380, 0 }, { 659, 120, 3 }, { 0, 380, 0 },
  { 659, 120, 6 }, { 0, 380, 0 }, { 988, 450, 2 }, { 0, 1350, 0 },
};
static const NtfStep kPatBoot[] = {    // the boot chime as a notification
  { 523, 110, 1 }, { 659, 110, 2 }, { 784, 110, 4 }, { 1047, 320, 7 },
  { 0, 1350, 0 },
};
static const NtfStep kPatPowerDn[] = { // power-down arpeggio, W-B-G-R
  { 1047, 110, 4 }, { 784, 110, 2 }, { 659, 110, 1 }, { 523, 320, 1 },
  { 0, 1350, 0 },
};
static const NtfStep kPatSweep[] = {   // rising sweep, LED climbs R->W
  { 392, 60, 1 }, { 523, 60, 1 }, { 659, 60, 2 }, { 784, 60, 2 },
  { 988, 60, 4 }, { 1175, 60, 4 }, { 1319, 60, 7 }, { 1568, 60, 7 },
  { 0, 1520, 0 },
};
static const NtfStep kPatSwoop[] = {   // falling sweep, LED sinks W->R
  { 1568, 60, 7 }, { 1319, 60, 7 }, { 1175, 60, 4 }, { 988, 60, 4 },
  { 784, 60, 2 }, { 659, 60, 2 }, { 523, 60, 1 }, { 392, 60, 1 },
  { 0, 1520, 0 },
};
static const NtfStep kPatWarble[] = {  // fast two-tone warble, R/B swap
  { 880, 120, 1 }, { 1175, 120, 4 },
};
static const NtfStep kPatWail[] = {    // slow up-down siren, R->B drift
  { 587, 300, 1 }, { 784, 300, 3 }, { 988, 300, 5 }, { 784, 300, 6 },
};
static const NtfStep kPatYelp[] = {    // fast siren yelp, R/B swap
  { 784, 180, 1 }, { 1047, 180, 4 },
};
static const NtfStep kPatHiLo[] = {    // British hi-lo two-tone
  { 784, 400, 1 }, { 587, 400, 4 },
};
static const NtfStep kPatKnight[] = {  // scanner sweep R->B->R with ticks
  { 1100, 60, 1 }, { 1100, 60, 3 }, { 1100, 60, 2 }, { 1100, 60, 6 },
  { 1100, 60, 4 }, { 1100, 60, 6 }, { 1100, 60, 2 }, { 1100, 60, 3 },
  { 0, 1520, 0 },
};
static const NtfStep kPatRainbow[] = { // walk a scale through all 7 LED hues
  { 523, 150, 1 }, { 587, 150, 3 }, { 659, 150, 2 }, { 698, 150, 6 },
  { 784, 150, 4 }, { 880, 150, 5 }, { 988, 150, 7 }, { 0, 950, 0 },
};
static const NtfStep kPatDing[] = {    // single soft bell every ~3s
  { 1319, 300, 7 }, { 0, 2700, 0 },
};
static const NtfStep kPatPing[] = {    // single high ping every ~2.5s
  { 2093, 60, 2 }, { 0, 2440, 0 },
};
static const NtfStep kPatBuzz[] = {    // low buzzer + red
  { 175, 400, 1 }, { 0, 1600, 0 },
};
static const NtfStep kPatAlarm[] = {   // classic alarm-clock beep x4
  { 1568, 90, 7 }, { 0, 90, 0 }, { 1568, 90, 7 }, { 0, 90, 0 },
  { 1568, 90, 7 }, { 0, 90, 0 }, { 1568, 90, 7 }, { 0, 1450, 0 },
};
static const NtfStep kPatBell[] = {    // bicycle bell ding-ding-ding
  { 2093, 120, 7 }, { 0, 80, 0 }, { 2093, 120, 7 }, { 0, 80, 0 },
  { 2093, 120, 7 }, { 0, 1640, 0 },
};
static const NtfStep kPatPhone[] = {   // old phone ring burst, cyan LED
  { 440, 60, 6 }, { 480, 60, 6 }, { 440, 60, 6 }, { 480, 60, 6 },
  { 440, 60, 6 }, { 480, 60, 6 }, { 0, 1240, 0 },
};
static const NtfStep kPatTada[] = {    // ta-da: short pickup + long white hit
  { 523, 160, 7 }, { 0, 60, 0 }, { 784, 500, 7 }, { 0, 1280, 0 },
};
static const NtfStep kPatBugle[] = {   // reveille-style bugle call
  { 392, 160, 2 }, { 523, 160, 2 }, { 659, 160, 2 }, { 784, 320, 7 },
  { 659, 160, 2 }, { 784, 500, 7 }, { 0, 1240, 0 },
};
static const NtfStep kPatHappy[] = {   // major triad up, green
  { 523, 140, 2 }, { 659, 140, 2 }, { 784, 280, 7 }, { 0, 1440, 0 },
};
static const NtfStep kPatSad[] = {     // minor descend, red
  { 440, 160, 1 }, { 349, 160, 1 }, { 294, 320, 1 }, { 0, 1520, 0 },
};
static const NtfStep kPatCoin[] = {    // arcade coin (B5 -> held E6)
  { 988, 90, 7 }, { 1319, 410, 7 }, { 0, 1500, 0 },
};
static const NtfStep kPatOneUp[] = {   // 1-up: E6 G6 E7 C7 D7 G6
  { 1319, 120, 7 }, { 1568, 120, 7 }, { 2637, 120, 7 }, { 2093, 120, 7 },
  { 2349, 120, 7 }, { 1568, 320, 7 }, { 0, 1080, 0 },
};
static const NtfStep kPatZelda[] = {   // secret-found falling arpeggio
  { 988, 110, 7 }, { 831, 110, 7 }, { 622, 110, 7 }, { 466, 110, 7 },
  { 988, 360, 7 }, { 0, 1380, 0 },
};
static const NtfStep kPatPacMan[] = {  // arcade intro chirp
  { 988, 100, 7 }, { 1319, 100, 7 }, { 1568, 100, 7 }, { 1976, 100, 7 },
  { 1568, 100, 7 }, { 1976, 240, 7 }, { 0, 1260, 0 },
};
static const NtfStep kPatTetris[] = {  // Korobeiniki opening phrase, green
  { 659, 160, 2 }, { 494, 80, 2 }, { 523, 80, 2 }, { 587, 160, 2 },
  { 523, 80, 2 }, { 494, 80, 2 }, { 440, 160, 2 }, { 440, 80, 2 },
  { 523, 80, 2 }, { 659, 160, 2 }, { 587, 80, 2 }, { 523, 80, 2 },
  { 494, 240, 2 }, { 0, 1440, 0 },
};
static const NtfStep kPatElise[] = {   // Fur Elise opening
  { 659, 90, 7 }, { 622, 90, 7 }, { 659, 90, 7 }, { 622, 90, 7 },
  { 659, 90, 7 }, { 494, 90, 7 }, { 587, 90, 7 }, { 523, 90, 7 },
  { 440, 240, 7 }, { 0, 1360, 0 },
};
static const NtfStep kPatOdeJoy[] = {  // Ode to Joy phrase
  { 659, 180, 7 }, { 659, 180, 7 }, { 698, 180, 7 }, { 784, 180, 7 },
  { 784, 180, 7 }, { 698, 180, 7 }, { 659, 180, 7 }, { 587, 180, 7 },
  { 523, 180, 7 }, { 523, 180, 7 }, { 587, 180, 7 }, { 659, 180, 7 },
  { 659, 270, 7 }, { 587, 90, 7 }, { 587, 360, 7 }, { 0, 1080, 0 },
};
static const NtfStep kPatFifth[] = {   // Beethoven's 5th: G G G Eb, red
  { 392, 140, 1 }, { 392, 140, 1 }, { 392, 140, 1 }, { 311, 500, 1 },
  { 0, 1180, 0 },
};
static const NtfStep kPatNokia[] = {   // classic phone ringtone
  { 659, 120, 7 }, { 587, 120, 7 }, { 370, 120, 7 }, { 415, 120, 7 },
  { 554, 120, 7 }, { 494, 120, 7 }, { 294, 120, 7 }, { 330, 120, 7 },
  { 494, 120, 7 }, { 440, 120, 7 }, { 277, 120, 7 }, { 330, 120, 7 },
  { 440, 400, 7 }, { 0, 1360, 0 },
};
static const NtfStep kPatSmoke[] = {   // guitar riff in 4ths
  { 330, 200, 7 }, { 392, 200, 7 }, { 440, 300, 7 }, { 330, 200, 7 },
  { 392, 200, 7 }, { 466, 100, 7 }, { 440, 300, 7 }, { 330, 200, 7 },
  { 392, 200, 7 }, { 440, 200, 7 }, { 392, 200, 7 }, { 330, 320, 7 },
  { 0, 1180, 0 },
};
static const NtfStep kPatDixie[] = {   // car-horn melody, white
  { 392, 130, 7 }, { 330, 130, 7 }, { 262, 130, 7 }, { 262, 130, 7 },
  { 262, 130, 7 }, { 294, 130, 7 }, { 330, 130, 7 }, { 349, 130, 7 },
  { 392, 130, 7 }, { 392, 130, 7 }, { 392, 130, 7 }, { 330, 320, 7 },
  { 0, 1250, 0 },
};
static const NtfStep kPatJingle[] = {  // jingle bells phrase
  { 659, 140, 7 }, { 659, 140, 7 }, { 659, 280, 7 }, { 0, 60, 0 },
  { 659, 140, 7 }, { 659, 140, 7 }, { 659, 280, 7 }, { 0, 60, 0 },
  { 659, 140, 7 }, { 784, 140, 7 }, { 523, 140, 7 }, { 587, 140, 7 },
  { 659, 400, 7 }, { 0, 1120, 0 },
};
// Wind-evoked sounds, for a future wind-advisory alert: a square-wave speaker
// can't make noise, so these suggest wind by gesture (chime drift, gust swell,
// gap whistle) instead of hiss. Amber LED reads as "caution", not emergency.
static const NtfStep kPatBreeze[] = {  // wind chimes in a gust
  { 1319, 90, 3 }, { 0, 210, 0 }, { 1568, 90, 3 }, { 0, 150, 0 },
  { 1175, 110, 3 }, { 0, 260, 0 }, { 1760, 80, 3 }, { 0, 180, 0 },
  { 1397, 120, 3 }, { 0, 900, 0 },
};
static const NtfStep kPatGust[] = {    // gust builds, peaks, then eases
  { 294, 90, 3 }, { 330, 90, 3 }, { 392, 90, 3 }, { 466, 90, 3 },
  { 523, 110, 3 }, { 587, 110, 3 }, { 659, 160, 3 }, { 0, 120, 0 },
  { 587, 90, 3 }, { 523, 90, 3 }, { 466, 90, 3 }, { 392, 150, 3 },
  { 0, 900, 0 },
};
static const NtfStep kPatWhistle[] = { // wind whistling through a gap
  { 587, 120, 3 }, { 622, 120, 3 }, { 659, 120, 3 }, { 622, 120, 3 },
  { 587, 120, 3 }, { 0, 300, 0 },
  { 523, 120, 3 }, { 587, 120, 3 }, { 622, 120, 3 }, { 587, 120, 3 },
  { 0, 1000, 0 },
};
// Indices are persisted (per-alarm in the NVS blob, and the callsign-notify
// choice in "watchntf"), so never reorder - append new presets at the end.
struct NtfPreset { const char* name; const NtfStep* steps; uint8_t n; };
#define NTFPRESET(nm, tab) { nm, tab, (uint8_t)(sizeof(tab) / sizeof(tab[0])) }
static const NtfPreset kNtfPresets[] = {
  NTFPRESET("Blink",    kPatBlink),
  NTFPRESET("Rapid",    kPatRapid),
  NTFPRESET("Double",   kPatDouble),
  NTFPRESET("Colors",   kPatColors),
  NTFPRESET("Pulse",    kPatPulse),
  NTFPRESET("Simple",   kPatSimple),
  NTFPRESET("Chime",    kPatChime),
  NTFPRESET("Siren",    kPatSiren),
  NTFPRESET("Radar",    kPatRadar),
  NTFPRESET("Morse",    kPatMorse),
  NTFPRESET("Charge",   kPatCharge),
  NTFPRESET("Two Bits", kPatTwoBits),
  NTFPRESET("Strobe",   kPatStrobe),
  NTFPRESET("Slow",     kPatSlow),
  NTFPRESET("Heart",    kPatHeart),
  NTFPRESET("Tick",     kPatTick),
  NTFPRESET("3-2-1 Go", kPatCountDn),
  NTFPRESET("Boot",     kPatBoot),
  NTFPRESET("Power Dn", kPatPowerDn),
  NTFPRESET("Sweep",    kPatSweep),
  NTFPRESET("Swoop",    kPatSwoop),
  NTFPRESET("Warble",   kPatWarble),
  NTFPRESET("Wail",     kPatWail),
  NTFPRESET("Yelp",     kPatYelp),
  NTFPRESET("Hi-Lo",    kPatHiLo),
  NTFPRESET("Knight",   kPatKnight),
  NTFPRESET("Rainbow",  kPatRainbow),
  NTFPRESET("Ding",     kPatDing),
  NTFPRESET("Ping",     kPatPing),
  NTFPRESET("Buzz",     kPatBuzz),
  NTFPRESET("Alarm",    kPatAlarm),
  NTFPRESET("Bell",     kPatBell),
  NTFPRESET("Phone",    kPatPhone),
  NTFPRESET("Tada",     kPatTada),
  NTFPRESET("Bugle",    kPatBugle),
  NTFPRESET("Happy",    kPatHappy),
  NTFPRESET("Sad",      kPatSad),
  NTFPRESET("Coin",     kPatCoin),
  NTFPRESET("One-Up",   kPatOneUp),
  NTFPRESET("Zelda",    kPatZelda),
  NTFPRESET("Pac-Man",  kPatPacMan),
  NTFPRESET("Tetris",   kPatTetris),
  NTFPRESET("Elise",    kPatElise),
  NTFPRESET("Ode Joy",  kPatOdeJoy),
  NTFPRESET("Fifth",    kPatFifth),
  NTFPRESET("Nokia",    kPatNokia),
  NTFPRESET("Smoke",    kPatSmoke),
  NTFPRESET("Dixie",    kPatDixie),
  NTFPRESET("Jingle",   kPatJingle),
  NTFPRESET("Breeze",   kPatBreeze),
  NTFPRESET("Gust",     kPatGust),
  NTFPRESET("Whistle",  kPatWhistle),
};
static const int kNumAlarmPresets =
    sizeof(kNtfPresets) / sizeof(kNtfPresets[0]);

// Accessors so other settings screens (e.g. the callsign notification in
// Flight Tracker) can offer the same presets without touching the table.
int alarmPresetCount() { return kNumAlarmPresets; }
const char* alarmPresetName(int i) {
  return kNtfPresets[constrain(i, 0, kNumAlarmPresets - 1)].name;
}

// Steppers cycle alphabetically but the NVS value stays a kNtfPresets index
// (never reorder the table); g_ntfOrder maps display position -> index.
static uint8_t g_ntfOrder[64];
static bool g_ntfOrderInit = false;
static void ntfOrderInit() {
  if (g_ntfOrderInit) return;
  for (int i = 0; i < kNumAlarmPresets; i++) g_ntfOrder[i] = i;
  for (int i = 1; i < kNumAlarmPresets; i++)
    for (int j = i; j > 0 &&
         strcasecmp(kNtfPresets[g_ntfOrder[j]].name,
                    kNtfPresets[g_ntfOrder[j - 1]].name) < 0; j--) {
      uint8_t t = g_ntfOrder[j];
      g_ntfOrder[j] = g_ntfOrder[j - 1];
      g_ntfOrder[j - 1] = t;
    }
  g_ntfOrderInit = true;
}

// Preset index one alphabetical step from `preset` (dir is ±1, wraps).
int alarmPresetStep(int preset, int dir) {
  ntfOrderInit();
  int pos = 0;
  for (int i = 0; i < kNumAlarmPresets; i++)
    if (g_ntfOrder[i] == preset) { pos = i; break; }
  return g_ntfOrder[(pos + kNumAlarmPresets + dir) % kNumAlarmPresets];
}

// The step active at `now`: the pattern loops, so now % period picks the step.
static const NtfStep* ntfStepAt(int preset, unsigned long now) {
  const NtfPreset& p = kNtfPresets[constrain(preset, 0, kNumAlarmPresets - 1)];
  uint32_t period = 0;
  for (int i = 0; i < p.n; i++) period += p.steps[i].ms;
  uint32_t t = period ? now % period : 0;
  for (int i = 0; i < p.n; i++) {
    if (t < p.steps[i].ms) return &p.steps[i];
    t -= p.steps[i].ms;
  }
  return &p.steps[p.n - 1];
}

// ---- storage ----
// Bump when the Alarm/AlarmStore layout changes; a mismatch loads defaults.
#define ALARM_STORE_VER 2
struct AlarmStore { uint8_t ver; uint8_t count; Alarm a[MAX_ALARMS]; };

void saveAlarms() {
  AlarmStore st;
  st.ver = ALARM_STORE_VER;
  st.count = (uint8_t)g_alarmCount;
  memcpy(st.a, g_alarms, sizeof st.a);
  prefs.begin("flight", false);
  prefs.putBytes("alarms", &st, sizeof st);
  prefs.end();
}

// Called from setup() while the "flight" prefs namespace is already open.
void loadAlarms() {
  AlarmStore st;
  if (prefs.getBytes("alarms", &st, sizeof st) == sizeof st &&
      st.ver == ALARM_STORE_VER &&
      st.count >= 1 && st.count <= MAX_ALARMS) {
    g_alarmCount = st.count;
    memcpy(g_alarms, st.a, sizeof g_alarms);
  } else {
    g_alarms[0] = Alarm{0, 7, 0, 0x3E, 0, 0, 0, 0};   // 7:00 AM, Mon-Fri, off
  }
}

bool anyAlarmEnabled() {
  for (int i = 0; i < g_alarmCount; i++) if (g_alarms[i].en) return true;
  return false;
}

// ---- scheduling ----
// Earliest future h:m on an allowed weekday (empty mask = one-time next h:m),
// or 0 if it can't fire. Strictly future so a firing doesn't reselect itself.
static time_t nextScheduled(int i, time_t now) {
  const Alarm& a = g_alarms[i];
  if (!a.en) return 0;
  uint8_t mask = a.days ? a.days : 0x7F;   // no days = one-time, next h:m
  struct tm t;
  if (!getLocalTime(&t, 0)) return 0;
  for (int d = 0; d < 8; d++) {
    struct tm c = t;
    c.tm_mday += d;
    c.tm_hour = a.h; c.tm_min = a.m; c.tm_sec = 0; c.tm_isdst = -1;
    time_t cand = mktime(&c);   // normalizes the date and fills tm_wday
    if (cand > now && ((mask >> c.tm_wday) & 1)) return cand;
  }
  return 0;
}

// Recompute nextFire + clear snoozes on dismiss/edit; 0 = "pending", filled
// once the clock syncs.
void rearmAlarm(int i) {
  Alarm& a = g_alarms[i];
  a.snoozes = 0;
  a.nextFire = (uint32_t)nextScheduled(i, time(nullptr));
}

// ---- firing ----
// "Snoozed" = nextFire earlier than the next scheduled occurrence - no flag to keep in sync.
static bool isSnoozed(int i, time_t now) {
  const Alarm& a = g_alarms[i];
  if (!a.en || a.nextFire == 0 || (time_t)a.nextFire <= now) return false;
  time_t sched = nextScheduled(i, now);
  return sched == 0 || (time_t)a.nextFire < sched;
}

// Index of the alarm whose pending snooze matures soonest, or -1.
static int soonestSnooze(time_t now) {
  int best = -1;
  for (int i = 0; i < g_alarmCount; i++)
    if (isSnoozed(i, now) &&
        (best < 0 || g_alarms[i].nextFire < g_alarms[best].nextFire))
      best = i;
  return best;
}

// True while any alarm is parked on snooze (flight polls pause; the
// dashboard status line shows the countdown).
bool snoozePending() {
  time_t now = time(nullptr);
  if (now < 1600000000L) return false;
  return soonestSnooze(now) >= 0;
}

// Epoch when the soonest pending snooze refires, or 0 when none is pending.
time_t snoozeRefireAt() {
  time_t now = time(nullptr);
  if (now < 1600000000L) return 0;
  int i = soonestSnooze(now);
  return i < 0 ? 0 : (time_t)g_alarms[i].nextFire;
}

// loop() tick: fire any alarm whose nextFire passed (schedule, matured snooze,
// or a firing missed while off); also fills in pending nextFires.
void checkAlarms() {
  if (g_alarmFiring || !g_timeReady) return;
  time_t now = time(nullptr);
  if (now < 1600000000L) return;
  bool changed = false;
  for (int i = 0; i < g_alarmCount; i++) {
    Alarm& a = g_alarms[i];
    if (!a.en) continue;
    if (!a.nextFire) { rearmAlarm(i); changed = true; }
    if (a.nextFire && now >= (time_t)a.nextFire) {
      if (changed) saveAlarms();
      fireAlarm(i);
      return;
    }
  }
  if (changed) saveAlarms();
}

// Same due-check without firing; sleeperRun() runs it on each deep-sleep
// wake so an alarm that came due while sleeping boots the device to fire.
bool alarmDueNow() {
  if (!g_timeReady) return false;
  time_t now = time(nullptr);
  if (now < 1600000000L) return false;
  bool due = false, changed = false;
  for (int i = 0; i < g_alarmCount; i++) {
    Alarm& a = g_alarms[i];
    if (!a.en) continue;
    if (!a.nextFire) { rearmAlarm(i); changed = true; }
    if (a.nextFire && now >= (time_t)a.nextFire) due = true;
  }
  if (changed) saveAlarms();
  return due;
}

// Earliest upcoming nextFire (0 if none) - enterDeepSleep() wakes at it instead
// of discovering the firing ~5 min late on the pool cadence.
time_t nextAlarmAt() {
  time_t now = time(nullptr);
  if (now < 1600000000L) return 0;
  uint32_t best = 0;
  for (int i = 0; i < g_alarmCount; i++)
    if (g_alarms[i].en && g_alarms[i].nextFire > (uint32_t)now &&
        (!best || g_alarms[i].nextFire < best))
      best = g_alarms[i].nextFire;
  return (time_t)best;
}

// Earliest enabled-alarm occurrence after `after`, counting parked snoozes; an
// `after` before now finds trailing-window occurrences (update-scan quiet win).
time_t nextAlarmAfter(time_t after) {
  uint32_t best = 0;
  for (int i = 0; i < g_alarmCount; i++) {
    const Alarm& a = g_alarms[i];
    if (!a.en) continue;
    time_t s = nextScheduled(i, after);
    if (s && (!best || (uint32_t)s < best)) best = (uint32_t)s;
    if (a.nextFire > (uint32_t)after && (!best || a.nextFire < best))
      best = a.nextFire;
  }
  return (time_t)best;
}

// "Snoozing for N minutes..." status text (ceiled so it reads 5..1); false
// when no snooze is pending.
bool snoozeStatusText(char* buf, size_t n) {
  time_t at = snoozeRefireAt();
  if (!at) return false;
  long rem = (long)(at - time(nullptr));
  int mins = (int)((rem + 59) / 60);
  if (mins < 1) mins = 1;
  snprintf(buf, n, "Snoozing for %d minute%s...", mins, mins == 1 ? "" : "s");
  return true;
}

void fireAlarm(int i) {
  g_fireIdx = i;
  g_alarmFiring = true;
  g_screen = SCR_ALARMFIRE;
  g_screenIdleUntil = millis() + SCREEN_IDLE_TIMEOUT_MS;
  dirty = true;
}

void alarmDismiss() {
  if (g_fireIdx >= 0 && g_fireIdx < g_alarmCount) {
    Alarm& a = g_alarms[g_fireIdx];
    if (a.days) {
      rearmAlarm(g_fireIdx);   // next scheduled day = "dismissed for today"
    } else {
      // One-time alarm (no days): the firing is the whole schedule, so the
      // episode ends by turning the alarm off instead of rearming it.
      a.en = 0;
      a.snoozes = 0;
      a.nextFire = 0;
    }
    saveAlarms();
  }
  g_alarmFiring = false;
  g_fireIdx = -1;
  g_screen = SCR_DASH;
  g_screenIdleUntil = 0;
  dirty = true;
}

// Park the firing alarm 5 min; past ALARM_MAX_SNOOZES the next request
// dismisses. nextFire is in NVS, so a snooze survives deep sleep.
void alarmSnooze() {
  if (g_fireIdx >= 0 && g_fireIdx < g_alarmCount) {
    Alarm& a = g_alarms[g_fireIdx];
    if (a.snoozes >= ALARM_MAX_SNOOZES) { alarmDismiss(); return; }
    a.snoozes++;
    a.nextFire = (uint32_t)(time(nullptr) + ALARM_SNOOZE_SEC);
    saveAlarms();
  }
  g_alarmFiring = false;
  g_fireIdx = -1;
  g_screen = SCR_DASH;
  g_screenIdleUntil = 0;
  dirty = true;
}

// ---- notification LED (non-blocking, called from loop()) ----
static void ledWrite(bool r, bool g, bool b) {   // active-low channels
  pinMode(CYD_LED_RED, OUTPUT); pinMode(CYD_LED_GREEN, OUTPUT); pinMode(CYD_LED_BLUE, OUTPUT);
  digitalWrite(CYD_LED_RED,   r ? LOW : HIGH);
  digitalWrite(CYD_LED_GREEN, g ? LOW : HIGH);
  digitalWrite(CYD_LED_BLUE,  b ? LOW : HIGH);
}

// One-shot preview of the picked preset on LED + speaker while editing.
static uint32_t g_ledPreviewUntil = 0;
static int      g_ledPreviewPreset = -1;

void previewAlarmLed(int preset) {
  g_ledPreviewPreset = preset;
  g_ledPreviewUntil = millis() + 3000;
}

// One-shot short beep (speaker only) played by updateAlarmLed() - used by the
// Notify Volume stepper so each change is heard at the new level.
static uint32_t g_testBeepUntil = 0;
void notifyTestBeep() { g_testBeepUntil = millis() + 160; }

// True while the alarm LED owns the LED: a firing alarm, a firing advisory
// popup, or a preset preview. The normal flight/status LED code skips its
// updates while this is set.
bool alarmLedBusy() {
  return g_alarmFiring || g_windFiring ||
         (g_ledPreviewPreset >= 0 && (long)(millis() - g_ledPreviewUntil) < 0);
}

// ---- notification speaker (non-blocking, driven with the LED) ----
// GPIO 26 = JST speaker via the onboard amp. Volume is the LEDC duty scaled by
// g_notifyVol; tone() is unusable - it pins duty at ~50% on a background task.
#define SPK_PIN 26
#define SPK_RES 10                  // LEDC bits; 50% duty = 511 like tone()
static int  g_toneFreq = -1;        // currently playing freq, -1 = silent
static bool g_spkAttached = false;  // SPK_PIN is attached to an LEDC channel

static void toneWrite(int freq) {
  if (freq == g_toneFreq) return;
  g_toneFreq = freq;
  // Both boards gate the amp behind GPIO 4 (active-low); re-asserted per tone
  // in case a TFT_RST pulse during tft.init() raised it.
  if (freq > 0) { pinMode(4, OUTPUT); digitalWrite(4, LOW); }
  if (freq <= 0) {
    if (g_spkAttached) ledcWrite(SPK_PIN, 0);   // silent; pin stays attached
    return;
  }
  // Retune with the duty still at 0 so the note starts cleanly, then apply
  // the volume-scaled duty.
  if (!g_spkAttached) { ledcAttach(SPK_PIN, freq, SPK_RES); g_spkAttached = true; }
  else                ledcChangeFrequency(SPK_PIN, freq, SPK_RES);
  ledcWrite(SPK_PIN, (uint32_t)g_notifyVol * 511 / 100);
}

void updateAlarmLed(unsigned long now) {
  static bool wasActive = false;
  int preset = -1;
  if (g_alarmFiring)
    preset = (g_fireIdx >= 0 && g_fireIdx < g_alarmCount)
                 ? g_alarms[g_fireIdx].preset : 0;
  else if (g_windFiring)
    preset = g_windNotify;
  else if (g_ledPreviewPreset >= 0 && (long)(now - g_ledPreviewUntil) < 0)
    preset = g_ledPreviewPreset;
  if (preset < 0) {
    g_ledPreviewPreset = -1;
    if (wasActive) { ledWrite(0, 0, 0); toneWrite(0); wasActive = false; }
    // Volume-test beep from the General page's Notify Volume stepper
    // (speaker only - no LED) so each step is heard at the new level.
    toneWrite((long)(now - g_testBeepUntil) < 0 ? 880 : 0);
    return;
  }
  wasActive = true;
  const NtfStep* s = ntfStepAt(preset, now);
  ledWrite(s->rgb & 1, s->rgb & 2, s->rgb & 4);
  toneWrite(s->freq);
}

// ---- watch-callsign notification ----
// While a watched flight shows, run its preset on LED + speaker (~5s per
// sighting). Gated on !alarmLedBusy() so a firing alarm wins; re-arms on
// leave/return or a new watched plane.
#define WATCH_TONE_MS 5000
static unsigned long g_watchToneStart = 0;
static bool g_watchToneArmed = false;
static bool g_watchLedOn = false;
static char g_watchIcao[7] = "";   // icao24 the current sighting belongs to

void updateWatchNotify(unsigned long now, bool active, const char* icao24) {
  if (!active) {
    // Only silence the speaker if this notifier was the one using it - the
    // volume-test beep and other one-shots must not be cut off.
    if (g_watchToneArmed) { g_watchToneArmed = false; toneWrite(0); }
    if (g_watchLedOn) { ledWrite(0, 0, 0); g_watchLedOn = false; }
    g_watchIcao[0] = 0;
    return;
  }
  g_watchLedOn = true;
  if (strncmp(icao24, g_watchIcao, 6) != 0) {
    strncpy(g_watchIcao, icao24, 6);
    g_watchIcao[6] = 0;
    g_watchToneArmed = true;
    g_watchToneStart = now;
  }
  const NtfStep* s = ntfStepAt(g_watchNotify, now);
  ledWrite(s->rgb & 1, s->rgb & 2, s->rgb & 4);
  // Speaker only for the first ~5s of a sighting; the LED pattern keeps
  // running while the watched flight is shown.
  toneWrite(now - g_watchToneStart < WATCH_TONE_MS ? s->freq : 0);
}

// ---- boot chime ----
// Rising C5-E5-G5-C6 arpeggio + LED sweep: "device is on". Blocking in setup(),
// cold boots only (deep-sleep wakes skip it).
static const NtfStep kBootChime[] = {
  { 523, 110, 1 }, { 659, 110, 2 }, { 784, 110, 4 }, { 1047, 320, 7 },
};

// Play a score once, blocking; trailing rest steps (loop padding) are skipped.
static void playSteps(const NtfStep* steps, int n) {
  while (n > 0 && steps[n - 1].freq == 0 && steps[n - 1].rgb == 0) n--;
  for (int i = 0; i < n; i++) {
    ledWrite(steps[i].rgb & 1, steps[i].rgb & 2, steps[i].rgb & 4);
    toneWrite(steps[i].freq);
    delay(steps[i].ms);
  }
  ledWrite(0, 0, 0);
  toneWrite(0);
}

void playBootChime() {
  playSteps(kBootChime, sizeof(kBootChime) / sizeof(kBootChime[0]));
}

// "Firmware updated" signature: the arcade 1-up run, played right after the
// boot chime on the first cold boot of a new version (see "lastver" in setup()).
void playUpgradeChime() {
  delay(250);   // silence gap so it reads as a second, separate sound
  playSteps(kPatOneUp, sizeof(kPatOneUp) / sizeof(kPatOneUp[0]));
}

// ---- drawing ----
// Format an alarm time honoring the 12/24-hour setting: "7:30 AM" or "19:30".
void fmtAlarmTime(char* buf, size_t n, int h, int m) {
  if (g_clock24) {
    snprintf(buf, n, "%d:%02d", h, m);
  } else {
    int h12 = h % 12; if (!h12) h12 = 12;
    snprintf(buf, n, "%d:%02d %s", h12, m, h < 12 ? "AM" : "PM");
  }
}

// Small bell glyph shown in the header's top marker slot while any alarm is
// enabled (above the AM/PM text, which occupies the bottom slot in 12h mode).
void drawAlarmBell(int x, int y, uint16_t bg) {
  tft.fillTriangle(x + 4, y, x, y + 6, x + 8, y + 6, TFT_YELLOW);
  tft.fillRect(x, y + 6, 9, 2, TFT_YELLOW);
  tft.fillRect(x + 3, y + 8, 3, 2, TFT_YELLOW);   // clapper
  (void)bg;
}

// Alarms editor "Alarms > N": enable toggle, time stepper (tap the time for
// keyboard), weekday toggles, preset stepper, < / New / Delete footer.
void drawAlarms() {
  Alarm& a = g_alarms[g_alarmIdx];
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 28, g_clockCol);
  tft.setTextColor(btnFg(g_clockCol), g_clockCol);
  tft.setTextFont(2);
  tft.setCursor(8, 6);
  tft.printf("Alarms > %d", g_alarmIdx + 1);
  backBtn("Back");

  // Delete confirmation replaces the editor body (same Yes/No layout as the
  // Reset confirmation). Back still works and cancels it.
  if (g_alarmDelConfirm) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.setCursor(10, 60);
    tft.printf("Delete alarm %d?", g_alarmIdx + 1);
    char tb[16];
    fmtAlarmTime(tb, sizeof tb, a.h, a.m);
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.setCursor(10, 86);
    tft.print(tb);
    tft.setTextFont(1);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 116);
    tft.print("This cannot be undone.");
    tft.fillRoundRect(CX - 130, 180, 110, 34, 6, dangerCol());
    tft.setTextColor(btnFg(dangerCol()), dangerCol());
    tft.setTextFont(2);
    tft.drawCentreString("Yes", CX - 75, 189, 2);
    themeBtn(CX + 20, 180, 110, 34, 6);
    tft.setTextColor(btnFg(btnCol()), btnCol());
    tft.drawCentreString("No", CX + 75, 189, 2);
    return;
  }

  // Enabled toggle (same row style as the Sleep screen)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 44);
  tft.print("Enabled");
  tft.setTextColor(a.en ? TFT_GREENYELLOW : TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(150, 44);
  tft.print(a.en ? "ON" : "OFF");
  themeBtn(RX(230), 40, 82, 24, 5);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.setTextFont(FONT_AUX);
  tft.setCursor(RX(250), 47);
  tft.print("Toggle");

  drawTimeAdj(80, "Time", a.h, a.m);

  // Days of week: seven toggles, bit 0 = Sunday. Selected days use the theme
  // color; unselected stay grey.
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(8, 133);
  tft.print("Days");
  const char* dl = "SMTWTFS";
  for (int i = 0; i < 7; i++) {
    bool sel = (a.days >> i) & 1;
    int bx = 64 + i * (36 + (DISP_W - 320) / 6);
    uint16_t c = sel ? btnCol() : disabledCol();
    if (sel) themeBtn(bx, 128, 32, 26, 4);   // white outline if near-black
    else tft.fillRoundRect(bx, 128, 32, 26, 4, c);
    tft.setTextColor(btnFg(c), c);
    tft.setCursor(bx + 11, 133);
    tft.print(dl[i]);
  }
  // An empty day mask isn't a dead alarm - it's a one-time alarm, so say so
  // or the row reads as "never fires".
  if (!a.days) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextFont(1);
    tft.setCursor(64, 159);
    tft.print("No days = One-Time Alarm");
  }

  // Notify preset stepper
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 172);
  tft.print("Notify");
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.drawCentreString(alarmPresetName(a.preset), RX(200), 172, 2);
  adjPair(TADJ_MX, 166);

  // Footer: < prev (left) | Delete (center) | > next or New (right)
  tft.setTextFont(2);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  if (g_alarmIdx > 0) {
    themeBtn(8, 206, 44, 26, 5);
    tft.drawCentreString("<", 30, 211, 2);
  }
  if (g_alarmCount > 1) {
    tft.fillRoundRect(CX - 40, 206, 80, 26, 5, dangerCol());
    tft.setTextColor(btnFg(dangerCol()), dangerCol());
    tft.drawCentreString("Delete", CX, 211, 2);
    tft.setTextColor(btnFg(btnCol()), btnCol());
  }
  if (g_alarmIdx + 1 < g_alarmCount) {
    themeBtn(RX(268), 206, 44, 26, 5);
    tft.drawCentreString(">", RX(290), 211, 2);
  } else if (g_alarmCount < MAX_ALARMS) {
    themeBtn(RX(228), 206, 84, 26, 5);
    tft.drawCentreString("New", RX(270), 211, 2);
  }
}

// Full-screen alert while an alarm is firing: big Dismiss / Snooze buttons.
void drawAlarmFire() {
  Alarm& a = g_alarms[constrain(g_fireIdx, 0, g_alarmCount - 1)];
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, DISP_W, 52, TFT_MAROON);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawCentreString("ALARM", CX, 12, 4);

  char tb[16];
  fmtAlarmTime(tb, sizeof tb, a.h, a.m);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(tb, CX, 78, 4);

  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (a.snoozes > 0) {
    char sb[24];
    snprintf(sb, sizeof sb, "Snoozed %d/%d", a.snoozes, ALARM_MAX_SNOOZES);
    tft.drawCentreString(sb, CX, 120, 2);
  }

  themeBtn(CX - 144, 158, 140, 62, 8);
  tft.setTextColor(btnFg(btnCol()), btnCol());
  tft.drawCentreString("Dismiss", CX - 74, 180, 2);
  themeBtn(CX + 4, 158, 140, 62, 8);
  tft.drawCentreString("Snooze", CX + 74, 174, 2);
  tft.drawCentreString("5 min", CX + 74, 194, 2);
}

// ---- touch ----
static void applyAlarmTimeAdj(int hit) {
  Alarm& a = g_alarms[g_alarmIdx];
  switch (hit) {
    case 1: a.h = (a.h + 1) % 24; break;    // hour wrap flips AM/PM on its own
    case 2: a.h = (a.h + 23) % 24; break;
    case 3: a.m = (a.m + 1) % 60; break;
    case 4: a.m = (a.m + 59) % 60; break;
  }
  rearmAlarm(g_alarmIdx);    // reschedule from the new time
  saveAlarms();
}

void handleAlarmsTouch(uint16_t x, uint16_t y) {
  Alarm& a = g_alarms[g_alarmIdx];
  if (inRect(x, y, RX(265), 4, RX(315), 24)) { g_alarmDelConfirm = false; g_screen = SCR_DASH; dirty = true; return; }
  // Delete confirmation: Yes removes the alarm, No (or Back) cancels.
  if (g_alarmDelConfirm) {
    if (inRect(x, y, CX - 130, 180, CX - 20, 214)) {
      g_alarmDelConfirm = false;
      for (int i = g_alarmIdx; i < g_alarmCount - 1; i++)
        g_alarms[i] = g_alarms[i + 1];
      g_alarmCount--;
      // A pending snooze rides along inside the shifted struct (nextFire),
      // so no index fixup is needed here.
      if (g_alarmIdx >= g_alarmCount) g_alarmIdx = g_alarmCount - 1;
      saveAlarms();
      dirty = true;
    } else if (inRect(x, y, CX + 20, 180, CX + 130, 214)) {
      g_alarmDelConfirm = false;
      dirty = true;
    }
    return;
  }
  if (inRect(x, y, RX(230), 40, RX(312), 64)) {   // Enabled toggle
    a.en = !a.en;
    rearmAlarm(g_alarmIdx);
    saveAlarms();
    dirty = true;
    return;
  }
  int hit = timeAdjHit(x, y, 80);
  if (hit == 7) {   // tapped the time itself -> manual HHMM keyboard entry
    char b[8];
    snprintf(b, sizeof b, "%02d%02d", a.h, a.m);
    g_alarmTimeStr = b;
    g_wifiSub = 8;
    g_screen = SCR_WIFI;
    dirty = true;
    return;
  }
  if (hit) { applyAlarmTimeAdj(hit); dirty = true; return; }
  for (int i = 0; i < 7; i++) {   // weekday toggles
    if (inRect(x, y, 64 + i * (36 + (DISP_W - 320) / 6), 128, 64 + i * (36 + (DISP_W - 320) / 6) + 32, 154)) {
      a.days ^= (1 << i);
      rearmAlarm(g_alarmIdx);
      saveAlarms();
      dirty = true;
      return;
    }
  }
  int np = adjPairHit(x, y, TADJ_MX, 166);   // notify preset stepper
  if (np) {
    a.preset = alarmPresetStep(a.preset, np);   // steppers cycle alphabetically
    previewAlarmLed(a.preset);   // play the picked pattern once on the LED
    saveAlarms();
    dirty = true;
    return;
  }
  // footer: < left | Delete center | > or New right
  if (g_alarmIdx > 0 && inRect(x, y, 8, 206, 52, 232)) {
    g_alarmIdx--; dirty = true; return;
  }
  if (g_alarmCount > 1 && inRect(x, y, CX - 40, 206, CX + 40, 232)) {
    g_alarmDelConfirm = true;   // ask first; the actual delete happens on Yes
    dirty = true;
    return;
  }
  if (g_alarmIdx + 1 < g_alarmCount) {
    if (inRect(x, y, RX(268), 206, RX(312), 232)) { g_alarmIdx++; dirty = true; return; }
  } else if (g_alarmCount < MAX_ALARMS &&
             inRect(x, y, RX(228), 206, RX(312), 232)) {
    g_alarms[g_alarmCount] = Alarm{0, 7, 0, 0x3E, 0, 0, 0, 0};   // 7:00 AM, Mon-Fri
    g_alarmIdx = g_alarmCount++;
    saveAlarms();
    dirty = true;
    return;
  }
}

void handleAlarmFireTouch(uint16_t x, uint16_t y) {
  if (inRect(x, y, CX - 144, 158, CX - 4, 220)) alarmDismiss();
  else if (inRect(x, y, CX + 4, 158, CX + 144, 220)) alarmSnooze();
}
