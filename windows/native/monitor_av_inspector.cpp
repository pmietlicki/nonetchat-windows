#include "monitor_av_inspector.h"

#include <bcrypt.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <propvarutil.h>
#include <strmif.h>

#include <array>
#include <limits>
#include <utility>

#include "monitor_av_index.h"
#include "monitor_recording_digest.h"
#include "monitor_media_stream.h"
namespace nonet {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::uint64_t kHns = 10000000;
constexpr DWORD kBadFlags = MF_SOURCE_READERF_ERROR |
                            MF_SOURCE_READERF_NEWSTREAM |
                            MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED;
HRESULT Seek(IMFSourceReader* reader, DWORD selected) {
  auto hr = reader->SetStreamSelection(
      static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(selected, TRUE);
  PROPVARIANT position;
  PropVariantInit(&position);
  position.vt = VT_I8;
  position.hVal.QuadPart = 0;
  if (SUCCEEDED(hr)) hr = reader->SetCurrentPosition(GUID_NULL, position);
  const auto clear = PropVariantClear(&position);
  return FAILED(hr) ? hr : clear;
}
HRESULT DecodedType(IMFSourceReader* reader, DWORD stream, bool video,
                    const av_inspection::Index& index) {
  ComPtr<IMFMediaType> type;
  auto hr = reader->GetCurrentMediaType(stream, &type);
  if (FAILED(hr) || !type) return FAILED(hr) ? hr : E_UNEXPECTED;
  GUID major{}, subtype{};
  hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major);
  if (SUCCEEDED(hr)) hr = type->GetGUID(MF_MT_SUBTYPE, &subtype);
  if (FAILED(hr)) return hr;
  if (video) {
    UINT32 width = 0, height = 0;
    hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    return SUCCEEDED(hr) && major == MFMediaType_Video &&
                   subtype == MFVideoFormat_NV12 && width == 640 &&
                   height == 480
               ? S_OK
               : MF_E_INVALIDMEDIATYPE;
  }
  UINT32 rate = 0, channels = 0, bits = 0, alignment = 0;
  hr = type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  if (SUCCEEDED(hr)) hr = type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
  if (SUCCEEDED(hr)) hr = type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
  if (SUCCEEDED(hr))
    hr = type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &alignment);
  return SUCCEEDED(hr) && major == MFMediaType_Audio &&
                 subtype == MFAudioFormat_PCM && rate == index.rate &&
                 channels == index.channels && bits == 16 &&
                 alignment == 2 * channels
             ? S_OK
             : MF_E_INVALIDMEDIATYPE;
}
HRESULT BoundVideoDecoder(IMFSourceReader* reader, DWORD stream) {
  ComPtr<IMFSourceReaderEx> extended;
  auto hr = reader->QueryInterface(IID_PPV_ARGS(&extended));
  if (FAILED(hr)) return hr;
  ComPtr<IMFTransform> transform;
  GUID category{};
  hr = extended->GetTransformForStream(stream, 0, &category, &transform);
  if (FAILED(hr) || category != MFT_CATEGORY_VIDEO_DECODER || !transform)
    return FAILED(hr) ? hr : E_UNEXPECTED;
  ComPtr<ICodecAPI> codec;
  hr = transform.As(&codec);
  if (FAILED(hr)) return hr;
  for (const auto& property : std::array<std::pair<GUID, LONG>, 2>{
           {{CODECAPI_AVDecVideoMaxCodedWidth, 640},
            {CODECAPI_AVDecVideoMaxCodedHeight, 480}}}) {
    // The installed Microsoft decoder reports VT_UI4; the Learn page says
    // VT_I4. Use only its actually reported numeric type, keep the same fixed
    // profile limit, and require an exact native readback before any decode.
    VARIANT value;
    VariantInit(&value);
    hr = codec->GetValue(&property.first, &value);
    if (hr != S_OK) {
      VariantClear(&value);
      return FAILED(hr) ? hr : E_UNEXPECTED;
    }
    const auto type = value.vt;
    if (type != VT_I4 && type != VT_UI4) {
      VariantClear(&value);
      return MF_E_INVALIDMEDIATYPE;
    }
    if (type == VT_I4)
      value.lVal = property.second;
    else
      value.ulVal = static_cast<ULONG>(property.second);
    hr = codec->SetValue(&property.first, &value);
    const auto input_clear = VariantClear(&value);
    if (hr != S_OK) return FAILED(hr) ? hr : E_UNEXPECTED;
    if (FAILED(input_clear)) return input_clear;
    VARIANT actual;
    VariantInit(&actual);
    hr = codec->GetValue(&property.first, &actual);
    const bool matches =
        hr == S_OK && actual.vt == type &&
        (type == VT_I4 ? actual.lVal == property.second
                       : actual.ulVal == static_cast<ULONG>(property.second));
    const auto clear = VariantClear(&actual);
    if (FAILED(hr)) return hr;
    if (FAILED(clear)) return clear;
    if (!matches) return E_UNEXPECTED;
  }
  return S_OK;
}
std::uint64_t Time(std::uint64_t ticks, std::uint32_t scale) {
  return ticks / scale * kHns + (ticks % scale) * kHns / scale;
}
bool Near(LONGLONG actual, std::uint64_t expected, std::uint32_t scale) {
  if (actual < 0) return false;
  const auto value = static_cast<std::uint64_t>(actual),
             difference =
                 value > expected ? value - expected : expected - value;
  // Original track quantum, as in the independent MF/ffprobe component oracle.
  return difference <= (kHns + scale - 1) / scale;
}
}  // namespace
struct MonitorAvInspector::Impl {
  Continue current;
  Factory factory;
  const DWORD thread = GetCurrentThreadId();
  ComPtr<ConfirmedMonitorByteStream> stream;
  ComPtr<IMFSourceReader> reader;
  MonitorAvMetadata metadata;
  av_inspection::Index index;
  std::array<ComPtr<IMFMediaType>, 2> native;
  DWORD audio = 0, video = 0;
  std::size_t audio_index = 0, video_index = 0;
  bool attempted = false, closing = false;
  HRESULT status = MF_E_INVALIDREQUEST;
  Impl(Continue c, Factory f) : current(std::move(c)), factory(std::move(f)) {}
  bool Eligible() noexcept {
    try {
      return GetCurrentThreadId() == thread && current && current();
    } catch (...) {
      return false;
    }
  }
  bool Current() noexcept { return !closing && Eligible(); }
  HRESULT Digest(MonitorRecordingFile& file, std::string& result) {
    return MonitorRecordingDigest(
        file, index.bytes, [this] { return Current(); }, result);
  }
  HRESULT Types() {
    std::array<bool, 2> seen{};
    for (DWORD i = 0; i < 2; ++i) {
      auto hr = reader->GetNativeMediaType(i, 0, &native[i]);
      if (FAILED(hr) || !native[i]) return FAILED(hr) ? hr : E_UNEXPECTED;
      GUID major{}, subtype{};
      hr = native[i]->GetGUID(MF_MT_MAJOR_TYPE, &major);
      if (SUCCEEDED(hr)) hr = native[i]->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (FAILED(hr)) return hr;
      if (major == MFMediaType_Audio && subtype == MFAudioFormat_AAC) {
        if (seen[0]) return MF_E_INVALIDMEDIATYPE;
        seen[0] = true;
        audio = i;
        UINT32 rate = 0, channels = 0;
        hr = native[i]->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        if (SUCCEEDED(hr))
          hr = native[i]->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        if (FAILED(hr) || rate != index.rate || channels != index.channels)
          return MF_E_INVALIDMEDIATYPE;
      } else if (major == MFMediaType_Video && subtype == MFVideoFormat_H264) {
        if (seen[1]) return MF_E_INVALIDMEDIATYPE;
        seen[1] = true;
        video = i;
        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(native[i].Get(), MF_MT_FRAME_SIZE, &width,
                                &height);
        if (FAILED(hr) || width != 640 || height != 480)
          return MF_E_INVALIDMEDIATYPE;
      } else
        return MF_E_INVALIDMEDIATYPE;
      hr = reader->SetCurrentMediaType(i, nullptr, native[i].Get());
      if (FAILED(hr)) return hr;
    }
    ComPtr<IMFMediaType> extra;
    if (reader->GetNativeMediaType(2, 0, &extra) != MF_E_INVALIDSTREAMNUMBER)
      return MF_E_INVALIDMEDIATYPE;
    if (!seen[0] || !seen[1]) return MF_E_INVALIDMEDIATYPE;
    video_index = index.init.tracks[0].video ? 0 : 1;
    audio_index = 1 - video_index;
    PROPVARIANT duration;
    PropVariantInit(&duration);
    auto hr = reader->GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
        &duration);
    const bool valid =
        SUCCEEDED(hr) && duration.vt == VT_UI8 && duration.uhVal.QuadPart > 0 &&
        duration.uhVal.QuadPart <= fmp4::AudioAdmission::max_seconds * kHns &&
        Near(static_cast<LONGLONG>(duration.uhVal.QuadPart),
             index.duration_us * 10, index.init.movie_scale);
    if (valid)
      metadata.audio.duration_us =
          static_cast<LONGLONG>(duration.uhVal.QuadPart / 10);
    const auto clear = PropVariantClear(&duration);
    if (FAILED(hr)) return hr;
    if (FAILED(clear)) return clear;
    return valid ? S_OK : MF_E_INVALID_FILE_FORMAT;
  }
  HRESULT Compressed(DWORD selected, std::size_t track_index) {
    auto hr = Seek(reader.Get(), selected);
    if (FAILED(hr)) return hr;
    std::uint64_t count = 0, calls = 0;
    LONGLONG first = 0, second = 0, last = 0, tick = -1;
    for (;;) {
      if (!Current()) return E_ABORT;
      if (++calls > index.bytes) return MF_E_INVALID_FILE_FORMAT;
      DWORD actual = 0, flags = 0;
      LONGLONG at = 0;
      ComPtr<IMFSample> sample;
      hr = reader->ReadSample(selected, 0, &actual, &flags, &at, &sample);
      if (FAILED(hr)) return hr;
      if (actual != selected ||
          (flags & (kBadFlags | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)))
        return MF_E_INVALID_FILE_FORMAT;
      if (!sample) {
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!(flags & MF_SOURCE_READERF_STREAMTICK) || at <= tick || at < 0 ||
            static_cast<std::uint64_t>(at) >
                fmp4::AudioAdmission::max_seconds * kHns)
          return MF_E_INVALID_FILE_FORMAT;
        tick = at;
        continue;
      }
      DWORD bytes = 0;
      hr = sample->GetTotalLength(&bytes);
      if (FAILED(hr)) return hr;
      if (!bytes || bytes > LocalPcmQueue::max_bytes || at < 0 ||
          (count && at <= last) || count >= index.tracks[track_index].samples)
        return MF_E_INVALID_FILE_FORMAT;
      if (count == 0)
        first = at;
      else if (count == 1)
        second = at;
      last = at;
      ++count;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (count != index.tracks[track_index].samples || count < 2 ||
        second <= first ||
        !Near(last,
              Time(index.tracks[track_index].last_pts,
                   index.init.tracks[track_index].scale),
              index.init.tracks[track_index].scale))
      return MF_E_INVALID_FILE_FORMAT;
    if (selected == audio) {
      metadata.audio.first_sample_us = first / 10;
      metadata.audio.second_sample_us = second / 10;
      metadata.audio.end_sample_us = last / 10;
    }
    return S_OK;
  }

  HRESULT Decode(bool is_video) {
    const DWORD selected = is_video ? video : audio;
    const auto track_index = is_video ? video_index : audio_index;
    ComPtr<IMFMediaType> output;
    auto hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr))
      hr = output->SetGUID(MF_MT_MAJOR_TYPE,
                           is_video ? MFMediaType_Video : MFMediaType_Audio);
    if (SUCCEEDED(hr))
      hr = output->SetGUID(MF_MT_SUBTYPE,
                           is_video ? MFVideoFormat_NV12 : MFAudioFormat_PCM);
    if (is_video) {
      if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, 640, 480);
    } else {
      if (SUCCEEDED(hr))
        hr = output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, index.rate);
      if (SUCCEEDED(hr))
        hr = output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, index.channels);
      if (SUCCEEDED(hr))
        hr = output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
      if (SUCCEEDED(hr))
        hr = output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * index.channels);
      if (SUCCEEDED(hr))
        hr = output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                               index.rate * 2 * index.channels);
    }
    if (SUCCEEDED(hr))
      hr = reader->SetCurrentMediaType(selected, nullptr, output.Get());
    if (SUCCEEDED(hr))
      hr = DecodedType(reader.Get(), selected, is_video, index);
    if (SUCCEEDED(hr) && is_video)
      hr = BoundVideoDecoder(reader.Get(), selected);
    if (SUCCEEDED(hr)) hr = Seek(reader.Get(), selected);
    if (FAILED(hr)) return hr;
    const auto access_units = index.tracks[track_index].samples;
    if (!is_video &&
        access_units > std::numeric_limits<std::uint64_t>::max() / 1024)
      return MF_E_INVALID_FILE_FORMAT;
    const std::uint64_t expected =
        is_video ? access_units : access_units * 1024;
    std::uint64_t frames = 0, count = 0, calls = 0;
    LONGLONG first = 0, second = 0, last = 0, tick = -1;
    for (;;) {
      if (!Current()) return E_ABORT;
      if (++calls > index.bytes) return MF_E_INVALID_FILE_FORMAT;
      DWORD actual = 0, flags = 0;
      LONGLONG at = 0;
      ComPtr<IMFSample> sample;
      hr = reader->ReadSample(selected, 0, &actual, &flags, &at, &sample);
      if (FAILED(hr)) return hr;
      if (actual != selected || (flags & kBadFlags))
        return MF_E_INVALID_FILE_FORMAT;
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        hr = DecodedType(reader.Get(), selected, is_video, index);
        if (FAILED(hr)) return hr;
      }
      if (!sample) {
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!(flags & MF_SOURCE_READERF_STREAMTICK) || at <= tick || at < 0 ||
            static_cast<std::uint64_t>(at) >
                fmp4::AudioAdmission::max_seconds * kHns)
          return MF_E_INVALID_FILE_FORMAT;
        tick = at;
        continue;
      }
      DWORD bytes = 0;
      hr = sample->GetTotalLength(&bytes);
      if (FAILED(hr)) return hr;
      if (!bytes || bytes > LocalPcmQueue::max_bytes || at < 0 ||
          (count && at <= last) ||
          static_cast<std::uint64_t>(at) >
              Time(index.tracks[track_index].presentation_end,
                   index.init.tracks[track_index].scale))
        return MF_E_INVALID_FILE_FORMAT;
      if (is_video ? bytes != 640 * 480 * 3 / 2
                   : bytes % (2 * index.channels) != 0)
        return MF_E_INVALID_FILE_FORMAT;
      const std::uint64_t added = is_video ? 1 : bytes / (2 * index.channels);
      if (frames > expected || added > expected - frames)
        return MF_E_INVALID_FILE_FORMAT;
      frames += added;
      if (count == 0)
        first = at;
      else if (count == 1)
        second = at;
      last = at;
      ++count;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (frames != expected || count < 2 || second <= first || last < second)
      return MF_E_INVALID_FILE_FORMAT;
    if (is_video) {
      if (!Near(last,
                Time(index.tracks[track_index].last_pts,
                     index.init.tracks[track_index].scale),
                index.init.tracks[track_index].scale))
        return MF_E_INVALID_FILE_FORMAT;
      metadata.video_frames = frames;
      metadata.first_frame_us = first / 10;
      metadata.second_frame_us = second / 10;
      metadata.end_frame_us = last / 10;
    } else
      metadata.decoded_audio_frames = frames;
    return S_OK;
  }
  HRESULT Run(MonitorRecordingFile& file) {
    if (!Current()) return E_ABORT;
    std::uint64_t bytes = 0;
    auto hr = file.ReadPinnedLength(&bytes);
    if (FAILED(hr)) return hr;
    HRESULT read_status = S_OK;
    const auto parsed = av_inspection::Inspect(
        bytes, [&](std::uint64_t at, std::uint8_t* data, std::size_t count) {
          if (!Current()) {
            read_status = E_ABORT;
            return false;
          }
          read_status = file.ReadPinnedAt(at, data, count);
          return SUCCEEDED(read_status);
        });
    if (!parsed)
      return FAILED(read_status) ? read_status : MF_E_INVALID_FILE_FORMAT;
    index = *parsed;
    metadata.audio.bytes = index.bytes;
    metadata.audio.rate = index.rate;
    metadata.audio.channels = index.channels;
    hr = Digest(file, metadata.sha256);
    if (FAILED(hr)) return hr;
    if (!file.stream()) return E_ACCESSDENIED;
    stream = Microsoft::WRL::Make<ConfirmedMonitorByteStream>(file.stream());
    if (!stream) return E_OUTOFMEMORY;
    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr))
      hr = attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    if (SUCCEEDED(hr))
      hr = factory ? factory(stream.Get(), attributes.Get(), &reader)
                   : MFCreateSourceReaderFromByteStream(
                         stream.Get(), attributes.Get(), &reader);
    if (FAILED(hr)) return hr;
    if (!reader) return E_UNEXPECTED;
    hr = Types();
    if (SUCCEEDED(hr)) hr = Compressed(audio, audio_index);
    if (SUCCEEDED(hr)) hr = Compressed(video, video_index);
    if (SUCCEEDED(hr)) hr = Decode(false);
    if (SUCCEEDED(hr)) hr = Decode(true);
    if (FAILED(hr)) return hr;
    std::string after;
    hr = Digest(file, after);
    if (FAILED(hr)) return hr;
    hr = file.ReadPinnedLength(&bytes);
    if (FAILED(hr)) return hr;
    if (bytes != index.bytes || after != metadata.sha256)
      return MF_E_INVALID_FILE_FORMAT;
    return Current() ? S_OK : E_ABORT;
  }
};
MonitorAvInspector::MonitorAvInspector(Continue current, Factory factory)
    : impl_(std::make_unique<Impl>(std::move(current), std::move(factory))) {}
MonitorAvInspector::~MonitorAvInspector() = default;
HRESULT MonitorAvInspector::Inspect(MonitorRecordingFile& file) noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return RPC_E_WRONG_THREAD;
  if (self.attempted || self.closing) return MF_E_INVALIDREQUEST;
  self.attempted = true;
  try {
    return self.status = self.Run(file);
  } catch (...) {
    return self.status = E_FAIL;
  }
}
MonitorAvInspectedClose MonitorAvInspector::Close() noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread)
    return {false, S_OK, RPC_E_WRONG_THREAD, std::nullopt};
  if (self.status == S_OK && !self.Eligible()) self.status = E_ABORT;
  self.closing = true;
  try {
    self.reader.Reset();
    for (auto& type : self.native) type.Reset();
    const auto hr = self.stream ? self.stream->Close() : S_OK;
    if (hr != S_OK) return {false, self.status, hr, std::nullopt};
    self.stream.Reset();
    return {true, self.status, S_OK,
            self.status == S_OK ? std::optional{self.metadata} : std::nullopt};
  } catch (...) {
    return {false, self.status, E_FAIL, std::nullopt};
  }
}
}  // namespace nonet
