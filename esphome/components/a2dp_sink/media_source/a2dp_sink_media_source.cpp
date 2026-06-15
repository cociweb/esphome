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
    if (this->pending_stop_)
      return;
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
    if (this->pending_stop_)
      return;
    if (!connected && this->get_state() != media_source::MediaSourceState::IDLE) {
      xEventGroupSetBits(this->event_group_, EVT_CMD_DRAIN);
    }
  });

#ifdef USE_A2DP_AVRCP
  this->parent_->add_on_avrcp_track_change_callback([this]() {
    if (this->pending_stop_)
      return;
    if (this->get_state() != media_source::MediaSourceState::IDLE)
      xEventGroupSetBits(this->event_group_, EVT_CMD_FLUSH);
  });
#endif
}

void A2DPSinkMediaSource::loop() {
  if (this->event_group_ == nullptr)
    return;

  EventBits_t bits = xEventGroupGetBits(this->event_group_);

  // Task wants to transition the orchestrator to IDLE.
  if (bits & EVT_TASK_WANT_IDLE) {
    xEventGroupClearBits(this->event_group_, EVT_TASK_WANT_IDLE);
    if (!this->pending_stop_)
      this->set_state_(media_source::MediaSourceState::IDLE);
  }

  // Task has suspended and is safe to deallocate.
  if (bits & EVT_TASK_SUSPENDED) {
    xEventGroupClearBits(this->event_group_, EVT_TASK_SUSPENDED);
    this->task_.deallocate();
    if (this->pending_stop_) {
      this->pending_stop_ = false;
    }
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
  this->parent_->get_parent()->set_audio_output_enabled(true);
  this->parent_->get_parent()->reset_audio_buffer();

  xEventGroupClearBits(this->event_group_, EVT_ALL_BITS);
  xEventGroupSetBits(this->event_group_, EVT_CMD_START);
  this->pending_stop_ = false;

  this->start_task_();
  this->set_state_(media_source::MediaSourceState::PLAYING);
  ESP_LOGI(TAG, "Started — waiting for Bluetooth audio");
  return true;
}

void A2DPSinkMediaSource::handle_command(media_source::MediaSourceCommand command) {
  switch (command) {
    case media_source::MediaSourceCommand::STOP:
      ESP_LOGI(TAG, "STOP");
      this->parent_->get_parent()->set_audio_output_enabled(false);
      this->parent_->get_parent()->request_audio_suspend();
#ifdef USE_A2DP_AVRCP
      this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE);
#endif
      xEventGroupClearBits(this->event_group_, EVT_ALL_CMD_BITS | EVT_TASK_WANT_IDLE);
      xEventGroupSetBits(this->event_group_, EVT_CMD_STOP);
      if (this->task_.is_created()) {
        this->pending_stop_ = true;
        this->set_state_(media_source::MediaSourceState::IDLE);
      } else {
        this->pending_stop_ = false;
        this->set_state_(media_source::MediaSourceState::IDLE);
      }
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
  uint8_t zero_write_count = 0;

  auto audio_source =
      audio::RingBufferAudioSource::create(this->parent_->get_ring_buffer(), READER_CHUNK_SIZE, 2 * sizeof(int16_t));
  if (audio_source == nullptr) {
    ESP_LOGE(TAG, "Failed to create ring buffer audio source");
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

    if (bits & EVT_CMD_FLUSH) {
      xEventGroupClearBits(this->event_group_, EVT_CMD_FLUSH | EVT_CMD_DRAIN);
      if (audio_source->available() > 0)
        audio_source->consume(audio_source->available());
      this->parent_->get_parent()->reset_audio_buffer();
      continue;
    }

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
        if (audio_source->available() == 0) {
          audio_source->fill(pdMS_TO_TICKS(RB_READ_TIMEOUT_MS), false);
        }
        size_t available = audio_source->available();
        if (available == 0) {
          drain_waited += IDLE_POLL_MS;
          vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
          continue;
        }
        if (xEventGroupGetBits(this->event_group_) & EVT_CMD_STOP)
          goto task_exit_no_idle;
        audio::AudioStreamInfo info(16, this->parent_->get_actual_channels(),
                                    this->parent_->get_actual_sample_rate());
        size_t written = this->write_output(audio_source->data(), available, WRITE_TIMEOUT_MS, info);
        if (written > 0) {
          zero_write_count = 0;
          audio_source->consume(written);
        } else if (++zero_write_count >= ZERO_WRITE_STOP_COUNT) {
          this->parent_->get_parent()->set_audio_output_enabled(false);
          this->parent_->get_parent()->request_audio_suspend();
          goto task_exit_with_idle;
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
      if (audio_source->available() == 0) {
        audio_source->fill(pdMS_TO_TICKS(RB_READ_TIMEOUT_MS), false);
      }
      size_t available = audio_source->available();
      if (available == 0) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
        continue;
      }
      if (xEventGroupGetBits(this->event_group_) & EVT_CMD_STOP)
        goto task_exit_no_idle;
      audio::AudioStreamInfo info(16, this->parent_->get_actual_channels(),
                                  this->parent_->get_actual_sample_rate());
      size_t written = this->write_output(audio_source->data(), available, WRITE_TIMEOUT_MS, info);
      if (written > 0) {
        zero_write_count = 0;
        audio_source->consume(written);
      } else if (++zero_write_count >= ZERO_WRITE_STOP_COUNT) {
        this->parent_->get_parent()->set_audio_output_enabled(false);
        this->parent_->get_parent()->request_audio_suspend();
        goto task_exit_with_idle;
      }
    }
  }

task_exit_with_idle:
  ESP_LOGD(TAG, "Reader task: drain done, signalling IDLE");
  if (audio_source->available() > 0)
    audio_source->consume(audio_source->available());
  this->parent_->get_parent()->reset_audio_buffer();
  xEventGroupSetBits(this->event_group_, EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED);
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
  return;

task_exit_no_idle:
  ESP_LOGD(TAG, "Reader task: stopped by command");
  if (audio_source->available() > 0)
    audio_source->consume(audio_source->available());
  this->parent_->get_parent()->reset_audio_buffer();
  xEventGroupSetBits(this->event_group_, EVT_TASK_SUSPENDED);
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
}

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_MEDIA_SOURCE
