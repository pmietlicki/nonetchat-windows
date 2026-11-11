#ifndef NONET_WINDOWS_MONITOR_RECORDING_STREAM_H_
#define NONET_WINDOWS_MONITOR_RECORDING_STREAM_H_

#include <windows.h>
#include <mfidl.h>
#include <objidl.h>

#include <cstdint>
#include <memory>
#include <string>

namespace nonet {

// Worker-owned output only. The caller pins and validates its private path.
// Reads of historical files continue through the ordinary read-only adapter.
class MonitorRecordingStream final {
 public:
  // FileTransferLayout.maxNativeBytes / MonitorRecording.maxAccountBytes.
  static constexpr std::uint64_t max_bytes = 2000000000ULL;
  MonitorRecordingStream();
  ~MonitorRecordingStream();
  MonitorRecordingStream(const MonitorRecordingStream&) = delete;
  MonitorRecordingStream& operator=(const MonitorRecordingStream&) = delete;
  HRESULT Create(const std::wstring& path, std::uint64_t byte_budget) noexcept;
  // Synchronous physical capability for a format-aware owner. No MF adapter is
  // acquired; the caller drains its own adapter before Close on this worker.
  HRESULT CreateStorage(const std::wstring& path, std::uint64_t byte_budget) noexcept;
  IStream* storage() const noexcept;
  IMFByteStream* stream() const noexcept;
  HRESULT failure() const noexcept;
  HRESULT Close() noexcept;

 private:
  HRESULT Open(const std::wstring&, std::uint64_t, bool) noexcept;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nonet
#endif
