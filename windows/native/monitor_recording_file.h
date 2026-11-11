#ifndef NONET_WINDOWS_MONITOR_RECORDING_FILE_H_
#define NONET_WINDOWS_MONITOR_RECORDING_FILE_H_

#include <mfidl.h>
#include <windows.h>
#include <wrl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "monitor_av_fragment_stream.h"
#include "monitor_fragment_stream.h"

namespace nonet {

// Same RoamingAppData/company/product root as path_provider_windows 2.3.0.
// Reads only the running executable's own resources; creates no directory.
std::wstring MonitorRecordingSupportDirectory() noexcept;

// Worker-thread file capability. A peer cannot select a path: it must match the
// local application's prepared account-hash/UID directory and ownership marker.
// Keep this object until Close confirms every stream/handle, including on
// error.
enum class MonitorRecordingContainer { audio_m4a, audio_video_mp4 };

class MonitorRecordingFile final {
 public:
  explicit MonitorRecordingFile(
      std::wstring trusted_support,
      MonitorFragmentStream::LimitCallback on_limit = {});
  ~MonitorRecordingFile();
  MonitorRecordingFile(const MonitorRecordingFile&) = delete;
  MonitorRecordingFile& operator=(const MonitorRecordingFile&) = delete;

  HRESULT Create(const std::wstring& requested, const std::string& file_id,
                 std::uint64_t byte_budget) noexcept;
  // AV preparation pins the exact typed account/marker before source attach,
  // without choosing per-track starts or creating payload bytes. The same
  // worker starts its owned stream only after both original local times exist.
  HRESULT PrepareAv(const std::wstring& requested, const std::string& file_id,
                    std::uint64_t byte_budget) noexcept;
  HRESULT StartAv(std::uint64_t audio_start_hns,
                  std::uint64_t video_start_hns) noexcept;
  HRESULT Read(const std::wstring& requested, const std::string& file_id,
               MonitorRecordingContainer container =
                   MonitorRecordingContainer::audio_m4a) noexcept;
  // Local MediaPlayer borrows only this pinned, read-only path, never a URL.
  HRESULT PinRead(const std::wstring& requested, const std::string& file_id,
                  MonitorRecordingContainer container =
                      MonitorRecordingContainer::audio_m4a) noexcept;
  // Read-only capability over the already pinned ORIGINAL payload handle.
  // Its cursor is separate from the MF stream; no path/handle escapes to a
  // parser or hash job. Only the original worker can use it before Close.
  HRESULT ReadPinnedLength(std::uint64_t*) noexcept;
  HRESULT ReadPinnedAt(std::uint64_t, std::uint8_t*, std::size_t) noexcept;
  HRESULT Close() noexcept;
  IMFByteStream* stream() const noexcept;
  HRESULT failure() const noexcept;
  bool quota_prefix() const noexcept;
  fmp4::Stop limit() const noexcept;
  const std::wstring& path() const noexcept { return path_; }
  bool closed() const noexcept { return closed_; }

 private:
  HRESULT Open(const std::wstring&, const std::string&, bool read, bool stream,
               std::uint64_t byte_budget = 0,
               MonitorRecordingContainer container =
                   MonitorRecordingContainer::audio_m4a) noexcept;
  HRESULT PinDirectory(const std::wstring& path);
  HRESULT PinFile(const std::wstring& path, bool marker);
  HRESULT CheckMarker(const std::string& account, const std::string& id);
  const DWORD thread_;
  std::wstring support_, path_;
  std::vector<HANDLE> handles_;
  HANDLE read_pin_ = INVALID_HANDLE_VALUE;  // Alias of an owned handles_ entry.
  std::uint64_t read_bytes_ = 0;
  bool read_ready_ = false;
  Microsoft::WRL::ComPtr<IMFByteStream> stream_;
  MonitorFragmentStream::LimitCallback on_limit_;
  std::unique_ptr<MonitorFragmentStream> writer_;
  std::unique_ptr<MonitorAvFragmentStream> av_writer_;
  MonitorRecordingContainer container_ = MonitorRecordingContainer::audio_m4a;
  std::uint64_t byte_budget_ = 0;
  bool prepared_av_ = false, av_attempted_ = false;
  bool attempted_ = false, stream_closed_ = false, closed_ = false;
};

}  // namespace nonet
#endif
