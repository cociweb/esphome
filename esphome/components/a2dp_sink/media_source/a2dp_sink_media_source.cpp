#include "a2dp_sink_media_source.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_MEDIA_SOURCE)

#include "esphome/core/application.h"
#include "esphome/core/log.h"

static const char *const TAG = "a2dp_sink.media_source";

static constexpr char A2DP_URI[] = "a2dp://stream";

namespace esphome::a2dp_sink {

// ---------------------------------------------------------------------------
// Static task trampoline
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::s_reader_task_(void *arg) {
  static_cast<A2DPSinkMediaSource *>(arg)->reader_task_();
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::setup() {
  ESP_LOGCONFIG(TAG, "Setting up A2DP Sink Media Source...");

  this->event_group_ = xEventGroupCreate();
  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }

  // BT audio streaming state changes → update event bits so the reader task reacts.
  // NOTE: these lambdas run on the main loop thread (dispatched via the event queue
  // in A2DPSink::loop()), so xEventGroupSetBits is safe here.
  this->parent_->add_on_audio_streaming_callback([this](bool streaming) {
    if (this->get_state() == media_source::MediaSourceState::IDLE)
      return;
    if (streaming) {
      xEventGroupClearBits(this->event_group_, EVT_CMD_DRAIN | EVT_CMD_PAUSE);
      xEventGroupSetBits(this->event_group_, EVT_CMD_START);
    } else {
      xEventGroupSetBits(this->event_group_, EVT_CMD_DRAIN);
    }
  });

  this->parent_->get_parent()->add_on_connection_callback([this](bool connected) {
    if (!connected && this->get_state() != media_source::MediaSourceState::IDLE) {
      xEventGroupSetBits(this->event_group_, EVT_CMD_DRAIN);
    }
  });
}

void A2DPSinkMediaSource::loop() {
  if (this->event_group_ == nullptr)
    return;

  EventBits_t bits = xEventGroupGetBits(this->event_group_);

  // Task wants to transition the orchestrator to IDLE.
  if (bits & EVT_TASK_WANT_IDLE) {
    xEventGroupClearBits(this->event_group_, EVT_TASK_WANT_IDLE);
    this->set_state_(media_source::MediaSourceState::IDLE);
  }

  // Task has suspended and is safe to deallocate.
  if (bits & EVT_TASK_SUSPENDED) {
    xEventGroupClearBits(this->event_group_, EVT_TASK_SUSPENDED);
    this->task_.deallocate();
  }
}

void A2DPSinkMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP Sink Media Source:");
  ESP_LOGCONFIG(TAG, "  Task Stack: %s", this->task_stack_in_psram_ ? "PSRAM" : "internal");
}

// ---------------------------------------------------------------------------
// MediaSource interface
// ---------------------------------------------------------------------------

bool A2DPSinkMediaSource::can_handle(const std::string &uri) const {
  return uri == A2DP_URI;
}

bool A2DPSinkMediaSource::play_uri(const std::string &uri) {
  if (!this->can_handle(uri))
    return false;

  if (this->get_state() == media_source::MediaSourceState::PLAYING) {
    ESP_LOGD(TAG, "play_uri: already playing");
    return true;
  }
  if (!this->parent_->get_parent()->is_enabled()) {
    ESP_LOGW(TAG, "play_uri: a2dp hub is not enabled");
    return false;
  }

  // Flush stale data from a previous session.
  this->parent_->get_ring_buffer()->reset();

  xEventGroupClearBits(this->event_group_, EVT_ALL_BITS);
  xEventGroupSetBits(this->event_group_, EVT_CMD_START);

  this->start_task_();
  this->set_state_(media_source::MediaSourceState::PLAYING);
  ESP_LOGI(TAG, "Started — waiting for Bluetooth audio");
  return true;
}

