#ifndef NONET_WINDOWS_MONITOR_AV_INSPECTOR_H_
#define NONET_WINDOWS_MONITOR_AV_INSPECTOR_H_
#include <string>

#include "monitor_aac_inspector.h"
#include "monitor_recording_file.h"
namespace nonet {
struct MonitorAvMetadata {
  MonitorAacMetadata audio;
  std::uint64_t decoded_audio_frames = 0, video_frames = 0;
  std::int64_t first_frame_us = 0, second_frame_us = 0, end_frame_us = 0;
  std::string sha256;
};
struct MonitorAvInspectedClose {
  bool resources_closed = false;
  HRESULT inspection_status = S_OK, cleanup_status = S_OK;
  std::optional<MonitorAvMetadata> media;
};
// Same original pinned file, one owned MF reader, bounded packets/decoded
// frames and independent hashes. No path, URL, capture or playback authority.
// Metadata remains private until the reader AND its original stream close.
class MonitorAvInspector final {
 public:
  using Continue = std::function<bool()>;
  using Factory = MonitorAacInspector::Factory;
  explicit MonitorAvInspector(Continue, Factory = {});
  ~MonitorAvInspector();
  MonitorAvInspector(const MonitorAvInspector&) = delete;
  MonitorAvInspector& operator=(const MonitorAvInspector&) = delete;
  HRESULT Inspect(MonitorRecordingFile&) noexcept;
  MonitorAvInspectedClose Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
