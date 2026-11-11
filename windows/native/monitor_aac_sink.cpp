#include "monitor_aac_sink.h"
#include "monitor_media_stream.h"

#include <mfapi.h>
#include <mferror.h>
#include <wrl.h>

#include <cstring>
#include <limits>
#include <utility>

namespace nonet {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
// Microsoft AAC Encoder's default96kbit/s, also used by the Windows voice profile.
constexpr UINT32 kAacBytesPerSecond = 12000;
// RFC7587's Opus clock; only PCM rates requiring conversion use this AAC rate.
constexpr UINT32 kOpusRate = 48000;
constexpr std::uint64_t kHnsPerSecond = 10'000'000;

HRESULT AudioType(MonitorPcmFormat format, bool compressed, IMFMediaType** out) {
  ComPtr<IMFMediaType> type;
  HRESULT hr = MFCreateMediaType(&type);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, compressed ? MFAudioFormat_AAC : MFAudioFormat_PCM);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.rate);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
      compressed ? kAacBytesPerSecond : format.rate * format.channels * 2);
  if (SUCCEEDED(hr) && compressed) hr = type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
  if (SUCCEEDED(hr) && !compressed) hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, format.channels * 2);
  if (SUCCEEDED(hr) && !compressed) hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  // Standard Windows speaker assignments, needed by the built-in resampler.
  if (SUCCEEDED(hr) && !compressed) hr = type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
      format.channels == 1 ? 0x4 : 0x3);
  if (SUCCEEDED(hr)) *out = type.Detach();
  return hr;
}

bool Timestamp(std::uint64_t frames, std::uint32_t rate, LONGLONG* result) noexcept {
  const auto quotient = frames / rate;
  const auto remainder = frames % rate;
  constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
  if (quotient > max / kHnsPerSecond) return false;
  const auto whole = quotient * kHnsPerSecond;
  const auto fraction = remainder * kHnsPerSecond / rate;
  if (fraction > max - whole) return false;
  *result = static_cast<LONGLONG>(whole + fraction); return true;
}
}  // namespace

MonitorMediaContext::MonitorMediaContext() noexcept : thread_(GetCurrentThreadId()) {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  com_ = initialized == S_OK || initialized == S_FALSE;
  if (com_) media_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
}
MonitorMediaContext::~MonitorMediaContext() {
  if (GetCurrentThreadId() != thread_) return;
  if (media_) MFShutdown();
  if (com_) CoUninitialize();
}
bool MonitorMediaContext::ready() const noexcept { return GetCurrentThreadId() == thread_ && media_; }

struct MonitorAacSink::Impl {
  explicit Impl(Factory value) : factory(std::move(value)), thread(GetCurrentThreadId()) {}
  Factory factory;
  const DWORD thread;
  ComPtr<ConfirmedMonitorByteStream> stream;
  ComPtr<IMFSinkWriter> writer;
  MonitorPcmFormat format{};
  DWORD stream_index = 0;
  std::uint64_t frames = 0;
  bool attempted = false, writing = false, closing = false, finalized = false, finalize_attempted = false;
  HRESULT encoding = S_OK;
  bool OnThread() const noexcept { return GetCurrentThreadId() == thread; }
  HRESULT Fail(HRESULT value) noexcept { if (SUCCEEDED(encoding)) encoding = value; return value; }
};

MonitorAacSink::MonitorAacSink(Factory factory) : impl_(std::make_unique<Impl>(std::move(factory))) {}
MonitorAacSink::~MonitorAacSink() = default;

