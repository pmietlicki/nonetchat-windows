#ifndef NONET_WINDOWS_WEBRTC_RUNTIME_OWNER_H_
#define NONET_WINDOWS_WEBRTC_RUNTIME_OWNER_H_
#include <memory>
#include <stdexcept>

#include "libwebrtc.h"
namespace nonet {
class WindowsWebRtcUnavailable final : public std::runtime_error {
 public:
  WindowsWebRtcUnavailable()
      : std::runtime_error("Native WebRTC audio unavailable") {}
};
// Native-only factory/runtime owner. The UI graph releases its copy after
// retiring presentation and observers; original selected source loans retain
// theirs through sink/callback/worker drain. No Flutter/TaskRunner is retained.
// Creation and last release belong to the original native platform thread.
class WindowsWebRtcRuntimeOwner final {
 public:
  static std::shared_ptr<WindowsWebRtcRuntimeOwner> Acquire(
      const libwebrtc::vector<libwebrtc::string>& trials);
  ~WindowsWebRtcRuntimeOwner();
  const libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory>& factory()
      const noexcept {
    return factory_;
  }
  WindowsWebRtcRuntimeOwner(const WindowsWebRtcRuntimeOwner&) = delete;
  WindowsWebRtcRuntimeOwner& operator=(const WindowsWebRtcRuntimeOwner&) =
      delete;

 private:
  explicit WindowsWebRtcRuntimeOwner(
      libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory>);
  libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory_;
  bool registered_ = false;
};
}  // namespace nonet
#endif
