#include "local_media_indicator.h"

#include <shellapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace nonet {
namespace {

bool UnlockedDesktop() noexcept {
  DWORD session = 0, bytes = 0;
  LPWSTR raw = nullptr;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) ||
      !WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session,
                                   WTSSessionInfoEx, &raw, &bytes))
    return false;
  const auto memory =
      std::unique_ptr<void, decltype(&WTSFreeMemory)>(raw, WTSFreeMemory);
  if (!raw || bytes < sizeof(WTSINFOEXW)) return false;
  const auto* info = reinterpret_cast<const WTSINFOEXW*>(raw);
  // The supported application floor is Windows 10, not Windows 7's inverted
  // WTS flags. Never inspect/export user, domain or logon information.
  if (info->Level != 1 ||
      info->Data.WTSInfoExLevel1.SessionFlags != WTS_SESSIONSTATE_UNLOCK)
    return false;
  const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!desktop) return false;
  constexpr wchar_t expected[] = L"Default";
  std::array<wchar_t, std::size(expected)> name{};
  DWORD needed = 0;
  const bool ok = GetUserObjectInformationW(desktop, UOI_NAME, name.data(),
                                            static_cast<DWORD>(sizeof(name)),
                                            &needed) != FALSE;
  CloseDesktop(desktop);
  return ok && std::equal(name.begin(), name.end(), std::begin(expected));
}

std::wstring Wide(const std::u16string& text) {
  static_assert(sizeof(wchar_t) == sizeof(char16_t));
  return std::wstring(text.begin(), text.end());
}

