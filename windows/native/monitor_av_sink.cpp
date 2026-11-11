#include "monitor_av_sink.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <roapi.h>
#include <strmif.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/base.h>
#include <wrl.h>

#include <cstring>
#include <limits>
#include <utility>

#include "monitor_media_stream.h"
namespace nonet {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT32 kAacBytesPerSecond = 12000, kOpusRate = 48000;
constexpr std::uint64_t kHnsPerSecond = 10000000;
HRESULT AudioType(MonitorPcmFormat format, bool compressed,
                  IMFMediaType** out) {
  ComPtr<IMFMediaType> type;
  HRESULT hr = MFCreateMediaType(&type);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (SUCCEEDED(hr))
    hr = type->SetGUID(MF_MT_SUBTYPE,
                       compressed ? MFAudioFormat_AAC : MFAudioFormat_PCM);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  if (SUCCEEDED(hr))
    hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.rate);
  if (SUCCEEDED(hr))
    hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels);
  if (SUCCEEDED(hr))
    hr = type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        compressed ? kAacBytesPerSecond : format.rate * format.channels * 2);
  if (SUCCEEDED(hr) && compressed)
    hr = type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
  if (SUCCEEDED(hr) && !compressed)
    hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, format.channels * 2);
  if (SUCCEEDED(hr) && !compressed)
    hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  // Standard Windows speaker assignments, needed by the built-in resampler.
  if (SUCCEEDED(hr) && !compressed)
    hr = type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                         format.channels == 1 ? 0x4 : 0x3);
  if (SUCCEEDED(hr)) *out = type.Detach();
  return hr;
}

bool Timestamp(std::uint64_t frames, std::uint32_t rate,
               LONGLONG* result) noexcept {
  const auto quotient = frames / rate;
  const auto remainder = frames % rate;
  constexpr auto max =
      static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
  if (quotient > max / kHnsPerSecond) return false;
  const auto whole = quotient * kHnsPerSecond;
  const auto fraction = remainder * kHnsPerSecond / rate;
  if (fraction > max - whole) return false;
  *result = static_cast<LONGLONG>(whole + fraction);
  return true;
}
HRESULT WriteSample(IMFSinkWriter* writer, DWORD stream, const void* bytes,
                    DWORD count, LONGLONG at, LONGLONG duration) {
  if (!bytes || !count || at < 0 || duration <= 0) return E_INVALIDARG;
  HRESULT hr;
  ComPtr<IMFSample> value;
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(hr = MFCreateSample(&value))) return hr;
  if (FAILED(hr = MFCreateMemoryBuffer(count, &buffer))) return hr;
  BYTE* destination = nullptr;
  if (FAILED(hr = buffer->Lock(&destination, nullptr, nullptr))) return hr;
  std::memcpy(destination, bytes, count);
  if (FAILED(hr = buffer->Unlock())) return hr;
  if (FAILED(hr = buffer->SetCurrentLength(count))) return hr;
  if (FAILED(hr = value->AddBuffer(buffer.Get()))) return hr;
  if (FAILED(hr = value->SetSampleTime(at))) return hr;
  if (FAILED(hr = value->SetSampleDuration(duration))) return hr;
  if (FAILED(hr = writer->WriteSample(stream, value.Get()))) return hr;
  return S_OK;
}

