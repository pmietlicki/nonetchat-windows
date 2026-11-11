#include "local_media_indicator.h"

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "../../native/local_media_ownership.h"
#include "monitor_recording_permission.h"

namespace nonet {
namespace {
using Value = flutter::EncodableValue;
using Map = flutter::EncodableMap;
using Call = flutter::MethodCall<Value>;
using Reply = flutter::MethodResult<Value>;
using Owner = LocalMediaOwnership<std::uint16_t>;

const Value* Field(const Map& args, const char* name) {
  const auto found = args.find(Value(name));
  return found == args.end() ? nullptr : &found->second;
}
const std::string& String(const Map& args, const char* name) {
  const auto* value = Field(args, name);
  const auto* text = value ? std::get_if<std::string>(value) : nullptr;
  if (!text) throw std::invalid_argument("Invalid local media field");
  return *text;
}
bool Boolean(const Map& args, const char* name, bool optional = false) {
  const auto* value = Field(args, name);
  if (!value && optional) return false;
  const auto* flag = value ? std::get_if<bool>(value) : nullptr;
  if (!flag) throw std::invalid_argument("Invalid local media flag");
  return *flag;
}
std::u16string Text(const std::string& text) {
  // Same 256 UTF-16-unit bound as Android LocalMediaPlugin.label/account.
  if (text.empty() || text.size() > 4 * 256) {
    throw std::invalid_argument("Invalid local media copy");
  }
  const int units =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (units <= 0 || units > 256)
    throw std::invalid_argument("Invalid media UTF-8");
  std::wstring wide(static_cast<std::size_t>(units), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(),
                          units) != units ||
      std::any_of(wide.begin(), wide.end(), [](wchar_t c) { return c == 0; }) ||
      std::all_of(wide.begin(), wide.end(),
                  [](wchar_t c) { return c <= L' '; })) {
    throw std::invalid_argument("Invalid media text");
  }
  return std::u16string(wide.begin(), wide.end());
}
std::string Utf8(const std::u16string& text) {
  const std::wstring wide(text.begin(), text.end());
  const int bytes = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
      nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) throw std::invalid_argument("Invalid media binding");
  std::string value(static_cast<std::size_t>(bytes), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                          static_cast<int>(wide.size()), value.data(), bytes,
                          nullptr, nullptr) != bytes) {
    throw std::invalid_argument("Invalid media binding");
  }
  return value;
}
void Require(bool condition) {
  if (!condition) throw std::runtime_error("Local media unavailable");
}
}  // namespace

struct LocalMediaIndicator::Impl : std::enable_shared_from_this<Impl> {
  explicit Impl(std::unique_ptr<LocalMediaIndicatorPlatform> platform)
      : platform_(std::move(platform)),
        ownership_([this] { return platform_->Now(); },
                   [this] {
                     return normal_mode_ && !closing_requested_ && !detached_;
                   },
                   [this] { return platform_->Foreground(); }),
        ui_thread_(GetCurrentThreadId()) {}

  ~Impl() { Detach(); }

  void Attach(flutter::BinaryMessenger* messenger,
              FlutterDesktopMessengerRef core) {
    if (core) {
      const std::weak_ptr<Impl> weak = shared_from_this();
      permission_ = std::make_shared<WindowsRecordingPermission>(
          [weak](const auto& value, bool first) {
            const auto self = weak.lock();
            return self && self->RecordingCurrent(value, first);
          },
          [weak](const auto& value, auto done) {
            const auto self = weak.lock();
            bool allowed = false;
            if (self && self->RecordingCurrent(value, true)) {
              const auto owner = self->ownership_.snapshot();
              try {
                allowed = owner &&
                          self->platform_->Update(self->Render(owner->key)) &&
                          self->RecordingCurrent(value, true);
              } catch (...) {
              }
            }
            done(allowed);
          });
      permission_->on_engine_drained = [weak] {
        if (const auto self = weak.lock()) {
          self->Idle();
          if (!self->permission_->HoldsEngine()) self->retained_.reset();
        }
      };
      core_ = core;
      BindWindowsRecordingPermission(core_, permission_);
    }
    channel_ = std::make_shared<flutter::MethodChannel<Value>>(
        messenger, "fr.pmietlicki.nonetchat/local_media",
        &flutter::StandardMethodCodec::GetInstance());
    const std::weak_ptr<Impl> weak = shared_from_this();
    channel_->SetMethodCallHandler(
        [weak](const Call& call, std::unique_ptr<Reply> result) {
          // No raw owner/messenger is captured by a retained SDK callback.
          const auto self = weak.lock();
          if (self) self->Dispatch(call, std::move(result));
        });
  }

