#ifndef NONET_WINDOWS_MONITOR_AV_SINK_H_
#define NONET_WINDOWS_MONITOR_AV_SINK_H_
#include "monitor_aac_sink.h"
#include "monitor_video_normalizer.h"

namespace nonet {
// Serial consuming-worker AAC/H264 writer. Both original local starts must
// already configure the owned AV fragment stream; this class has no path or
// source lookup. No capture, network, consent or playback authority is granted.
class MonitorAvSink final {
 public:
  explicit MonitorAvSink(MonitorAacSink::Factory factory = {});
  ~MonitorAvSink();
  MonitorAvSink(const MonitorAvSink&) = delete;
  MonitorAvSink& operator=(const MonitorAvSink&) = delete;
  HRESULT Open(IMFByteStream* owned_stream, MonitorPcmFormat format,
               std::uint64_t first_audio_hns) noexcept;
  HRESULT Audio(const LocalPcmBlock&) noexcept;
  HRESULT Video(const NormalizedMonitorVideo&) noexcept;
  MonitorAacClose Close() noexcept;
  std::uint64_t nominal_video_duration_hns() const noexcept;
  std::uint64_t audio_frames() const noexcept;
  std::uint64_t video_frames() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
