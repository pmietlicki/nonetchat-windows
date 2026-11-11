#ifndef NONET_WINDOWS_MONITOR_AAC_SINK_H_
#define NONET_WINDOWS_MONITOR_AAC_SINK_H_

#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace nonet {

struct MonitorPcmFormat {
  std::uint32_t rate;
  std::uint32_t channels;
  bool operator==(const MonitorPcmFormat& other) const noexcept {
    return rate == other.rate && channels == other.channels;
  }
};

struct MonitorAacClose {
  bool resources_closed = false;
  bool content_finalized = false;  // Still requires the independent inspector.
  HRESULT encoding_status = S_OK;
  HRESULT cleanup_status = S_OK;
};

// One COM/MF context on the dedicated native worker, never on an MF work queue.
// Its owner destroys it after every writer/reader and callback has drained.
class MonitorMediaContext final {
 public:
  MonitorMediaContext() noexcept;
  ~MonitorMediaContext();
  bool ready() const noexcept;
  MonitorMediaContext(const MonitorMediaContext&) = delete;
  MonitorMediaContext& operator=(const MonitorMediaContext&) = delete;
 private:
  DWORD thread_;
  bool com_ = false, media_ = false;
};

// Single-use, worker-thread-only AAC-LC/MPEG4 sink. Accepts only an already
// owned local byte-stream capability: cannot choose a path, capture or network.
// The host must retain an unconfirmed Close and its file lease for retry.
// No destructor or return value grants permission to publish/preview the bytes.
class MonitorAacSink final {
 public:
  using Factory = std::function<HRESULT(IMFByteStream*, IMFAttributes*, IMFSinkWriter**)>;
  explicit MonitorAacSink(Factory factory = {});
  ~MonitorAacSink();
  MonitorAacSink(const MonitorAacSink&) = delete;
  MonitorAacSink& operator=(const MonitorAacSink&) = delete;
  HRESULT Open(IMFByteStream* owned_stream, MonitorPcmFormat format) noexcept;
  HRESULT Write(MonitorPcmFormat format, const void* pcm16, std::size_t bytes) noexcept;
  MonitorAacClose Close() noexcept;
  std::uint64_t written_frames() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nonet
#endif
