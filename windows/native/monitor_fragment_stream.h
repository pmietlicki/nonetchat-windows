#ifndef NONET_WINDOWS_MONITOR_FRAGMENT_STREAM_H_
#define NONET_WINDOWS_MONITOR_FRAGMENT_STREAM_H_
#include <mfidl.h>
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "fmp4_audio_admission.h"
namespace nonet {
// Private native status: only this owner plus its closed prefix can turn it
// into normal completion. A disk or arbitrary HRESULT alone never suffices.
inline const HRESULT kMonitorRecordingLimit =
    HRESULT_FROM_WIN32(ERROR_DISK_QUOTA_EXCEEDED);
class MonitorFragmentStream final {
 public:
  using LimitCallback = std::function<void()>;
  explicit MonitorFragmentStream(LimitCallback callback = {});
  ~MonitorFragmentStream();
  MonitorFragmentStream(const MonitorFragmentStream&) = delete;
  MonitorFragmentStream& operator=(const MonitorFragmentStream&) = delete;
  HRESULT Create(const std::wstring&, std::uint64_t budget) noexcept;
  IMFByteStream* stream() const noexcept;
  HRESULT failure() const noexcept;
  HRESULT Close() noexcept;
  bool quota_prefix() const noexcept;
  fmp4::Stop limit() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
