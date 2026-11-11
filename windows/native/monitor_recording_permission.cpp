#include "monitor_recording_permission.h"

#include <map>
#include <mutex>
#include <utility>
namespace nonet {
namespace {
std::mutex permission_mutex;
std::map<FlutterDesktopMessengerRef, std::weak_ptr<WindowsRecordingPermission>>
    permissions;
}  // namespace
void BindWindowsRecordingPermission(
    FlutterDesktopMessengerRef core,
    std::shared_ptr<WindowsRecordingPermission> value) {
  if (!core || !value || !value->OnPlatformThread()) return;
  std::shared_ptr<WindowsRecordingPermission> previous;
  {
    std::lock_guard<std::mutex> held(permission_mutex);
    const auto found = permissions.find(core);
    if (found != permissions.end()) previous = found->second.lock();
    if (previous == value || (previous && !previous->OnPlatformThread()))
      return;
    permissions[core] = value;
  }
  if (previous) previous->Retire();
}
void UnbindWindowsRecordingPermission(
    FlutterDesktopMessengerRef core,
    const std::shared_ptr<WindowsRecordingPermission>& value) {
  if (!core || !value || !value->OnPlatformThread()) return;
  bool removed = false;
  {
    std::lock_guard<std::mutex> held(permission_mutex);
    const auto found = permissions.find(core);
    if (found != permissions.end() && found->second.lock() == value) {
      permissions.erase(found);
      removed = true;
    }
  }
  if (removed) value->Retire();
}
void ReadWindowsRecordingPermission(FlutterDesktopMessengerRef core,
                                    void* output, std::size_t size) {
  if (!output || size != sizeof(std::shared_ptr<WindowsRecordingPermission>))
    return;
  auto& result =
      *static_cast<std::shared_ptr<WindowsRecordingPermission>*>(output);
  result.reset();
  if (!core) return;
  std::lock_guard<std::mutex> held(permission_mutex);
  const auto found = permissions.find(core);
  if (found != permissions.end()) result = found->second.lock();
}
}  // namespace nonet
extern "C" __declspec(dllexport) void nonet_windows_recording_permission_v1(
    FlutterDesktopMessengerRef core, void* output, std::size_t size) noexcept {
  try {
    nonet::ReadWindowsRecordingPermission(core, output, size);
  } catch (...) {
  }
}
