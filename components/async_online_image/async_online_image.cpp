#include "async_online_image.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "pngle.h"

namespace esphome {
namespace async_online_image {

static const char *const TAG = "async_online_image";

// ===== AsyncImageSlot =====

AsyncImageSlot::AsyncImageSlot(int w, int h)
    : image::Image(nullptr, w, h, image::IMAGE_TYPE_RGB565,
                   image::TRANSPARENCY_ALPHA_CHANNEL) {}

void AsyncImageSlot::set_pixel_buffer(const uint8_t *buf) {
  // Image::data_start_ is protected. Swapping here is safe as long as the
  // caller guarantees buffer lifetime (the parent component owns allocations
  // and only frees the old buffer after publishing the new pointer).
  this->data_start_ = buf;
}

// ===== pngle decode context =====

struct DecodeCtx {
  uint8_t *dst{nullptr};
  int width{0};
  int height{0};
  uint32_t callback_count{0};
  uint32_t pixel_count{0};
};

// Pngle draw callback. ESPHome image RGB565 + ALPHA_CHANNEL layout (matches
// LVGL v9 LV_COLOR_FORMAT_RGB565A8):
//   - Color plane at offset 0: w*h*2 bytes, little-endian RGB565 [lo, hi]
//   - Alpha plane at offset w*h*2: w*h bytes, 1 byte/pixel
// See esphome/components/image/image.cpp::get_rgb565_pixel_.
static void pngle_draw_cb(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t h, const uint8_t rgba[4]) {
  auto *ctx = static_cast<DecodeCtx *>(pngle_get_user_data(pngle));
  if (ctx == nullptr || ctx->dst == nullptr)
    return;
  ctx->callback_count++;
  const uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
  const uint16_t rgb565 =
      ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
  const uint8_t hi = (rgb565 >> 8) & 0xFF;
  const uint8_t lo = rgb565 & 0xFF;

  const uint32_t x_end =
      (x + w < (uint32_t)ctx->width) ? (x + w) : (uint32_t)ctx->width;
  const uint32_t y_end =
      (y + h < (uint32_t)ctx->height) ? (y + h) : (uint32_t)ctx->height;

  uint8_t *const color_plane = ctx->dst;
  uint8_t *const alpha_plane =
      ctx->dst + (size_t)ctx->width * ctx->height * 2;

  for (uint32_t iy = y; iy < y_end; iy++) {
    for (uint32_t ix = x; ix < x_end; ix++) {
      const size_t idx = (size_t)iy * ctx->width + ix;
      color_plane[idx * 2 + 0] = lo;  // little-endian
      color_plane[idx * 2 + 1] = hi;
      alpha_plane[idx] = a;
      ctx->pixel_count++;
    }
  }
}

// ===== AsyncOnlineImageComponent =====

void AsyncOnlineImageComponent::setup() {
  this->frame_buf_size_ = (size_t)this->width_ * this->height_ * 3;

  // Placeholder buffer: fully transparent (alpha=0), black color.
  this->placeholder_buf_ = (uint8_t *)heap_caps_calloc(
      1, this->frame_buf_size_, MALLOC_CAP_SPIRAM);
  if (this->placeholder_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate placeholder buffer (%u B in PSRAM)",
             (unsigned)this->frame_buf_size_);
    this->mark_failed();
    return;
  }

  this->slots_ = std::vector<SlotState>(this->slots_count_);
  this->images_.reserve(this->slots_count_);
  this->last_ready_seen_.assign(this->slots_count_, false);
  this->last_error_seen_.assign(this->slots_count_, false);

  for (size_t i = 0; i < this->slots_count_; i++) {
    auto *img = new AsyncImageSlot(this->width_, this->height_);
    img->set_pixel_buffer(this->placeholder_buf_);
    this->images_.push_back(img);
    this->slots_[i].pixel_buf = this->placeholder_buf_;  // marker: placeholder, don't free
  }

  this->download_queue_ = xQueueCreate(this->slots_count_, sizeof(DownloadReq));
  if (this->download_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create download queue");
    this->mark_failed();
    return;
  }

