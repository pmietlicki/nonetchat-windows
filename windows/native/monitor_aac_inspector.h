#ifndef NONET_WINDOWS_MONITOR_AAC_INSPECTOR_H_
#define NONET_WINDOWS_MONITOR_AAC_INSPECTOR_H_

#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace nonet {

struct MonitorAacMetadata {
  std::uint64_t bytes = 0;
  std::int64_t duration_us = 0;
  std::uint32_t rate = 0, channels = 0;
  std::int64_t first_sample_us = 0, second_sample_us = 0, end_sample_us = 0;
};

struct MonitorAacInspectedClose {
  bool resources_closed = false;
  HRESULT inspection_status = S_OK, cleanup_status = S_OK;
  // Metadata is deliberately withheld until the reader and stream are closed.
  std::optional<MonitorAacMetadata> media;
};

// Read-only demuxing of a closed owned recording, on the same dedicated COM/MF
// worker as its enclosing file lease. No URL, microphone, decoder or playback.
// A successful scan is not evidence that the sound is audible or fully decodes.
class MonitorAacInspector final {
 public:
  using Factory = std::function<HRESULT(IMFByteStream*, IMFAttributes*, IMFSourceReader**)>;
  explicit MonitorAacInspector(Factory factory = {});
  ~MonitorAacInspector();
  MonitorAacInspector(const MonitorAacInspector&) = delete;
  MonitorAacInspector& operator=(const MonitorAacInspector&) = delete;
  HRESULT Inspect(IMFByteStream* owned_stream) noexcept;
  MonitorAacInspectedClose Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nonet
#endif