class WindowsMediaPlatform final : public LocalMediaIndicatorPlatform {
 public:
  WindowsMediaPlatform(HWND window, int resource) : window_(window) {
    if (!window || !IsWindow(window))
      throw std::invalid_argument("Media window unavailable");
    callback_ = RegisterWindowMessageW(L"NoNetChat.LocalMediaIndicator");
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    const UINT dpi = GetDpiForWindow(window);
    const int width = dpi ? GetSystemMetricsForDpi(SM_CXSMICON, dpi) : 0;
    const int height = dpi ? GetSystemMetricsForDpi(SM_CYSMICON, dpi) : 0;
    if (!callback_ || !taskbar_created_ || width <= 0 || height <= 0) {
      throw std::runtime_error("Media indicator initialization unavailable");
    }
    icon_ = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource),
                   IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    if (!icon_) throw std::runtime_error("Media icon unavailable");
  }

  ~WindowsMediaPlatform() override {
    // The host retires admission and removes its indicator after SDK drain.
    // Shell owns its copy; this is only the process-local image handle.
    if (icon_) DestroyIcon(icon_);
  }

  std::uint64_t Now() const override { return GetTickCount64(); }
  bool OwnsWindow(HWND window) const noexcept override {
    return window == window_;
  }
  bool Foreground() const override {
    if (!UnlockedDesktop() || !IsWindowVisible(window_) ||
        !IsWindowEnabled(window_) || IsIconic(window_))
      return false;
    const HWND foreground = GetForegroundWindow();
    // An owned picker is not the application's visible Flutter page.
    return foreground == window_ ||
           (foreground && GetAncestor(foreground, GA_ROOT) == window_);
  }

  bool Present(const LocalMediaIndicatorView& view) override {
    auto data = Data(view);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIconW(NIM_SETVERSION, &data) && Exists(view.id);
  }

  bool Update(const LocalMediaIndicatorView& view) override {
    auto data = Data(view);
    return Shell_NotifyIconW(NIM_MODIFY, &data) && Exists(view.id);
  }

  bool Exists(std::uint16_t id) const override {
    RECT bounds{};
    auto identifier = Identifier(id);
    return Shell_NotifyIconGetRect(&identifier, &bounds) == S_OK &&
           bounds.right > bounds.left && bounds.bottom > bounds.top;
  }

  bool Remove(std::uint16_t id) override {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = id;
    return Shell_NotifyIconW(NIM_DELETE, &data) != FALSE;
  }

  void OpenApplication() override {
    if (!UnlockedDesktop()) return;
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(MenuOwner());
  }

  void CancelMenu() noexcept override {
    if (menu_active_) EndMenu();
  }

  bool PostClose() noexcept override {
    return PostMessageW(window_, WM_CLOSE, 0, 0) != FALSE;
  }

  std::optional<LocalMediaIndicatorEvent> HandleMessage(
      HWND window, UINT message, WPARAM wparam, LPARAM lparam,
      const LocalMediaIndicatorView& view) override {
    using Event = LocalMediaIndicatorEvent;
    if (!OwnsWindow(window)) return std::nullopt;
    if (message == taskbar_created_) return Event{Event::Kind::kLost, view.id};
    if (message != callback_) return std::nullopt;
    const auto id = static_cast<std::uint16_t>(HIWORD(lparam));
    if (id != view.id) return Event{Event::Kind::kHandled, id};
    const auto action = LOWORD(lparam);
    if (action == NIN_SELECT || action == NIN_KEYSELECT) {
      return Event{Event::Kind::kOpen, id};
    }
    if (action != WM_CONTEXTMENU || !UnlockedDesktop()) {
      return Event{Event::Kind::kHandled, id};
    }
    struct Menu {
      HMENU handle = CreatePopupMenu();
      ~Menu() {
        if (handle) DestroyMenu(handle);
      }
    } menu;
    constexpr UINT stop = 1;  // Menu-local command, returned synchronously.
    const auto caption = Wide(view.stop);
    if (!menu.handle ||
        !AppendMenuW(menu.handle, MF_STRING, stop, caption.c_str())) {
      return Event{Event::Kind::kHandled, id};
    }
    POINT point{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
    if (point.x == -1 && point.y == -1) {
      RECT bounds{};
      auto identifier = Identifier(id);
      if (Shell_NotifyIconGetRect(&identifier, &bounds) == S_OK) {
        point = POINT{bounds.left, bounds.bottom};
      } else if (!GetCursorPos(&point)) {
        return Event{Event::Kind::kHandled, id};
      }
    }
    const HWND owner = MenuOwner();
    SetForegroundWindow(owner);
    menu_active_ = true;
    const UINT selected =
        TrackPopupMenu(menu.handle,
                       TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                           (view.right_to_left ? TPM_LAYOUTRTL : 0),
                       point.x, point.y, 0, owner, nullptr);
    menu_active_ = false;
    NOTIFYICONDATAW focus{};
    focus.cbSize = sizeof(focus);
    focus.hWnd = window_;
    focus.uID = id;
    Shell_NotifyIconW(NIM_SETFOCUS, &focus);
    return Event{selected == stop ? Event::Kind::kStop : Event::Kind::kHandled,
                 id};
  }

 private:
  HWND MenuOwner() const noexcept {
    const HWND popup = GetLastActivePopup(window_);
    return popup && IsWindowVisible(popup) &&
                   GetAncestor(popup, GA_ROOTOWNER) == window_
               ? popup
               : window_;
  }

  NOTIFYICONIDENTIFIER Identifier(std::uint16_t id) const noexcept {
    NOTIFYICONIDENTIFIER value{};
    value.cbSize = sizeof(value);
    value.hWnd = window_;
    value.uID = id;
    return value;
  }

  NOTIFYICONDATAW Data(const LocalMediaIndicatorView& view) const {
    NOTIFYICONDATAW value{};
    value.cbSize = sizeof(value);
    value.hWnd = window_;
    value.uID = view.id;
    value.uCallbackMessage = callback_;
    value.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    value.hIcon = icon_;
    // Preserve the status first, including a recording disclosure. Full copy
    // stays on the application page; Shell's UTF-16 tooltip has 128 slots.
    const auto text = Wide(view.body + u" \u2014 " + view.title);
    auto count = std::min(text.size(), std::size(value.szTip) - 1);
    if (count && count < text.size() && text[count - 1] >= 0xD800 &&
        text[count - 1] <= 0xDBFF)
      --count;
    std::copy_n(text.begin(), count, value.szTip);
    value.szTip[count] = L'\0';
    return value;
  }

  HWND window_;
  HICON icon_ = nullptr;
  UINT callback_ = 0, taskbar_created_ = 0;
  bool menu_active_ = false;
};

}  // namespace

std::unique_ptr<LocalMediaIndicatorPlatform> MakeWindowsLocalMediaPlatform(
    HWND window, int icon_resource) noexcept {
  try {
    return std::make_unique<WindowsMediaPlatform>(window, icon_resource);
  } catch (...) {
    return nullptr;
  }
}

}  // namespace nonet
