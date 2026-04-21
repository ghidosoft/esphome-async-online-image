# esphome-async-online-image

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

An ESPHome external component that downloads and decodes PNG images on a
background FreeRTOS task, so the main loop (and LVGL rendering) is never
blocked on HTTP or PNG decoding.

## Why

ESPHome's stock [`online_image`](https://esphome.io/components/online_image)
component performs HTTP fetch and PNG decoding on the main loop. With an
LVGL UI on a mid-sized display, each refresh can freeze touch input for
500–1500 ms — long enough to trip the ESP-IDF task watchdog on TLS + large
alpha PNGs. `async_online_image` moves that work to a dedicated worker task
pinned to core 0, next to the WiFi stack.

It also exposes multiple "slots" per component, sharing a single worker and
queue — useful when you're displaying an animated sequence (e.g. 6 frames
of a weather radar) and want all slots to progress in parallel without
blocking each other.

## Features

- Download + PNG decode on a dedicated FreeRTOS task (core 0).
- Streaming PNG decode via [pngle](https://github.com/kikuchan/pngle) —
  no full compressed buffer allocation.
- N slots per component (1..16), each an `image::Image` you can bind to
  any ESPHome widget or LVGL image.
- Pixel buffers allocated in PSRAM; compatible with LVGL v9's
  `LV_COLOR_FORMAT_RGB565A8` (2 B color + 1 B alpha per pixel).
- Triggers: `on_slot_ready`, `on_all_ready`, `on_error`.

## Requirements

- ESP32 with **PSRAM** (a 256×256 RGBA frame is ~192 KB).
- Framework: **ESP-IDF** (uses `esp_http_client` + `esp_crt_bundle`).
- ESPHome **2026.x** or later (LVGL v9 integration).
- Your YAML must configure `http_request:` (so the `esp_http_client` IDF
  library ends up linked) and enable the certificate bundle in
  `esp32.framework.sdkconfig_options`:

  ```yaml
  esp32:
    framework:
      type: esp-idf
      sdkconfig_options:
        CONFIG_MBEDTLS_CERTIFICATE_BUNDLE: "y"
        CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL: "y"

  http_request:
    verify_ssl: true
    timeout: 10s
  ```

## Installation

Add the repo as an external component:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/ghidosoft/esphome-async-online-image
      ref: v0.1.0
```

## Configuration

```yaml
async_online_image:
  id: my_image
  slots: 1          # 1..16, default 1
  width: 256        # px, default 256
  height: 256       # px, default 256
  http_timeout: 10s # default 10s
  on_slot_ready:
    - logger.log:
        format: "slot %u ready"
        args: ['(unsigned) x']
  on_all_ready:
    - logger.log: "All slots loaded"
  on_error:
    - logger.log:
        format: "slot %u failed"
        args: ['(unsigned) x']
        level: WARN
```

All PNGs are decoded as RGB565 + alpha_channel (3 bytes/pixel). Non-PNG
formats (BMP/JPEG) are not currently supported.

## Usage

### Triggering downloads

Call `set_url(idx, url)` from any lambda. The call is non-blocking: it
enqueues the request on the worker and returns immediately. If the new
URL equals the one currently loaded, the call is a no-op.

```yaml
sensor:
  - platform: homeassistant
    id: my_url
    entity_id: sensor.image_url
    on_value:
      - lambda: 'id(my_image)->set_url(0, x);'
```

### Binding to LVGL

Use a lambda that returns the slot's `image::Image *`:

```yaml
lvgl:
  pages:
    - id: main_page
      widgets:
        - image:
            id: my_img_widget
            align: CENTER
            src: some_placeholder_image   # initial src; replaced at runtime

# Whenever you want to refresh what the widget is pointing at:
- lvgl.image.update:
    id: my_img_widget
    src: !lambda 'return id(my_image)->get_slot(0);'
```

Typically you'll call `lvgl.image.update` from the component's
`on_slot_ready` trigger (for the slot currently displayed) so the widget
updates as soon as the new pixel data is ready.

### C++ API (available inside any `!lambda`)

```cpp
// Queue a download (non-blocking).
id(my_image)->set_url(size_t idx, const std::string &url);

// Returns the image::Image* for slot idx (always non-null; returns a
// transparent placeholder until the first decode succeeds).
id(my_image)->get_slot(size_t idx);

// Is slot idx decoded and ready to show?
id(my_image)->is_ready(size_t idx);

// How many slots are currently ready (0..slots).
id(my_image)->ready_count();
```

## Example

See [`example/example.yaml`](example/example.yaml) for a complete working
configuration.

## Known limitations

- **LVGL v9 only.** The buffer layout (`RGB565A8` with separate color +
  alpha planes) matches LVGL v9. It won't render correctly against
  LVGL v8.
- **ESP-IDF framework only.** No Arduino-framework support (the
  component uses `esp_http_client` and `esp_crt_bundle_attach`).
- **PNG only.** BMP/JPEG are not implemented.
- **Fixed dimensions per component.** All slots share the same width
  and height; PNGs whose decoded dimensions differ are rejected. For
  mixed sizes, declare multiple `async_online_image` components.
- Minor transient visual glitch possible if a slot's URL is changed
  while its previous download is still in flight (the stale decode may
  publish briefly before being superseded). Acceptable for most use
  cases; not a crash.

## Roadmap

- Shift-buffer cache: preserve buffers across URL changes when a
  content hash / timestamp matches (useful for animated sequences
  where only one frame changes per refresh).
- BMP/JPEG decoders as optional build flags.
- Drop-in single-image form mirroring `online_image:` exactly.

## License

MIT © 2026 Andrea Ghidini