  // Pin worker to core 0 (alongside WiFi). Main ESPHome loop runs on core 1;
  // keeping the worker on core 0 ensures the UI thread is never preempted.
  BaseType_t ok = xTaskCreatePinnedToCore(
      &AsyncOnlineImageComponent::worker_entry_, "aoi_worker", 12288, this, 5,
      &this->worker_task_, 0);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to spawn worker task");
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "AsyncOnlineImage ready: %u slots, %dx%d, buf=%u B each",
                (unsigned)this->slots_count_, this->width_, this->height_,
                (unsigned)this->frame_buf_size_);
}

void AsyncOnlineImageComponent::loop() {
  bool any_change = false;
  for (size_t i = 0; i < this->slots_count_; i++) {
    const bool ready_now = this->slots_[i].ready.load(std::memory_order_acquire);
    if (ready_now && !this->last_ready_seen_[i]) {
      this->last_ready_seen_[i] = true;
      this->last_error_seen_[i] = false;
      any_change = true;
      for (auto *t : this->on_slot_ready_)
        t->trigger(i);
    } else if (!ready_now && this->last_ready_seen_[i]) {
      // Slot invalidated (e.g. new URL queued): reset local seen flag.
      this->last_ready_seen_[i] = false;
    }

    const bool err_now = this->slots_[i].error.load(std::memory_order_acquire);
    if (err_now && !this->last_error_seen_[i]) {
      this->last_error_seen_[i] = true;
      for (auto *t : this->on_error_)
        t->trigger(i);
    } else if (!err_now && this->last_error_seen_[i]) {
      this->last_error_seen_[i] = false;
    }
  }

  if (any_change) {
    const int rc = this->ready_count();
    if (rc >= (int)this->slots_count_ && !this->all_ready_fired_) {
      this->all_ready_fired_ = true;
      for (auto *t : this->on_all_ready_)
        t->trigger();
    } else if (rc < (int)this->slots_count_) {
      this->all_ready_fired_ = false;
    }
    this->last_ready_count_ = rc;
  }
}

void AsyncOnlineImageComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Async Online Image:");
  ESP_LOGCONFIG(TAG, "  Slots: %u", (unsigned)this->slots_count_);
  ESP_LOGCONFIG(TAG, "  Dimensions: %dx%d", this->width_, this->height_);
  ESP_LOGCONFIG(TAG, "  HTTP timeout: %u ms", (unsigned)this->http_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Frame buffer: %u B each (PSRAM)",
                (unsigned)this->frame_buf_size_);
}

image::Image *AsyncOnlineImageComponent::get_slot(size_t idx) {
  if (idx >= this->slots_count_)
    idx = 0;
  return this->images_[idx];
}

bool AsyncOnlineImageComponent::is_ready(size_t idx) {
  if (idx >= this->slots_count_)
    return false;
  return this->slots_[idx].ready.load(std::memory_order_acquire);
}

int AsyncOnlineImageComponent::ready_count() {
  int n = 0;
  for (size_t i = 0; i < this->slots_count_; i++)
    if (this->slots_[i].ready.load(std::memory_order_acquire))
      n++;
  return n;
}

