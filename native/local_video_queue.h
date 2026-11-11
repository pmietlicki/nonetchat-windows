#ifndef NONET_LOCAL_VIDEO_QUEUE_H_
#define NONET_LOCAL_VIDEO_QUEUE_H_

#include "local_pcm_queue.h"

namespace nonet {

enum class LocalVideoAdmission { accepted, retired, invalid, full, allocation_failed };

struct LocalI420View {
  int width, height, rotation;
  const std::uint8_t *y, *u, *v;
  int stride_y, stride_u, stride_v;
  // Elapsed nanoseconds in the recording owner's local continuous epoch.
  // Never use the sender's wall clock or restart this epoch per track.
  std::int64_t local_ns;
};

struct LocalI420Layout {
  std::size_t width, height, chroma_width, chroma_height, y_bytes, chroma_bytes;

  static std::optional<LocalI420Layout> Create(int width, int height) {
    if (width <= 0 || height <= 0) return std::nullopt;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto cw = w / 2 + w % 2, ch = h / 2 + h % 2;
    constexpr auto bound = LocalPcmQueue::max_bytes;
    if (w > bound || h > bound / w) return std::nullopt;
    const auto y = w * h;
    if (ch > (bound - y) / 2 / cw) return std::nullopt;
    return LocalI420Layout{w, h, cw, ch, y, cw * ch};
  }

  std::size_t bytes() const { return y_bytes + 2 * chroma_bytes; }
};

inline bool LocalVideoRotationValid(int rotation) {
  return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}

struct LocalVideoBlock {
  int width, height, rotation;
  std::int64_t local_ns;
  std::size_t y_bytes, chroma_bytes;
  std::vector<std::uint8_t> bytes;

  bool valid() const {
    const auto layout = LocalI420Layout::Create(width, height);
    return layout && LocalVideoRotationValid(rotation) && local_ns >= 0 &&
        y_bytes == layout->y_bytes && chroma_bytes == layout->chroma_bytes &&
        bytes.size() == layout->bytes();
  }
};

struct LocalVideoQueueState {
  bool accepting;
  LocalVideoAdmission failure;
  std::size_t blocks, bytes;
};

// Copy-only callback handoff, using the same GStreamer queue defaults as PCM:
// 200 buffers, 10 MiB and one second. Overflow fails the recording rather than
// blocking WebRTC or silently dropping images. Dynamic dimensions are normal.
// The owner authenticates/borrows the source; this queue grants no such right.
// Scaling, rotation, encoding, file IO and replies all belong on the worker.
class LocalVideoQueue final {
 public:
  LocalVideoQueue() = default;
  LocalVideoQueue(const LocalVideoQueue&) = delete;
  LocalVideoQueue& operator=(const LocalVideoQueue&) = delete;

  LocalVideoAdmission Push(const LocalI420View& frame) {
    std::lock_guard<std::mutex> held(mutex_);
    if (!accepting_) return failure_ == LocalVideoAdmission::accepted ?
        LocalVideoAdmission::retired : failure_;
    if (frame.width <= 0 || frame.height <= 0 || frame.local_ns < 0 ||
        !LocalVideoRotationValid(frame.rotation) ||
        (seen_ && frame.local_ns < last_ns_) || !frame.y || !frame.u || !frame.v) {
      return FailLocked(LocalVideoAdmission::invalid);
    }
    const auto layout = LocalI420Layout::Create(frame.width, frame.height);
    if (!layout) return FailLocked(LocalVideoAdmission::full);
    if (!PlaneFits(frame.stride_y, layout->width, layout->height) ||
        !PlaneFits(frame.stride_u, layout->chroma_width, layout->chroma_height) ||
        !PlaneFits(frame.stride_v, layout->chroma_width, layout->chroma_height)) {
      return FailLocked(LocalVideoAdmission::invalid);
    }
    if (blocks_.size() >= LocalPcmQueue::max_blocks ||
        layout->bytes() > LocalPcmQueue::max_bytes - bytes_ ||
        (!blocks_.empty() && frame.local_ns - blocks_.front().local_ns >= 1000000000)) {
      return FailLocked(LocalVideoAdmission::full);
    }
    try {
      LocalVideoBlock block{frame.width, frame.height, frame.rotation, frame.local_ns,
          layout->y_bytes, layout->chroma_bytes, {}};
      block.bytes.resize(layout->bytes());
      CopyPlane(frame.y, frame.stride_y, layout->width, layout->height, block.bytes.data());
      CopyPlane(frame.u, frame.stride_u, layout->chroma_width, layout->chroma_height,
          block.bytes.data() + layout->y_bytes);
      CopyPlane(frame.v, frame.stride_v, layout->chroma_width, layout->chroma_height,
          block.bytes.data() + layout->y_bytes + layout->chroma_bytes);
      blocks_.push_back(std::move(block));
      bytes_ += layout->bytes();
      last_ns_ = frame.local_ns;
      seen_ = true;
      changed_.notify_one();
      return LocalVideoAdmission::accepted;
    } catch (...) { return FailLocked(LocalVideoAdmission::allocation_failed); }
  }

  std::optional<LocalVideoBlock> WaitTake() {
    std::unique_lock<std::mutex> held(mutex_);
    changed_.wait(held, [&] { return !blocks_.empty() || !accepting_; });
    if (blocks_.empty()) return std::nullopt;
    auto block = std::move(blocks_.front());
    blocks_.pop_front();
    bytes_ -= block.bytes.size();
    return block;
  }

  void Seal() {
    std::lock_guard<std::mutex> held(mutex_);
    accepting_ = false;
    changed_.notify_all();
  }

  LocalVideoQueueState state() const {
    std::lock_guard<std::mutex> held(mutex_);
    return {accepting_, failure_, blocks_.size(), bytes_};
  }

 private:
  static bool PlaneFits(int stride, std::size_t width, std::size_t height) {
    constexpr auto bound = LocalPcmQueue::max_bytes;
    return stride > 0 && static_cast<std::size_t>(stride) >= width &&
        static_cast<std::size_t>(stride) <= bound &&
        height - 1 <= (bound - width) / static_cast<std::size_t>(stride);
  }

  static void CopyPlane(const std::uint8_t* input, int stride, std::size_t width,
      std::size_t height, std::uint8_t* output) {
    for (std::size_t row = 0; row < height; ++row) {
      std::memcpy(output + row * width, input + row * static_cast<std::size_t>(stride), width);
    }
  }

  LocalVideoAdmission FailLocked(LocalVideoAdmission failure) noexcept {
    if (failure_ == LocalVideoAdmission::accepted) failure_ = failure;
    accepting_ = false;
    changed_.notify_all();
    return failure_;
  }
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<LocalVideoBlock> blocks_;
  std::size_t bytes_ = 0;
  std::int64_t last_ns_ = 0;
  bool accepting_ = true, seen_ = false;
  LocalVideoAdmission failure_ = LocalVideoAdmission::accepted;
};

}  // namespace nonet
#endif
