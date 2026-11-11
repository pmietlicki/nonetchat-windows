#include "webrtc_runtime_owner.h"

#include <cstdint>
#include <mutex>
#include <utility>
namespace nonet {
namespace {
// Same serialized factory/SSL lifecycle as the existing Linux runtime owner.
// This process-global state grants no peer/account/application permission.
std::mutex runtime_mutex;
std::uint64_t runtime_leases = 0;
}  // namespace
WindowsWebRtcRuntimeOwner::WindowsWebRtcRuntimeOwner(
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory)
    : factory_(std::move(factory)) {}
std::shared_ptr<WindowsWebRtcRuntimeOwner> WindowsWebRtcRuntimeOwner::Acquire(
    const libwebrtc::vector<libwebrtc::string>& trials) {
  // Allocate the control block before touching SDK/global state. An allocation
  // failure cannot destroy an uncounted owner and decrement another engine.
  auto owner = std::shared_ptr<WindowsWebRtcRuntimeOwner>(
      new WindowsWebRtcRuntimeOwner({}));
  std::lock_guard<std::mutex> held(runtime_mutex);
  libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory;
  try {
    libwebrtc::LibWebRTC::InitializeWithFieldTrials(trials);
    factory = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
    if (!factory || !factory->Initialize()) throw WindowsWebRtcUnavailable();
    owner->factory_ = factory;
    ++runtime_leases;
    owner->registered_ = true;
    return owner;
  } catch (...) {
    factory = nullptr;
    if (!runtime_leases) libwebrtc::LibWebRTC::Terminate();
    throw;
  }
}
WindowsWebRtcRuntimeOwner::~WindowsWebRtcRuntimeOwner() {
  if (!registered_) return;
  // Drop the original factory before the last process-wide SSL release. The
  // registry/loans arrange their SDK member order so no source outlives this.
  if (factory_) {
    factory_->Terminate();
    factory_ = nullptr;
  }
  std::lock_guard<std::mutex> held(runtime_mutex);
  if (runtime_leases && --runtime_leases == 0)
    libwebrtc::LibWebRTC::Terminate();
}
}  // namespace nonet
