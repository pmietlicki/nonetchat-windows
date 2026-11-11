#include "monitor_preview_video_surface.h"
#include "local_video_queue.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl.h>
#include <mferror.h>
namespace nonet {
namespace {
constexpr UINT width = 640, height = 480;
static_assert(width * height * 4 <= LocalPcmQueue::max_bytes,
              "same decoded video byte ceiling");
HRESULT Failure() noexcept {
  try {
    throw;
  } catch (const winrt::hresult_error& e) {
    return e.code();
  } catch (...) {
    return E_FAIL;
  }
}
}  // namespace
struct MonitorPreviewVideoSurface::Impl {
  const DWORD thread = GetCurrentThreadId();
  bool attempted = false, closed = false;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> target, staging;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface{
      nullptr};
};
MonitorPreviewVideoSurface::MonitorPreviewVideoSurface()
    : impl_(std::make_unique<Impl>()) {}
MonitorPreviewVideoSurface::~MonitorPreviewVideoSurface() = default;
HRESULT MonitorPreviewVideoSurface::Open() noexcept {
  auto& s = *impl_;
  if (GetCurrentThreadId() != s.thread) return RPC_E_WRONG_THREAD;
  if (s.attempted || s.closed) return MF_E_INVALIDREQUEST;
  s.attempted = true;
  try {
    auto hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, &s.device, nullptr, &s.context);
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = s.device->CreateTexture2D(&d, nullptr, &s.target);
    if (FAILED(hr)) return hr;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = s.device->CreateTexture2D(&d, nullptr, &s.staging);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IDXGISurface> dxgi;
    hr = s.target.As(&dxgi);
    if (FAILED(hr)) return hr;
    winrt::com_ptr<IInspectable> inspectable;
    hr = CreateDirect3D11SurfaceFromDXGISurface(dxgi.Get(), inspectable.put());
    if (FAILED(hr)) return hr;
    s.surface = inspectable.as<
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>();
    return S_OK;
  } catch (...) {
    return Failure();
  }
}
HRESULT MonitorPreviewVideoSurface::Copy(
    const winrt::Windows::Media::Playback::MediaPlayer& player,
    MonitorPreviewFrame* result) noexcept {
  auto& s = *impl_;
  if (GetCurrentThreadId() != s.thread) return RPC_E_WRONG_THREAD;
  if (!result) return E_POINTER;
  if (s.closed || !s.surface || !player) return MF_E_INVALIDREQUEST;
  try {
    const auto session = player.PlaybackSession();
    if (session.NaturalVideoWidth() != width ||
        session.NaturalVideoHeight() != height)
      return MF_E_INVALIDMEDIATYPE;
    player.CopyFrameToVideoSurface(s.surface);
    s.context->CopyResource(s.staging.Get(), s.target.Get());
    D3D11_MAPPED_SUBRESOURCE view{};
    const auto hr =
        s.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &view);
    if (FAILED(hr)) return hr;
    struct Mapping {
      ID3D11DeviceContext* context;
      ID3D11Texture2D* texture;
      ~Mapping() { context->Unmap(texture, 0); }
    } mapping{s.context.Get(), s.staging.Get()};
    if (!view.pData || view.RowPitch < width * 4 ||
        view.RowPitch > SIZE_MAX / height)
      return E_UNEXPECTED;
    MonitorPreviewFrame owned;
    owned.width = width;
    owned.height = height;
    owned.rgba.resize(width * height * 4);
    const auto* base = static_cast<const std::uint8_t*>(view.pData);
    for (UINT row = 0; row < height; ++row) {
      const auto* input = base + static_cast<std::size_t>(row) * view.RowPitch;
      auto* output =
          owned.rgba.data() + static_cast<std::size_t>(row) * width * 4;
      for (UINT x = 0; x < width; ++x) {
        output[4 * x] = input[4 * x + 2];
        output[4 * x + 1] = input[4 * x + 1];
        output[4 * x + 2] = input[4 * x];
        output[4 * x + 3] = input[4 * x + 3];
      }
    }
    *result = std::move(owned);
    return S_OK;
  } catch (...) {
    return Failure();
  }
}
HRESULT MonitorPreviewVideoSurface::Close() noexcept {
  auto& s = *impl_;
  if (GetCurrentThreadId() != s.thread) return RPC_E_WRONG_THREAD;
  // Every Copy and its synchronous staging Map/Unmap has returned on this
  // worker before the player owner calls Close. No pixel borrow points here.
  s.surface = nullptr;
  s.staging.Reset();
  s.target.Reset();
  s.context.Reset();
  s.device.Reset();
  s.closed = true;
  return S_OK;
}
}  // namespace nonet
