#ifndef NONET_ENGINE_LIFETIME_MESSENGER_H_
#define NONET_ENGINE_LIFETIME_MESSENGER_H_

#include <flutter/binary_messenger.h>
#include <flutter_messenger.h>

#include <memory>
#include <utility>

namespace nonet {

// Flutter destroys the engine before the plugin registrars. Event-channel
// cleanup must still unregister while the engine lives, and skip the native
// call after shutdown. Retain the messenger handle, not the engine itself.
class EngineLifetimeMessenger final : public flutter::BinaryMessenger {
 public:
  EngineLifetimeMessenger(flutter::BinaryMessenger* delegate,
                          FlutterDesktopMessengerRef core)
      : delegate_(delegate),
        core_(core ? FlutterDesktopMessengerAddRef(core) : nullptr) {}

  ~EngineLifetimeMessenger() override {
    if (core_) FlutterDesktopMessengerRelease(core_);
  }

  EngineLifetimeMessenger(const EngineLifetimeMessenger&) = delete;
  EngineLifetimeMessenger& operator=(const EngineLifetimeMessenger&) = delete;

  void Send(const std::string& channel, const uint8_t* message,
            size_t message_size,
            flutter::BinaryReply reply = nullptr) const override {
    if (!core_) return;
    const auto lock = Lock();
    if (FlutterDesktopMessengerIsAvailable(lock.get())) {
      delegate_->Send(channel, message, message_size, std::move(reply));
    }
  }

  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    if (!core_) return;
    const auto lock = Lock();
    if (FlutterDesktopMessengerIsAvailable(lock.get())) {
      delegate_->SetMessageHandler(channel, std::move(handler));
    }
  }

  // Read-only engine identity for first-party native endpoints. This is not
  // a borrowed cpp wrapper identity or permission to call the messenger.
  FlutterDesktopMessengerRef core() const noexcept { return core_; }

 private:
  using MessengerLock =
      std::unique_ptr<FlutterDesktopMessenger,
                      decltype(&FlutterDesktopMessengerUnlock)>;

  MessengerLock Lock() const {
    return MessengerLock(FlutterDesktopMessengerLock(core_),
                         &FlutterDesktopMessengerUnlock);
  }

  flutter::BinaryMessenger* const delegate_;
  FlutterDesktopMessengerRef const core_;
};

}  // namespace nonet

#endif  // NONET_ENGINE_LIFETIME_MESSENGER_H_
