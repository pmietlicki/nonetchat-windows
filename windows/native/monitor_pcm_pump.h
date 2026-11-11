#ifndef NONET_WINDOWS_MONITOR_PCM_PUMP_H_
#define NONET_WINDOWS_MONITOR_PCM_PUMP_H_

#include "monitor_aac_sink.h"
#include "monitor_media_stream.h"
#include "local_pcm_queue.h"

namespace nonet {

// One worker-side recording purpose. The selected-track callback sees only
// LocalPcmQueue::Push; it never calls an encoder, opens a file or waits for IO.
// Seal from the owning stop request wakes Run. Its worker calls Close afterwards
// and retains this pump plus the file lease until a positive closure proof.
class MonitorPcmPump final {
 public:
  explicit MonitorPcmPump(LocalPcmQueue& queue, MonitorAacSink::Factory factory = {});
  MonitorPcmPump(const MonitorPcmPump&) = delete;
  MonitorPcmPump& operator=(const MonitorPcmPump&) = delete;
  HRESULT Run(IMFByteStream* owned_stream) noexcept;
  MonitorAacClose Close() noexcept;
  std::uint64_t written_frames() const noexcept;

 private:
  bool OnThread() const noexcept;
  HRESULT Fail(HRESULT reason) noexcept;
  LocalPcmQueue& queue_;
  const DWORD thread_;
  MonitorAacSink sink_;
  Microsoft::WRL::ComPtr<ConfirmedMonitorByteStream> stream_;
  bool attempted_ = false, running_ = false, closing_ = false;
  HRESULT error_ = S_OK;
};

}  // namespace nonet
#endif
