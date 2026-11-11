#include "monitor_selected_program.h"

#include <mferror.h>

#include <utility>
namespace nonet {
MonitorSelectedProgram::MonitorSelectedProgram(
    MonitorReceivedMedia& media, bool video, std::wstring support,
    MonitorAvProgram::Revoked revoked)
    : media_(media), revoked_(std::move(revoked)) {
  if (video)
    video_ = std::make_unique<MonitorAvProgram>(media_, std::move(support),
                                                revoked_);
  else
    file_ = std::make_unique<MonitorRecordingFile>(std::move(support),
                                                   [&media] { media.Seal(); });
}
HRESULT MonitorSelectedProgram::Fail(HRESULT hr) noexcept {
  if (FAILED(hr) && SUCCEEDED(error_)) error_ = hr;
  return hr;
}
bool MonitorSelectedProgram::Live() noexcept {
  try {
    if (revoked_ && !revoked_()) return true;
  } catch (...) {
  }
  Fail(E_ABORT);
  media_.Seal();
  return false;
}
HRESULT MonitorSelectedProgram::Prepare(const std::wstring& path,
                                        const std::string& id,
                                        std::uint64_t budget) noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (prepared_ || attempted_ || closing_) return MF_E_INVALIDREQUEST;
  if (!context_.ready() || !Live()) return Fail(E_ABORT);
  const auto hr = video_ ? video_->Prepare(path, id, budget)
                         : file_->Create(path, id, budget);
  if (FAILED(hr)) return Fail(hr);
  prepared_ = true;
  return S_OK;
}
HRESULT MonitorSelectedProgram::Run() noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (!prepared_ || attempted_ || closing_) return MF_E_INVALIDREQUEST;
  attempted_ = true;
  if (video_) return run_ = video_->Run();
  const bool drained = media_.DrainAudio([this](LocalPcmQueue& queue) {
    if (!Live()) return false;
    pump_ = std::make_unique<MonitorPcmPump>(queue);
    run_ = pump_->Run(file_->stream());
    return SUCCEEDED(run_) || run_ == kMonitorRecordingLimit;
  });
  if (!drained && SUCCEEDED(error_) && (SUCCEEDED(run_) || run_ == E_PENDING))
    Fail(E_FAIL);
  if (FAILED(run_) && run_ != kMonitorRecordingLimit) Fail(run_);
  return error_;
}
MonitorAvProgramClosed MonitorSelectedProgram::Close() noexcept {
  if (GetCurrentThreadId() != thread_)
    return {false, false, S_OK, RPC_E_WRONG_THREAD};
  closing_ = true;
  media_.Seal();
  if (video_) {
    auto result = video_->Close();
    if (!Live()) {
      result.content_finalized = false;
      result.encoding = error_;
    }
    return result;
  }
  const auto done =
      pump_ ? pump_->Close() : MonitorAacClose{true, false, error_, S_OK};
  if (!done.resources_closed)
    return {false, false, error_, done.cleanup_status};
  const auto cleanup = file_->Close();
  if (cleanup != S_OK) return {false, false, error_, cleanup};
  const auto physical = file_->failure();
  const bool quota = file_->quota_prefix() &&
                     (run_ == S_OK || run_ == kMonitorRecordingLimit) &&
                     (done.encoding_status == S_OK ||
                      done.encoding_status == kMonitorRecordingLimit) &&
                     physical == kMonitorRecordingLimit && SUCCEEDED(error_);
  if (!quota) {
    Fail(done.encoding_status);
    Fail(physical);
  }
  const bool valid =
      Live() && (done.content_finalized || quota) && SUCCEEDED(error_);
  return {true,
          valid,
          error_,
          S_OK,
          valid && quota ? file_->limit() : fmp4::Stop::none,
          pump_ ? pump_->written_frames() : 0,
          0};
}
}  // namespace nonet
