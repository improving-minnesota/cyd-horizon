# Developer / Advanced Details

Advanced setup for the `cyd-horizon` firmware. For an end-user overview and
the settings guide, see the [README](README.md).

## Author

- **Site:** improving.com
- **Developer:** Paul Hassinger
- **Email:** paul.hassinger (at) improving.com

## Development workflow

All changes to this repo are made through **pull requests** that are merged
into `main`. Never commit directly to `main`. The workflow is:

1. **Create a feature branch** off the latest `main`:
   ```bash
   git checkout main
   git pull
   git checkout -b feature/short-description
   ```
2. **Make your changes** and commit them on the branch, following the project's
   existing commit style (short imperative subject lines).
3. **Push the branch** to GitHub:
   ```bash
   git push -u origin feature/short-description
   ```
4. **Open a pull request** against `main` (e.g. `gh pr create` or via the
   GitHub UI), including a summary of the change and how it was verified
   (compile/upload results, screenshots of the display, etc.).
5. **Review and address feedback**, keeping the branch up to date with `main`
   as needed (rebase or merge, then re-push).
6. **Merge the PR into `main`** once it's reviewed and green. Delete the branch
   locally and on the remote after merging:
   ```bash
   git branch -d feature/short-description
   git push origin --delete feature/short-description
   ```

Keep PRs small and focused on a single logical change so they're quick to
review. The primary branch is `main`; everything that lands there is intended
to be shippable.

> **Keep the README and the on-device Help screen in sync.** Every user-facing
> change to `README.md` (features, settings, credentials, troubleshooting, etc.)
> must also be mirrored in the on-device Help screen (`kHelpLines[]` in
> `cyd-horizon/settings.ino`) — and vice versa. They are not generated from one
> another. The Help screen is a fixed-width, pre-wrapped string array, so keep
> each line short (≤ ~32 chars) and update it whenever the README's user-facing
> guidance changes.

## Code comments

Comments serve two purposes: they explain *why* (not *what*), and they organize
code into readable sections. The code already shows what it does; a comment
should either add context that reading it doesn't reveal, or group related code
so it's easier to scan. Keep comments short and current — a stale comment is
worse than none.

- Say **why**, not **what**; don't restate the line you're commenting.
- **Organize** with short section/grouping headers that break long functions or
  files into readable blocks (e.g. `// ---- OpenSky fetch + parse ----`,
  `// network row`, `// 7-day forecast (right/bottom)`).
- Document **non-obvious constraints**: ordering, concurrency, why a workaround
  exists, what a magic number means, and edge cases you're guarding against.
- **Prefer clear names** so most code needs no comment at all.
- Keep inline `//` notes to a few words — normally one line. When a constraint
  genuinely needs more than ~2 lines, move the explanation into this file (or
  the relevant `docs/` README) and leave a one-line pointer in the code.

Good: the `configTime()` lwip-crash note, the `collectHeaders()` header-drop
note, the `g_timeReady` timezone gotcha, and section headers that group a
function's steps. Avoid: paragraphs that re-narrate the `if/else` they sit
above.

## The sketch

The `cyd-horizon/` sketch targets CYD-family boards via the
`esp32:esp32:jczn_2432s028r` FQBN (it covers every supported variant — the
board differences come from `-DCYD_*` flags, see "Board variants"). It uses a
custom partition table (see
"Partition table" below), so the FQBN must include `:PartitionScheme=custom`:

```bash
arduino-cli compile --fqbn esp32:esp32:jczn_2432s028r:PartitionScheme=custom \
  --build-property "compiler.cpp.extra_flags=-DAPP_VERSION=$(jq -r '.["."]' .release-please-manifest.json)" cyd-horizon
```

### Board variants

One source tree serves every supported board; a `-DCYD_<MODEL>` compile flag
selects the variant (display driver, pins, scaling, OTA asset name). The same
`jczn_2432s028r` FQBN is used for all plain-ESP32 boards — the flag does the
rest.

| Flag | Board | Panel | OTA release asset | Build dir |
|---|---|---|---|---|
| *(none)* | 2.8" ESP32-2432S028R | ILI9341 320x240, touch on VSPI | `cyd-horizon-2432s028r.ino.bin` | `build/release` |
| `-DCYD_E32R40T=1` | 4.0" E32R40T | ST7796 480x320, touch shares the TFT's HSPI, backlight GPIO 27, speaker amp enable GPIO 4 | `cyd-horizon-e32r40t.ino.bin` | `build/release-e32r40t` |

