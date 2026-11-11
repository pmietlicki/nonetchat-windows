#include "flutter_window.h"

#include <flutter/standard_method_codec.h>
#include <flutter_plugin_registrar.h>

#include <optional>

#include "flutter/generated_plugin_registrant.h"
#include "resource.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {
  // The base destructor cannot dispatch to our override. Also cover a partial
  // OnCreate failure while the engine and its messenger are still alive.
  OnDestroy();
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());
  // Optional media admission must not prevent ordinary offline messaging if
  // the native indicator cannot initialize. Start then fails closed through
  // the missing channel; it never acquires a microphone without indication.
  const auto media_registrar =
      flutter_controller_->engine()->GetRegistrarForPlugin(
          "NoNetChatLocalMedia");
  local_media_indicator_ = nonet::LocalMediaIndicator::Create(
      flutter_controller_->engine()->messenger(),
      nonet::MakeWindowsLocalMediaPlatform(GetHandle(), IDI_APP_ICON),
      media_registrar
          ? FlutterDesktopPluginRegistrarGetMessenger(media_registrar)
          : nullptr);
  relay_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "com.nonetchat/relay",
          &flutter::StandardMethodCodec::GetInstance());
  relay_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    SYSTEM_POWER_STATUS power{};
    const bool known =
        GetSystemPowerStatus(&power) && power.ACLineStatus != 255;
    const bool powered = known && power.ACLineStatus == 1;
    if (call.method_name() == "state") {
      flutter::EncodableMap state;
      state[flutter::EncodableValue("powered")] =
          known ? flutter::EncodableValue(powered) : flutter::EncodableValue();
      result->Success(flutter::EncodableValue(state));
    } else if (call.method_name() == "active") {
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      bool wanted = false;
      if (args) {
        auto value = args->find(flutter::EncodableValue("enabled"));
        if (value != args->end()) {
          if (const auto* enabled = std::get_if<bool>(&value->second))
            wanted = *enabled;
        }
      }
      const bool active = wanted && powered;
      const auto ok = SetThreadExecutionState(
          ES_CONTINUOUS | (active ? ES_SYSTEM_REQUIRED : 0));
      if (ok) relay_active_ = active;
      if (!ok)
        result->Error("UNAVAILABLE", "Suspend lease unavailable");
      else
        result->Success(flutter::EncodableValue(relay_active_));
    } else
      result->NotImplemented();
  });

  flutter_controller_->engine()->SetNextFrameCallback([&]() { this->Show(); });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  if (local_media_indicator_) local_media_indicator_->DetachBeforeEngineClose();
  // The FFI backend has no Flutter plugin destructor. Close callback admission
  // before destroying the Dart isolate, without loading an unused DLL.
  if (const auto module =
          GetModuleHandleW(L"flutter_local_notifications_windows.dll")) {
    using CloseNotifications = void (*)();
    const auto close = reinterpret_cast<CloseNotifications>(
        GetProcAddress(module, "nonet_close_notification_callbacks"));
    if (close) close();
  }
  if (relay_active_) SetThreadExecutionState(ES_CONTINUOUS);
  relay_active_ = false;
  if (relay_channel_) relay_channel_->SetMethodCallHandler(nullptr);
  relay_channel_.reset();
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }
  if (local_media_indicator_) {
    local_media_indicator_->ReleaseAfterEngineClose();
    local_media_indicator_.reset();
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  if (local_media_indicator_) {
    const auto result = local_media_indicator_->HandleWindowMessage(
        hwnd, message, wparam, lparam);
    if (result) return *result;
  }
  if (message == WM_POWERBROADCAST && relay_channel_) {
    SYSTEM_POWER_STATUS power{};
    if (relay_active_ &&
        (!GetSystemPowerStatus(&power) || power.ACLineStatus != 1)) {
      SetThreadExecutionState(ES_CONTINUOUS);
      relay_active_ = false;
    }
    relay_channel_->InvokeMethod("changed", nullptr);
  }
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      if (flutter_controller_)
        flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
