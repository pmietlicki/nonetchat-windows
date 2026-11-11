#ifndef NONET_WINDOWS_MONITOR_RECEIVED_MEDIA_H_
#define NONET_WINDOWS_MONITOR_RECEIVED_MEDIA_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "local_video_queue.h"

namespace nonet {

enum class MonitorReceivedFailure { none, clock, audio, video, wake, worker };

struct MonitorReceivedState {
  // drained means the consuming operation has returned; codec_stopped requires
  // its separate Finish result to distinguish a normal quota from an IO fault.
  // It never claims that every copied block was encoded after codec refusal.
  bool activated, accepting, draining, drained, codec_stopped;
  MonitorReceivedFailure failure;
  std::uint64_t audio_blocks, video_blocks;
};

// Copy-only callbacks and ONE consuming worker. No SDK handle, consent,
// capture, file authority or playback permission lives here. Activate is
// local/UI only, after the helper is ready and the exact source/indicator is
// revalidated. Seal is permanent and preserves the admitted prefix. Drain never
// runs on the construction/UI thread and never calls SDK/policy code. The owner
// separately removes/drains the source and waits for the original file/writer
// Close proofs. A drained queue alone proves neither source/file release nor
// playable media.
class MonitorReceivedMedia final {
 public:
  using Clock = std::function<std::int64_t()>;
  explicit MonitorReceivedMedia(Clock clock = {});
  ~MonitorReceivedMedia();
  MonitorReceivedMedia(const MonitorReceivedMedia&) = delete;
  MonitorReceivedMedia& operator=(const MonitorReceivedMedia&) = delete;

  bool Activate() noexcept;
  void Audio(const void* pcm, int bits, int rate, std::size_t channels,
             std::size_t frames) noexcept;
  void Video(const LocalI420View& frame) noexcept;
  void Seal() noexcept;
  using Start =
      std::function<bool(const LocalPcmBlock&, std::int64_t first_audio_ns,
                         std::int64_t first_video_ns)>;
  using AudioConsumer = std::function<bool(const LocalPcmBlock&)>;
  using VideoConsumer = std::function<bool(const LocalVideoBlock&)>;
  bool Drain(const Start&, const AudioConsumer&, const VideoConsumer&) noexcept;
  // The existing audio-only pump consumes this same queue on its sole worker.
  // No queue/reference is exposed to the platform or SDK callback owner.
  using AudioDrain = std::function<bool(LocalPcmQueue&)>;
  bool DrainAudio(const AudioDrain&) noexcept;
  MonitorReceivedState state() const noexcept;

 private:
  static std::int64_t ContinuousNow() noexcept;
  std::int64_t Elapsed() noexcept;
  void Fail(MonitorReceivedFailure reason) noexcept;
  void Wake() noexcept;

  const std::thread::id ui_thread_;
  Clock clock_;
  std::mutex wake_mutex_;
  std::condition_variable changed_;
  std::uint64_t generation_ = 0;
  // One atomic gate prevents an Activate racing permanent retirement.
  enum class Gate { prepared, active, retired };
  std::atomic<Gate> gate_{Gate::prepared};
  std::atomic<bool> activated_{false}, sealed_{false}, codec_stopped_{false};
  std::atomic<bool> claimed_{false}, draining_{false}, drained_{false};
  std::atomic<MonitorReceivedFailure> failure_{MonitorReceivedFailure::none};
  std::atomic<std::int64_t> first_audio_ns_{-1};
  std::atomic<std::uint64_t> audio_blocks_{0}, video_blocks_{0};
  std::int64_t epoch_ns_ = -1;
  LocalPcmQueue audio_;
  LocalVideoQueue video_;
};

}  // namespace nonet
#endif
