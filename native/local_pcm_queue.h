#ifndef NONET_LOCAL_PCM_QUEUE_H_
#define NONET_LOCAL_PCM_QUEUE_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace nonet {

enum class LocalPcmAdmission { accepted, retired, invalid, full, allocation_failed };

struct LocalPcmBlock {
  std::uint32_t rate = 0, channels = 0;
  std::size_t frames = 0;
  std::vector<std::int16_t> samples;
};

struct LocalPcmQueueState {
  bool accepting = false;
  LocalPcmAdmission failure = LocalPcmAdmission::accepted;
  std::size_t blocks = 0, bytes = 0, frames = 0;
};

// Callback-to-worker queue, containing only the selected track's copied PCM16.
// Reference: GStreamer queue max-size-buffers=200, max-size-bytes=10485760,
// max-size-time=1s. For one immutable format, one second is exactly rate frames.
// Unlike GStreamer's blocking default, a full recorder queue fails visibly:
// never block WebRTC's real-time callback, or silently drop/replace samples.
class LocalPcmQueue final {
 public:
  static constexpr std::size_t max_blocks = 200;
  static constexpr std::size_t max_bytes = 10 * 1024 * 1024;
  LocalPcmQueue() = default;
  LocalPcmQueue(const LocalPcmQueue&) = delete;
  LocalPcmQueue& operator=(const LocalPcmQueue&) = delete;

  LocalPcmAdmission Push(const void* pcm, int bits, int rate,
                         std::size_t channels, std::size_t frames) {
    std::lock_guard<std::mutex> held(mutex_);
    if (!accepting_) return failure_ == LocalPcmAdmission::accepted ?
        LocalPcmAdmission::retired : failure_;
    if (!pcm || bits != 16 || rate <= 0 || (channels != 1 && channels != 2) || !frames) {
      return FailLocked(LocalPcmAdmission::invalid);
    }
    if (rate_ && (rate_ != static_cast<std::uint32_t>(rate) || channels_ != channels)) {
      return FailLocked(LocalPcmAdmission::invalid);
    }
    // Validate before multiplying or allocating, including hostile callback sizes.
    if (frames > max_bytes / (channels * sizeof(std::int16_t))) {
      return FailLocked(LocalPcmAdmission::full);
    }
    const auto bytes = frames * channels * sizeof(std::int16_t);
    if (blocks_.size() >= max_blocks || bytes > max_bytes - bytes_ ||
        frames > static_cast<std::size_t>(rate) - frames_) {
      return FailLocked(LocalPcmAdmission::full);
    }
    try {
      LocalPcmBlock block;
      block.rate = static_cast<std::uint32_t>(rate);
      block.channels = static_cast<std::uint32_t>(channels);
      block.frames = frames;
      block.samples.resize(frames * channels);
      std::memcpy(block.samples.data(), pcm, bytes);
      blocks_.push_back(std::move(block));
      rate_ = static_cast<std::uint32_t>(rate);
      channels_ = static_cast<std::uint32_t>(channels);
      bytes_ += bytes;
      frames_ += frames;
      changed_.notify_one();
      return LocalPcmAdmission::accepted;
    } catch (...) { return FailLocked(LocalPcmAdmission::allocation_failed); }
  }

  // Worker only. Seal/error wakes a waiting worker, which still drains every
  // previously admitted block. Close never discards a successful tail silently.
  std::optional<LocalPcmBlock> WaitTake() {
    std::unique_lock<std::mutex> held(mutex_);
    changed_.wait(held, [&] { return !blocks_.empty() || !accepting_; });
    if (blocks_.empty()) return std::nullopt;
    auto block = std::move(blocks_.front());
    blocks_.pop_front();
    frames_ -= block.frames;
    bytes_ -= block.samples.size() * sizeof(std::int16_t);
    return block;
  }

  void Seal() {
    std::lock_guard<std::mutex> held(mutex_);
    accepting_ = false;
    changed_.notify_all();
  }

  LocalPcmQueueState state() const {
    std::lock_guard<std::mutex> held(mutex_);
    return {accepting_, failure_, blocks_.size(), bytes_, frames_};
  }

 private:
  LocalPcmAdmission FailLocked(LocalPcmAdmission reason) noexcept {
    if (failure_ == LocalPcmAdmission::accepted) failure_ = reason;
    accepting_ = false;
    changed_.notify_all();
    return failure_;
  }
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<LocalPcmBlock> blocks_;
  bool accepting_ = true;
  LocalPcmAdmission failure_ = LocalPcmAdmission::accepted;
  std::uint32_t rate_ = 0, channels_ = 0;
  std::size_t bytes_ = 0, frames_ = 0;
};

}  // namespace nonet
#endif
