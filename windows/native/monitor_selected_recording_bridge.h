#ifndef NONET_WINDOWS_MONITOR_SELECTED_RECORDING_BRIDGE_H_
#define NONET_WINDOWS_MONITOR_SELECTED_RECORDING_BRIDGE_H_
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_result.h>

#include "monitor_received_registry.h"
#include "monitor_recording_permission.h"
namespace nonet {
// Local API5, separate from protocol v4. Intercepts the recording methods only;
// the existing pinned inspection/audio preview bridge keeps its own ownership.
class MonitorSelectedRecordingBridge final {
 public:
  using SupportDirectory = std::function<std::wstring()>;
  MonitorSelectedRecordingBridge(std::shared_ptr<MonitorReceivedRegistry>,
                                 FlutterDesktopMessengerRef, SupportDirectory,
                                 bool software_ready);
  ~MonitorSelectedRecordingBridge();
  bool Handle(const flutter::MethodCall<flutter::EncodableValue>&,
              std::unique_ptr<
                  flutter::MethodResult<flutter::EncodableValue>>&) noexcept;
  void Retire() noexcept;
  // Original plugin calls this AFTER closing/releasing its Flutter SDK graph.
  // This alone is not recorder drain; the native window survives until both.
  void GraphClosed() noexcept;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace nonet
#endif
