#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/image/image.h"

#include <atomic>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace esphome {
namespace async_online_image {

// image::Image subclass with a swappable pixel buffer. The underlying format
// is ESPHome's "RGB565 + alpha_channel", which maps to LVGL v9's
// LV_COLOR_FORMAT_RGB565A8: a color plane (w*h*2 B, little-endian RGB565)
// followed by an alpha plane (w*h*1 B).
class AsyncImageSlot : public image::Image {
 public:
  AsyncImageSlot(int w, int h);
  void set_pixel_buffer(const uint8_t *buf);
};

struct SlotState {
  std::string url;
  uint32_t timestamp{0};  // reserved for future shift-buffer keying
  uint8_t *pixel_buf{nullptr};
  std::atomic<bool> ready{false};
  std::atomic<bool> pending{false};
  std::atomic<bool> error{false};
};

class AsyncOnlineImageComponent : public Component {
 public:
  // Config
  void set_slots_count(size_t n) { slots_count_ = n; }
  void set_dimensions(int w, int h) {
    width_ = w;
    height_ = h;
  }
  void set_http_timeout(uint32_t ms) { http_timeout_ms_ = ms; }

  // Lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // API callable from YAML lambdas
  void set_url(size_t idx, const std::string &url, uint32_t timestamp = 0);
  image::Image *get_slot(size_t idx);
  bool is_ready(size_t idx);
  int ready_count();

  // Triggers (called by generated trigger classes below)
  void add_on_slot_ready_trigger(Trigger<size_t> *t) { on_slot_ready_.push_back(t); }
  void add_on_all_ready_trigger(Trigger<> *t) { on_all_ready_.push_back(t); }
  void add_on_error_trigger(Trigger<size_t> *t) { on_error_.push_back(t); }

 protected:
  static void worker_entry_(void *arg) {
    static_cast<AsyncOnlineImageComponent *>(arg)->worker_loop_();
  }
  void worker_loop_();

  bool download_and_decode_(size_t idx, const std::string &url);

  uint8_t *alloc_frame_buf_();
  void free_frame_buf_(uint8_t *buf);

  // Config
  size_t slots_count_{1};
  int width_{256};
  int height_{256};
  uint32_t http_timeout_ms_{10000};
  size_t frame_buf_size_{0};  // width * height * 3 (2 color + 1 alpha)

  // Slots
  std::vector<SlotState> slots_;
  std::vector<AsyncImageSlot *> images_;
  uint8_t *placeholder_buf_{nullptr};

  // Worker
  QueueHandle_t download_queue_{nullptr};
  TaskHandle_t worker_task_{nullptr};
  struct DownloadReq {
    size_t idx;
    char url[256];
  };

  // Main-loop trigger polling
  std::vector<bool> last_ready_seen_;
  std::vector<bool> last_error_seen_;
  int last_ready_count_{0};
  bool all_ready_fired_{false};

  std::vector<Trigger<size_t> *> on_slot_ready_;
  std::vector<Trigger<> *> on_all_ready_;
  std::vector<Trigger<size_t> *> on_error_;
};

// Trigger classes registered by codegen (__init__.py).
class SlotReadyTrigger : public Trigger<size_t> {
 public:
  explicit SlotReadyTrigger(AsyncOnlineImageComponent *parent) {
    parent->add_on_slot_ready_trigger(this);
  }
};
class AllReadyTrigger : public Trigger<> {
 public:
  explicit AllReadyTrigger(AsyncOnlineImageComponent *parent) {
    parent->add_on_all_ready_trigger(this);
  }
};
class SlotErrorTrigger : public Trigger<size_t> {
 public:
  explicit SlotErrorTrigger(AsyncOnlineImageComponent *parent) {
    parent->add_on_error_trigger(this);
  }
};

}  // namespace async_online_image
}  // namespace esphome
