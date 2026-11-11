#ifndef NONET_LOCAL_MEDIA_OWNERSHIP_H_
#define NONET_LOCAL_MEDIA_OWNERSHIP_H_

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace nonet {

// UI-thread ownership only. This class cannot capture audio, post a notice or
// persist a grant. The bridge separately confirms the native indicator, and
// only the existing local SDK owner may confirm that media has really closed.
template <typename NativeId> class LocalMediaOwnership final {
  static_assert(std::is_integral_v<NativeId> && std::is_unsigned_v<NativeId> &&
      sizeof(NativeId) < sizeof(std::uint64_t), "Use the SDK's unsigned 16/32-bit ID");
 private:
  struct Scope {};

 public:
  enum class Role { kMonitor, kListener };

  class Key final {
   public:
    const std::string& token() const noexcept { return token_; }
    const std::u16string& account() const noexcept { return account_; }
    NativeId native_id() const noexcept { return native_id_; }
    bool SamePurpose(const Key& other) const noexcept {
      return scope_ == other.scope_ && native_id_ == other.native_id_ &&
             token_ == other.token_ && account_ == other.account_;
    }

   private:
    friend class LocalMediaOwnership;
    Key(std::shared_ptr<const Scope> scope, std::string token,
        std::u16string account, NativeId native_id)
        : scope_(std::move(scope)), token_(std::move(token)),
          account_(std::move(account)), native_id_(native_id) {}
    std::shared_ptr<const Scope> scope_;
    std::string token_;
    std::u16string account_;
    NativeId native_id_;
  };

  struct Claim final {
    Key key;
    Role role;
    std::uint64_t issued_at;
    bool presented = false;
    bool acquiring = false;
    bool active = false;
    bool retired = false;
    std::optional<std::string> recording = std::nullopt;
  };

  using Clock = std::function<std::uint64_t()>;
  using Check = std::function<bool()>;

  // Same local admission budget as Android LocalMediaOwnership.START_BUDGET_MS
  // and NoiseSessionPolicy.responderTimeout, never a live-session lifetime.
  static constexpr std::uint64_t kStartBudgetMs = 20'000;

  LocalMediaOwnership(Clock now, Check allowed, Check visible,
                      std::uint64_t first_native_id = 1)
      : now_(std::move(now)), allowed_(std::move(allowed)),
        visible_(std::move(visible)), scope_(std::make_shared<Scope>()),
        next_native_id_(first_native_id) {
    if (!now_ || !allowed_ || !visible_ || !first_native_id) {
      throw std::invalid_argument("Local media ownership requires its scope");
    }
  }
  LocalMediaOwnership(const LocalMediaOwnership&) = delete;
  LocalMediaOwnership& operator=(const LocalMediaOwnership&) = delete;
  LocalMediaOwnership(LocalMediaOwnership&&) = delete;
  LocalMediaOwnership& operator=(LocalMediaOwnership&&) = delete;

  std::optional<Claim> snapshot() const { return owner_; }
  bool has_owner() const noexcept { return owner_.has_value(); }

  std::optional<Key> Reserve(std::string token, std::u16string account,
                             Role role) {
    // Windows NOTIFYICON_VERSION_4 uses 16 bits; D-Bus notification IDs use 32.
    // Never wrap either: an old native callback must not address a successor.
    if (owner_ || next_native_id_ > std::numeric_limits<NativeId>::max() ||
        !ValidToken(token) ||
        account.empty() || account.size() > 256 ||
        std::find(account.begin(), account.end(), char16_t(0)) != account.end() ||
        std::all_of(account.begin(), account.end(),
                    [](char16_t c) { return c <= u' '; }) ||
        (role != Role::kMonitor && role != Role::kListener)) {
      return std::nullopt;
    }
    const auto id = next_native_id_;
    if (!allowed_() || !visible_()) return std::nullopt;
    const auto issued_at = now_();
    // An injected OS read can reenter a local callback. Even if a new purpose
    // already released again, its consumed ID fences this obsolete admission.
    if (owner_ || next_native_id_ != id || !allowed_() ||
        owner_ || next_native_id_ != id) return std::nullopt;
    Key key(scope_, std::move(token), std::move(account),
            static_cast<NativeId>(id));
    owner_.emplace(Claim{key, role, issued_at});
    ++next_native_id_;
    return key;
  }

  bool Matches(const Key& key) const noexcept {
    return owner_ && owner_->key.SamePurpose(key);
  }

  bool Current(const Key& key) {
    if (!Matches(key) || owner_->retired) return false;
    const bool allowed = allowed_();
    if (!Matches(key) || owner_->retired) return false;
    if (!allowed) { RequestStop(key); return false; }
    if (!owner_->active) {
      const auto issued_at = owner_->issued_at;
      const bool visible = visible_();
      const auto now = now_();
      if (!Matches(key) || owner_->retired) return false;
      if (!owner_->active && (!visible || now < issued_at ||
          now - issued_at >= kStartBudgetMs)) {
        RequestStop(key);
        return false;
      }
    }
    return Matches(key) && !owner_->retired;
  }

  bool ConfirmPresentation(const Key& key) {
    if (!Current(key)) return false;
    owner_->presented = true;
    return true;
  }

  bool ReserveCapture(const Key& key) {
    if (!Current(key) || !owner_->presented || owner_->active ||
        owner_->acquiring || owner_->role != Role::kMonitor) return false;
    owner_->acquiring = true;
    return true;
  }

  bool Activate(const Key& key) {
    if (!Current(key) || !owner_->presented || owner_->active ||
        (owner_->role == Role::kMonitor && !owner_->acquiring)) return false;
    owner_->active = true;
    owner_->acquiring = false;
    return true;
  }

  bool ReserveRecording(const Key& key, const std::string& recording) {
    if (!Current(key) || !owner_->active || !owner_->presented ||
        !ValidToken(recording) ||
        (owner_->recording && *owner_->recording != recording)) return false;
    if (owner_->role == Role::kListener && (!visible_() || !Current(key))) return false;
    if (!Matches(key) || owner_->retired || !owner_->active || !owner_->presented ||
        (owner_->recording && *owner_->recording != recording)) return false;
    owner_->recording = recording;
    return true;
  }

  bool OwnsRecording(const Key& key, const std::string& recording) const noexcept {
    return Matches(key) && owner_->recording && *owner_->recording == recording;
  }

  // The platform caller has positively confirmed the indicator change after
  // OUTPUT closed. Cleanup remains possible after permission/route revocation.
  bool ConfirmRecordingReleased(const Key& key, const std::string& recording) {
    if (!OwnsRecording(key, recording)) return false;
    owner_->recording.reset();
    return true;
  }

  bool RequestStop(const Key& key) noexcept {
    if (!Matches(key)) return false;
    owner_->retired = true;
    return true;
  }

  // Caller confirms actual SDK/PCM and system-indicator release first.
  bool ConfirmReleased(const Key& key) noexcept {
    if (!Matches(key) || owner_->recording) return false;
    owner_.reset();
    return true;
  }

  static bool ValidToken(const std::string& token) noexcept {
    return token.size() == 24 && std::all_of(token.begin(), token.end(), [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
  }

 private:
  Clock now_;
  Check allowed_, visible_;
  std::shared_ptr<const Scope> scope_;
  std::uint64_t next_native_id_;
  std::optional<Claim> owner_;
};

}  // namespace nonet
#endif  // NONET_LOCAL_MEDIA_OWNERSHIP_H_
