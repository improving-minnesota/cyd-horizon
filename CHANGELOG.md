# Changelog

## [3.8.3](https://github.com/improving-minnesota/cyd-horizon/compare/v3.8.2...v3.8.3) (2026-09-20)


### Bug Fixes

* suspend TLS arena during OTA so the download can complete ([#166](https://github.com/improving-minnesota/cyd-horizon/issues/166)) ([7e97524](https://github.com/improving-minnesota/cyd-horizon/commit/7e9752452df697255689689aaca5f335b290ea2e))

## [3.8.2](https://github.com/improving-minnesota/cyd-horizon/releases/tag/v3.8.2) (2026-09-20)


### Bug Fixes

* OpenSky credit-backoff recovery and TLS heap arena ([#161](https://github.com/improving-minnesota/cyd-horizon/issues/161)) ([e5773f2](https://github.com/improving-minnesota/cyd-horizon/commit/e5773f2a9362f5b4fdb30a2aeffb569d04914a1e))

## 3.8.1 (2026-09-19) — **REMOVED**

> Release and tag withdrawn: an OpenSky token TLS failure could fall back to
> anonymous access, burn the anonymous credit bucket, and latch "No Flight
> Credits" until reboot. Fixed in 3.8.2.


### Bug Fixes

* wildcard track refresh, keyboard caret, watch blip gating ([3a1f65b](https://github.com/improving-minnesota/cyd-horizon/commit/3a1f65b0906c041e38d30545b87bd85157d3da45))
* wildcard track refresh, keyboard caret, watch blip gating ([dcd1749](https://github.com/improving-minnesota/cyd-horizon/commit/dcd1749ed5fd7d361c895a13434cef1b227413ea))

## 3.8.0 (2026-09-19) — **REMOVED**

> Release and tag withdrawn: same OpenSky anonymous-fallback defect as 3.8.1.
> Fixed in 3.8.2.


### Features

* include pending + finer-tier extremes in history graph ranges ([8954fc6](https://github.com/improving-minnesota/cyd-horizon/commit/8954fc6cf9649cf8b2ee204ae0cae4d68ae36ebb))
* include pending + finer-tier extremes in history graph ranges ([23f11d5](https://github.com/improving-minnesota/cyd-horizon/commit/23f11d5e78b2b0828cdc8c6e8ef5a1234cd16fda))