void A2DPSinkMediaSource::handle_command(media_source::MediaSourceCommand command) {
  switch (command) {
    case media_source::MediaSourceCommand::STOP:
      ESP_LOGI(TAG, "STOP");
      xEventGroupClearBits(this->event_group_, EVT_ALL_CMD_BITS);
      xEventGroupSetBits(this->event_group_, EVT_CMD_STOP);
      // Report IDLE immediately from the main loop — the task will clean up asynchronously.
      this->set_state_(media_source::MediaSourceState::IDLE);
      break;

    case media_source::MediaSourceCommand::PAUSE:
      if (this->get_state() == media_source::MediaSourceState::PLAYING) {
        ESP_LOGI(TAG, "PAUSE");
        xEventGroupClearBits(this->event_group_, EVT_CMD_START);
        xEventGroupSetBits(this->event_group_, EVT_CMD_PAUSE);
        this->set_state_(media_source::MediaSourceState::PAUSED);
      }
      break;

    case media_source::MediaSourceCommand::PLAY:
      if (this->get_state() == media_source::MediaSourceState::PAUSED) {
        ESP_LOGI(TAG, "PLAY (resume)");
        xEventGroupClearBits(this->event_group_, EVT_CMD_PAUSE);
        xEventGroupSetBits(this->event_group_, EVT_CMD_START);
        this->set_state_(media_source::MediaSourceState::PLAYING);
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Task management
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::start_task_() {
  if (this->task_.is_created())
    return;
  if (!this->task_.create(s_reader_task_, "a2dp_reader", READER_TASK_STACK, this, READER_TASK_PRIORITY,
                          this->task_stack_in_psram_)) {
    ESP_LOGE(TAG, "Failed to create reader task");
  }
}

// ---------------------------------------------------------------------------
// Reader task — NEVER calls set_state_() directly; uses event bits instead.
// ---------------------------------------------------------------------------

void A2DPSinkMediaSource::reader_task_() {
  ESP_LOGD(TAG, "Reader task started");

  const uint32_t drain_ms = this->parent_->get_pcm_drain_throttle_ms();
  const uint32_t output_delay_ms = this->parent_->get_output_delay_ms();

  auto *rb = this->parent_->get_ring_buffer();
  if (rb == nullptr) {
    ESP_LOGE(TAG, "Ring buffer is null");
    xEventGroupSetBits(this->event_group_, EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED);
    App.wake_loop_threadsafe();
    vTaskSuspend(nullptr);
    return;
  }

  // Initial output delay: let the ring buffer accumulate data before we start draining.
  if (output_delay_ms > 0) {
    uint32_t waited = 0;
    while (waited < output_delay_ms) {
      if (xEventGroupGetBits(this->event_group_) & EVT_CMD_STOP)
        goto task_exit_no_idle;
      vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
      waited += IDLE_POLL_MS;
    }
  }

  // Main read loop.
  while (true) {
    EventBits_t bits = xEventGroupGetBits(this->event_group_);

    if (bits & EVT_CMD_STOP)
      goto task_exit_no_idle;

    if (bits & EVT_CMD_DRAIN) {
      // BT audio stopped: drain remaining ring buffer data, then signal IDLE.
      uint32_t drain_waited = 0;
      while (drain_waited < drain_ms) {
        bits = xEventGroupGetBits(this->event_group_);
        if (bits & EVT_CMD_STOP)
          goto task_exit_no_idle;
        if (bits & EVT_CMD_START) {
          // BT resumed streaming — cancel drain.
          xEventGroupClearBits(this->event_group_, EVT_CMD_DRAIN);
          goto read_chunk;
        }
        size_t available = rb->available();
        if (available == 0) {
          drain_waited += IDLE_POLL_MS;
          vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
          continue;
        }
        size_t to_read = std::min(available, READER_CHUNK_SIZE);
        size_t read = rb->read(this->read_buf_, to_read, pdMS_TO_TICKS(RB_READ_TIMEOUT_MS));
        if (read > 0) {
          audio::AudioStreamInfo info(16, this->parent_->get_actual_channels(),
                                      this->parent_->get_actual_sample_rate());
          this->write_output(this->read_buf_, read, WRITE_TIMEOUT_MS, info);
        }
      }
      // Drain timeout expired — signal the main loop to report IDLE.
      xEventGroupClearBits(this->event_group_, EVT_CMD_DRAIN);
      goto task_exit_with_idle;
    }

    if (bits & EVT_CMD_PAUSE) {
      vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
      continue;
    }

read_chunk:
    {
      size_t available = rb->available();
      if (available == 0) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
        continue;
      }
      size_t to_read = std::min(available, READER_CHUNK_SIZE);
      size_t read = rb->read(this->read_buf_, to_read, pdMS_TO_TICKS(RB_READ_TIMEOUT_MS));
      if (read > 0) {
        audio::AudioStreamInfo info(16, this->parent_->get_actual_channels(),
                                    this->parent_->get_actual_sample_rate());
        this->write_output(this->read_buf_, read, WRITE_TIMEOUT_MS, info);
      }
    }
  }

task_exit_with_idle:
  ESP_LOGD(TAG, "Reader task: drain done, signalling IDLE");
  xEventGroupSetBits(this->event_group_, EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED);
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
  return;

task_exit_no_idle:
  ESP_LOGD(TAG, "Reader task: stopped by command");
  xEventGroupSetBits(this->event_group_, EVT_TASK_SUSPENDED);
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
}

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_MEDIA_SOURCE
