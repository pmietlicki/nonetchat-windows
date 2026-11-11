#ifndef NONET_WINDOWS_MONITOR_PREVIEW_PLAYER_H_
#define NONET_WINDOWS_MONITOR_PREVIEW_PLAYER_H_

#include <windows.h>
#include <mfidl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include "monitor_preview_video_surface.h"

namespace nonet {

struct MonitorPreviewState {
  bool ready = false, playing = false, completed = false, failed = false;
  std::int64_t position_us = 0, duration_us = 0;
};
enum class MonitorPreviewResource { player, source, stream, video_surface };

// Worker-thread local-file playback. No path/URL, microphone, global player,
// background transport controls or automatic resume. Keep until actual Close.
class MonitorPreviewPlayer final {
 public:
  // Nonblocking inert worker signal only; never call this player or an engine
  // from Wake. Close retires and drains every admitted signal before returning.
  using Wake = std::function<void()>;
  using CloseGate = std::function<HRESULT(MonitorPreviewResource)>;
  explicit MonitorPreviewPlayer(Wake wake, CloseGate close_gate = {});
  ~MonitorPreviewPlayer();
  MonitorPreviewPlayer(const MonitorPreviewPlayer&) = delete;
  MonitorPreviewPlayer& operator=(const MonitorPreviewPlayer&) = delete;
  static bool Supported() noexcept;
  static bool VideoSupported() noexcept;
  HRESULT Open(IMFByteStream* owned_file, bool video = false) noexcept;
  // S_FALSE means no new native frame event; it grants no image proof.
  HRESULT Frame(MonitorPreviewFrame*) noexcept;
  HRESULT Play() noexcept;
  HRESULT Pause() noexcept;
  HRESULT Seek(std::int64_t position_us) noexcept;
  HRESULT State(MonitorPreviewState* result) noexcept;
  // S_OK is the positive release proof. A failure retains the same resources;
  // another Close retries only the resources not yet positively closed.
  HRESULT Close() noexcept;
  bool closed() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nonet
#endif