HRESULT MonitorAacSink::Open(IMFByteStream* owned_stream, MonitorPcmFormat format) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (self.attempted) return MF_E_INVALIDREQUEST;
  self.attempted = true;
  if (!owned_stream || !format.rate || (format.channels != 1 && format.channels != 2) ||
      format.rate > std::numeric_limits<UINT32>::max() / (format.channels * 2)) return self.Fail(E_INVALIDARG);
  try {
    self.stream = Make<ConfirmedMonitorByteStream>(owned_stream);
    if (!self.stream) return self.Fail(E_OUTOFMEMORY);
    self.format = format;
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);
    // Keep the SDK default backpressure on this worker. The real-time source
    // callback only copies into LocalPcmQueue; never disable throttling and
    // turn that bounded handoff into an unbounded sink-writer input queue.
    // No async finalize callback: completion means the container work returned.
    if (SUCCEEDED(hr)) hr = self.factory ? self.factory(self.stream.Get(), attributes.Get(), &self.writer) :
        MFCreateSinkWriterFromURL(nullptr, self.stream.Get(), attributes.Get(), &self.writer);
    if (SUCCEEDED(hr) && !self.writer) hr = E_UNEXPECTED;
    ComPtr<IMFMediaType> output, input;
    const MonitorPcmFormat encoded{format.rate == 44100 ? 44100u : kOpusRate, format.channels};
    if (SUCCEEDED(hr)) hr = AudioType(encoded, true, &output);
    if (SUCCEEDED(hr)) hr = self.writer->AddStream(output.Get(), &self.stream_index);
    if (SUCCEEDED(hr)) hr = AudioType(format, false, &input);
    if (SUCCEEDED(hr)) hr = self.writer->SetInputMediaType(self.stream_index, input.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = self.writer->BeginWriting();
    if (FAILED(hr)) return self.Fail(hr);
    self.writing = true; return S_OK;
  } catch (...) { return self.Fail(E_FAIL); }
}

HRESULT MonitorAacSink::Write(MonitorPcmFormat format, const void* pcm16, std::size_t bytes) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (!self.writing || self.closing || !self.writer) return MF_E_INVALIDREQUEST;
  if (FAILED(self.encoding)) return self.encoding;
  const auto alignment = self.format.channels * 2;
  if (!(format == self.format) || !pcm16 || !bytes || bytes % alignment ||
      bytes > std::numeric_limits<DWORD>::max()) return self.Fail(E_INVALIDARG);
  const auto frames = static_cast<std::uint64_t>(bytes / alignment);
  if (frames > std::numeric_limits<std::uint64_t>::max() - self.frames) return self.Fail(E_INVALIDARG);
  LONGLONG start = 0, end = 0;
  if (!Timestamp(self.frames, format.rate, &start) || !Timestamp(self.frames + frames, format.rate, &end) ||
      end <= start) return self.Fail(E_INVALIDARG);
  try {
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      std::memcpy(data, pcm16, bytes);
      hr = buffer->Unlock();
    }
    if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(static_cast<DWORD>(bytes));
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(start);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(end - start);
    if (SUCCEEDED(hr)) hr = self.writer->WriteSample(self.stream_index, sample.Get());
    if (FAILED(hr)) return self.Fail(hr);
    self.frames += frames; return S_OK;
  } catch (...) { return self.Fail(E_FAIL); }
}

MonitorAacClose MonitorAacSink::Close() noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return {false, false, S_OK, RPC_E_WRONG_THREAD};
  self.closing = true;
  self.attempted = true;  // Closing a fresh purpose also retires it irreversibly.
  self.writing = false;
  if (self.writer && !self.finalize_attempted) {
    self.finalize_attempted = true;
    try {
      const auto hr = self.writer->Finalize();
      if (FAILED(hr)) self.Fail(hr);
      else self.finalized = true;
    } catch (...) {
      self.Fail(E_FAIL);  // Still try to close the already owned byte stream.
    }
  }
  try {
    const auto status = self.stream ? self.stream->Close() : S_OK;
    if (status != S_OK) {
      // Keep writer and stream owned for retry; never repeat Finalize.
      return {false, false, self.encoding, status};
    }
    self.writer.Reset();
    self.stream.Reset();
    return {true, self.finalized && SUCCEEDED(self.encoding), self.encoding, S_OK};
  } catch (...) { return {false, false, self.encoding, E_FAIL}; }
}

std::uint64_t MonitorAacSink::written_frames() const noexcept {
  return impl_->OnThread() ? impl_->frames : 0;
}
}  // namespace nonet
