#ifndef NONET_WINDOWS_MONITOR_PREVIEW_VIDEO_SURFACE_H_
#define NONET_WINDOWS_MONITOR_PREVIEW_VIDEO_SURFACE_H_
#include <windows.h>
#include <winrt/Windows.Media.Playback.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "monitor_preview_frame.h"
namespace nonet {
// One original worker's fixed local frame-server surface; no media source,
// capture, path, registrar or UI callback. Its staging read synchronizes the
// completed GPU copy before pixels leave the worker.
class MonitorPreviewVideoSurface final {
 public:
  MonitorPreviewVideoSurface();
  ~MonitorPreviewVideoSurface();
  MonitorPreviewVideoSurface(const MonitorPreviewVideoSurface&) = delete;
  MonitorPreviewVideoSurface& operator=(const MonitorPreviewVideoSurface&) =
      delete;
  HRESULT Open() noexcept;
  HRESULT Copy(const winrt::Windows::Media::Playback::MediaPlayer&,
               MonitorPreviewFrame*) noexcept;
  HRESULT Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
