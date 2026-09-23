<!-- Tri-fold Letter landscape, printed duplex (flip on long edge).
     Each side is three panels listed left-to-right as printed; <!-- PANEL -->
     splits them. OUTSIDE = [inside flap | back cover | front cover] — the
     rightmost panel is the cover; the leftmost tucks inside when folded.
     INSIDE = the three-panel spread read left to right. -->

<!-- SIDE:OUTSIDE -->

<!-- inside flap: the welcome panel, first thing seen when opened -->
## ✨ What It Shows

- **Alarms** — up to 6 weekday-scheduled alarms with an LED + sound alert.
- **Clock & date** — always on top.
- **Firmware updates** — checks for new firmware once a day and installs it
  over WiFi, or update manually.
- **Flight recall** — tap the aircraft count to bring the last overhead
  flight's details back up, even after it's gone.
- **Flights overhead** *(optional — on by default)* — a mini radar of live
  aircraft near you: callsign, route, altitude, speed, distance, airline logo,
  and the plane's actual ground track.
- **Pool temperature** *(optional — off by default)* — current water
  temperature and history graphs, if you have a Govee pool thermometer.
- **Settings** — every option on the device is configurable on-screen:
  units, clock, WiFi, location, flight tracking, alarms, and more.
- **Sleep mode** — the screen sleeps overnight and wakes on touch.
- **Startup chime** — a short rising melody + LED sweep plays on power-on
  (stays silent when it wakes from sleep mode). Additionally, a chime plays
  after a firmware update.
- **Weather** — current temperature, feels-like, humidity, sunrise/sunset, a
  7-day forecast, and temperature history graphs (Day / Week / Month / Year).

<div class="shot"><img src="DEVELOPER-USERGUIDE-idle.png"
     alt="Idle dashboard: weather, sunrise/sunset, pool temperature, 7-day forecast"></div>

<!-- PANEL -->

<!-- back cover -->
## 🔄 Resetting (Settings → Reset)

Every option asks you to confirm and says exactly what it clears:

- **Restart** — reboots only; clears nothing.
- **Graph Data** — clears pool and weather temperature history.
- **Settings** — clears settings and credentials (keeps touch calibration).
- **Network** — clears WiFi credentials and IP settings (static IP, hostname);
  everything else is kept.
- **Factory Reset** — clears *everything*: settings, credentials, files,
  history, and touch calibration. The next boot recalibrates touch and asks
  for WiFi again.
- **Cancel** — backs out without changing anything.

<div class="warn" markdown="1">⚠ **Factory Reset erases any pre-loaded
airline logos** — restoring logos requires regenerating and reloading them
from a development computer.
</div>

## 🛠 Troubleshooting

- **Taps land in the wrong spot** — press and hold anywhere on the screen for
  **10 seconds** to recalibrate.
- **Wrong weather or no flights nearby** — fix your location under
  **Settings → Location**.
- **A flight's route or track looks wrong** — route and ground-track data
  come from public flight-data feeds that may not have the
  latest data, so they can be missing, outdated, or inaccurate.
- **New WiFi network** — re-enter it under **Settings → Network**.

<div class="shot"><img src="DEVELOPER-USERGUIDE-ftracker.png"
     alt="Flight Tracker settings: enable toggle, radius, ceiling, poll interval"></div>

<!-- PANEL -->

<!-- front cover -->
<div class="cover" markdown="1">

<img class="logo" src="cyd-horizon-guide-logo.png"
     alt="cyd-horizon logo">

<div class="desc">
<p><strong>Live flight tracker &amp; weather station</strong> for <strong>Cheap
Yellow Display (CYD)</strong> boards:</p>
<ul>
<li><strong>2.8" 2432S028R</strong></li>
<li><strong>4" E32R40T</strong></li>
</ul>
<p>Once it's set up it runs on your WiFi — no computer needed.</p>
</div>

<img class="hero" src="DEVELOPER-USERGUIDE-dashboard.png"
     alt="Simulated cyd-horizon screen: a real flight overhead
     with callsign, route, live radar, ground track, and OpenSky credits">

<img class="hero" src="DEVELOPER-USERGUIDE-wxgraph.png"
     alt="Weather temperature history graph showing the last day">

<div class="cover-bottom">
{{VERSION}}
<p class="footer"><em><a href="https://github.com/improving-minnesota/cyd-horizon">github.com/improving-minnesota/cyd-horizon</a></em></p>
</div>

</div>

<!-- SIDE:INSIDE -->

<!-- inside spread, panel 1: setup -->
## 🚀 Getting Started

1. **Plug it in.** On a brand-new device (or after a Factory Reset) it first
   asks you to tap a few crosshairs to calibrate the touchscreen — this is
   skipped if calibration is already saved.
2. **Enter WiFi.** Type your network name and password with the on-screen
   keyboard. (Change it later under **Settings → Network**.)
3. Done — the dashboard appears and starts loading weather and flights.

## 📍 Set Your Location

Weather and flights need to know where you are. On first boot the device
guesses your location from your internet connection — check that it's right:

Open **Settings → Location**, then use whichever is easiest:

