#include "monitor_aac_inspector.h"
#include "monitor_media_stream.h"

#include <mfapi.h>
#include <mferror.h>
#include <propvarutil.h>

#include <limits>
#include <utility>

namespace nonet {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

HRESULT Scan(IMFSourceReader* reader, MonitorAacMetadata* media) {
  ComPtr<IMFMediaType> type;
  HRESULT hr = reader->GetNativeMediaType(0, 0, &type);
  if (FAILED(hr)) return hr;
  if (!type) return E_UNEXPECTED;
  GUID major{}, subtype{};
  hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major);
  if (SUCCEEDED(hr)) hr = type->GetGUID(MF_MT_SUBTYPE, &subtype);
  if (FAILED(hr)) return hr;
  if (major != MFMediaType_Audio || subtype != MFAudioFormat_AAC) return MF_E_INVALIDMEDIATYPE;
  ComPtr<IMFMediaType> other;
  // A success, or any error other than "no such stream", cannot prove one track.
  if (reader->GetNativeMediaType(1, 0, &other) != MF_E_INVALIDSTREAMNUMBER) return MF_E_INVALIDMEDIATYPE;
  hr = type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &media->rate);
  if (SUCCEEDED(hr)) hr = type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &media->channels);
  if (FAILED(hr)) return hr;
  if (!media->rate || !media->channels) return MF_E_INVALIDMEDIATYPE;
  PROPVARIANT duration;
  PropVariantInit(&duration);
  hr = reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                       MF_PD_DURATION, &duration);
  const bool valid_duration = SUCCEEDED(hr) && duration.vt == VT_UI8 && duration.uhVal.QuadPart > 0 &&
      duration.uhVal.QuadPart <= static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max());
  if (valid_duration) media->duration_us = static_cast<LONGLONG>(duration.uhVal.QuadPart) / 10;
  const auto clear = PropVariantClear(&duration);
  if (FAILED(hr)) return hr;
  if (FAILED(clear)) return clear;
  if (!valid_duration || media->duration_us <= 0) return MF_E_INVALID_FILE_FORMAT;
  hr = reader->SetCurrentMediaType(0, nullptr, type.Get());
  if (FAILED(hr)) return hr;
  bool first = true, second = false;
  LONGLONG previous = 0;
  while (true) {
    ComPtr<IMFSample> sample;
    DWORD flags = 0, stream_index = 0;
    LONGLONG timestamp = 0;
    hr = reader->ReadSample(0, 0, &stream_index, &flags, &timestamp, &sample);
    if (FAILED(hr)) return hr;
    if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_NEWSTREAM |
                 MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
      return MF_E_INVALID_FILE_FORMAT;
    }
    // Native compressed packets only, no unbounded PCM buffer or full decode.
    // Reject a non-progressing response instead of spinning on absent samples.
    if (!sample && (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
    if (!sample || stream_index != 0) return MF_E_INVALID_FILE_FORMAT;
    DWORD bytes = 0;
    hr = sample->GetTotalLength(&bytes);
    if (FAILED(hr)) return hr;
    if (!bytes || (!first && timestamp <= previous)) return MF_E_INVALID_FILE_FORMAT;
    const auto us = timestamp / 10;  // Negative AAC priming PTS is legitimate.
    if (first) media->first_sample_us = us;
    else if (!second) { media->second_sample_us = us; second = true; }
    media->end_sample_us = us;
    previous = timestamp;
    first = false;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  if (!second || media->second_sample_us <= media->first_sample_us ||
      media->end_sample_us < media->second_sample_us) return MF_E_INVALID_FILE_FORMAT;
  return S_OK;
}
}  // namespace

struct MonitorAacInspector::Impl {
  explicit Impl(Factory value) : factory(std::move(value)), thread(GetCurrentThreadId()) {}
  Factory factory;
  const DWORD thread;
  ComPtr<ConfirmedMonitorByteStream> stream;
  ComPtr<IMFSourceReader> reader;
  MonitorAacMetadata media;
  bool attempted = false, closing = false;
  HRESULT status = MF_E_INVALIDREQUEST;
  bool OnThread() const noexcept { return GetCurrentThreadId() == thread; }
};

MonitorAacInspector::MonitorAacInspector(Factory factory)
    : impl_(std::make_unique<Impl>(std::move(factory))) {}
MonitorAacInspector::~MonitorAacInspector() = default;

HRESULT MonitorAacInspector::Inspect(IMFByteStream* owned_stream) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (self.attempted || self.closing) return MF_E_INVALIDREQUEST;
  self.attempted = true;
  if (!owned_stream) return self.status = E_INVALIDARG;
  try {
    self.stream = Make<ConfirmedMonitorByteStream>(owned_stream);
    if (!self.stream) return self.status = E_OUTOFMEMORY;
    QWORD bytes = 0;
    auto hr = self.stream->GetLength(&bytes);
    if (FAILED(hr)) return self.status = hr;
    if (!bytes || bytes > static_cast<QWORD>(std::numeric_limits<LONGLONG>::max())) {
      return self.status = MF_E_INVALID_FILE_FORMAT;
    }
    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    if (SUCCEEDED(hr)) hr = self.factory ? self.factory(self.stream.Get(), attributes.Get(), &self.reader) :
        MFCreateSourceReaderFromByteStream(self.stream.Get(), attributes.Get(), &self.reader);
    if (FAILED(hr)) return self.status = hr;
    if (!self.reader) return self.status = E_UNEXPECTED;
    hr = Scan(self.reader.Get(), &self.media);
    if (FAILED(hr)) return self.status = hr;
    QWORD after = 0;
    hr = self.stream->GetLength(&after);
    if (FAILED(hr)) return self.status = hr;
    if (after != bytes) return self.status = MF_E_INVALID_FILE_FORMAT;
    self.media.bytes = bytes;
    return self.status = S_OK;
  } catch (...) { return self.status = E_FAIL; }
}

MonitorAacInspectedClose MonitorAacInspector::Close() noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return {false, S_OK, RPC_E_WRONG_THREAD, std::nullopt};
  self.closing = true;
  try {
    // Synchronous reading has returned; release the parser before its stream.
    self.reader.Reset();
    const auto status = self.stream ? self.stream->Close() : S_OK;
    if (status != S_OK) return {false, self.status, status, std::nullopt};
    self.stream.Reset();
    return {true, self.status, S_OK, self.status == S_OK ? std::optional{self.media} : std::nullopt};
  } catch (...) { return {false, self.status, E_FAIL, std::nullopt}; }
}
}  // namespace nonet
