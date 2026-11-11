#ifndef NONET_WINDOWS_MONITOR_AV_PROGRAM_H_
#define NONET_WINDOWS_MONITOR_AV_PROGRAM_H_
#include <functional>

#include "monitor_av_sink.h"
#include "monitor_received_media.h"
#include "monitor_recording_file.h"
namespace nonet {
struct MonitorAvProgramClosed {
  bool resources_closed = false, content_finalized = false;
  HRESULT encoding = S_OK, cleanup = S_OK;
  fmp4::Stop completion = fmp4::Stop::none;
  std::uint64_t input_audio_frames = 0, input_video_frames = 0;
};
// Worker-confined selected AV purpose, after a separate engine/source/indicator
// authorization. Owns the prepared typed file, original writer and normalizer
// until a positive Close. No SDK object, UI or permission can be acquired here.
class MonitorAvProgram final {
 public:
  // A voluntary Stop only seals media. This guard means policy/source revoked,
  // which invalidates even an otherwise closed prefix.
  using Revoked = std::function<bool()>;
  MonitorAvProgram(MonitorReceivedMedia&, std::wstring support, Revoked);
  HRESULT Prepare(const std::wstring& path, const std::string& id,
                  std::uint64_t budget) noexcept;
  HRESULT Run() noexcept;
  MonitorAvProgramClosed Close() noexcept;

 private:
  HRESULT Fail(HRESULT) noexcept;
  bool Live() noexcept;
  bool Start(const LocalPcmBlock&, std::int64_t audio_ns,
             std::int64_t video_ns) noexcept;
  bool Audio(const LocalPcmBlock&) noexcept;
  bool Video(const LocalVideoBlock&) noexcept;
  const DWORD thread_ = GetCurrentThreadId();
  MonitorReceivedMedia& media_;
  Revoked revoked_;
  MonitorRecordingFile file_;
  MonitorAvSink sink_;
  MonitorVideoNormalizer normalizer_;
  MonitorPcmFormat input_format_{};
  HRESULT error_ = S_OK;
  std::uint64_t first_audio_ns_ = 0, origin_ns_ = 0;
  bool prepared_ = false, attempted_ = false, running_ = false,
       closing_ = false;
  bool started_ = false, raw_duration_ = false;
};
}  // namespace nonet
#endif
