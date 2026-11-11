#include "monitor_video_normalizer.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl.h>

#include <algorithm>
#include <limits>

namespace nonet {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD kCanvasBytes = 640 * 480 * 3 / 2;
HRESULT Type(GUID subtype, UINT32 width, UINT32 height, IMFMediaType** result) {
  ComPtr<IMFMediaType> type;
  HRESULT hr = MFCreateMediaType(&type);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
  if (SUCCEEDED(hr))
    hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
  if (SUCCEEDED(hr))
    hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(hr))
    hr = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  if (SUCCEEDED(hr))
    hr = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
  if (SUCCEEDED(hr)) *result = type.Detach();
  return hr;
}
void Rotate(const std::uint8_t* source, std::size_t width, std::size_t height,
            int rotation, std::uint8_t* destination, std::size_t stride) {
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      std::size_t dx = x, dy = y;
      switch (rotation) {
        case 90:
          dx = height - 1 - y;
          dy = x;
          break;
        case 180:
          dx = width - 1 - x;
          dy = height - 1 - y;
          break;
        case 270:
          dx = y;
          dy = width - 1 - x;
          break;
        default:
          break;
      }
      destination[dy * stride + dx] = source[y * width + x];
    }
  }
}
HRESULT Sample(DWORD bytes, IMFSample** out, IMFMediaBuffer** buffer) {
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> memory;
  HRESULT hr = MFCreateSample(&sample);
  if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(bytes, &memory);
  if (SUCCEEDED(hr)) hr = sample->AddBuffer(memory.Get());
  if (SUCCEEDED(hr)) {
    *out = sample.Detach();
    *buffer = memory.Detach();
  }
  return hr;
}
}  // namespace
struct MonitorVideoNormalizer::Impl {
  const DWORD thread = GetCurrentThreadId();
  ComPtr<IMFTransform> transform;
  int width = 0, height = 0;
  HRESULT error = S_OK;
  bool closed = false;
  HRESULT drain_failure = S_OK;
  HRESULT Fail(HRESULT hr) noexcept {
    if (SUCCEEDED(error) && FAILED(hr)) error = hr;
    return hr;
  }
  HRESULT Empty() {
    if (!transform) return S_OK;
    MFT_OUTPUT_DATA_BUFFER output{};
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = Sample(kCanvasBytes, &sample, &buffer);
    if (FAILED(hr)) return hr;
    output.pSample = sample.Get();
    DWORD status = 0;
    hr = transform->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    return hr == MF_E_TRANSFORM_NEED_MORE_INPUT ? S_OK
           : FAILED(hr)                         ? hr
                                                : E_UNEXPECTED;
  }
  HRESULT Drain() {
    if (FAILED(drain_failure)) return drain_failure;
    if (!transform) return S_OK;
    HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (SUCCEEDED(hr))
      hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (SUCCEEDED(hr)) hr = Empty();
    if (SUCCEEDED(hr))
      hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (SUCCEEDED(hr))
      transform.Reset();
    else
      drain_failure =
          hr;  // An uncertain original drain never becomes a later no-op proof.
    return hr;
  }
  HRESULT Configure(int w, int h, RECT rectangle) {
    HRESULT hr = Drain();
    if (SUCCEEDED(hr))
      hr = CoCreateInstance(CLSID_VideoProcessorMFT, nullptr,
                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(hr)) hr = transform->GetAttributes(&attributes);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_XVP_DISABLE_FRC, TRUE);
    if (SUCCEEDED(hr))
      hr = attributes->SetUINT32(MF_XVP_CALLER_ALLOCATES_OUTPUT, TRUE);
    ComPtr<IMFMediaType> input, output;
    if (SUCCEEDED(hr))
      hr = Type(MFVideoFormat_IYUV, static_cast<UINT32>(w),
                static_cast<UINT32>(h), &input);
    if (SUCCEEDED(hr)) hr = Type(MFVideoFormat_NV12, 640, 480, &output);
    if (SUCCEEDED(hr)) hr = transform->SetInputType(0, input.Get(), 0);
    if (SUCCEEDED(hr)) hr = transform->SetOutputType(0, output.Get(), 0);
    ComPtr<IMFVideoProcessorControl> control;
    if (SUCCEEDED(hr)) hr = transform.As(&control);
    if (SUCCEEDED(hr)) hr = control->SetDestinationRectangle(&rectangle);
    MFARGB black{};
    black.rgbAlpha = 255;
    if (SUCCEEDED(hr)) hr = control->SetBorderColor(&black);
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(hr)) hr = transform->GetOutputStreamInfo(0, &info);
    if (SUCCEEDED(hr) && (info.cbSize > kCanvasBytes ||
                          (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)))
      hr = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED(hr))
      hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr))
      hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (SUCCEEDED(hr)) {
      width = w;
      height = h;
    }
    return hr;
  }
};
MonitorVideoNormalizer::MonitorVideoNormalizer()
    : impl_(std::make_unique<Impl>()) {}