void AsyncOnlineImageComponent::set_url(size_t idx, const std::string &url) {
  if (idx >= this->slots_count_) {
    ESP_LOGW(TAG, "set_url: idx %u out of range", (unsigned)idx);
    return;
  }
  if (url.empty() || url == "unknown" || url == "unavailable" ||
      (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)) {
    ESP_LOGD(TAG, "set_url[%u]: ignoring invalid url '%s'", (unsigned)idx,
             url.c_str());
    return;
  }
  if (this->slots_[idx].url == url) {
    ESP_LOGD(TAG, "set_url[%u]: unchanged, skip", (unsigned)idx);
    return;
  }

  // Shift-buffer cache: if some other stable slot already holds content for
  // this exact URL, swap the two slot states instead of queuing a new
  // download. Useful when a sliding window of frames rotates and most frames
  // are still the same content at shifted indices (e.g., RainViewer radar
  // tiles — when the window advances 1 step, 5 of 6 new URLs are string-
  // identical to URLs already loaded in other slots).
  if (!this->slots_[idx].pending.load(std::memory_order_acquire)) {
    for (size_t j = 0; j < this->slots_count_; j++) {
      if (j == idx)
        continue;
      if (this->slots_[j].url != url)
        continue;
      if (!this->slots_[j].ready.load(std::memory_order_acquire))
        continue;
      if (this->slots_[j].pending.load(std::memory_order_acquire))
        continue;
      if (this->slots_[j].error.load(std::memory_order_acquire))
        continue;

      // Found a cached match. Swap slot contents. Both slots have
      // pending=false and error=false by construction, so no worker is
      // currently touching either.
      std::swap(this->slots_[idx].url, this->slots_[j].url);
      std::swap(this->slots_[idx].pixel_buf, this->slots_[j].pixel_buf);
      const bool r_i = this->slots_[idx].ready.load(std::memory_order_acquire);
      const bool r_j = this->slots_[j].ready.load(std::memory_order_acquire);
      this->slots_[idx].ready.store(r_j, std::memory_order_release);
      this->slots_[j].ready.store(r_i, std::memory_order_release);

      // Re-point Image data_start_ to the now-current pixel buffers.
      this->images_[idx]->set_pixel_buffer(this->slots_[idx].pixel_buf);
      this->images_[j]->set_pixel_buffer(this->slots_[j].pixel_buf);

      // Force on_slot_ready to re-fire for idx on the next loop() so the UI
      // binding picks up the new buffer (ready was already true, so a naive
      // edge-triggered detector would miss the content change).
      this->last_ready_seen_[idx] = false;

      ESP_LOGI(TAG, "set_url[%u]: shift from slot %u", (unsigned)idx,
               (unsigned)j);
      return;
    }
  }

  ESP_LOGI(TAG, "set_url[%u]: queueing '%s'", (unsigned)idx, url.c_str());
  this->slots_[idx].url = url;
  this->slots_[idx].ready.store(false, std::memory_order_release);
  this->slots_[idx].error.store(false, std::memory_order_release);
  this->slots_[idx].pending.store(true, std::memory_order_release);

  // Point slot back to placeholder until decoded. Any existing LVGL src
  // binding remains valid; a further lvgl.image.update will pick up the new
  // buffer when the slot becomes ready.
  this->images_[idx]->set_pixel_buffer(this->placeholder_buf_);

  DownloadReq req{};
  req.idx = idx;
  std::strncpy(req.url, url.c_str(), sizeof(req.url) - 1);
  req.url[sizeof(req.url) - 1] = '\0';
  if (xQueueSend(this->download_queue_, &req, 0) != pdTRUE) {
    ESP_LOGW(TAG, "set_url[%u]: queue full, dropping", (unsigned)idx);
    this->slots_[idx].pending.store(false, std::memory_order_release);
  }
}

uint8_t *AsyncOnlineImageComponent::alloc_frame_buf_() {
  return (uint8_t *)heap_caps_malloc(this->frame_buf_size_, MALLOC_CAP_SPIRAM);
}

void AsyncOnlineImageComponent::free_frame_buf_(uint8_t *buf) {
  if (buf != nullptr && buf != this->placeholder_buf_)
    heap_caps_free(buf);
}

