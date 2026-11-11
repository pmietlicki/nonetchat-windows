#ifndef NONET_LOCAL_MEDIA_INDICATOR_H_
#define NONET_LOCAL_MEDIA_INDICATOR_H_

#include <flutter_messenger.h>
#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace flutter {
class BinaryMessenger;
}

namespace nonet {

// Only localized application copy reaches the Shell; account/session/nonces
// stay inside the scoped owner. The 16-bit ID is a local callback generation.
struct LocalMediaIndicatorView {
  std::uint16_t id = 0;
  std::u16string title, body, stop, stopping;
  bool right_to_left = false;
};

struct LocalMediaIndicatorEvent {
  enum class Kind { kHandled, kOpen, kStop, kLost };
  Kind kind;
  std::uint16_t id;
};

// Injected OS boundary. No implementation may acquire media, grant permission,
// restart a purpose or clear another owner's state. Tests use the same bridge
// and codec with a fake Shell; the production implementation remains Win32.
class LocalMediaIndicatorPlatform {
 public:
  virtual ~LocalMediaIndicatorPlatform() = default;
  virtual std::uint64_t Now() const = 0;
  virtual bool Foreground() const = 0;
  virtual bool OwnsWindow(HWND window) const noexcept = 0;
  virtual bool Present(const LocalMediaIndicatorView& view) = 0;
  virtual bool Update(const LocalMediaIndicatorView& view) = 0;
  virtual bool Exists(std::uint16_t id) const = 0;
  virtual bool Remove(std::uint16_t id) = 0;
  virtual void OpenApplication() = 0;
  virtual void CancelMenu() noexcept = 0;
  virtual bool PostClose() noexcept = 0;
  virtual std::optional<LocalMediaIndicatorEvent> HandleMessage(
      HWND window, UINT message, WPARAM wparam, LPARAM lparam,
      const LocalMediaIndicatorView& view) = 0;
};

std::unique_ptr<LocalMediaIndicatorPlatform> MakeWindowsLocalMediaPlatform(
    HWND window, int icon_resource) noexcept;

// One bridge per Flutter engine/window. Neither a notification nor a received
// control can start capture. Window close first asks the existing Dart owner to
// drain media; only its confirmed release allows the deferred WM_CLOSE through.
class LocalMediaIndicator final {
 public:
  static std::unique_ptr<LocalMediaIndicator> Create(
      flutter::BinaryMessenger* messenger,
      std::unique_ptr<LocalMediaIndicatorPlatform> platform,
      FlutterDesktopMessengerRef core = nullptr) noexcept;
  ~LocalMediaIndicator();
  LocalMediaIndicator(const LocalMediaIndicator&) = delete;
  LocalMediaIndicator& operator=(const LocalMediaIndicator&) = delete;

  std::optional<LRESULT> HandleWindowMessage(HWND window, UINT message,
                                             WPARAM wparam,
                                             LPARAM lparam) noexcept;
  void DetachBeforeEngineClose() noexcept;
  void ReleaseAfterEngineClose() noexcept;

 private:
  struct Impl;
  explicit LocalMediaIndicator(std::shared_ptr<Impl> impl) noexcept;
  std::shared_ptr<Impl> impl_;
};

}  // namespace nonet
#endif  // NONET_LOCAL_MEDIA_INDICATOR_H_