HRESULT VideoTypes(IMFMediaType** compressed, IMFMediaType** raw,
                   std::uint64_t* duration) {
  using namespace winrt::Windows::Media::MediaProperties;
  auto profile = MediaEncodingProfile::CreateMp4(VideoEncodingQuality::Vga);
  auto video = profile.Video();
  if (video.Subtype() != L"H264" || video.Width() != 640 ||
      video.Height() != 480 || video.FrameRate().Numerator() != 30 ||
      video.FrameRate().Denominator() != 1)
    return MF_E_INVALIDMEDIATYPE;
  ComPtr<IMFMediaType> output, input;
  HRESULT hr =
      MFCreateMediaTypeFromProperties(winrt::get_unknown(video), &output);
  if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input);
  if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (SUCCEEDED(hr))
    hr = input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 640, 480);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 30, 1);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(hr)) {
    *compressed = output.Detach();
    *raw = input.Detach();
    *duration = kHnsPerSecond * video.FrameRate().Denominator() /
                video.FrameRate().Numerator();
  }
  return hr;
}
HRESULT ConfirmEncoder(IMFSinkWriter* writer, DWORD stream) {
  ComPtr<IMFSinkWriterEx> extended;
  HRESULT hr = writer->QueryInterface(IID_PPV_ARGS(&extended));
  if (FAILED(hr)) return hr;
  bool found = false;
  for (DWORD index = 0; index != UINT32_MAX; ++index) {
    GUID category{};
    ComPtr<IMFTransform> transform;
    hr = extended->GetTransformForStream(stream, index, &category, &transform);
    if (FAILED(hr)) return found ? S_OK : hr;
    if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
    if (found) return E_UNEXPECTED;
    found = true;
    ComPtr<ICodecAPI> api;
    hr = transform.As(&api);
    if (FAILED(hr)) return hr;
    if (api->IsSupported(&CODECAPI_AVEncMPVDefaultBPictureCount) != S_OK)
      return MF_E_INVALIDMEDIATYPE;
    VARIANT observed{};
    hr = api->GetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &observed);
    const bool zero =
        SUCCEEDED(hr) && observed.vt == VT_UI4 && observed.ulVal == 0;
    const auto clear = VariantClear(&observed);
    if (FAILED(hr)) return hr;
    if (FAILED(clear)) return clear;
    if (!zero) return MF_E_INVALIDMEDIATYPE;
  }
  return E_UNEXPECTED;
}
}  // namespace
struct MonitorAvSink::Impl {
  struct Runtime {
    const DWORD thread = GetCurrentThreadId();
    const HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
    ~Runtime() {
      if (SUCCEEDED(status) && GetCurrentThreadId() == thread) RoUninitialize();
    }
  } runtime;  // Declared first: outlives all writer/stream/WinRT resources.
  explicit Impl(MonitorAacSink::Factory value) : factory(std::move(value)) {}
  const DWORD thread = GetCurrentThreadId();
  MonitorAacSink::Factory factory;
  ComPtr<ConfirmedMonitorByteStream> stream;
  ComPtr<IMFSinkWriter> writer;
  MonitorPcmFormat format{};
  DWORD audio = 0, video = 0;
  std::uint64_t first_audio = 0, frames = 0, images = 0, nominal_video = 0;
  LONGLONG last_video = -1;
  bool attempted = false, writing = false, closing = false, finalized = false,
       finalize_attempted = false;
  HRESULT encoding = S_OK;
  bool OnThread() const noexcept { return GetCurrentThreadId() == thread; }
  HRESULT Fail(HRESULT hr) noexcept {
    if (SUCCEEDED(encoding) && FAILED(hr)) encoding = hr;
    return hr;
  }
};
MonitorAvSink::MonitorAvSink(MonitorAacSink::Factory factory)
    : impl_(std::make_unique<Impl>(std::move(factory))) {}