  bool RecordingCurrent(const WindowsRecordingDisclosure& value, bool first) {
    if (GetCurrentThreadId() != ui_thread_ || detached_ || !normal_mode_ ||
        closing_requested_ || !recording_presented_ || !recording_kind_ ||
        value.kind != *recording_kind_ || (first && !platform_->Foreground()))
      return false;
    const auto snapshot = ownership_.snapshot();
    return snapshot && snapshot->role == Owner::Role::kListener &&
           snapshot->active && snapshot->key.token() == value.token &&
           Utf8(snapshot->key.account()) == value.account &&
           ownership_.OwnsRecording(snapshot->key, value.recording) &&
           PresentAndCurrent(snapshot->key);
  }

  void RecordingChanged() noexcept {
    if (permission_) permission_->Changed();
  }

  struct Operation {
    explicit Operation(Impl& owner) : owner(owner) { ++owner.operations_; }
    ~Operation() {
      --owner.operations_;
      owner.Idle();
    }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Impl& owner;
  };

  Owner::Key Binding(const Map& args) const {
    const auto snapshot = ownership_.snapshot();
    Require(snapshot && snapshot->key.token() == String(args, "token") &&
            snapshot->key.account() == Text(String(args, "account")));
    return snapshot->key;
  }

  LocalMediaIndicatorView Render(const Owner::Key& key,
                                 bool stopping = false) const {
    Require(ownership_.Matches(key) && base_);
    auto view = *base_;
    const auto snapshot = ownership_.snapshot();
    const bool retired = stopping || snapshot->retired;
    if (snapshot->recording && recording_body_) {
      view.body = *recording_body_;
      if (retired) view.body += u" \u2014 " + view.stopping;
    } else if (retired) {
      view.body = view.stopping;
    }
    return view;
  }

  void Stop(const Owner::Key& key) noexcept {
    if (!ownership_.RequestStop(key) || notifying_stop_ || detached_) return;
    RecordingChanged();
    Operation operation(*this);
    notifying_stop_ = true;
    try {
      if (base_) platform_->Update(Render(key));
    } catch (...) {
    }
    try {
      // InvokeMethod lends the channel's name to BinaryMessenger::Send. A
      // reentrant engine retirement may clear channel_, so retain that object
      // for the full synchronous send without admitting another callback.
      const auto channel = channel_;
      if (!detached_ && ownership_.Matches(key) && channel) {
        Map binding{{Value("token"), Value(key.token())},
                    {Value("account"), Value(Utf8(key.account()))}};
        channel->InvokeMethod("stop",
                              std::make_unique<Value>(std::move(binding)));
      }
    } catch (...) {
      // Retired ownership remains visible and retryable. No engine callback
      // or failure can grant another capture purpose.
    }
    notifying_stop_ = false;
  }

  bool PresentAndCurrent(const Owner::Key& key) {
    try {
      const auto snapshot = ownership_.snapshot();
      const bool current = snapshot && snapshot->presented &&
                           ownership_.Current(key) &&
                           platform_->Exists(key.native_id());
      if (!current) Stop(key);
      return current && ownership_.Matches(key) && !detached_;
    } catch (...) {
      Stop(key);
      return false;
    }
  }

  bool FinishRelease(const Owner::Key& key) {
    if (!ownership_.Matches(key)) return true;
    Require(!ownership_.snapshot()->recording);
    ownership_.RequestStop(key);
    const auto cleanup = Render(key, true);
    bool removed = platform_->Remove(key.native_id());
    if (!ownership_.Matches(key)) return true;
    if (!removed) {
      // NIM_DELETE has no typed "already absent" result. Positively reconcile
      // only this retired indicator, after real media drain, then remove it.
      bool restored = platform_->Present(cleanup);
      if (!ownership_.Matches(key)) return true;
      if (!restored) {
        restored =
            platform_->Exists(key.native_id()) && platform_->Update(cleanup);
      }
      if (!ownership_.Matches(key)) return true;
      removed = restored && platform_->Remove(key.native_id());
    }
    if (!ownership_.Matches(key)) return true;
    if (!removed || !ownership_.ConfirmReleased(key)) return false;
    base_.reset();
    recording_body_.reset();
    recording_kind_.reset();
    recording_presented_ = false;
    RecordingChanged();
    return true;
  }

