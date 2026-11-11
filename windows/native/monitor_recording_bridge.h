#ifndef NONET_WINDOWS_MONITOR_RECORDING_BRIDGE_H_
#define NONET_WINDOWS_MONITOR_RECORDING_BRIDGE_H_

#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_result.h>
#include <flutter/texture_registrar.h>

#include <functional>
#include <memory>
#include <string>

#include "rtc_peerconnection.h"

namespace nonet {

// One FlutterWebRTC instance's selected-receiver recorder. The resolver must
// borrow only from that instance, never FlutterWebRTCPluginSharedInstance().
// Methods and retirement run on the engine UI thread; media/IO use a worker.
class MonitorRecordingBridge final {
 public:
  using PeerForId =
      std::function<libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection>(
          const std::string&)>;
  using SupportDirectory = std::function<std::wstring()>;
  MonitorRecordingBridge(PeerForId resolver, bool software_ready = false,
                         SupportDirectory support = {},
                         flutter::TextureRegistrar* textures = nullptr);
  ~MonitorRecordingBridge();
  MonitorRecordingBridge(const MonitorRecordingBridge&) = delete;
  MonitorRecordingBridge& operator=(const MonitorRecordingBridge&) = delete;

  // False leaves result untouched for the ordinary SDK handler.
  bool Handle(const flutter::MethodCall<flutter::EncodableValue>& call,
              std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>&
                  result) noexcept;
  void Retire() noexcept;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace nonet
#endif
