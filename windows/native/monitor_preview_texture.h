#ifndef NONET_WINDOWS_MONITOR_PREVIEW_TEXTURE_H_
#define NONET_WINDOWS_MONITOR_PREVIEW_TEXTURE_H_
#include <windows.h>
#include <flutter/texture_registrar.h>
#include <functional>
#include <memory>
#include "monitor_preview_frame.h"
namespace nonet {
struct MonitorPreviewTextureState {
  std::int64_t id = -1;
  bool registered = false, image_ready = false, closing = false, closed = false,
       failed = false;
  std::size_t bytes = 0, borrows = 0;
};
// Exactly one original engine registrar. Pixel publication never calls the
// engine from a media worker: Update, Register, ContinueClose and EngineRetired
// belong to the original UI thread. Wake is an inert nonblocking signal only.
// The owner retains this capability until unregister AND every raster borrow
// positively drain. Engine retirement alone grants no closure proof.
class MonitorPreviewTexture final {
 public:
  using Wake = std::function<void()>;
  MonitorPreviewTexture(flutter::TextureRegistrar*, Wake);
  ~MonitorPreviewTexture();
  MonitorPreviewTexture(const MonitorPreviewTexture&) = delete;
  MonitorPreviewTexture& operator=(const MonitorPreviewTexture&) = delete;
  HRESULT Register() noexcept;
  HRESULT Publish(MonitorPreviewFrame&&) noexcept;
  HRESULT Update() noexcept;
  void Retire() noexcept;
  HRESULT ContinueClose() noexcept;
  HRESULT EngineRetired() noexcept;
  MonitorPreviewTextureState State() const noexcept;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