  void Dispatch(const Call& call, std::unique_ptr<Reply> reply) noexcept {
    // The default engine handler runs on its platform/UI thread. Refuse a
    // foreign thread without reading mutable scope state or invoking media.
    if (GetCurrentThreadId() != ui_thread_ || detached_) return;
    Operation operation(*this);
    bool answered = false;
    const auto success = [&](const Value& value = Value()) {
      if (answered || detached_) return;
      answered = true;
      reply->Success(value);
    };
    try {
      const auto& method = call.method_name();
      if (method == "recordingCapabilities") {
        success(Value(2));
        return;
      }
      if (method != "present" && method != "capture" && method != "active" &&
          method != "state" && method != "recording" && method != "release") {
        answered = true;
        reply->NotImplemented();
        return;
      }
      const auto* args =
          call.arguments() ? std::get_if<Map>(call.arguments()) : nullptr;
      Require(args != nullptr);
      if (method == "present") {
        Require(!closing_requested_ && !ownership_.snapshot() &&
                Boolean(*args, "normalMode"));
        LocalMediaIndicatorView copy;
        copy.title = Text(String(*args, "title"));
        copy.body = Text(String(*args, "body"));
        copy.stop = Text(String(*args, "stop"));
        copy.stopping = Text(String(*args, "stopping"));
        copy.right_to_left = Boolean(*args, "rtl", true);
        const auto& role = String(*args, "role");
        Require(role == "monitor" || role == "listener");
        normal_mode_ = true;
        const auto key = ownership_.Reserve(
            String(*args, "token"), Text(String(*args, "account")),
            role == "monitor" ? Owner::Role::kMonitor : Owner::Role::kListener);
        Require(key.has_value());
        copy.id = key->native_id();
        base_ = std::move(copy);
        const auto presentation =
            *base_;  // A reentrant release may clear base_.
        Require(platform_->Present(presentation) &&
                ownership_.ConfirmPresentation(*key) &&
                PresentAndCurrent(*key));
        success(Value(true));
        return;
      }
      if (method == "release" && !ownership_.snapshot()) {
        success();
        return;
      }
      const auto key = Binding(*args);
      if (method == "release") {
        Require(FinishRelease(key));
        success();
      } else if (method == "state") {
        success(Value(PresentAndCurrent(key)));
      } else if (method == "capture" || method == "active") {
        Require(PresentAndCurrent(key));
        Require(method == "capture" ? ownership_.ReserveCapture(key)
                                    : ownership_.Activate(key));
        success(Value(true));
      } else {
        const auto& nonce = String(*args, "recordingToken");
        Require(Owner::ValidToken(nonce));
        const auto kind = Field(*args, "recordingKind")
                              ? String(*args, "recordingKind")
                              : "audio";
        Require(kind == "audio" || kind == "audioVideo");
        Require(!ownership_.snapshot()->recording ||
                (recording_kind_ && *recording_kind_ == kind));
        const bool on = Boolean(*args, "on");
        if (on) {
          auto body = Text(String(*args, "body"));
          Require(PresentAndCurrent(key) &&
                  ownership_.ReserveRecording(key, nonce));
          recording_presented_ = false;
          recording_kind_ = kind;
          recording_body_ = std::move(body);
          Require(platform_->Update(Render(key)) && PresentAndCurrent(key) &&
                  ownership_.ReserveRecording(key, nonce));
          recording_presented_ = true;
          RecordingChanged();
        } else {
          const auto snapshot = ownership_.snapshot();
          if (!snapshot->recording) {
            success(Value(true));
            return;
          }
          Require(ownership_.OwnsRecording(key, nonce) && base_);
          auto copy = *base_;
          if (snapshot->retired) copy.body = copy.stopping;
          Require(platform_->Update(copy) &&
                  platform_->Exists(key.native_id()) &&
                  ownership_.ConfirmRecordingReleased(key, nonce));
          recording_body_.reset();
          recording_kind_.reset();
          recording_presented_ = false;
          RecordingChanged();
        }
        success(Value(true));
      }
    } catch (...) {
      // Mark attempted before calling the SDK reply: a throwing reply must
      // never be completed twice. Never put a binding/path/exception in it.
      if (!answered && !detached_) {
        answered = true;
        try {
          reply->Error("UNAVAILABLE", "Local media unavailable");
        } catch (...) {
        }
      }
    }
  }