MonitorAvSink::~MonitorAvSink() = default;
HRESULT MonitorAvSink::Open(IMFByteStream* stream, MonitorPcmFormat format,
                            std::uint64_t first_audio_hns) noexcept {
  auto& s = *impl_;
  if (!s.OnThread()) return RPC_E_WRONG_THREAD;
  if (s.attempted || s.closing) return MF_E_INVALIDREQUEST;
  s.attempted = true;
  if (FAILED(s.runtime.status)) return s.Fail(s.runtime.status);
  if (!stream || !format.rate ||
      (format.channels != 1 && format.channels != 2) ||
      format.rate > UINT32_MAX / (format.channels * 2) ||
      first_audio_hns > INT64_MAX)
    return s.Fail(E_INVALIDARG);
  try {
    s.stream = Microsoft::WRL::Make<ConfirmedMonitorByteStream>(stream);
    if (!s.stream) return s.Fail(E_OUTOFMEMORY);
    s.format = format;
    s.first_audio = first_audio_hns;
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr))
      hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                               MFTranscodeContainerType_FMPEG4);
    if (SUCCEEDED(hr))
      hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);
    if (SUCCEEDED(hr))
      hr = s.factory ? s.factory(s.stream.Get(), attributes.Get(), &s.writer)
                     : MFCreateSinkWriterFromURL(nullptr, s.stream.Get(),
                                                 attributes.Get(), &s.writer);
    if (SUCCEEDED(hr) && !s.writer) hr = E_UNEXPECTED;
    ComPtr<IMFMediaType> audio_out, audio_in, video_out, video_in;
    const MonitorPcmFormat encoded{format.rate == 44100 ? 44100u : kOpusRate,
                                   format.channels};
    if (SUCCEEDED(hr)) hr = AudioType(encoded, true, &audio_out);
    if (SUCCEEDED(hr)) hr = AudioType(format, false, &audio_in);
    if (SUCCEEDED(hr)) hr = VideoTypes(&video_out, &video_in, &s.nominal_video);
    if (SUCCEEDED(hr)) hr = s.writer->AddStream(audio_out.Get(), &s.audio);
    if (SUCCEEDED(hr)) hr = s.writer->AddStream(video_out.Get(), &s.video);
    if (SUCCEEDED(hr) && s.audio == s.video) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
      hr = s.writer->SetInputMediaType(s.audio, audio_in.Get(), nullptr);
    ComPtr<IMFAttributes> parameters;
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&parameters, 1);
    if (SUCCEEDED(hr))
      hr = parameters->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    if (SUCCEEDED(hr))
      hr = s.writer->SetInputMediaType(s.video, video_in.Get(),
                                       parameters.Get());
    if (SUCCEEDED(hr)) hr = ConfirmEncoder(s.writer.Get(), s.video);
    if (SUCCEEDED(hr)) hr = s.writer->BeginWriting();
    if (FAILED(hr)) return s.Fail(hr);
    s.writing = true;
    return S_OK;
  } catch (const winrt::hresult_error& error) {
    return s.Fail(error.code().value);
  } catch (...) {
    return s.Fail(E_FAIL);
  }
}
HRESULT MonitorAvSink::Audio(const LocalPcmBlock& block) noexcept {
  auto& s = *impl_;
  if (!s.OnThread()) return RPC_E_WRONG_THREAD;
  if (!s.writing || s.closing || !s.writer) return MF_E_INVALIDREQUEST;
  if (FAILED(s.encoding)) return s.encoding;
  if (block.rate != s.format.rate || block.channels != s.format.channels ||
      !block.frames ||
      block.frames >
          LocalPcmQueue::max_bytes / (block.channels * sizeof(std::int16_t)) ||
      block.samples.size() != block.frames * block.channels ||
      block.frames > UINT64_MAX - s.frames)
    return s.Fail(E_INVALIDARG);
  LONGLONG at = 0, end = 0;
  if (!Timestamp(s.frames, block.rate, &at) ||
      !Timestamp(s.frames + block.frames, block.rate, &end) ||
      at > INT64_MAX - static_cast<LONGLONG>(s.first_audio) ||
      end > INT64_MAX - static_cast<LONGLONG>(s.first_audio) || end <= at)
    return s.Fail(E_INVALIDARG);
  try {
    const auto hr = WriteSample(
        s.writer.Get(), s.audio, block.samples.data(),
        static_cast<DWORD>(block.samples.size() * sizeof(std::int16_t)),
        at + static_cast<LONGLONG>(s.first_audio), end - at);
    if (FAILED(hr)) return s.Fail(hr);
    s.frames += block.frames;
    return S_OK;
  } catch (...) {
    return s.Fail(E_FAIL);
  }
}
HRESULT MonitorAvSink::Video(const NormalizedMonitorVideo& block) noexcept {
  auto& s = *impl_;
  if (!s.OnThread()) return RPC_E_WRONG_THREAD;
  if (!s.writing || s.closing || !s.writer) return MF_E_INVALIDREQUEST;
  if (FAILED(s.encoding)) return s.encoding;
  if (block.local_ns < 0 || !block.duration_hns ||
      block.duration_hns > INT64_MAX || block.local_ns / 100 <= s.last_video ||
      block.local_ns / 100 >
          INT64_MAX - static_cast<LONGLONG>(block.duration_hns) ||
      block.nv12.size() != 640 * 480 * 3 / 2 || s.images == UINT64_MAX)
    return s.Fail(E_INVALIDARG);
  try {
    const auto hr = WriteSample(s.writer.Get(), s.video, block.nv12.data(),
                                static_cast<DWORD>(block.nv12.size()),
                                block.local_ns / 100, block.duration_hns);
    if (FAILED(hr)) return s.Fail(hr);
    s.last_video = block.local_ns / 100;
    ++s.images;
    return S_OK;
  } catch (...) {
    return s.Fail(E_FAIL);
  }
}
MonitorAacClose MonitorAvSink::Close() noexcept {
  auto& s = *impl_;
  if (!s.OnThread()) return {false, false, S_OK, RPC_E_WRONG_THREAD};
  s.closing = s.attempted = true;
  s.writing = false;
  if (s.writer && !s.finalize_attempted) {
    s.finalize_attempted = true;
    try {
      const auto hr = s.writer->Finalize();
      if (FAILED(hr))
        s.Fail(hr);
      else
        s.finalized = true;
    } catch (...) {
      s.Fail(E_FAIL);
    }
  }
  try {
    // All writer processing is synchronous. Retire the original writer before
    // the physical AV owner seals its final durations and closes the HANDLE.
    s.writer.Reset();
    const auto status = s.stream ? s.stream->Close() : S_OK;
    if (status != S_OK) return {false, false, s.encoding, status};
    s.stream.Reset();
    return {true, s.finalized && SUCCEEDED(s.encoding) && s.frames && s.images,
            s.encoding, S_OK};
  } catch (...) {
    return {false, false, s.encoding, E_FAIL};
  }
}
std::uint64_t MonitorAvSink::nominal_video_duration_hns() const noexcept {
  return impl_->OnThread() ? impl_->nominal_video : 0;
}
std::uint64_t MonitorAvSink::audio_frames() const noexcept {
  return impl_->OnThread() ? impl_->frames : 0;
}
std::uint64_t MonitorAvSink::video_frames() const noexcept {
  return impl_->OnThread() ? impl_->images : 0;
}
}  // namespace nonet