- **Search Address** — type your city or address and pick the match.
- **Set** — type your latitude and longitude directly.
- **Find by IP** — guess again from your connection.

## 🔑 OpenSky Account (Recommended)

Flights work out of the box, but a free OpenSky account raises your daily
limit and removes the yellow "anonymous" warning.

1. Create a free account at **opensky-network.org**.
2. Go to **My OpenSky → Account** and create an **API client** — that
   downloads a file containing your **client ID** and **client secret**.
3. On the device: open **Settings → Flight Tracker**, page right to the
   **OpenSky Credentials** screen, and enter both.

The three small readouts in the header — **CRP**, **CRL**, **CFT** — show
your remaining daily OpenSky credits (tap them for detail). Grey is healthy,
yellow is low, pink is nearly out. Credits reset daily.

## 🌡️ Govee Pool Thermometer (Optional)

Only if you own a Govee pool thermometer:

1. Create a free developer account at **developer.govee.com** and generate an
   **API key**.
2. On the device: open **Settings → Pool Temp**, turn it **on**, enter the
   key, tap **Fetch Devices**, and pick your thermometer.

<!-- PANEL -->

<!-- inside spread, panel 2: daily use -->
## 👆 Using the Dashboard

The main screen isn't just a display — several spots are **buttons you can
tap**:

- **Cog (bottom-right)** — open settings.
- **Weather temperature (top-left)** — weather history graph.
- **Pool reading** — pool temperature history graph.
- **Aircraft count (bottom-left)** — details of the last overhead flight.
- **CRP / CRL / CFT (header)** — OpenSky credits detail.
- **Clock/date (header)** — open the alarm editor.
- Any settings or graph screen returns to the dashboard after 2 minutes
  untouched.

## ⏰ Alarms

Up to 6, each with its own weekdays and a **Notify** pattern. Leave all
weekdays off for a one-time alarm. Set the
time with the
**▼ / ▲** arrow buttons — hour on the
left, minute on the right (the hour rolls through AM/PM) — or tap the time
to type it. A bell icon sits beside the clock while any alarm is on. **Delete** asks
for confirmation before removing an alarm.

When one fires: **Dismiss** stops it for today; **Snooze** refires in 5 min
(up to 3 snoozes, then it dismisses for the day). Alarms fire on time even
during Sleep Mode — the device wakes itself at the alarm time — and a
snoozed or missed alarm still fires after a power loss.

<p class="callout">🔊 Sound requires an <strong>external speaker</strong> plugged into the board's small
JST port — the display has no built-in speaker, so without one alarms flash
the LED only. Loudness comes from <strong>Notify Volume</strong> under
Settings → General; it applies to alarms, the callsign alert, and the boot
chime.</p>

<div class="shot"><img src="DEVELOPER-USERGUIDE-alarms.png"
     alt="Alarms editor: enable toggle, time steppers, weekday pickers, Notify preset"></div>

<!-- PANEL -->

<!-- inside spread, panel 3: reference -->
## ⚙️ Settings at a Glance

Defaults shown in parentheses.

- **About** — version, author, and firmware update status / install.
- **Calibrate Touch** — rerun touch calibration.
- **Flight Tracker** — enabled (on); radar radius (3.5, in mi/km/nm — how far
  away to look); altitude ceiling (15,000, in ft/m — planes above it are
  ignored); poll interval (30 s); timer bar (off); home
  airport (none); watch callsign — (none, matches
  any part of a callsign; \* matches every flight);
  callsign notify (Radar); LED blink (on); Show IATA Airports (on);
  OpenSky credentials.
- **General** — theme color picked from swatch rows (tap a hue, then a
  shade or grey — colors the
  header and screen buttons); auto-update (on); Notify Volume (levels 1–10,
  default 10); units (Imperial ft/mph/mi,
  Metric m/kts/km, or Aviation ft/kts/nm — also sets weather and pool temps
  to °F or °C); clock (12-hour with AM/PM, or 24-hour).
- **Location** — your coordinates (auto-guessed on first boot).
- **Network** — WiFi network (shows *Scanning* while it searches); DHCP or
  static IP; device hostname.
- **Pool Temp** — (off) enable, enter the API key, pick your thermometer.
  While off, no data is collected.
- **Reset** — see the back cover.
- **Sleep Mode** — enabled (on); sleeps 10:00 PM – 8:00 AM; a touch wakes it
  for 5 minutes.

## 💡 LED & Border Colors

- **Red border / LED** — something needs attention: no WiFi, bad OpenSky
  credentials, exhausted OpenSky credits, or missing weather/pool data.
- **Yellow** — OpenSky is running anonymously; flights still work.
- **Flight blinks** — blue for any overhead flight; red when it's departing
  your home airport; green when arriving; yellow when both ends are your home
  airport. A watched callsign instead plays its **Callsign Notify**
  pattern on the LED and speaker
  while its details are shown — independent of the blink setting.

## 📦 Firmware Updates

The device keeps its firmware current over WiFi. **Auto-Update** (on by
default) checks once a day and installs new releases; to update
manually, open **Settings → About** and tap **Install** if a newer
version is offered. If an update fails, it rolls back automatically.
