# Security Policy

## Supported versions

The latest release on `main` is supported. Older releases may not receive
security fixes; always update to the newest firmware.

## Reporting a vulnerability

**Do not open a public issue** for a security vulnerability. Please report it
privately via one of:

- **GitHub private vulnerability reporting** — use the "Report a vulnerability"
  button under the repo's **Security** tab (if enabled for this repository), or
- **Email** the maintainer, Paul Hassinger:
  `paul.hassinger (at) improving.com`.

Please include:

- The firmware version (**Settings → About**) and how to reproduce the issue.
- The impact, if known (e.g. is it exploitable remotely via WiFi?).

You'll receive an acknowledgment within a few days and an update on the fix and
release timeline. Security issues are handled before general bug reports.

## Scope

This is an ESP32 hobby/display device. It connects to external APIs (OpenSky,
Govee, weather) over WiFi. Things that matter here: insecure storage of
credentials on the device, unsafe handling of network data, and anything that
could let an attacker execute arbitrary code on the device or exfiltrate its
stored credentials.

## Firmware update (OTA) security

Firmware is delivered over HTTPS from this repository's GitHub releases and is
authenticated at two independent layers:

- **Transport (TLS certificate verification).** Both the release-metadata
  request (`api.github.com`) and the firmware download
  (`objects.githubusercontent.com`) are verified against the CA bundle built
  into the firmware (the core's Mozilla root store). The device never retries
  an unverified connection after a certificate-validation failure, so a
  man-in-the-middle cannot force it to skip verification.
- **Content integrity (SHA-256).** The streamed firmware image is hashed and
  compared against the asset digest published by the GitHub API before it is
  flashed. A mismatch aborts the update without touching the running slot.

**No insecure fallback.** Earlier firmware skipped certificate validation when
its bundled roots had passed their expiry (`OTA_CA_EXPIRY`), so a stale device
could still update. Verification now uses the core's Mozilla bundle, which is
refreshed by firmware updates, so that window is gone: every HTTPS connection,
OTA included, is certificate-verified. A device too old to verify a current
root must be updated over USB.

## Third-party API (data provider) TLS

The non-OTA HTTPS calls — OpenSky (token exchange, flight data, and flight
history), the weather provider (open-meteo), the address geocoder (Nominatim),
Govee pool-temp, and the ADSB.lol static route mirror — are all TLS-verified
against the core's built-in Mozilla CA bundle. One host
(`vrs-standing-data.adsb.lol`) is documented to sometimes omit its
intermediate, which a roots-only store can't build; it currently sends the full
chain and verifies, and DEVELOPER.md records the fallback to add if that
changes. There is **no** `setInsecure()` fallback on any path, so a certificate
validation failure is never downgraded to an insecure connection. Transient transport/TLS failures are retried a few times
(`httpsRequestRetry`) before being reported. A genuine OpenSky credential
rejection (a non-200 response from
the token exchange, or a 401 on the flight-data call) is surfaced as a
failed-auth state rather than falling back to anonymous, so a man-in-the-middle
cannot quietly capture credentials by defeating verification. A mere transport
blip on the token exchange is treated as transient: the last known auth state is
kept and a flight-data response that still arrives over verified TLS is used, so
it won't show a false "Invalid Credentials" error.

Note: the default-location lookup (`ip-api.com`) intentionally uses plain HTTP
(supported by that endpoint) and sends no credentials.