void AsyncOnlineImageComponent::worker_loop_() {
  DownloadReq req{};
  while (true) {
    if (xQueueReceive(this->download_queue_, &req, portMAX_DELAY) != pdTRUE)
      continue;

    const size_t idx = req.idx;
    if (idx >= this->slots_count_)
      continue;

    const uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    const bool ok = this->download_and_decode_(idx, req.url);
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - t0;

    if (ok) {
      this->slots_[idx].error.store(false, std::memory_order_release);
      this->slots_[idx].pending.store(false, std::memory_order_release);
      this->slots_[idx].ready.store(true, std::memory_order_release);
      ESP_LOGI(TAG, "slot %u ready in %u ms (heap=%u psram=%u)", (unsigned)idx,
               (unsigned)elapsed,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
      this->slots_[idx].pending.store(false, std::memory_order_release);
      this->slots_[idx].error.store(true, std::memory_order_release);
      ESP_LOGW(TAG, "slot %u failed after %u ms", (unsigned)idx,
               (unsigned)elapsed);
    }

    vTaskDelay(pdMS_TO_TICKS(50));  // yield between downloads
  }
}

bool AsyncOnlineImageComponent::download_and_decode_(size_t idx,
                                                     const std::string &url) {
  uint8_t *dst = this->alloc_frame_buf_();
  if (dst == nullptr) {
    ESP_LOGE(TAG, "slot %u: PSRAM alloc failed (%u B)", (unsigned)idx,
             (unsigned)this->frame_buf_size_);
    return false;
  }
  // Init as fully transparent to avoid showing garbage on partial decode.
  std::memset(dst, 0, this->frame_buf_size_);

  DecodeCtx ctx{};
  ctx.dst = dst;
  ctx.width = this->width_;
  ctx.height = this->height_;

  pngle_t *pngle = pngle_new();
  if (pngle == nullptr) {
    ESP_LOGE(TAG, "slot %u: pngle_new failed", (unsigned)idx);
    heap_caps_free(dst);
    return false;
  }
  pngle_set_user_data(pngle, &ctx);
  pngle_set_draw_callback(pngle, pngle_draw_cb);

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = (int)this->http_timeout_ms_;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGE(TAG, "slot %u: http_client_init failed", (unsigned)idx);
    pngle_destroy(pngle);
    heap_caps_free(dst);
    return false;
  }

  bool success = false;
  do {
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "slot %u: http open failed (%d)", (unsigned)idx, (int)err);
      break;
    }
    const int content_length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "slot %u: HTTP %d content-length=%d", (unsigned)idx, status,
             content_length);
    if (status != 200) {
      ESP_LOGW(TAG, "slot %u: HTTP %d (len=%d)", (unsigned)idx, status,
               content_length);
      break;
    }

    uint8_t chunk[4096];
    int total_read = 0;
    bool decode_err = false;
    int zero_reads = 0;
    while (true) {
      int n = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
      if (n < 0) {
        ESP_LOGW(TAG, "slot %u: http read err after %d B", (unsigned)idx,
                 total_read);
        decode_err = true;
        break;
      }
      if (n == 0) {
        // Chunked encoding can yield transient 0 reads between chunks.
        if (esp_http_client_is_complete_data_received(client))
          break;
        if (content_length > 0 && total_read >= content_length)
          break;
        if (++zero_reads > 20) {
          ESP_LOGW(TAG, "slot %u: 20 consecutive 0-reads after %d B, giving up",
                   (unsigned)idx, total_read);
          decode_err = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      zero_reads = 0;
      total_read += n;
      int fed = pngle_feed(pngle, chunk, (size_t)n);
      if (fed < 0) {
        ESP_LOGW(TAG, "slot %u: pngle_feed err after %d B: %s", (unsigned)idx,
                 total_read, pngle_error(pngle));
        decode_err = true;
        break;
      }
    }

    ESP_LOGD(TAG,
             "slot %u: read %d/%d B, callbacks=%u pixels=%u (expected=%d)",
             (unsigned)idx, total_read, content_length,
             (unsigned)ctx.callback_count, (unsigned)ctx.pixel_count,
             this->width_ * this->height_);

    if (decode_err)
      break;
    if (total_read == 0) {
      ESP_LOGW(TAG, "slot %u: empty response", (unsigned)idx);
      break;
    }
    const uint32_t png_w = pngle_get_width(pngle);
    const uint32_t png_h = pngle_get_height(pngle);
    if ((int)png_w != this->width_ || (int)png_h != this->height_) {
      ESP_LOGW(TAG, "slot %u: unexpected dims %ux%u (want %dx%d)",
               (unsigned)idx, (unsigned)png_w, (unsigned)png_h, this->width_,
               this->height_);
      break;
    }
    success = true;
  } while (false);

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  pngle_destroy(pngle);

  if (!success) {
    heap_caps_free(dst);
    return false;
  }

  // Swap: publish new buffer, free old.
  uint8_t *old_buf = this->slots_[idx].pixel_buf;
  this->slots_[idx].pixel_buf = dst;
  this->images_[idx]->set_pixel_buffer(dst);
  // Release barrier is provided by the subsequent ready.store(release) in
  // worker_loop_().
  this->free_frame_buf_(old_buf);
  return true;
}

}  // namespace async_online_image
}  // namespace esphome
