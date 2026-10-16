# Changelog

## [3.10.0](https://github.com/improving-minnesota/cyd-horizon/compare/v3.9.1...v3.10.0) (2026-10-16)


### Features

* show wind direction, speed and gusts on the idle dashboard ([#176](https://github.com/improving-minnesota/cyd-horizon/issues/176)) ([4b37b53](https://github.com/improving-minnesota/cyd-horizon/commit/4b37b539052d3f42f23601542a105d10e698600d))

## [3.9.1](https://github.com/improving-minnesota/cyd-horizon/releases/tag/v3.9.1) (2026-10-10)


### Bug Fixes

* don't tear down the net task's TLS client from the OTA task ([#172](https://github.com/improving-minnesota/cyd-horizon/issues/172)) ([669eb6e](https://github.com/improving-minnesota/cyd-horizon/commit/669eb6e113514f88715c6e7c5b2b10fcac62d148))
* verify TLS against the built-in Mozilla CA bundle ([#171](https://github.com/improving-minnesota/cyd-horizon/issues/171)) ([8856181](https://github.com/improving-minnesota/cyd-horizon/commit/8856181bc412e035ae0863eb38131a723fc29888))


### Update by Flashing

Previous versions may not be able to update: pressing **Install** can crash-loop the device. Update over USB (flashing) with a browser instead. This keeps your WiFi, settings, touch calibration, airline logos and graph history.

1. Download the file for your board from the [v3.9.1 release](https://github.com/improving-minnesota/cyd-horizon/releases/tag/v3.9.1):
   - 4" screen: [`cyd-horizon-e32r40t.ino.bin`](https://github.com/improving-minnesota/cyd-horizon/releases/download/v3.9.1/cyd-horizon-e32r40t.ino.bin)
   - 2.8" screen: [`cyd-horizon-2432s028r.ino.bin`](https://github.com/improving-minnesota/cyd-horizon/releases/download/v3.9.1/cyd-horizon-2432s028r.ino.bin)
2. Plug in the board. In **Chrome or Edge**, open **https://espressif.github.io/esptool-js/**.
3. Set **Baudrate** to **115200**, click **Connect**, and choose the board's port.
4. Click **Add File** twice, using the same file for both rows. **Each new row defaults to `0x1000`, so you must change both addresses:**

   | Row | Address | File |
   |---|---|---|
   | 1 | `0x10000` | your `.ino.bin` |
   | 2 | `0x190000` | the same `.ino.bin` |

   Check them before continuing. `0x1000` has one zero fewer than `0x10000`. Writing to `0x1000` overwrites the bootloader and your saved settings, and the board will stop booting.
5. Click **Program** (about 3 minutes), then reboot the device.

Do **not** click **Erase Flash** or add any other files. If Connect fails, hold **BOOT** while plugging in USB.

## 3.9.0 (2026-09-23) — **REMOVED**

> Release and tag withdrawn: the 3.9.0 firmware could not complete a TLS
> handshake to the Let's Encrypt-signed hosts once the Gen-Y chain grew, and
> pressing Install crash-looped the device. Fixed in 3.9.1.


### Features

* add Network reset option to Settings &gt; Reset ([#169](https://github.com/improving-minnesota/cyd-horizon/issues/169)) ([4fa96ea](https://github.com/improving-minnesota/cyd-horizon/commit/4fa96ea6a1a0f743f196af9f6d45097dbf72b356))

## 3.8.3 (2026-09-20) — **REMOVED**

> Release and tag withdrawn as part of a clean-slate cleanup; superseded by
> 3.9.1.


### Bug Fixes

* suspend TLS arena during OTA so the download can complete ([#166](https://github.com/improving-minnesota/cyd-horizon/issues/166)) ([7e97524](https://github.com/improving-minnesota/cyd-horizon/commit/7e9752452df697255689689aaca5f335b290ea2e))

## 3.8.2 (2026-09-20) — **REMOVED**

> Release and tag withdrawn: GitHub OTA could not complete - the TLS arena
> plus a fragmented heap starved the post-redirect handshake, so the download
> stalled or failed. Fixed in 3.8.3.


### Bug Fixes

* OpenSky credit-backoff recovery and TLS heap arena ([#161](https://github.com/improving-minnesota/cyd-horizon/issues/161)) ([e5773f2](https://github.com/improving-minnesota/cyd-horizon/commit/e5773f2a9362f5b4fdb30a2aeffb569d04914a1e))

## 3.8.1 (2026-09-19) — **REMOVED**

> Release and tag withdrawn: an OpenSky token TLS failure could fall back to
> anonymous access, burn the anonymous credit bucket, and latch "No Flight
> Credits" until reboot. Fixed in 3.8.3.


### Bug Fixes

* wildcard track refresh, keyboard caret, watch blip gating ([3a1f65b](https://github.com/improving-minnesota/cyd-horizon/commit/3a1f65b0906c041e38d30545b87bd85157d3da45))
* wildcard track refresh, keyboard caret, watch blip gating ([dcd1749](https://github.com/improving-minnesota/cyd-horizon/commit/dcd1749ed5fd7d361c895a13434cef1b227413ea))

## 3.8.0 (2026-09-19) — **REMOVED**

> Release and tag withdrawn: same OpenSky anonymous-fallback defect as 3.8.1.
> Fixed in 3.8.3.


### Features

* include pending + finer-tier extremes in history graph ranges ([8954fc6](https://github.com/improving-minnesota/cyd-horizon/commit/8954fc6cf9649cf8b2ee204ae0cae4d68ae36ebb))
* include pending + finer-tier extremes in history graph ranges ([23f11d5](https://github.com/improving-minnesota/cyd-horizon/commit/23f11d5e78b2b0828cdc8c6e8ef5a1234cd16fda))