  std::optional<LRESULT> Message(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam) noexcept {
    if (GetCurrentThreadId() != ui_thread_ || detached_ ||
        !platform_->OwnsWindow(window)) {
      return std::nullopt;
    }
    try {
      if (message == WM_CLOSE) {
        close_posted_ = false;
        const auto snapshot = ownership_.snapshot();
        if (!snapshot && operations_ == 0) {
          // A later framework close veto may leave the window open. All old
          // media has closed; allow only a genuinely new purpose afterwards.
          closing_requested_ = false;
          return std::nullopt;
        }
        closing_requested_ = true;
        platform_->CancelMenu();
        if (snapshot) Stop(snapshot->key);
        Idle();
        return 0;
      }
      const auto snapshot = ownership_.snapshot();
      if (!snapshot || !base_) return std::nullopt;
      Operation operation(*this);
      const auto copy = Render(snapshot->key);
      const auto event =
          platform_->HandleMessage(window, message, wparam, lparam, copy);
      if (!event) return std::nullopt;
      if (ownership_.Matches(snapshot->key) &&
          event->id == snapshot->key.native_id()) {
        using Kind = LocalMediaIndicatorEvent::Kind;
        if (event->kind == Kind::kStop || event->kind == Kind::kLost)
          Stop(snapshot->key);
        else if (event->kind == Kind::kOpen && !closing_requested_)
          platform_->OpenApplication();
      }
      return 0;
    } catch (...) {
      try {
        const auto snapshot = ownership_.snapshot();
        if (snapshot) Stop(snapshot->key);
      } catch (...) {
      }
      return 0;
    }
  }

  void Idle() noexcept {
    if (operations_) return;
    if (engine_closed_) {
      if (permission_ && permission_->HoldsEngine()) return;
      FinalCleanup();
      return;
    }
    if (!detached_ && closing_requested_ && !close_posted_ &&
        !ownership_.has_owner()) {
      close_posted_ = platform_->PostClose();
    }
  }

  void Detach() noexcept {
    if (GetCurrentThreadId() != ui_thread_ || detached_) return;
    detached_ = true;
    if (permission_) UnbindWindowsRecordingPermission(core_, permission_);
    platform_->CancelMenu();
    try {
      const auto snapshot = ownership_.snapshot();
      if (snapshot) ownership_.RequestStop(snapshot->key);
      if (channel_) channel_->SetMethodCallHandler(nullptr);
    } catch (...) {
    }
    channel_.reset();
  }

  void EngineClosed() noexcept {
    if (GetCurrentThreadId() != ui_thread_) return;
    engine_closed_ = true;
    if (permission_ && permission_->HoldsEngine())
      retained_ = shared_from_this();
    Detach();
    Idle();
  }

  void FinalCleanup() noexcept {
    if (finalizing_) return;
    finalizing_ = true;
    try {
      const auto snapshot = ownership_.snapshot();
      if (snapshot) {
        // Only the runner's post-engine hook has this postcondition. No
        // received control or normal release may skip a recording nonce.
        if (snapshot->recording) {
          ownership_.ConfirmRecordingReleased(snapshot->key,
                                              *snapshot->recording);
        }
        FinishRelease(snapshot->key);
      }
    } catch (...) {
    }
    finalizing_ = false;
  }

  FlutterDesktopMessengerRef core_ = nullptr;
  std::shared_ptr<WindowsRecordingPermission> permission_;
  std::shared_ptr<Impl> retained_;
  std::optional<std::string> recording_kind_;
  bool recording_presented_ = false;
  std::unique_ptr<LocalMediaIndicatorPlatform> platform_;
  Owner ownership_;
  const DWORD ui_thread_;
  std::shared_ptr<flutter::MethodChannel<Value>> channel_;
  std::optional<LocalMediaIndicatorView> base_;
  std::optional<std::u16string> recording_body_;
  std::size_t operations_ = 0;
  bool normal_mode_ = false, closing_requested_ = false, close_posted_ = false;
  bool notifying_stop_ = false, detached_ = false, engine_closed_ = false,
       finalizing_ = false;
};

LocalMediaIndicator::LocalMediaIndicator(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
LocalMediaIndicator::~LocalMediaIndicator() {
  if (impl_) impl_->Detach();
}

std::unique_ptr<LocalMediaIndicator> LocalMediaIndicator::Create(
    flutter::BinaryMessenger* messenger,
    std::unique_ptr<LocalMediaIndicatorPlatform> platform,
    FlutterDesktopMessengerRef core) noexcept {
  if (!messenger || !platform) return nullptr;
  try {
    auto impl = std::make_shared<Impl>(std::move(platform));
    impl->Attach(messenger, core);
    return std::unique_ptr<LocalMediaIndicator>(
        new LocalMediaIndicator(std::move(impl)));
  } catch (...) {
    return nullptr;
  }
}
std::optional<LRESULT> LocalMediaIndicator::HandleWindowMessage(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
  const auto impl = impl_;
  return impl ? impl->Message(window, message, wparam, lparam) : std::nullopt;
}
void LocalMediaIndicator::DetachBeforeEngineClose() noexcept {
  const auto impl = impl_;
  if (impl) impl->Detach();
}
void LocalMediaIndicator::ReleaseAfterEngineClose() noexcept {
  const auto impl = impl_;
  if (impl) impl->EngineClosed();
}

}  // namespace nonet
