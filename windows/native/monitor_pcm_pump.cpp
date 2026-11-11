#include "monitor_pcm_pump.h"

#include <mferror.h>

#include <utility>

namespace nonet {
MonitorPcmPump::MonitorPcmPump(LocalPcmQueue& queue, MonitorAacSink::Factory factory)
    : queue_(queue), thread_(GetCurrentThreadId()), sink_(std::move(factory)) {}

bool MonitorPcmPump::OnThread() const noexcept { return GetCurrentThreadId() == thread_; }
HRESULT MonitorPcmPump::Fail(HRESULT reason) noexcept {
  if (SUCCEEDED(error_)) error_ = reason;
  return reason;
}

HRESULT MonitorPcmPump::Run(IMFByteStream* owned_stream) noexcept {
  if (!OnThread()) return RPC_E_WRONG_THREAD;
  if (attempted_ || closing_) return MF_E_INVALIDREQUEST;
  attempted_ = true;
  running_ = true;
  bool encoder_attempted = false;
  try {
    if (owned_stream) stream_ = Microsoft::WRL::Make<ConfirmedMonitorByteStream>(owned_stream);
    if (!stream_) {
      Fail(owned_stream ? E_OUTOFMEMORY : E_INVALIDARG);
      queue_.Seal();
    }
    while (auto block = queue_.WaitTake()) {
      if (FAILED(error_)) continue;  // Discard only after a visible terminal error.
      const MonitorPcmFormat format{block->rate, block->channels};
      if (!encoder_attempted) {
        encoder_attempted = true;
        const auto hr = sink_.Open(stream_.Get(), format);
        if (FAILED(hr)) {
          Fail(hr);
          queue_.Seal();
          continue;
        }
      }
      const auto hr = sink_.Write(format, block->samples.data(),
                                  block->samples.size() * sizeof(std::int16_t));
      if (FAILED(hr)) { Fail(hr); queue_.Seal(); }
    }
    switch (queue_.state().failure) {
      case LocalPcmAdmission::accepted:
      case LocalPcmAdmission::retired: break;
      case LocalPcmAdmission::invalid: Fail(E_INVALIDARG); break;
      case LocalPcmAdmission::full: Fail(MF_E_NOTACCEPTING); break;
      case LocalPcmAdmission::allocation_failed: Fail(E_OUTOFMEMORY); break;
    }
    if (!encoder_attempted) Fail(MF_E_INVALID_FILE_FORMAT);
  } catch (...) {
    Fail(E_FAIL);
    try { queue_.Seal(); } catch (...) {}
  }
  running_ = false;
  return error_;
}

MonitorAacClose MonitorPcmPump::Close() noexcept {
  if (!OnThread()) return {false, false, S_OK, RPC_E_WRONG_THREAD};
  closing_ = attempted_ = true;
  if (running_) return {false, false, S_OK, MF_E_NOTACCEPTING};
  try { queue_.Seal(); } catch (...) { return {false, false, error_, E_FAIL}; }
  const auto done = sink_.Close();
  if (FAILED(done.encoding_status)) Fail(done.encoding_status);
  if (!done.resources_closed) return {false, false, error_, done.cleanup_status};
  const auto status = stream_ ? stream_->Close() : S_OK;
  if (status != S_OK) return {false, false, error_, status};
  stream_.Reset();
  return {true, done.content_finalized && SUCCEEDED(error_), error_, S_OK};
}

std::uint64_t MonitorPcmPump::written_frames() const noexcept { return sink_.written_frames(); }
}  // namespace nonet
