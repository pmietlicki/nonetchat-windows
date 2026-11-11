#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

// ffi_api.h supplies NativeNotificationCallback/NativeLaunchDetails. The SDK
// calls a NativeCallable.listener: admission queues Dart work without waiting
// for the Flutter UI. Close drains only that short native admission section.
namespace nonet {
class NotificationCallbackGate {
 public:
  static std::shared_ptr<NotificationCallbackGate> Create(NativeNotificationCallback callback) {
    auto gate = std::shared_ptr<NotificationCallbackGate>(new NotificationCallbackGate(callback));
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& gates = Registry();
    gates.erase(std::remove_if(gates.begin(), gates.end(), [](const auto& g) { return g.expired(); }), gates.end());
    if (Closing()) gate->callback_ = nullptr;
    gates.push_back(gate);
    return gate;
  }
  bool Invoke(NativeLaunchDetails details) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!callback_) return false;
    callback_(details);
    return true;
  }
  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
  }
  static void CloseAll() {
    std::vector<std::shared_ptr<NotificationCallbackGate>> live;
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      Closing() = true;
      for (const auto& weak : Registry()) if (auto gate = weak.lock()) live.push_back(gate);
    }
    for (const auto& gate : live) gate->Close();
  }
 private:
  explicit NotificationCallbackGate(NativeNotificationCallback callback) : callback_(callback) {}
  std::mutex mutex_;
  NativeNotificationCallback callback_;
  static std::mutex& RegistryMutex() { static std::mutex value; return value; }
  static bool& Closing() { static bool value = false; return value; }
  static std::vector<std::weak_ptr<NotificationCallbackGate>>& Registry() {
    static std::vector<std::weak_ptr<NotificationCallbackGate>> value;
    return value;
  }
};
}  // namespace nonet
