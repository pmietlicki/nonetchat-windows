#include "monitor_av_program.h"

#include <mferror.h>

#include <algorithm>
#include <utility>
namespace nonet {
namespace {
// Same Pixel Recorder/owned-container limit as the existing Linux and Android
// paths. Never sampled from remote wall time or extended by incoming traffic.
constexpr std::uint64_t kMaximumNs = 18ULL * 60 * 60 * 1000000000;
bool AudioDuration(std::uint64_t frames, std::uint32_t rate,
                   std::uint64_t& ns) {
  if (!rate || frames / rate > kMaximumNs / 1000000000) return false;
  ns = frames / rate * 1000000000 + frames % rate * 1000000000 / rate;
  return ns <= kMaximumNs;
}
}  // namespace
MonitorAvProgram::MonitorAvProgram(MonitorReceivedMedia& media,
                                   std::wstring support, Revoked revoked)
    : media_(media),
      revoked_(std::move(revoked)),
      file_(std::move(support), [&media] { media.Seal(); }) {}
HRESULT MonitorAvProgram::Fail(HRESULT hr) noexcept {
  if (SUCCEEDED(error_) && FAILED(hr)) error_ = hr;
  return hr;
}
bool MonitorAvProgram::Live() noexcept {
  try {
    if (revoked_ && !revoked_()) return true;
  } catch (...) {
  }
  Fail(E_ABORT);
  media_.Seal();
  return false;
}
HRESULT MonitorAvProgram::Prepare(const std::wstring& path,
                                  const std::string& id,
                                  std::uint64_t budget) noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (prepared_ || attempted_ || closing_) return MF_E_INVALIDREQUEST;
  if (!Live()) return error_;
  const auto hr = file_.PrepareAv(path, id, budget);
  if (FAILED(hr)) return Fail(hr);
  prepared_ = true;
  return S_OK;
}
bool MonitorAvProgram::Start(const LocalPcmBlock& first, std::int64_t audio,
                             std::int64_t video) noexcept {
  if (!Live()) return false;
  if (audio < 0 || video < 0 ||
      static_cast<std::uint64_t>(audio) >= kMaximumNs ||
      static_cast<std::uint64_t>(video) >= kMaximumNs) {
    Fail(E_INVALIDARG);
    return false;
  }
  first_audio_ns_ =
      static_cast<std::uint64_t>(audio);  // Original 18h admission budget.
  // Both first callbacks normally arrive AFTER local activation. Choose one
  // actual shared media origin, never zero each track independently. This is
  // a uniform translation for a NEW file, retaining the entire original gap.
  origin_ns_ = static_cast<std::uint64_t>((std::min)(audio, video));
  const auto audio_start =
      (static_cast<std::uint64_t>(audio) - origin_ns_) / 100;
  const auto video_start =
      (static_cast<std::uint64_t>(video) - origin_ns_) / 100;
  auto hr = file_.StartAv(audio_start, video_start);
  if (SUCCEEDED(hr) && Live())
    hr = sink_.Open(file_.stream(), {first.rate, first.channels}, audio_start);
  if (FAILED(hr)) {
    Fail(hr);
    return false;
  }
  input_format_ = {first.rate, first.channels};
  return started_ = Live();
}
bool MonitorAvProgram::Audio(const LocalPcmBlock& block) noexcept {
  if (!Live()) return false;
  if (block.rate != input_format_.rate ||
      block.channels != input_format_.channels || !block.rate ||
      !block.channels || !block.frames ||
      block.frames >
          LocalPcmQueue::max_bytes / (block.channels * sizeof(std::int16_t)) ||
      block.samples.size() != block.frames * block.channels ||
      block.frames > UINT64_MAX - sink_.audio_frames()) {
    Fail(E_INVALIDARG);
    return false;
  }
  std::uint64_t duration = 0;
  if (!AudioDuration(sink_.audio_frames() + block.frames, block.rate,
                     duration) ||
      duration > kMaximumNs - first_audio_ns_) {
    raw_duration_ = true;
    media_.Seal();
    return false;
  }
  const auto hr = sink_.Audio(block);
  if (FAILED(hr)) {
    Fail(hr);
    return false;
  }
  return true;
}
bool MonitorAvProgram::Video(const LocalVideoBlock& block) noexcept {
  if (!Live()) return false;
  const auto duration = sink_.nominal_video_duration_hns();
  if (!block.valid() || !duration) {
    Fail(E_INVALIDARG);
    return false;
  }
  if (duration > kMaximumNs / 100 ||
      static_cast<std::uint64_t>(block.local_ns) >
          kMaximumNs - duration * 100) {
    raw_duration_ = true;
    media_.Seal();
    return false;
  }
  try {
    // The nominal last-image interval comes from the actual accepted system
    // VGA profile, like the existing Linux policy; each original local PTS is
    // kept, without frame-rate conversion, invented frames or timestamp repair.
    auto converted = normalizer_.Convert(block, duration);
    if (!converted) {
      Fail(normalizer_.failure());
      return false;
    }
    if (static_cast<std::uint64_t>(converted->local_ns) < origin_ns_) {
      Fail(E_INVALIDARG);
      return false;
    }
    // Preserve the normalizer's original callback PTS proof, then translate
    // by the SAME origin used by audio, without smoothing/duplicating frames.
    converted->local_ns -= static_cast<std::int64_t>(origin_ns_);
    const auto hr = sink_.Video(*converted);
    if (FAILED(hr)) {
      Fail(hr);
      return false;
    }
    return true;
  } catch (...) {
    Fail(E_FAIL);
    return false;
  }
}
HRESULT MonitorAvProgram::Run() noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (!prepared_ || attempted_ || closing_) return MF_E_INVALIDREQUEST;
  attempted_ = running_ = true;
  const bool drained =
      media_.Drain([this](const auto& block, auto audio,
                          auto video) { return Start(block, audio, video); },
                   [this](const auto& block) { return Audio(block); },
                   [this](const auto& block) { return Video(block); });
  running_ = false;
  if (!drained || !started_) Fail(MF_E_INVALID_FILE_FORMAT);
  return error_;
}
MonitorAvProgramClosed MonitorAvProgram::Close() noexcept {
  if (GetCurrentThreadId() != thread_)
    return {false, false, S_OK, RPC_E_WRONG_THREAD};
  closing_ = true;
  media_.Seal();
  if (running_) return {false, false, error_, MF_E_NOTACCEPTING};
  const auto transformed = normalizer_.Close();
  if (transformed != S_OK) return {false, false, error_, transformed};
  const auto sink = sink_.Close();
  if (FAILED(sink.encoding_status)) Fail(sink.encoding_status);
  if (!sink.resources_closed)
    return {false, false, error_, sink.cleanup_status};
  const auto cleanup = file_.Close();
  if (cleanup != S_OK) return {false, false, error_, cleanup};
  const auto physical = file_.failure();
  if (FAILED(physical)) Fail(physical);
  const bool prefix =
      file_.quota_prefix() && physical == kMonitorAvRecordingLimit;
  const bool valid =
      started_ &&
      (SUCCEEDED(error_) || (prefix && error_ == kMonitorAvRecordingLimit)) &&
      (sink.content_finalized || prefix);
  return {true,
          valid,
          error_,
          S_OK,
          valid ? raw_duration_ ? fmp4::Stop::duration
                  : prefix      ? file_.limit()
                                : fmp4::Stop::none
                : fmp4::Stop::none,
          sink_.audio_frames(),
          sink_.video_frames()};
}
}  // namespace nonet
