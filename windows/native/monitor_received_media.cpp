#include "monitor_received_media.h"

#include <windows.h>

#include <limits>
#include <utility>

namespace nonet {

MonitorReceivedMedia::MonitorReceivedMedia(Clock clock)
    : ui_thread_(std::this_thread::get_id()),
      clock_(clock ? std::move(clock) : Clock(ContinuousNow)) {}
MonitorReceivedMedia::~MonitorReceivedMedia() = default;
std::int64_t MonitorReceivedMedia::ContinuousNow() noexcept {
  ULONGLONG ticks = 0;
  QueryInterruptTimePrecise(&ticks);
  if (ticks > static_cast<ULONGLONG>(INT64_MAX / 100)) return -1;
  return static_cast<std::int64_t>(ticks * 100);
}
bool MonitorReceivedMedia::Activate() noexcept {
  if (std::this_thread::get_id() != ui_thread_ ||
      gate_.load() != Gate::prepared || activated_.exchange(true))
    return false;
  try {
    epoch_ns_ = clock_();
  } catch (...) {
    epoch_ns_ = -1;
  }
  if (epoch_ns_ < 0) {
    Fail(MonitorReceivedFailure::clock);
    return false;
  }
  auto prepared = Gate::prepared;
  return gate_.compare_exchange_strong(prepared, Gate::active);
}
std::int64_t MonitorReceivedMedia::Elapsed() noexcept {
  try {
    const auto now = clock_();
    if (epoch_ns_ >= 0 && now >= epoch_ns_) return now - epoch_ns_;
  } catch (...) {
  }
  Fail(MonitorReceivedFailure::clock);
  return -1;
}
void MonitorReceivedMedia::Wake() noexcept {
  try {
    std::lock_guard<std::mutex> held(wake_mutex_);
    ++generation_;
    changed_.notify_one();
  } catch (...) {
    auto expected = MonitorReceivedFailure::none;
    failure_.compare_exchange_strong(expected, MonitorReceivedFailure::wake);
  }
}
void MonitorReceivedMedia::Seal() noexcept {
  gate_.store(Gate::retired);
  audio_.Seal();
  video_.Seal();
  sealed_.store(true);  // Publish only AFTER both queues refuse new copies.
  Wake();
}
void MonitorReceivedMedia::Fail(MonitorReceivedFailure reason) noexcept {
  auto expected = MonitorReceivedFailure::none;
  failure_.compare_exchange_strong(expected, reason);
  Seal();
}
void MonitorReceivedMedia::Audio(const void* pcm, int bits, int rate,
                                 std::size_t channels,
                                 std::size_t frames) noexcept {
  if (gate_.load() != Gate::active) return;
  try {
    if (first_audio_ns_.load() < 0) {
      const auto now = Elapsed();
      if (now < 0) return;
      std::int64_t empty = -1;
      first_audio_ns_.compare_exchange_strong(empty, now);
    }
    const auto admitted = audio_.Push(pcm, bits, rate, channels, frames);
    if (admitted != LocalPcmAdmission::accepted &&
        admitted != LocalPcmAdmission::retired) {
      Fail(MonitorReceivedFailure::audio);
    } else {
      Wake();
    }
  } catch (...) {
    Fail(MonitorReceivedFailure::audio);
  }
}
void MonitorReceivedMedia::Video(const LocalI420View& input) noexcept {
  if (gate_.load() != Gate::active) return;
  try {
    auto frame = input;
    frame.local_ns = Elapsed();  // Never accept a sender's timestamp.
    if (frame.local_ns < 0) return;
    const auto admitted = video_.Push(frame);
    if (admitted != LocalVideoAdmission::accepted &&
        admitted != LocalVideoAdmission::retired) {
      Fail(MonitorReceivedFailure::video);
    } else {
      Wake();
    }
  } catch (...) {
    Fail(MonitorReceivedFailure::video);
  }
}
bool MonitorReceivedMedia::Drain(const Start& start,
                                 const AudioConsumer& audio_consume,
                                 const VideoConsumer& video_consume) noexcept {
  if (std::this_thread::get_id() == ui_thread_ || !activated_.load() ||
      !start || !audio_consume || !video_consume || claimed_.exchange(true))
    return false;
  draining_.store(true);
  try {
    bool done = false, started = false;
    std::uint64_t observed = 0;
    while (!done) {
      {
        std::unique_lock<std::mutex> held(wake_mutex_);
        changed_.wait(
            held, [&] { return generation_ != observed || sealed_.load(); });
        observed = generation_;
      }
      for (;;) {
        // This is the sole consumer. Counts cannot disappear between state and
        // WaitTake. Both first blocks stay in their bounded original queues
        // until the shared origin/format can prepare one two-track writer.
        const bool audio = audio_.state().blocks != 0;
        const bool video = video_.state().blocks != 0;
        if (!started) {
          if (!audio || !video) {
            if (sealed_.load()) {
              Fail(MonitorReceivedFailure::worker);
              done = true;
            }
            break;
          }
          auto first_audio = audio_.WaitTake();
          auto first_video = video_.WaitTake();
          if (!first_audio || !first_video ||
              !start(*first_audio, first_audio_ns_.load(),
                     first_video->local_ns) ||
              !audio_consume(*first_audio) || !video_consume(*first_video)) {
            codec_stopped_.store(true);
            Seal();
            done = true;
            break;
          }
          ++audio_blocks_;
          ++video_blocks_;
          started = true;
          continue;
        }
        if (!audio && !video) {
          if (!sealed_.load()) break;
          if (audio_.state().blocks || video_.state().blocks) continue;
          done = true;
          break;
        }
        if (audio) {
          auto block = audio_.WaitTake();
          if (!block || !audio_consume(*block)) {
            codec_stopped_.store(true);
            Seal();
            done = true;
            break;
          }
          ++audio_blocks_;
        }
        if (video) {
          auto block = video_.WaitTake();
          if (!block || !video_consume(*block)) {
            codec_stopped_.store(true);
            Seal();
            done = true;
            break;
          }
          ++video_blocks_;
        }
      }
    }
  } catch (...) {
    Fail(MonitorReceivedFailure::worker);
  }
  draining_.store(false);
  drained_.store(true);
  return failure_.load() == MonitorReceivedFailure::none;
}
bool MonitorReceivedMedia::DrainAudio(const AudioDrain& consume) noexcept {
  if (std::this_thread::get_id() == ui_thread_ || !activated_.load() ||
      !consume || claimed_.exchange(true))
    return false;
  draining_.store(true);
  bool accepted = false;
  try {
    if (video_.state().blocks)
      Fail(MonitorReceivedFailure::video);
    else
      accepted = consume(audio_);
    if (!accepted) {
      codec_stopped_.store(true);
      Seal();
    }
  } catch (...) {
    Fail(MonitorReceivedFailure::worker);
  }
  draining_.store(false);
  drained_.store(true);
  return accepted && failure_.load() == MonitorReceivedFailure::none;
}
MonitorReceivedState MonitorReceivedMedia::state() const noexcept {
  return {activated_.load(),    gate_.load() == Gate::active, draining_.load(),
          drained_.load(),      codec_stopped_.load(),        failure_.load(),
          audio_blocks_.load(), video_blocks_.load()};
}

}  // namespace nonet
