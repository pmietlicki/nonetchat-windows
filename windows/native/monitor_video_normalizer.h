#ifndef NONET_WINDOWS_MONITOR_VIDEO_NORMALIZER_H_
#define NONET_WINDOWS_MONITOR_VIDEO_NORMALIZER_H_
#include <mfidl.h>
#include <windows.h>

#include <memory>

#include "local_video_queue.h"

namespace nonet {
struct NormalizedMonitorVideo {
  static constexpr int width = 640, height = 480;
  int x = 0, y = 0, picture_width = width, picture_height = height;
  std::int64_t local_ns = 0;
  std::uint64_t duration_hns = 0;
  std::vector<std::uint8_t> nv12;
};
// Worker-only conversion of owned callback copies. No source/file/capture API.
// System Video Processor MFT scales; cadence conversion is disabled. Rotation
// uses the same bounded plane permutation as the existing Linux normalizer.
class MonitorVideoNormalizer final {
 public:
  MonitorVideoNormalizer();
  ~MonitorVideoNormalizer();
  MonitorVideoNormalizer(const MonitorVideoNormalizer&) = delete;
  MonitorVideoNormalizer& operator=(const MonitorVideoNormalizer&) = delete;
  std::optional<NormalizedMonitorVideo> Convert(const LocalVideoBlock&,
                                                std::uint64_t duration_hns);
  HRESULT failure() const noexcept;
  HRESULT Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
