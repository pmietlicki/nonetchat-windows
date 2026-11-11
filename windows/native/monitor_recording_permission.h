#ifndef NONET_WINDOWS_MONITOR_RECORDING_PERMISSION_H_
#define NONET_WINDOWS_MONITOR_RECORDING_PERMISSION_H_
#include <flutter_messenger.h>
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
namespace nonet {
struct WindowsRecordingDisclosure {
  std::string token, account, recording, kind;
  bool operator==(const WindowsRecordingDisclosure& other) const noexcept {
    return token == other.token && account == other.account &&
           recording == other.recording && kind == other.kind;
  }
};
// Same scoped contract as the existing Linux permission v2. Calls are on the
// original platform thread. This grants neither a source, a file nor capture.
// A voluntary recorder stop is distinct from purpose/account revocation.
class WindowsRecordingPermission final
    : public std::enable_shared_from_this<WindowsRecordingPermission> {
 public:
  using Done = std::function<void(bool)>;
  using Check = std::function<bool(const WindowsRecordingDisclosure&,
                                   bool first_admission)>;
  using Confirm = std::function<void(const WindowsRecordingDisclosure&, Done)>;
  WindowsRecordingPermission(Check current, Confirm confirm)
      : thread_(std::this_thread::get_id()),
        current_(std::move(current)),
        confirm_(std::move(confirm)) {}
  std::function<void()> on_changed;
  bool OnPlatformThread() const noexcept { return OnThread(); }
  bool ClaimEngine() noexcept {
    if (!OnThread() || !live_ || engine_claimed_) return false;
    engine_claimed_ = true;
    return true;
  }
  bool HoldsEngine() const noexcept {
    return !OnThread() || (engine_claimed_ && !engine_drained_);
  }
  // Called only after this engine's recorder AND SDK media have drained.
  // Removing a registry/handler is deliberately not this proof.
  std::function<void()> on_engine_drained;
  bool Current(const WindowsRecordingDisclosure& value,
               bool first_admission = false) noexcept {
    if (!OnThread() || !live_ || !current_) return false;
    try {
      return current_(value, first_admission) && live_;
    } catch (...) {
      return false;
    }
  }
  void ConfirmCurrent(const WindowsRecordingDisclosure& value,
                      Done done) noexcept {
    if (!Current(value, true) || !confirm_) {
      Complete(done, false);
      return;
    }
    try {
      const auto self = shared_from_this();
      const auto pending = std::make_shared<Done>(std::move(done));
      auto finish = [self, pending, value](bool allowed) {
        auto callback = std::move(*pending);
        *pending = {};
        if (!callback) return;
        Complete(callback, allowed && self->Current(value, true));
      };
      try {
        confirm_(value, finish);
      } catch (...) {
        finish(false);
      }
    } catch (...) {
      Complete(done, false);
    }
  }
  void Changed() noexcept {
    if (!OnThread()) return;
    try {
      const auto callback = on_changed;
      if (callback) callback();
    } catch (...) {
    }
  }
  void Retire() noexcept {
    if (!OnThread()) return;
    live_ = false;
    Changed();
  }
  void EngineDrained() noexcept {
    if (!OnThread() || engine_drained_) return;
    engine_drained_ = true;
    Retire();
    try {
      const auto callback = on_engine_drained;
      if (callback) callback();
    } catch (...) {
    }
  }

 private:
  bool OnThread() const noexcept {
    return std::this_thread::get_id() == thread_;
  }
  static void Complete(const Done& done, bool value) noexcept {
    try {
      if (done) done(value);
    } catch (...) {
    }
  }
  const std::thread::id thread_;
  Check current_;
  Confirm confirm_;
  bool live_ = true, engine_claimed_ = false, engine_drained_ = false;
};

// Private ABI within the same executable/plugin build, never the SDK DLL or
// a wire packet. The executable resolves only its exact core-messenger scope.
// Both modules use the same MSVC runtime/configuration and this same header.
// No C++ object is returned by value through the C entry point.
using WindowsRecordingPermissionLookup = void (*)(FlutterDesktopMessengerRef,
                                                  void*, std::size_t);
inline std::shared_ptr<WindowsRecordingPermission>
WindowsRecordingPermissionFor(FlutterDesktopMessengerRef core) noexcept {
  std::shared_ptr<WindowsRecordingPermission> result;
  if (!core) return result;
  const auto module = GetModuleHandleW(nullptr);
  const auto lookup =
      module
          ? reinterpret_cast<WindowsRecordingPermissionLookup>(
                GetProcAddress(module, "nonet_windows_recording_permission_v1"))
          : nullptr;
  if (!lookup) return result;
  try {
    lookup(core, &result, sizeof(result));
  } catch (...) {
    result.reset();
  }
  return result;
}
// Implemented in the executable's indicator module, not an inline process map
// duplicated inside a plugin DLL. Replacing an endpoint retires the old one.
void BindWindowsRecordingPermission(
    FlutterDesktopMessengerRef, std::shared_ptr<WindowsRecordingPermission>);
void UnbindWindowsRecordingPermission(
    FlutterDesktopMessengerRef,
    const std::shared_ptr<WindowsRecordingPermission>&);
}  // namespace nonet
#endif
