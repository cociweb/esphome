#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_MEDIA_SOURCE)

#include "esphome/components/a2dp_sink/a2dp_sink.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"
#include "esphome/core/static_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace esphome::a2dp_sink {

/// @brief FreeRTOS task stack size (bytes) for the ring-buffer reader task.
static constexpr uint32_t READER_TASK_STACK = 3072;
/// @brief Priority for the reader task — above normal but below BT callbacks.
static constexpr UBaseType_t READER_TASK_PRIORITY = 5;
/// @brief Chunk size read per iteration in the reader task (bytes).
/// 2048 == 512 stereo 16-bit frames ≈ 11.5 ms at 44100 Hz.
static constexpr size_t READER_CHUNK_SIZE = 2048;
/// @brief Max milliseconds the ring buffer will block waiting for data.
static constexpr uint32_t RB_READ_TIMEOUT_MS = 20;
/// @brief Timeout (ms) passed to write_output() per call.
static constexpr uint32_t WRITE_TIMEOUT_MS = 100;
/// @brief Polling interval (ms) when idle / draining.
static constexpr uint32_t IDLE_POLL_MS = 10;
static constexpr uint8_t ZERO_WRITE_STOP_COUNT = 3;

// --- Event bits: main loop → reader task ---
static constexpr EventBits_t EVT_CMD_START = BIT0;  ///< play_uri / resume
static constexpr EventBits_t EVT_CMD_STOP  = BIT1;  ///< stop immediately
static constexpr EventBits_t EVT_CMD_PAUSE = BIT2;  ///< pause output
static constexpr EventBits_t EVT_CMD_DRAIN = BIT3;  ///< BT audio stopped, drain buffer
static constexpr EventBits_t EVT_CMD_FLUSH = BIT6;  ///< track changed, discard stale buffered PCM

// --- Event bits: reader task → main loop ---
/// Task finished normally and wants the orchestrator notified of IDLE.
static constexpr EventBits_t EVT_TASK_WANT_IDLE  = BIT4;
/// Task has suspended; main loop may safely call task_.deallocate().
static constexpr EventBits_t EVT_TASK_SUSPENDED  = BIT5;

static constexpr EventBits_t EVT_ALL_CMD_BITS =
    EVT_CMD_START | EVT_CMD_STOP | EVT_CMD_PAUSE | EVT_CMD_DRAIN | EVT_CMD_FLUSH;
static constexpr EventBits_t EVT_ALL_BITS =
    EVT_ALL_CMD_BITS | EVT_TASK_WANT_IDLE | EVT_TASK_SUSPENDED;

/**
 * @brief A MediaSource that consumes audio data from an A2DPSink ring buffer.
 *
 * Accepted URI: "a2dp://stream"
 *
 * Lifecycle:
 *   - play_uri("a2dp://stream") → starts the reader FreeRTOS task, reports PLAYING.
 *   - BT source starts streaming → PCM data flows from the ring buffer to the
 *     speaker pipeline via write_output().
 *   - BT audio stopped / disconnected → drains the ring buffer for
 *     pcm_drain_throttle_ms_, suspends, then main loop reports IDLE.
 *   - handle_command(STOP) → signals task to stop; main loop reports IDLE.
 *
 * Threading:
 *   - set_state_() is only called from the main loop (loop() method).
 *   - The reader task signals state transitions via event bits and suspends;
 *     the main loop calls task_.deallocate() once it has processed the bits.
 */
class A2DPSinkMediaSource : public Component,
                            public media_source::MediaSource,
                            public Parented<A2DPSink> {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_task_stack_in_psram(bool v) { this->task_stack_in_psram_ = v; }

  // --- MediaSource interface ---
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;

 protected:
  /// @brief Static trampoline for the FreeRTOS task.
  static void s_reader_task_(void *arg);
  /// @brief The reader task body.
  void reader_task_();

  /// @brief Start the reader task (idempotent).
  void start_task_();

  StaticTask task_;
  EventGroupHandle_t event_group_{nullptr};
  bool task_stack_in_psram_{false};
  bool pending_stop_{false};
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_MEDIA_SOURCE