MonitorVideoNormalizer::~MonitorVideoNormalizer() = default;
HRESULT MonitorVideoNormalizer::failure() const noexcept {
  return impl_->error;
}
HRESULT MonitorVideoNormalizer::Close() noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return RPC_E_WRONG_THREAD;
  self.closed = true;
  try {
    return self.Fail(self.Drain());
  } catch (...) {
    return self.Fail(E_FAIL);
  }
}
std::optional<NormalizedMonitorVideo> MonitorVideoNormalizer::Convert(
    const LocalVideoBlock& block, std::uint64_t duration_hns) {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return std::nullopt;
  if (FAILED(self.error) || self.closed) return std::nullopt;
  auto fail = [&](HRESULT hr) -> std::optional<NormalizedMonitorVideo> {
    self.Fail(hr);
    return std::nullopt;
  };
  if (!block.valid() || !duration_hns ||
      duration_hns > static_cast<std::uint64_t>(INT64_MAX) ||
      block.local_ns / 100 >
          INT64_MAX - static_cast<std::int64_t>(duration_hns))
    return fail(E_INVALIDARG);
  try {
    const bool sideways = block.rotation == 90 || block.rotation == 270;
    const int actual_w = sideways ? block.height : block.width;
    const int actual_h = sideways ? block.width : block.height;
    // IYUV's even canvas pads an odd visible edge; it never reads a missing
    // row.
    const int w = actual_w + actual_w % 2, h = actual_h + actual_h % 2;
    const auto layout = LocalI420Layout::Create(w, h);
    if (!layout) return fail(E_INVALIDARG);
    int picture_w = 640, picture_h = 480;
    if (static_cast<std::int64_t>(w) * 480 > static_cast<std::int64_t>(h) * 640)
      picture_h = static_cast<int>(static_cast<std::int64_t>(h) * 640 / w);
    else
      picture_w = static_cast<int>(static_cast<std::int64_t>(w) * 480 / h);
    picture_w = std::max(2, picture_w & ~1);
    picture_h = std::max(2, picture_h & ~1);
    const int x = ((640 - picture_w) / 2) & ~1,
              y = ((480 - picture_h) / 2) & ~1;
    if (!self.transform || self.width != w || self.height != h) {
      const auto hr =
          self.Configure(w, h, RECT{x, y, x + picture_w, y + picture_h});
      if (FAILED(hr)) return fail(hr);
    }
    ComPtr<IMFSample> input, output;
    ComPtr<IMFMediaBuffer> in_buffer, out_buffer;
    HRESULT hr =
        Sample(static_cast<DWORD>(layout->bytes()), &input, &in_buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = in_buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      std::memset(data, 16, layout->y_bytes);
      std::memset(data + layout->y_bytes, 128, 2 * layout->chroma_bytes);
      const auto original = LocalI420Layout::Create(block.width, block.height);
      Rotate(block.bytes.data(), original->width, original->height,
             block.rotation, data, layout->width);
      Rotate(block.bytes.data() + block.y_bytes, original->chroma_width,
             original->chroma_height, block.rotation, data + layout->y_bytes,
             layout->chroma_width);
      Rotate(block.bytes.data() + block.y_bytes + block.chroma_bytes,
             original->chroma_width, original->chroma_height, block.rotation,
             data + layout->y_bytes + layout->chroma_bytes,
             layout->chroma_width);
      hr = in_buffer->Unlock();
    }
    if (SUCCEEDED(hr))
      hr = in_buffer->SetCurrentLength(static_cast<DWORD>(layout->bytes()));
    if (SUCCEEDED(hr)) hr = input->SetSampleTime(block.local_ns / 100);
    if (SUCCEEDED(hr))
      hr = input->SetSampleDuration(static_cast<LONGLONG>(duration_hns));
    if (SUCCEEDED(hr)) hr = Sample(kCanvasBytes, &output, &out_buffer);
    if (SUCCEEDED(hr)) hr = self.transform->ProcessInput(0, input.Get(), 0);
    MFT_OUTPUT_DATA_BUFFER result{};
    result.pSample = output.Get();
    DWORD status = 0;
    if (SUCCEEDED(hr))
      hr = self.transform->ProcessOutput(0, 1, &result, &status);
    if (result.pEvents) result.pEvents->Release();
    if (FAILED(hr)) return fail(hr);
    DWORD size = 0;
    LONGLONG at = 0, duration = 0;
    hr = out_buffer->GetCurrentLength(&size);
    if (SUCCEEDED(hr)) hr = output->GetSampleTime(&at);
    if (SUCCEEDED(hr)) hr = output->GetSampleDuration(&duration);
    if (SUCCEEDED(hr) && (size != kCanvasBytes || at != block.local_ns / 100 ||
                          duration != static_cast<LONGLONG>(duration_hns)))
      hr = E_UNEXPECTED;
    if (FAILED(hr)) return fail(hr);
    NormalizedMonitorVideo value{x,
                                 y,
                                 picture_w,
                                 picture_h,
                                 block.local_ns,
                                 duration_hns,
                                 std::vector<std::uint8_t>(kCanvasBytes)};
    hr = out_buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      // The software XVP leaves its border at zero on the actual native host,
      // despite SetBorderColor. Own only the scaled picture; explicitly fill
      // limited-range black around it, as the existing Linux normalizer does.
      std::fill(value.nv12.begin(), value.nv12.begin() + 640 * 480,
                std::uint8_t(16));
      std::fill(value.nv12.begin() + 640 * 480, value.nv12.end(),
                std::uint8_t(128));
      for (int row = y; row < y + picture_h; ++row) {
        const auto offset = static_cast<std::size_t>(row) * 640 + x;
        std::memcpy(value.nv12.data() + offset, data + offset, picture_w);
      }
      for (int row = y / 2; row < (y + picture_h) / 2; ++row) {
        const auto offset = 640 * 480 + static_cast<std::size_t>(row) * 640 + x;
        std::memcpy(value.nv12.data() + offset, data + offset, picture_w);
      }
      hr = out_buffer->Unlock();
    }
    if (SUCCEEDED(hr)) hr = self.Empty();
    if (FAILED(hr)) return fail(hr);
    return value;
  } catch (...) {
    return fail(E_FAIL);
  }
}
}  // namespace nonet