All layout code targets a logical DISP_W x 240 screen: 320x240 on the 2.8"
board, 360x240 on the E32R40T. The `tft` object is a scaling wrapper that
maps logical -> panel with a uniform x4/3 factor on the 4" board, so nothing
distorts; the extra 40 logical columns are spent on spacing via `RX(x)`
(right-edge anchored positions) and `CX` (screen centre) rather than
stretching. On the 4" board, text uses smooth FreeFonts instead of scaling
the classic bitmap fonts: F1 stays the classic 8px font (the only tier that
fits the header's three credit rows), F2->FreeSans9, F4->FreeSansBold18
(big values), and F6->FreeSansBold18 (header clock). `FONT_AUX` is the
secondary-text tier (button captions, forecast cells, status lines): F1 on
the 2.8", F2 on the 4". The wrapper compensates for FreeFonts' baseline
origin so setCursor() keeps "top of text" semantics. `touchReadXY` maps panel
coordinates back to logical space. Raw panel access (init/rotation) uses
`lcd`.

Build + push the 4" variant:

```bash
arduino-cli compile --fqbn esp32:esp32:jczn_2432s028r:PartitionScheme=custom \
  --build-property "compiler.cpp.extra_flags=-DCYD_E32R40T=1 -DAPP_VERSION=...-dev -DBUILD_NUM=N -DENABLE_LOCAL_OTA=1 -DENABLE_SERIAL_PROVISION=1" \
  --output-dir build/release-e32r40t cyd-horizon
cyd-horizon/.venv/bin/python scripts/ota_push.py --dir build/release-e32r40t
```

> The E32R40T's CH340 also fails at 921600 baud but tolerates 460800, so a
> first USB flash uses `--upload-property upload.speed=460800`.

Release assets are named per board (`OTA_ASSET` in `ota.ino`); `release.yml`
builds each variant and `build-firmware.yml` compiles all of them on every
PR. Only board-named assets are published: firmware old enough to poll for
the bare `cyd-horizon.ino.bin` name (before board-named assets existed) can
no longer see new releases and must be updated over USB.

#### Identifying boards on serial ports

Every build prints `[boot] board=<model>` and `[boot] version=<v> build=<n>`
right after reset (neither is dev-gated; releases report `build=0`).
`scripts/detect_boards.py` resets every `/dev/cu.usbserial-*` port in
parallel and reports which board each holds (plus version/build, and IP with
`--wait-ip`):

```bash
cyd-horizon/.venv/bin/python scripts/detect_boards.py [--wait-ip] [--json]
```

Both update scripts use it:

- `ota_push.py --board e32r40t` (or inferred from `--dir build/release-e32r40t`)
  picks the matching port when several boards are plugged in.
- `scripts/flash.py --board e32r40t` does the USB upload with the correct
  per-board baud (115200 for 2432s028r, 460800 for the E32R40T's CH340).

Firmware older than the marker reports `unknown` — pass `--port` once to get
a self-identifying build onto it.

### Getting the image onto the device: OTA by default, USB only for the first flash

Pick by **device state**, not by habit:

| Device state | How to flash |
|---|---|
| Dev build with the OTA flags already running (board on WiFi) | **`cyd-horizon/.venv/bin/python scripts/ota_push.py`** — serves `build/release`, resets the board, waits for `[net] ip=`, sends `OTA_URL`, and streams `[OTA]` progress to reboot. ~5 s over WiFi. `--all` updates **every** detected board in parallel, each with its variant's binary from `build/release*` (serial output is prefixed per-board). |
| Fresh board, or first flash after changing the OTA flags | `arduino-cli upload` (below) **once** — then switch to `ota_push.py` |
| Device can't reach WiFi / serial OTA path | `arduino-cli upload` (below) |

> **Do NOT use `arduino-cli upload` for routine dev iteration.** It is ~80 s
> at 115200 baud, monopolizes the serial port, and — when the FQBN is dropped
> or the cache is stale — can flash the wrong partition table or image.
> `ota_push.py` exists precisely so OTA is less effort than USB. Agents:
> if you reach for `arduino-cli upload` while a flagged dev build is running,
> stop and use `ota_push.py` instead.

First flash over USB (only the cases in the table above):

```bash
arduino-cli upload -p /dev/cu.usbserial-XXXX -b esp32:esp32:jczn_2432s028r:PartitionScheme=custom --upload-property upload.speed=115200 cyd-horizon
```

> **Keep the upload FQBN identical to the compile FQBN.** `PartitionScheme` is
> resolved at *compile* time (it generates `partitions.bin`). If the upload FQBN
> drops `:PartitionScheme=custom`, `arduino-cli` may fail to find the matching
> build and silently recompile with the **default** scheme, then flash a default
> partition table over the custom one — scrambling where the OTA slots, `spiffs`,
> and `logos` partitions live. If you already built a custom image, flash it
> directly with `--input-dir <dir>` instead.
>
> **`upload.speed=115200`.** The CYD's onboard CH340 USB-serial chip is
> unreliable at the board's default 921600 baud (drops mid-flash); 115200 works
> on every variant. Set it via `--upload-property` so the FQBN stays identical
> to compile, rather than a board-menu option that would change it.

> **Use `--clean` for reliable local builds.** `arduino-cli` incremental-build
> caching can silently reuse a stale object file for a `.ino` that changed, and
> `arduino-cli upload` will happily flash that stale binary — so what runs on
> the device may not match your source. This bit us in practice: a TLS fix was
> added to `flight_details.ino` but the flashed image was an older build, which
> masked the result of a hardware test and wasted several flash cycles. For any
> build whose output you intend to trust (especially before flashing), add
> `--clean` to force a full rebuild:
> ```bash
> arduino-cli compile --clean --fqbn esp32:esp32:jczn_2432s028r:PartitionScheme=custom \
>   --build-property "compiler.cpp.extra_flags=-DAPP_VERSION=$(jq -r '.["."]' .release-please-manifest.json)-dev" cyd-horizon
> ```
> A `--clean` build also surfaces compile errors that a stale cache would hide
> (e.g. a call to a method that doesn't exist in the installed core), so it's a
> good habit before flashing when you're iterating on the device.

> **Version flag:** the `-DAPP_VERSION` flag above derives the local build's
> version from `.release-please-manifest.json` at compile time via `jq`
> (`brew install jq` if missing), so About always
> shows an accurate version without needing manual updates. Add the `-dev`
> suffix (as in the blocks below) for a development build — `isDevBuild()`
> keys off it to enable serial diagnostics and dev-only features. If you
> compile without it (e.g. from the Arduino IDE), `kVersion` falls back to a
> hardcoded literal in `cyd-horizon.ino` that can drift out of date - prefer
> the `arduino-cli` command above.
>
> **Build number:** `BUILD_NUM` is optional and can be passed the same way
> (`-DBUILD_NUM=<n>`). The sketch defaults to `0` if it is not defined, and
> `settings.ino` hides the build number on the About screen when it is `0`.
>
> **Dev build with local OTA + build number:** a dev build that can pull a
> firmware image over plain HTTP needs **three** flags together — all are
> required, and each is a silent no-op without the others:
> - `-DAPP_VERSION=...-dev` — `isDevBuild()` gates the HTTP OTA path
>   (`scheduleOTA` in `wifi_config.ino`).
> - `-DENABLE_LOCAL_OTA=1` — allows `performOTA()` to fetch over `http://`
>   (production builds reject non-HTTPS URLs).
> - `-DENABLE_SERIAL_PROVISION=1` — compiles in `handleSerialCommands()`, the
>   runtime serial listener in `loop()` that parses `OTA_*` commands; without
>   it the commands are ignored (it also enables the ~4s `KEY=VALUE`
>   provisioning window at boot, `PROV: listen 4s for KEY=VALUE`).
>
> A typical dev build and first flash (`build/release` is git-ignored):
> ```bash
> arduino-cli compile --clean --fqbn esp32:esp32:jczn_2432s028r:PartitionScheme=custom \
>   --build-property "compiler.cpp.extra_flags=-DAPP_VERSION=$(jq -r '.["."]' .release-please-manifest.json)-dev -DBUILD_NUM=1 -DENABLE_LOCAL_OTA=1 -DENABLE_SERIAL_PROVISION=1" \
>   --output-dir build/release cyd-horizon
> arduino-cli upload -p /dev/cu.usbserial-XXXX -b esp32:esp32:jczn_2432s028r:PartitionScheme=custom --upload-property upload.speed=115200 --input-dir build/release cyd-horizon
> ```
>
> **Iteration: after the first USB flash, use triggered local OTA for every
> later dev update.** The USB flash above is only needed to get a dev build
> with these flags onto the device. Once it's running, rebuild into the same
> `build/release` and trigger a local OTA — it's much faster than serial
> flashing and keeps the USB port free for a monitor. Keep the three flags on
> every iteration so each new image can still accept the next OTA trigger.
>
> The easy way is **`scripts/ota_push.py`** — one command that serves
> `build/release/` over HTTP, resets the board, waits for the `[net] ip=`
> line (so WiFi is up before triggering — avoiding the `code=-1` race noted
> below), sends `OTA_URL`, and streams `[OTA]` progress until reboot:
> ```bash
> cyd-horizon/.venv/bin/python scripts/ota_push.py   # --board e32r40t or --port /dev/cu.usbserial-XXXX if ambiguous
> ```
>
> If the board's boot log reports a release build (`[boot] version=` without
> `-dev` — printed on every build now), the push falls back to a one-time USB
> flash of the same build dir instead of waiting out the WiFi timeout:
> release firmware has no serial-OTA listener, so that board gets a dev build
> over USB and accepts OTA from then on. `--all` USB-flashes release boards
> the same way. Boards running firmware old enough to not print `version=` at
> all can't be distinguished up front and still hit the timeout — flash them
> once with `scripts/flash.py`.
>
> Manual alternative: serve the `.bin` over HTTP from a machine the board can
> reach (a static server at the build directory's root), then pass the full
> URL to the device:
> ```bash
> python3 -m http.server 8080 --directory build/release
> ```
> ```text
> OTA_URL=http://<host-ip>:8080/cyd-horizon.ino.bin
> ```
> Alternatively, a server that maps `GET /firmware?file=<name>` to `build/release/<name>`
> on port 8080 supports the `OTA_IP`/`OTA_FILE`/`OTA_GO` form (see
> `buildOtaUrl` in `wifi_config.ino`), which is convenient when the host IP
> rarely changes:
> ```text
> OTA_IP=192.168.4.137
> OTA_FILE=cyd-horizon.ino.bin
> OTA_GO
> ```
>
> `OTA_VER=<label>` (e.g. `OTA_VER=1.18.0-dev, Build 25`) sets the version
> label the OTA progress screen shows after "Updating to v" — send it before
> the trigger command. `ota_push.py` does this automatically: it extracts the
> `CYD_TAG=...` string the firmware embeds in the binary (`kBuildTag` in
> `cyd-horizon.ino`, built from the same `APP_VERSION`/`BUILD_NUM` flags)
> and sends it ahead of `OTA_URL`. Without it the screen falls back to "dev".
>
> **Avoid a reset before sending the command.** Many serial terminals and
> programming tools pulse DTR/RTS on open, which resets the ESP32 into a fresh
> boot. When the OTA command is captured in the boot provisioning window, the
> OTA task can start before the WiFi link is ready and `HTTPClient` returns
> `code=-1` (connection failed). Use a terminal that does **not** reset on open
> — on macOS `screen /dev/cu.usbserial-XXXX 115200` worked without resetting
> the board, while `pyserial` and `arduino-cli` did. Sending the command once
> the device has already connected to WiFi is the most reliable approach.
>
> Commands are sent over the USB serial console at **115200 baud**. `OTA_IP`
> persists to NVS (`prefs.putString("otahost", ...)`), so on later reboots only
> `OTA_GO` is needed. Successful scheduling logs `CMD: OTA scheduled from <url>`;
> the download runs on a dedicated OTA task (`otaTaskEntry`) and the device
> reboots when done (`[OTA] attempt 1 got=...` then `SW_CPU_RESET`). Newer
> builds also wait up to 20 s for `WL_CONNECTED` inside `performOTA()` so an
> at-boot trigger is less likely to race the link.
>
> **Monitoring the serial console:** the board logs at 115200 baud. Use the
> actual USB-serial port the board is on; on macOS this is typically
> `/dev/cu.usbserial-XXXX`, on Linux `/dev/ttyUSB0` or `/dev/ttyACM0`:
> ```bash
> cyd-horizon/.venv/bin/python scripts/serial_monitor.py   # --reset for a clean boot log
> # or:
> arduino-cli monitor -p /dev/cu.usbserial-XXXX --config baudrate=115200
> ```
> Or use any 115200 terminal such as `screen /dev/cu.usbserial-XXXX 115200`,
> `picocom -b 115200 /dev/cu.usbserial-XXXX`, or `minicom -D /dev/cu.usbserial-XXXX -b 115200`.
> Only one process can hold the port at a time, so close the monitor before
> `arduino-cli upload` or sending an `OTA_*` command.
>
> **Once a dev build with `ENABLE_LOCAL_OTA` + `ENABLE_SERIAL_PROVISION` is
> already on the device, use local OTA instead of `arduino-cli upload`.** It is
> much faster (the image downloads over Wi-Fi in seconds instead of ~80 s of
> serial flashing at 115200 baud), it does not need the USB port free (so a
> serial monitor can keep logging), and the serial commands above stay usable —
> only the *first* dev flash after changing these flags needs to go over USB.
> Keep the three flags on every iteration so each new image can still accept the
> next OTA trigger.

> **Important:** the display pinout is configured in the sketch's own
> `cyd-horizon/tft_setup.h`. TFT_eSPI auto-detects a `tft_setup.h` in the
> sketch folder and uses it instead of its `User_Setup_Select.h`, so **no edits
> to the installed TFT_eSPI library are needed** and the build is identical
> locally and in CI. It sets the correct CYD wiring (TFT on HSPI, touch on VSPI,
> touch IRQ GPIO 36); touch-wake from deep sleep uses GPIO 36.

## Continuous integration & releases

GitHub Actions builds and releases the firmware on standard hosted runners
(these are free/unlimited for **public** repos). Two workflows live in
`.github/workflows/`:

- **`pr.yml`** — runs on every PR to `main`:
  1. **Lint** the PR title with `amannn/action-semantic-pull-request` (conventional
     commits). This is **required** for release-please to determine the next
     version in `release.yml`, so PR titles must look like `feat: Add widget`,
     `fix: Correct scaling`, `chore: Update docs`, etc.
  2. **Build** the firmware: installs arduino-cli, the `esp32` core (3.3.11),
     TFT_eSPI (2.5.43) and ArduinoJson (7.4.3), then compiles with the
     production FQBN. (The CYD display pinout comes from the sketch's own
     `tft_setup.h`, so no library patching is required.)

> **Docs-only PRs skip the firmware compile.** A `changes` gate job in
> `pr.yml` lists the PR's files; if every one is docs/release metadata
> (markdown, `docs/`, `LICENSE`, `.gitignore`, release-please manifest/config —
> so release-please's own PRs qualify), it passes `skip=true` to
> `build-firmware.yml`, whose heavy steps are skipped and the job succeeds in
> seconds. The skip must happen inside the called workflow — a job-level `if`
> reports the "Build firmware" check as *skipped*, which the required-check
> ruleset does not accept (it would block the merge), and a workflow-level
> `paths` filter would leave the check "Expected" forever. Any file outside
> the skip-list defaults to building.
>
> The gate answers "can this PR break the firmware build?", not "will this PR
> release?". Releasability is decided separately by release-please from the
> conventional commit type: only `feat`, `fix`, `perf`, `revert`, and breaking
> changes are user-facing — `ci`, `chore`, `docs`, etc. merge without producing
> a release PR. So a `ci:` PR still compiles in CI but cuts no release.

- **`release.yml`** — runs on every push/merge to `main` and drives the release
  flow with `googleapis/release-please-action@v4`, configured by
  `release-please-config.json` + `.release-please-manifest.json`:
  1. release-please opens a **"release-please" PR** that bumps the version in
     `.release-please-manifest.json` (the manifest is the single source of
     truth for the version) and updates `CHANGELOG.md` based on the
     conventional-commit PRs merged since the last release. It only runs when
     the manifest version already has a published release, so merging a
     release PR can't spawn a stale next-release PR before its tag exists.
     Merge that PR through the normal review process.
  2. Once merged, the workflow builds the OTA firmware with
     `-DAPP_VERSION=<version>` (so **Settings → About** shows the release
     version), creates a draft GitHub **release**, attaches
     the per-board `.bin` assets (raw app images for the inactive OTA slot), and
     publishes it (release-please runs with `skip-github-release`, so devices
     never see a release before the `.bin` is attached).
  3. After the release is published, the merged release-please PR is marked
     `autorelease: published` so the next release cycle is not blocked.

The version shown on the About screen comes from the `APP_VERSION` compile-time
macro (`kVersion` in `cyd-horizon.ino`); it falls back to a hardcoded
`0.0.0-dev` literal when the flag isn't set, so local builds work without it —
pass the flag as shown above so About shows the real version. Any `-dev`
version is treated as a **dev build** that never auto-updates (see below). OTA
*delivery* of the `.bin` to a device is handled on-device — see **OTA updates
(firmware delivery)** below.

> **Token:** `release.yml` authenticates release-please with a dedicated GitHub
> App installation token (minted by `create-github-app-token` from the
> `RELEASE_PLEASE_CLIENT_ID` / `RELEASE_PLEASE_PRIVATE_KEY` secrets) rather than
> the default `GITHUB_TOKEN`. This is what lets the bot open/update the release
> PR and create the release even where the org blocks the default token from
> opening PRs. If release PRs ever stop picking up checks, verify those secrets
> are still set on the repo.

## OTA updates (firmware delivery)

The device updates itself over WiFi from this repository's public GitHub
releases. All of this lives in `cyd-horizon/ota.ino`.

### How it works

1. **Check.** `fetchLatestRelease()` GETs
   `https://api.github.com/repos/improving-minnesota/cyd-horizon/releases/latest`
   (no auth — the repo is public), parses the `tag_name` (e.g. `v1.0.1`), and
   finds the board's `OTA_ASSET` asset URL.
2. **Compare.** `compareVersions()` / `isNewerThanRunning()` strip the leading
   `v` and compare semver against the running `kVersion`.
3. **Install.** `performOTA()` downloads the `.bin` in 4 KB chunks, streams them
   to the inactive OTA slot via `Update.write()` (`U_FLASH`), shows a progress
   screen, then `Update.end()` + `ESP.restart()`. On success it never returns.
   It runs on a **dedicated 12 KB-stack task** (`otaTaskEntry`, `g_otaTask` in
   `cyd-horizon.ino`), created once at boot and left idle until an OTA is
   requested, because the mbedtls TLS handshake overflows the ~8 KB default
   `loopTask`. The OTA task owns the display, so the main loop yields while it
   runs. It is **not** subscribed to the task watchdog, so there is no
   `esp_task_wdt_reset()` call during the download; the HTTP timeouts bound it.

### What triggers a check

- **Daily auto-scan** (`maybeAutoUpdate()`), run once per calendar day
  (tracked in NVS as the epoch day under `lastscan`). When a new day is
  detected — first WiFi connect after boot, or midnight rollover on an
  always-on device — the scheduler in `loop()` picks a random time within
  `AUTOSCAN_JITTER_S` (1 h) so fleets that wake together don't hit GitHub in
  the same minute. At fire time the scan is deferred past any enabled alarm
  occurrence within `AUTOSCAN_ALARM_QUIET_S` (±1 h, via `nextAlarmAfter()`,
  which covers scheduled firings and parked snoozes); if deferring would land
  past the next sleep-window start — i.e. no quiet slot remains while the
  device is awake — it takes `bestEffortSlot()` instead, the awake moment
  furthest from any firing, so dense alarm schedules can't starve the update.
  The scheduler is also fully gated off inside the sleep window — a
  touch-wake or alarm-due wake there can never start an OTA, whose reboot
  would play the boot sound at night; a pending scan fires at the first
  awake opportunity after the window ends.
  If Auto-Update is ON and a newer release exists, it starts the OTA.
  Toggling **Auto-Update** ON in **Settings → General** clears `lastscan` and
  schedules an immediate scan (the alarm window still applies).
- **Manual** — opening **Settings → About** triggers a check (via the net task,
  so the UI doesn't freeze); if newer, it shows **Upgrade Available (vX.Y.Z)**
  with an **Install** button.

### Auto-Update toggle & dev builds

- `g_autoUpdate` defaults to `true` (persisted as `autoupd`).
- A **dev build** (version ending in `-dev`) forces Auto-Update OFF for that
  boot, so flashing source never silently upgrades — but it does **not** change
  the saved preference in NVS. Your Auto-Update setting is preserved, so it's
  still ON when you later flash a release build. Dev builds can still update
  manually from About or by toggling Auto-Update on.

### TLS

Verified HTTPS connections use `httpsBegin()`/`httpsRequestRetry()`, which
picks the smallest bundled root set that covers the target host:

- `kIsrgRootCAs` — OpenSky, open-meteo, Nominatim, and GitHub release assets
  (`*.githubusercontent.com`) (ISRG / Let's Encrypt).
- `kAmazonRootCAs` — Govee (Amazon Root CA 1).
- `kSectigoUSERTrustEccRootCAs` — GitHub API hostnames (`github.com`, `api.github.com`)
  (Sectigo / USERTrust ECC root).
- `kGlobalSignEccRootCAs` — `vrs-standing-data.adsb.lol` route data (GlobalSign ECC
  Root CA - R4, bundled with the WE1 intermediate because the server does not
  always send the full chain).

There is no fallback bundle; an unmapped host returns `nullptr` from
`trustStoreForUrl()` and `httpsBegin()` fails cleanly rather than silently using
a bundle that may not cover the host's chain.

### OpenSky authentication

`openskyEnsureToken()` mints and caches an OAuth2 bearer token from
`auth.opensky-network.org`. Tokens are cached until `g_osTokenExpiry` (the
response's `expires_in`, or 30 minutes by default, minus a 30-second safety
margin) and reused until then. When credentials are changed in Settings or via
serial provision, `invalidateOsAuth()` drops the cached token, resets the
`AUTH_BAD` / 401 backoff state, and requests an immediate re-poll.

On a 401 from the `/states/all` radar poll, `fetchFlights()` now **immediately**
in-mints a fresh token and retries once in the same cycle. If that retry also
401s, `g_authState` becomes `AUTH_BAD` and the dashboard shows `Invalid OpenSky
Creds`. A `g_auth401Streak` counter clears on any successful poll; after 5
consecutive 401s it falls back to the 15-minute `CREDIT_RECOVERY_MS` cadence to
avoid hammering the API.

`dashboardCriticalLabel()` now also reports `OpenSky Data Unavailable` for radar
TLS/HTTP/parse failures and `Weather Data Unavailable` for failed open-meteo
fetches, keeping the LED and border in sync with the data state.

`OTA_CA_EXPIRY` is still the earliest root expiry; past that **only the OTA
path** falls back to `setInsecure(true)` so a root rotation can't block
updates. Data fetches never take that fallback. There is **no** insecure retry
on a failed handshake — that would let a man-in-the-middle defeat certificate
validation.
Transient transport failures on these
verified connections (e.g. a connect dropped after prolonged uptime) are retried
a few times with clean socket teardown between attempts — always over the same
verified TLS, never insecure. GitHub release downloads are redirected from
`github.com` to a `githubusercontent.com` asset host; `performOTA()` follows
those redirects manually so each TLS connection receives the trust store for
its actual host. Firmware integrity is independently pinned:
`performOTA()` hashes the streamed image (SHA-256) and compares it to the asset's
`digest` from the GitHub API before flashing (an empty digest skips the check).
A mismatch aborts the update without touching the running slot.

**TLS heap arena.** An mbedTLS handshake peaks at ~60–70 KB total (two ~16.7 KB
record buffers — `MBEDTLS_SSL_MAX_CONTENT_LEN=16384` is compiled into the
shipped libs — plus certificate parsing and RSA workspace) — the app's largest
contiguous need and its biggest alloc/free churner. On a PSRAM-less board, long
uptimes fragment the main heap until a big record-buffer alloc can't find a
contiguous block; connects then fail and only a reboot recovers (the ESP-IDF
heap has no compaction). `setup()` therefore installs a private arena:
`mbedtls_platform_set_calloc_free()` routes mbedTLS allocations **≥4 KB** into
a 40 KB block managed by `multi_heap` (`s_tlsArena` in `cyd-horizon.ino`),
while smaller allocations stay on the main heap. Each side falls back to the
other on failure, so a miss never fails outright.

Size-routing matters: the *whole* handshake does not fit in a 40–56 KB arena
(the CA-bundle parse alone is ~8–16 KB of small allocs), and oversizing the
arena starves WiFi RX pbufs — which is what stalled OTA downloads mid-stream.
The arena only needs to guarantee the big, fragmentation-sensitive buffers;
small allocs tolerate a fragmented heap. It's `heap_caps_malloc`'d once at boot
(a static array overflows `dram0_0`); the stock core ships
`MBEDTLS_PLATFORM_MEMORY` enabled so the runtime hook needs no rebuilt
libraries. TLS is serialized by design (one fetch at a time; the OTA task waits
on `netBusy`), so the arena only ever serves one connection; a `portMUX_TYPE`
spinlock guards the multi_heap (a FreeRTOS semaphore is NOT interchangeable —
it deadlocks). Dev builds log `arenafree=` around each TLS attempt plus
`arena-miss`/`both-fail` diagnostics.

> TODO: update the certificate-bundle documentation once the trimmed
> `kIsrgRootCAs` set (X1, X2, Root YR, Root YE — see "TLS" above) is final.
> Document which anchors cover which chain shapes (classic R/E intermediates
> vs Gen-Y cross-signed vs bare roots) and the Let's Encrypt certificate
> sources used to refresh the PEMs.

### HTTP body streaming and JSON parsing

Large OpenSky responses (`/states/all`, `/tracks/all`) are no longer fully
buffered into a single `JsonDocument`. `cyd-horizon/http_body.h` provides a
framing-aware `HttpBodyStream` wrapper that:

- Buffers reads in 512-byte chunks (ArduinoJson otherwise asks one byte at a
time, each triggering a full `mbedtls_ssl_read()` round trip).
- Knows `Content-Length` and `Transfer-Encoding: chunked` boundaries.
- Reports `stalled()` and `complete()` so a short body is detected instead of
being treated as an empty response.

`HttpBodyStream` is paired with `seekArray()` and `nextElement()` helpers so
`fetchFlights()` in `cyd-horizon.ino` and `fetchTrack()` in
`flight_details.ino` parse the `states` and `path` arrays one element at a time.
Peak parse heap for a 50 KB response drops from ~48 KB to a few kilobytes (one
state row or one track point at a time).

On dev builds, boot and each OpenSky request log the loaded NVS coordinates and
actual bounding box. This is useful for distinguishing an NVS/location race
from a legitimate `{"states":null}` response for a small area. The location
shown in Settings and the location used for a request should therefore be
compared with the `[boot] NVS location` and `[net] flights query` lines.

`BoundedAllocator` in `cyd-horizon.ino` caps the working set for each parse
using `malloc_usable_size()`-based accounting, so the cap is a real working-set
limit rather than a churn counter. Weather, Govee, token, IP location,
geocoding, route, and release metadata parsing also uses bounded allocators and
checks the framed body before accepting the result.

Dev builds log the internal free heap and largest internal block around TLS
attempts and in periodic heap diagnostics. This distinguishes total free heap
from the contiguous internal block most relevant to mbedTLS handshakes.

The `dirty` redraw flag is volatile because the network task can request a
redraw while the loop task is rendering. The loop clears it before, rather than
after, drawing so a concurrent network update cannot be lost. This matters for
the bottom-left status text: a completed flight request must replace the boot
`connecting...` text on the next redraw.

### Rollback

After OTA the new slot boots in the ESP32's "pending verify" state. If it
crashes before a grace period, the bootloader rolls back to the previous slot.
`markAppValidBoot()` is called ~30 s into a successful run to cancel that
rollback and lock in the new firmware.

### Partitions

OTA requires two app slots + `otadata`, already present in `partitions.csv`
(`app0`/`app1` at 1.5 MB each, plus `otadata`). The release `.bin` is the **raw
app image** for the inactive slot, built by the release workflow with
`PartitionScheme=custom`.

### Release versioning (release-please)

Versioning is fully automated — you never hand-edit
`.release-please-manifest.json` or bump the version yourself. release-please
reads the current version from the manifest, then derives the **next** version
from the conventional-commit **PR titles**
merged to `main` since the last release, following Semantic Versioning
(`MAJOR.MINOR.PATCH`):

| PR title | Bump | Example |
|---|---|---|
| `fix: ...` — a bug fix / correction | **PATCH** | `1.2.3 → 1.2.4` |
| `feat: ...` — a new feature or behavior change | **MINOR** | `1.2.3 → 1.3.0` |
| Breaking change — `feat!: ...` / `fix!: ...`, or a commit body with a `BREAKING CHANGE:` footer | **MAJOR** | `1.2.3 → 2.0.0` |
| `docs:`, `chore:`, `refactor:`, `build:`, `ci:` — no functional change | **no release** on its own | — |

How to decide (guidance for this project):

- **PATCH (`fix:`)** — a bug fix or correction that doesn't add functionality.
  Example: `fix: Correct pool graph scaling`.
- **MINOR (`feat:`)** — a new feature or setting, or any change to behavior a
  user would notice. Example: `feat: Add "Blink for Flight" toggle`.
- **MAJOR (breaking)** — a genuinely breaking change, e.g. a partition-table
  change, an incompatible NVS/credentials layout, or anything that requires the
  user to re-provision or re-flash from scratch. Reserve this for real breakage.
  Example: `feat!: Move airline logos to a dedicated partition` (the change that
  reformatted LittleFS).

Behavior notes:

- When a batch of merged PRs contains multiple bump types, the **largest
  applicable bump wins** (breaking > feat > fix).
- A **release-please version-bump PR** is opened automatically after a
  conventional-commit PR merges. Merging that PR updates
  `.release-please-manifest.json` (and `CHANGELOG.md`). Pushing that merge to
  `main` triggers the `release.yml`
  workflow, which builds the firmware, creates and publishes the
  `vMAJOR.MINOR.PATCH` GitHub release, attaches the per-board `.bin` assets, and
  then marks the release-please PR as `autorelease: published` so the next
  release cycle is not blocked. The build job bakes the new version into
  **Settings → About**.
- Do not hand-edit `.release-please-manifest.json`. If a release-please PR ever
  gets stuck with an `autorelease: pending` label after the release is live,
  the workflow now corrects it automatically.
- Non-functional changes (`docs`, `chore`, `refactor`, `build`, `ci`) update the
  changelog but, on their own, do not trigger a release.

## Partition table

`cyd-horizon/partitions.csv` is a custom partition table (the Arduino
build system picks up a `partitions.csv` in the sketch folder automatically).
It keeps two OTA-capable app slots, trims the app slots from the stock
"default" size, and carves out a dedicated LittleFS **logos** partition so
airline logos are never compiled into the firmware (see "Airline logos"
below):

| Partition | Stock "default" | This project | Notes |
|---|---|---|---|
| app0 / app1 (each) | 1.31 MB | 1.5 MB | Two OTA slots |
| spiffs (LittleFS) | 1.375 MB | 384 KB | Pool + weather temp history |
| logos (LittleFS) | — | 512 KB | Airline logos (runtime) |

384 KB of spiffs is still comfortably above the combined pool + weather temp
history worst-case usage. Pool's raw/hourly/daily CSV tiers peak at roughly
150-200 KB before compaction trims them; weather is sampled half as often
(every 10 min vs pool's 5 min) so its tiers peak at roughly 50-110 KB. Combined
they stay well under 384 KB even before compaction, so this isn't expected to
be a practical constraint. The 512 KB logos partition holds ~364 KB of the current
54 logos, leaving ~148 KB of headroom to add more without ever changing the
table again.

Here's the same layout drawn to approximate proportion (bar widths are relative;
exact offsets/sizes are in the table above):

```
0x0000   ┌ HD  bootloader + partition table + NVS (0x9000) + otadata (0xE000)
0x10000  ├─ app0     ██████████████████████████████████████████ 1.5 MB   (OTA slot 0)
0x190000 ├─ app1     ██████████████████████████████████████████ 1.5 MB   (OTA slot 1)
0x310000 ├─ spiffs   ████████████ 384 KB   (pool + weather temp history, "spiffs")
0x370000 ├─ logos    █████████████████ 512 KB   (airline logos, "logos")
0x3F0000 └─ coredump ████ 64 KB
0x400000   (end of 4 MB flash)
```

Key takeaways from the map:

- **app0 / app1** — the two OTA slots. OTA writes the inactive slot and swaps,
  so a failed update rolls back automatically. They hold the logo-less firmware
  (~1.15 MB), leaving ~350 KB of headroom per slot.
- **spiffs** — pool + weather temperature history (LittleFS, global `LittleFS`).
- **logos** — airline logos as files (LittleFS, separate `LogosFS` instance).
  OTA never touches this partition, so logos persist across updates; add/remove/
  update them by re-provisioning files, not reflashing.
- **HD** — system header: bootloader (0x1000), partition table (0x8000), NVS
  settings/credentials (0x9000, survives re-partition since its offset is
  unchanged), and otadata for OTA rollback selection.

Because the FQBN's declared max size only matches reality when
`PartitionScheme=custom` is selected (otherwise the tool still assumes the
stock "default" ceiling and may reject a build that's actually well within the
real partition), **always build/upload with `:PartitionScheme=custom`** as
shown above.

> **One-time consequence when applying this change to an already-provisioned
> board:** NVS (settings/credentials) keeps the same offset, so it survives.
> The LittleFS partitions (pool "spiffs" and "logos") move to different flash
> offsets, so any existing pool/weather temp history data does not carry over —
> LittleFS will format fresh on the next boot — and the logos partition must be
> provisioned once (see "Airline logos"). This is a one-time effect of
> adopting the new partition table, not something that happens on every flash
> afterward. **Once provisioned, do not change the partition table again:**
> moving app0/app1 shifts where OTA updates land, and any table change
> reformats the LittleFS partitions.

> **⚠ Do NOT flash the full `*.merged.bin` at offset `0x0` for a routine
> firmware update.** The merged image is a complete 4 MB dump of the entire
> flash — it overwrites **NVS** (all settings, credentials, and touch
> calibration at `0x9000`), the **spiffs** pool/weather history (`0x310000`),
> and the **logos** partition (`0x370000`) with erased (`0xFF`) data. Flashing
> it wipes the device's saved data and forces a full first-boot re-setup.
>
> To update the firmware while keeping NVS / history / logos, flash **only the
> app image** to the active app slot:
> ```bash
> esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX --baud 460800 \
>   write_flash 0x10000 <build>/cyd-horizon.ino.bin
> ```
> (or use `arduino-cli upload` / OTA, both of which write only the app).
> Reserve a full `0x0` merged flash for a deliberate clean re-provision /
> factory reset.

### Reducing flash usage

1. **Partition scheme** (see above) - the single biggest lever. Going from the
   stock "default" scheme's 1.31 MB app partition to this project's 1.5 MB
   custom one freed a lot of room.
2. **Keep logos out of the app** - the airline logos (once the largest chunk of
   the binary, ~364 KB for 54 logos) are no longer compiled into the firmware;
   they live in the dedicated LittleFS **logos** partition and are read at
   runtime. The OTA app is therefore logo-less and small. If you add a logo,
   it counts against the 512 KB logos partition's headroom, not the app slots.
   The logo box size (`BOX_W`/`BOX_H` in `scripts/convert_logos.py`) still trades
   detail for flash directly (each doubling of width and height quadruples the
   per-logo byte count), which matters for fitting logos in the partition.

When app flash usage becomes tight, the biggest levers are growing the app
slots at the cost of the logos partition, or reducing other data (e.g. the
OpenSky airport city lookup table in `flight_details.ino`).

## Temperature history persistence (pool + weather)

The pool-temp and weather-temp history graphs (Day / Week / Month / Year) are
both backed by a tiered store on the LittleFS (spiffs) flash partition, so the
data survives reboots and deep-sleep wakes. The two features use the same
scheme (see `poolfs.ino` for pool, `weatherfs.ino` for weather) but different
files and sampling rates.

**Pool** (Govee thermometer, ~every 5 min):

- `/pool.csv` — raw `epoch,temp` samples, logged roughly every 5 minutes. Feeds
  the Day & Week graphs. Compacts to keep the newest ~2500 lines (~8.7 days).
- `/pool_hour.csv` — hourly `epoch,avg,lo,hi` rollups. Feeds the Month graph.
  Compacts to keep the newest ~900 lines (~37 days).
- `/pool_day.csv` — daily `epoch,avg,lo,hi` rollups. Feeds the Year graph.
  Compacts to keep the newest ~800 lines (~2.2 years).
- `/pool_rollup.bin` — the in-progress hour/day accumulators, saved right
  before each deep sleep and restored at boot (see below).

**Weather** (Open-Meteo current temperature, ~every 10 min — half the pool
rate — so roughly half the raw samples for the same retained time range):

- `/weather.csv` — raw `epoch,temp` samples, logged on each weather fetch
  (every ~10 min while awake). Feeds the Day & Week graphs. Compacts to keep
  the newest ~1200 lines (~8.3 days).
- `/weather_hour.csv` — hourly `epoch,avg,lo,hi` rollups. Feeds the Month
  graph. Compacts to keep the newest ~900 lines (~37 days).
- `/weather_day.csv` — daily `epoch,avg,lo,hi` rollups. Feeds the Year graph.
  Compacts to keep the newest ~800 lines (~2.2 years).
- `/weather_rollup.bin` — the in-progress hour/day accumulators, saved before
  each deep sleep and restored at boot.

Weather history is **always on** (no enable toggle). It's logged from
`fetchWeather()` via `weatherLog()` in `weatherfs.ino`.

Each tier's `*_rollup.bin` holds the in-progress hour/day accumulators, saved
right before each deep sleep and restored at boot. Each 5-minute deep-sleep
wake is a fresh boot, so without this the hourly/daily tiers would never flush
during the night (starving the Month/Year graphs of overnight data). The
Month/Year graphs also fold that in-progress bucket into the displayed range
(`plotSeries`' `pend` param): its lo/hi widen the Lo/Hi labels and y-scale and
its partial average counts toward the avg line, so today's extremes — the
current reading included — count before the bucket flushes. It is not drawn
as a point; the plotted line still ends at the last flushed bucket. Both
views additionally widen the range with the finer tiers' in-window extremes
(`widenRange()` — Month reads the raw ring, Year reads the hourly + raw
rings), so the shown Lo/Hi are the true extremes of all retained data, not
just the plotted tier's.

At boot the CSV tiers are loaded back into RAM ring buffers (keeping the
newest samples) so the graphs can draw them immediately. The graphs show
"Waiting for time sync..." until NTP has synced, so they never plot samples
against an unsynced clock.

On a freshly flashed board the spiffs partition has never been initialized —
sketch-only flashes (bootloader, partition table, app) never write the data
partition — so the first mount would fail. `poolfsInit()` therefore calls
`LittleFS.begin(true)` to format the partition once on that first mount
failure; afterwards it mounts normally. `weatherfsInit()` runs after it and
shares that same mounted partition. If the partition is moved or erased
(e.g. a partition-table change), it is formatted once again and the previous
history is dropped.

Data collection runs both while awake and while asleep. Pool logging happens
only while the Pool Temp feature is enabled (`g_poolEnabled` gates every fetch
path: boot, first-connect, the 5-min poll, and the sleep wake); the weather
log has no toggle. The first data load of a boot (weather, pool, flights,
and the IP-location guess on a first boot) fires from `loop()` at a random
offset within `BOOT_FETCH_JITTER_MS` (30 s), gated on SNTP-synced time up to a
10 s cap — verified TLS fails on future-dated certs before sync — so a fleet
restored by the same outage spreads its calls while still landing data inside
~1 min. While asleep the
deep-sleep timer wakes every ~5 min (sooner if an alarm's `nextFire` lands
first — see "Alarms"); the **same single wake** connects, pauses a random
`SLEEP_WAKE_JITTER_MS` (boards that powered up together share the 5-min wake
phase), logs
the pool temp (when enabled) and the weather temp, and goes back to sleep —
weather logging
adds **no additional wake-ups**, only one extra HTTPS call within the wake the
pool already needs.

Both providers rate-limit: Govee returns `X-RateLimit-*` (per-day) and
`API-RateLimit-*` (per-minute) headers with `*-Reset` as a UTC epoch plus
`Retry-After` on 429; Open-Meteo's 429 body names the exceeded bucket
("Minutely/Hourly/Daily..." or "Too many concurrent requests"), each resetting
on its UTC boundary. On a 429 either fetch parks until the stated deadline —
persisted in NVS (`goveerl` / `wxrl`) so deep-sleep wakes honor it — with
`esp_random()` jitter so boards sharing an API key / NAT IP don't resume in
lockstep; Open-Meteo "concurrent"/minutely 429s arm a sooner-than-cadence
retry (`g_wxRetryAt`), and an unparsed reason falls back to exponential
backoff escalating to next-UTC-midnight after 5 strikes. A fetch suppressed by
a stored window re-arms to fire when the deadline lifts (`g_wxRetryAt` /
`g_poolRetryAt`) rather than waiting out the cadence, and an absent
weather/pool result retries at a per-boot random 55–65 s for the first 5 min
of uptime — either way a short touch-wake still gets data before re-sleeping.

### Power loss vs. deep sleep

- **Deep sleep** — flash is retained, so all persisted history survives. On
  each wake the ring buffers are reloaded from the CSV files and the rollup
  accumulator is restored, then the wake logs one more sample before sleeping.
- **Power loss (unplugged)** — LittleFS and NVS are non-volatile, so all history
  and settings survive a full power cycle. Collection simply pauses while there
  is no power and resumes where it left off once powered on; the only "missing"
  data is the samples that would have been logged during the outage itself.
- **Re-flash / partition change** — ordinary firmware re-flashes leave LittleFS
  intact. Replacing the partition table (as when this project adopted the custom
  scheme) moves the LittleFS partition offset, so LittleFS formats fresh and any
  prior history is cleared. That is a one-time event, not something that happens
  on every flash.

## Provisioning credentials

WiFi/OpenSky/Govee credentials are **not compiled into the firmware**. Put them
in the git-ignored `cyd-horizon/.env` file (`WIFI_SSID`, `WIFI_PASSWORD`,
`OPENSKY_CLIENT_ID`, `OPENSKY_CLIENT_SECRET`, `GOVEE_KEY`), then stream them
into the device's NVS over USB serial. Network addressing can also be
provisioned this way (`WIFI_MODE=static`, `WIFI_IP`, `WIFI_SUBNET`,
`WIFI_GATEWAY`, `WIFI_DNS`, `WIFI_HOSTNAME`) — the same values as **Settings →
Network → IP Setup** on the device:

```bash
cyd-horizon/.venv/bin/python scripts/provision_config.py --port /dev/cu.usbserial-XXXX
```

First-time setup: `python3 -m venv cyd-horizon/.venv && cyd-horizon/.venv/bin/pip install pyserial`

### How provisioning works

The firmware's `serialProvision()` (in `wifi_config.ino`) listens for a few
seconds at boot, writes each `KEY=VALUE` line straight into NVS, and the script
confirms the `=OK` acks. To change credentials, edit `.env` and re-run the
script — no firmware re-flash needed.

> **Testing-only code, disabled by default:** the provisioning listener is a
> temporary aid and is compiled **out** by default (`ENABLE_SERIAL_PROVISION`
> is `0` in `cyd-horizon.ino`), so the serial `.env` flow above only works
> after you set it to `1` and re-flash. Credentials persist in NVS, so once
> provisioned the board works without the listener. To ship production code,
> leave `ENABLE_SERIAL_PROVISION` at `0` (or delete the guarded block in
> `wifi_config.ino` and the `serialProvision()` call in `setup()`).

Credentials themselves (WiFi, OpenSky, Govee) are entered on the device's
settings screens. For instructions on obtaining them, see the README's
[Getting the credentials](#getting-the-credentials) section.

## Airline logos

The flight view shows an airline's logo (when available) plus its name and
brand color. Two separate lookups drive this:

- **Logo bitmap** — loaded at runtime from a dedicated LittleFS **logos**
  partition (see the partition table above). The logos are **not** compiled
  into the firmware.
- **Name & color** — the `kAirlines` table in `flight_details.ino` (this is
  still compiled in; it's just text/color, not bitmap data).

The PNG source icons in `airline-logos/` are **git-ignored** and kept local,
because we're not sure we can redistribute the brand logos. `scripts/convert_logos.py`
turns them into per-airline `<ICAO>.bin` files (a 12-byte header + raw RGB565
pixels, stored at their on-screen 72x48 size, transparent color `0xF81F`), and
`scripts/provision_logos.py` packs those into a LittleFS image and flashes it to the
logos partition once per device.

At boot, `logosInit()` (in `logos.ino`) mounts the logos partition (an
independent `fs::LittleFSFS` instance on label `"logos"`, separate from the
pool temp history `"spiffs"` one) and pre-allocates one 72×48 RGB565 buffer
(`72 × 48 × 2 = 6,912` bytes) PSRAM-first, falling back to the internal heap.
Only one logo is ever drawn at a time, so a single reusable buffer removes the
previous 16-entry cache that could retain ~110 KB of mid-heap blocks on the
PSRAM-less CYD and erode the contiguous heap needed for TLS handshakes.
`findAirlineLogo()` resolves a callsign's 3-letter ICAO prefix, opens
`/<ICAO>.bin`, and decodes it into the shared buffer; `logoRelease()` is a
no-op. Any failure — partition absent, file missing, or an allocation failure —
simply means **no logo is drawn**; the device never crashes and OTA firmware
never embeds logos.

> **Update cadence:** adding, removing, or replacing a logo is a *filesystem*
> change on the logos partition, not a firmware change. You never touch the
> app slots or recompile for logos, and OTA updates don't affect them.

### Adding a new airline logo

1. **Add a PNG icon** — drop the logo into `cyd-horizon/airline-logos/`,
   e.g. `FedEx Icon.png`.
2. **Map it in `scripts/convert_logos.py`** — add an entry to the `AIRLINES` list:
   `("FedEx", "FDX", "FedEx Express")`. The first item is a keyword matched
   (case-insensitively) against the filename, and must match **exactly one**
   file. If the filename could match another keyword, use a more specific
   keyword and place it **before** the conflicting one — e.g. UPS uses
   `"Parcel"` placed before `"United"` so the UPS icon isn't captured as United
   Airlines.
3. **(Optional) name & color** — if not already present, add the ICAO code,
   display name, and brand color to `kAirlines` in `flight_details.ino`, e.g.
   `{"FDX", "FedEx Express", TFT_PURPLE}`. This drives the name and colored
   badge shown when no logo is available. This one *does* require a recompile.
4. **Generate the logo files and provision them** (no firmware recompile):
   ```bash
   # writes <ICAO>.bin files, packs a LittleFS image, and flashes it to the
   # logos partition of every board detect_boards.py finds (use --port to
   # target just one; --no-flash only builds the image).
   cyd-horizon/.venv/bin/python scripts/provision_logos.py
   ```
   The script fails loudly if any `AIRLINES` keyword matches zero or more than
   one source file. The logos partition layout is identical on every board
   variant, so one image serves them all. Logos count
   against the logos partition's ~148 KB of free headroom (see the partition
   table), not the app slots.

## Settings & status indicators

Settings are stored in NVS under the `"flight"` namespace (see `setup()` in
`cyd-horizon.ino` for the load, and the Reset confirmation in
`handleTouch` for the wipe). Notable keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `timer` | bool | `false` | Show the dashboard countdown/timer bar (Flight Tracker → Enable Timer). |
| `showiata` | bool | `true` | Display route airports as `ICAO | IATA` when ADSB.lol provides an IATA code (Flight Tracker → Show IATA Airports). |
| `clkcol` | uint32 | `TFT_BLUE` | RGB565 theme color from the General → Clock Color picker — tap-target swatch rows for hue, shade, and greyscale (`hsv565()`/`colorPickEnter()`). Drives the header band and every ordinary button; `btnFg()` picks black text on light colors, white on dark, and `btnCol()` nudges buttons a shade darker on light themes / toward white on dark ones. Header Back buttons are ordinary buttons (`backBtn()`); destructive buttons use `dangerCol()` — red, or yellow when the theme is near-red. History graphs draw on the button color (`graphBgCol()`), the data line is the theme pushed 75% toward white/black (`graphLineCol()`), and the avg line is the theme's complement at the same blend (`graphAvgCol()`). Semantic controls keep their own colors. |
| `units` | int | `0` | Device units (General → Units): 0 = Imperial (ft/mph/mi), 1 = Metric (m/kts/km), 2 = Aviation (ft/kts/nm). Radius and ceiling are still stored in miles/feet; the selected unit only changes what's displayed and what the sliders edit — switching units keeps the displayed number and reinterprets it in the new unit. Temperatures are fetched/logged in °F and converted at display (`tempDisp()`), so Metric and Aviation show °C. Migrates the old `metric` bool on first boot. |
| `clock24` | bool | `false` | 24-hour clock (General → Clock). Affects the header clock, time editors, alarm times, and sunrise/sunset; `false` shows 12-hour times with AM/PM markers. |
| `homeap` | string | `""` | Home Airport (ICAO). Used for the LED blink: red when origin matches, green when destination matches. Leave empty to disable. |
| `watchcs` | string | `""` | Watched callsign (Flight Tracker → Watch Callsign (ICAO)) — substring match (`isWatchedCallsign`), case-insensitive: "DAL" matches DAL1234, "5432" matches DAL5432; a lone "*" matches every flight. When a matching plane appears in a poll and its radar blip is inside the screen bounds it is promoted to the tracked flight (closest match wins — `planes[]` is distance-sorted); while shown it alerts with the `watchntf` pattern on the LED + speaker. Leave empty to disable. |
| `watchntf` | int | index of "Radar" | Callsign Notify preset (Flight Tracker → Callsign Notify): index into `kNtfPresets` in `alarms.ino` (49 presets — blink styles, sirens, sweeps, Morse, chimes/bells, melodies). The default is looked up by name at boot so reordering can't change it; persisted indices must still never be reordered — append new presets at the end. Runs on the LED + speaker while the watched flight is shown; independent of `blinkf`. |
| `ntfvol` | int | `100` | Notify Volume (General → Notify Volume): one of 10 levels — 1/2/3/4/5/15/25/50/75/100 percent (`kNtfVolLevels`; the 1% floor keeps notifications audible). The UI shows the level number (1–10); NVS stores the percent. Non-level values stored by older builds snap to the nearest level on load. Scales the LEDC duty cycle for every speaker sound — alarm/callsign patterns, the volume-test beep, and the boot chime (`playBootChime()` at the end of `setup()`). `tone()` can't be used for volume: it fixes duty at ~50%, so `toneWrite()` in `alarms.ino` drives `ledcWrite()` directly. |
| `ipdhcp` | bool | `true` | Network addressing mode (Network → IP Setup). `true` = DHCP; `false` = static using the keys below. |
| `ipaddr` / `ipmask` / `ipgw` / `ipdns` | string | `""` | Static IP, subnet mask, gateway, DNS. Applied via `WiFi.config()`; blank DNS falls back to the gateway, and an incomplete/invalid set falls back to DHCP. |
| `hostname` | string | `"cyd-horizon"` | STA hostname via `WiFi.setHostname()`; applies in both DHCP and static modes. |

The onboard RGB LED blinks for every new overhead flight when **Blink for Flight**
is on (`g_blinkForFlight`, persisted as `blinkf`). `fetchFlights()` recomputes
the color on every poll and re-blinks whenever the route state changes, so a
flight that first appears with no route data still gets the correct color once
its route arrives. Color priority is:

- **Watched callsign** — a configured callsign (`watchcs`) promoted to the tracked flight runs its `watchntf` notification preset (same LED + speaker patterns as alarms) while its details are shown, plus a ~5 s speaker alert once per sighting (re-armed when the watched streak ends or a different watched plane takes over the tracked slot). Independent of **Blink for Flight**; suppressed on the recall flight-detail page and while an alarm owns the LED/speaker.
- **Yellow** — origin and destination are both `homeap` (same home airport).
- **Green** — destination matches `homeap`.
- **Red** — origin matches `homeap`.
- **Blue** — all other overhead flights, including flights with no route data.

For each field the LED prefers the OpenSky route value when it is non-empty and
falls back to the ADSB.lol planned route value when OpenSky is empty. Each color
blinks 5 times at 240 ms on/off. Red, green, and yellow then stay lit while the live
flight is displayed on the dashboard; they turn off when the flight leaves, the user
dismisses it, or the screen switches to recalled flight details. Blue turns off after
its blink. When no flight notification is active on the home screen,
the LED mirrors the dashboard border: solid red for a critical issue (No WiFi, invalid
OpenSky credentials, exhausted radar credits, rate-limited or unavailable
OpenSky/weather/pool temp
data) or solid yellow when OpenSky is running anonymously. The blink and status
handling is performed in `loop()` after the flight view is drawn.

The onboard RGB LED pin mapping is set in `cyd-horizon.ino`: **GPIO 22 is the red
channel** on the verified unit, not the value claimed in the `jczn_2432s028r` variant
file. GPIO 4 is the panel reset, so do not change `CYD_LED_RED` to 4 on this hardware.

Any non-dashboard screen (settings, graphs, flight detail, etc.) automatically
returns to the dashboard after 2 minutes of inactivity. Any touch resets this
timer; boot screens (calibration and first-time WiFi setup) are excluded, and
the timer is suspended while an OTA update is pending or running so a download
can't be interrupted by an auto-return redraw.

`prefs.clear()` in the Reset handler removes **all** keys for both "Factory
Reset" and "Settings" resets (the "Settings" reset only re-writes the four
touch-calibration keys afterwards).

The dashboard draws a colored screen border and tints the clock bar to flag
state (`drawAuthBorder` / `drawStatusBorder` in `cyd-horizon.ino`):

- **Red** (critical): no WiFi, invalid OpenSky credentials, the OpenSky
  **radar-polling** credits exhausted, or weather/pool data rate-limited or
  unavailable. The clock bar
  turns maroon to match.
- **Yellow** (warning): running OpenSky **anonymously** (no credentials
  configured). This is non-critical — flights still work at a lower rate limit
  — so the clock bar keeps its normal (user-selected) color. Anonymous is only
  flagged after the first real OpenSky fetch (`g_authChecked`).

`dashboardCriticalLabel()` and `dashboardWarningLabel()` decide the two cases;
the clock bar color is `g_clockCol` (persisted `clkcol`) except when a critical
issue overrides it with maroon.

## Network screen & WiFi scanning

Opening **Settings → Network** switches to `SCR_WIFI` immediately and starts a
non-blocking scan — `scanWifi()` calls `WiFi.scanNetworks(true)` (async) and
returns; `pollWifiScan()` runs from `loop()` while `g_scanning` and reads
`WiFi.scanComplete()`. Until results arrive the results area shows an animated
"Scanning" placeholder (redrawn in place so it doesn't flicker); on completion
the list is deduplicated/sorted and `dirty` triggers the redraw. `g_netCount`
is cleared at scan start so row taps during the scan can't select phantom
entries, and all the screen's other controls stay live. The same path serves
the bottom **Scan** re-scan button and the first-boot setup wizard (which
forces `WIFI_STA` mode first, since there are no credentials yet).

## OpenSky Credits screen & header readout

With **Flight Tracker** enabled, the header band shows the three independent
credit buckets as stacked readouts — **CRP** (radar polling), **CRL** (route
lookup), **CFT** (flight tracking) — drawn in FONT1 by `drawHeaderCredit()`.
Tapping them opens an **OpenSky Credits** screen (`drawCredits()`) showing the
same three buckets with their last-known remaining balances:

| Bucket | Endpoint | Device uses for |
|---|---|---|
| Radar Polling | `/states/*` | live positions (the per-second poll) |
| Route Lookup | `/flights/*` | origin/destination (`/flights/aircraft`) |
| Flight Tracking | `/tracks/*` | ground track (`/tracks`) |

Each bucket's `X-Rate-Limit-Remaining` is captured from its own endpoint's
response header: the states bucket on every poll, the flights bucket in
`fetchRoute()`, and the tracks bucket in `fetchTrack()`. Both the header readout
and the Credits screen show the last-known values (they update as the device
calls each endpoint); the header redraws on change via `updateDashboard()`, and
until a bucket is first fetched it shows a yellow "?". A `429` response for any
bucket sets that bucket's remaining credits to **0** and marks it as known, so
the readout immediately shows 0 (pink) instead of leaving the previous stale
value.

The header value color is tiered by absolute remaining amounts (no assumed
daily budget): **grey** when healthy, **yellow** below 500, **pink** below 50.
A pending (unfetched) bucket shows "?" in yellow. Because the tiers don't assume
a fixed limit, they stay meaningful for contributors whose credit allocation
differs from the standard anonymous/token quotas.

The periodic flight poll is gated **only** by the Radar Polling bucket: while the
`/states/*` credits are exhausted, the normal poll cadence backs off to a
15-minute recovery check (`CREDIT_RECOVERY_MS` in `cyd-horizon.ino`) so the
device notices once the credits refill (OpenSky resets daily) without hammering
the API. Each backed-off attempt **re-arms the deadline before firing** — a
failed recheck (TLS/HTTP error, dead link) must not leave `g_nextRadarMs` in
the past or the poll would re-fire every loop iteration; a `429` still
overrides the deadline with the server's `X-Rate-Limit-Retry-After-Seconds`.
A `200` that arrives *without* `X-Rate-Limit-Remaining` clears the exhausted
flag — a served response contradicts it, and a truly empty bucket re-latches
via `429` on the next poll. The Route Lookup and Flight Tracking buckets don't
affect the poll cadence.

Anonymous polling is used **only** when no OpenSky credentials are configured:
the anonymous bucket is 400/day keyed by source IP, so every device on a LAN
shares it — a token-mint failure that silently fell back to anonymous used to
burn that bucket and latch "No Flight Credits" for hours while the
authenticated bucket was untouched. When credentials are configured but the
token mint fails transiently (TLS/5xx/429), the states request is skipped for
that cycle and the poll retries normally; a mint rejected with 400/401 counts
toward the `AUTH_401_BACKOFF_AFTER` streak like a rejected request.

`fetchRoute()` and `fetchTrack()` (both in `flight_details.ino`) are only called
for the closest overhead plane (`planes[0]` when `distMi <= g_radiusMi`), and
each is fetched once per overhead identity. The results (`g_routeOrigin` /
`g_routeDest` and `g_trackPts` / `g_trackBearingDeg`) are cached while that plane
remains the closest overhead, so the same flight does not trigger repeated
route/track calls on every poll. The exception is a promoted watched callsign
(`watchcs`, including `*`): while its flight is shown, `fetchTrack()` refetches
every poll so the track + blip follow the real trajectory. If a fetch fails
with a TLS error, a `429` (credits exhausted), or a truncated/bad-framing
response, the failure is treated as "fetched" for that plane and is not
retried, preventing burned credits on errors that won't resolve before the
plane passes. A pure no-WiFi exit is transient, so `fetchRoute()` leaves the
fetched flag false and will retry once the link returns.

`fetchTrack()` (in `flight_details.ino`) also retrieves the tracked plane's
ground-track polyline from `/tracks` and stores a bounded set of points (max 64,
within ~2× the radar range of the observer). The radar draws them as a
light-grey **dotted line** on the dashboard and the flight-detail recall page,
and the overhead flight dead-recks along the track's real bearing when it's
available (falling back to heading/speed otherwise). A failed or empty track
(e.g. no `/tracks/*` credits) simply draws no line.

The adsb.lol callsign route (`fetchAdsbRoute()`) additionally carries each
airport's **IATA** code (`_airports[].iata`). When the **Show IATA Airports** setting is
on (default), the displayed origin/destination codes render as
`ICAO | IATA` (e.g. `OJAI | AMM`); with it off or when no IATA is available the
plain ICAO code is shown. All comparisons — the home-airport LED logic, the
OpenSky-vs-adsb.lol route diff, and the ICAO-keyed city lookup — always use the
ICAO codes.

## Alarms

Implemented in `alarms.ino` (`SCR_ALARMS` editor + `SCR_ALARMFIRE` alert
screen). Reached by tapping the header's date/clock region; the header also
shows a small bell beside the clock (top slot, above the AM/PM marker) while
any alarm is enabled.

- **Storage** — one NVS blob (`alarms` in the `flight` namespace, versioned
  by `ALARM_STORE_VER`) holding `count` + a packed `Alarm` array (`en`, `h`,
  `m`, weekday bitmask (0 = one-time alarm), `preset`, `snoozes`, `nextFire`). Max `MAX_ALARMS`
  (6). Settings/Factory reset wipes it like any other setting.
- **Firing** — each alarm carries `nextFire`, the persisted epoch of its
  next firing. `checkAlarms()` runs every `loop()` tick: an alarm is due iff
  `now >= nextFire`, so a firing missed while asleep or powered off goes off
  on the next run with no grace-window bookkeeping. `nextFire` is rewritten
  whenever the situation changes: any editor write or a Dismiss rearms it to
  the next matching weekday (`nextScheduled()`, strictly future — an empty
  `days` mask schedules as all-days so a one-time alarm fires at the next
  h:m), and a Snooze parks it at `now + 5 min`. `nextFire == 0` means "recompute once
  the clock is synced" (e.g. an alarm edited before NTP). A snoozed alarm
  needs no flag — `isSnoozed()` is just `nextFire` earlier than the next
  scheduled occurrence — and each alarm tracks its own `nextFire`, so
  multiple alarms can be snoozed at once.
  Firing switches to `SCR_ALARMFIRE` from any screen and runs the alarm's
  notification pattern until handled. Presets
  (`kNtfPresets` — 49 patterns from Blink/Simple through sirens, sweeps,
  Morse, and melodies like Nokia/Tetris/Charge) are tables of `NtfStep`
  (freq, ms, rgb) that drive both the RGB
  LED and an external speaker on the JST header at GPIO 26 — one table keeps
  light and sound in sync. `toneWrite()` pushes each step's frequency through
  LEDC with a duty cycle scaled by Notify Volume (`ntfvol`); `tone()`
  isn't used because it fixes duty at ~50%. The board has no built-in speaker
  — audio needs a speaker
  plugged into that header, otherwise alarms are LED-only. Picking a
  preset in the editor previews both for ~3 s. A short rising C5–E5–G5–C6
  chime + LED sweep (`playBootChime()`) plays on cold boots as the "device
  is on" signature; `setup()` skips it when `esp_reset_reason()` reports a
  deep-sleep wake.
- **Dismiss** rearms `nextFire` to the next scheduled weekday and returns to
  the dashboard — or, for a one-time alarm (empty `days` mask), clears `en`
  instead so the single firing ends it.
- **Snooze** sets `nextFire = now + 5 min`, increments `a.snoozes`, and
  returns to the dashboard; the alarm refires when `nextFire` passes —
  surviving deep sleep and power loss since it's in NVS. The dashboard's
  bottom-left status line counts down ("Snoozing for N minutes...") and
  flight polls pause while any snooze is pending (`snoozePending()`).
- **Snooze cap** — an episode allows `ALARM_MAX_SNOOZES` (3) snoozes; the
  next snooze request — idle timeout or button — dismisses for the day
  (the cap lives inside `alarmSnooze()`).
- **Deep sleep** — `enterDeepSleep()` wakes at `min(5-min pool cadence,
  nextAlarmAt())`, so an alarm (or matured snooze) goes off on time even
  mid-sleep-window rather than up to ~5 min late. A pending snooze does not
  block sleep entry; a ringing alarm can't be slept on because firing
  switched the screen to `SCR_ALARMFIRE` (sleep only engages from
  `SCR_DASH`). `sleeperRun()` also calls `alarmDueNow()` on each wake as a
  fallback.
- **Time editing** — the shared `drawTimeAdj`/`timeAdjHit` widget (a
  horizontal `[▼][▲]` hour pair left of the time, a minute pair right; the
  hour wraps through AM/PM on its own; tap the time for the manual HHMM
  keyboard, `g_wifiSub == 8`) is also used for Sleep Mode start/end. Wake Min
  reuses the same `adjPair`/`adjPairHit` arrow pair (±1, range 1–120).
- **Deleting** — `Delete` sets `g_alarmDelConfirm` and the editor body swaps to
  a Yes/No prompt (same pattern as the Reset confirmation); only Yes shifts
  the array and saves.

## Printable user guide

[`docs/user-guide/DEVELOPER-USERGUIDE.md`](docs/user-guide/DEVELOPER-USERGUIDE.md)
is the markdown source for `DEVELOPER-USERGUIDE.pdf` in the same folder — a
tri-fold printable guide for new users (Letter landscape, duplex flip on long
edge, fold along the dashed lines). It condenses the README's user-facing
content — keep it in sync with `README.md` and the on-device Help screen
whenever user-facing behavior changes, then regenerate the PDF:

The six panels follow standard letter-fold order —
`OUTSIDE = [inside flap | back cover | front cover]`,
`INSIDE = [setup | daily use | reference]` — marked in the markdown with
`<!-- SIDE:... -->` and `<!-- PANEL -->` comments (panels listed in printed
left-to-right order). The cover shows the firmware version, injected from
`.release-please-manifest.json` via a `{{VERSION}}` token. The flap panel is
~1/16" narrower so it tucks inside the fold cleanly.

```bash
# needs the `markdown` package once: cyd-horizon/.venv/bin/python -m pip install markdown
cyd-horizon/.venv/bin/python docs/user-guide/render_userguide.py
```

`render_userguide.py` first runs `render_dash_mock.py` (same folder), which
draws four simulated screens as SVG → PNG via CairoSVG, all populated with
**live data**:

- `DEVELOPER-USERGUIDE-dashboard.png` — the flight-overhead view (or idle
  view when no plane is near): callsign/airline, route, radar with heading-
  rotated plane-icon blips (tracked cyan 1.6x, red in-radius, green outside),
  the cyan dotted ground track, and the projection ray.
- `DEVELOPER-USERGUIDE-idle.png` — the idle weather view with the real Govee
  pool temperature.
- `DEVELOPER-USERGUIDE-wxgraph.png` — the History > Weather Temperature graph
  (Day) plotted from the last 24 h of real Open-Meteo temps.
- `DEVELOPER-USERGUIDE-ftracker.png` — the Settings > Flight Tracker screen
  (page 1) with the device's default values.
- `DEVELOPER-USERGUIDE-alarms.png` — the Alarms > N editor (hour/minute
  steppers flanking the time, weekday toggles, LED preset, < / Delete / New
  footer) showing a second alarm so all three footer buttons render.

Data sources: OpenSky `/states/all` + `/tracks/all` and the adsb.lol callsign
route using `cyd-horizon/.env` creds, Open-Meteo weather, the Govee device
state API, and a fixed header date/time (`MOCK_DT`) so the screenshots stay
stable between renders. Fetched data is cached in `mock_data.json` (same
folder) — renders reuse it, so the PDF doesn't hit the APIs each time; pass
`--refresh` to re-pull live data and re-save the cache. On a refresh the mock
scans nearby planes until one has a usable track (`--monitor SECONDS` extends
the window; `--once` for a single pass). All inputs are optional: with no
creds/network it still renders with placeholders.

**Page backgrounds** — each side is a full-bleed `.sheet` flex row
(`@page size: Letter landscape; margin: 0`) carrying a saved background
image (`bg-lines.png` outside, `bg-leaf.png` inside) with dashed fold guides
between panels; the cover panel layers `bg-blur.png` on top for contrast.
The images were generated once via Pollinations.ai by
`fetch_backgrounds.py`, which crops the watermark strip, then blurs + washes
each toward white so they stay low-contrast under text. They're checked in —
normal PDF renders never hit the network; re-run
`fetch_backgrounds.py --refresh [--seed N]` only to try a new look. The images are then converted with the markdown to styled HTML
and printed via headless Chrome (`--print-to-pdf`). It reports the rendered
page count — if it ever exceeds 2, trim the guide or tighten the CSS in the
script (panels clip with `overflow: hidden`, so a quiet overflow shows up as
missing content at a panel's bottom edge — eyeball the PDF after editing).

## Printable packaging

[`docs/packaging/`](docs/packaging/README.md) holds printable box-cover
labels for both boards: hand-maintained SVG artwork plus generated 300 DPI
PNGs and Letter-landscape label sheets — 4.5" × 2.25" covers for the 2.8"
2432S028R (6 per sheet) and 5.0" × 2.75" for the 4.0" E32R40T (4 per sheet).
Print at Actual Size / 100% and cut along the dashed borders. Regenerate
all PNGs and label PDFs with:

```bash
cyd-horizon/.venv/bin/python docs/packaging/build_packaging.py
```

The guide's front-cover logo is a vertically-trimmed derivative of the
2.8" cover — if the cover artwork changes, re-derive it (see
`docs/user-guide/README.md`). `docs/packaging/README.md` documents the
artwork conventions: layer order, radar clipping/fades, font sizes, and
the rule that the two sizes stay in sync.

