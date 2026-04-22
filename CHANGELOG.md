# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-04-22

### Added

- **Shift-buffer cache.** When `set_url(idx, url)` is called with a URL
  that another slot already holds (and that slot is `ready=true,
  pending=false, error=false`), the two slot contents are swapped and no
  download is queued. Typical use case: a sliding window of frames
  rotates by one position at each update — 5 of 6 new URLs match URLs
  already in cache, so only 1 frame is actually re-downloaded. Shift
  events are logged at INFO level: `set_url[N]: shift from slot M`.

### Changed

- **BREAKING**: removed the third `timestamp` argument from `set_url`.
  Signature is now `set_url(size_t idx, const std::string &url)`.
  Rationale: URL-string equality turned out to be both simpler and more
  robust for the shift-match check than numeric timestamp keying —
  notably, URL patterns that encode frame identity as a hex hash (e.g.
  the HA RainViewer integration) defeat any naive timestamp extraction.
  Callers that previously passed a timestamp must drop the third arg.
- `set_url` queueing log no longer prints the timestamp (which was just
  noise).

### Known limitations (new)

- Out-of-order `set_url` bursts can miss shift opportunities — see
  README "Known limitations". Worst case: extra downloads, not
  incorrect content.

## [0.1.0] - 2026-04-21

### Added

- Initial release.
- Async HTTP download + PNG decode on a dedicated FreeRTOS worker task
  pinned to core 0; the main loop is never blocked on TLS or PNG.
- N slots (1..16) per component, each an `image::Image` subclass with a
  swappable pixel buffer in PSRAM.
- Output layout matches ESPHome's `type: RGB565 + transparency:
  alpha_channel`, which maps to LVGL v9's `LV_COLOR_FORMAT_RGB565A8`
  (color plane little-endian 2 B/px followed by alpha plane 1 B/px).
- Streaming PNG decode via the [pngle](https://github.com/kikuchan/pngle)
  library (no full compressed buffer allocation).
- Triggers: `on_slot_ready(size_t idx)`, `on_all_ready`,
  `on_error(size_t idx)`.
- API callable from YAML lambdas: `set_url`, `get_slot`, `is_ready`,
  `ready_count`.
- Requires ESP-IDF framework, PSRAM, `http_request:` present in YAML,
  and the sdkconfig options `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` +
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`.
