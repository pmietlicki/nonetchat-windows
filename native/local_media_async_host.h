#ifndef NONET_LOCAL_MEDIA_ASYNC_HOST_H_
#define NONET_LOCAL_MEDIA_ASYNC_HOST_H_

#include "local_media_ownership.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace nonet {

using AsyncMediaOwner = LocalMediaOwnership<std::uint32_t>;
using AsyncMediaKey = AsyncMediaOwner::Key;
using MediaCompletion = std::function<void(bool)>;

struct LocalMediaCopy {
  std::u16string title, body, stop, stopping;
  bool right_to_left = false;
};

struct AsyncMediaView {
  AsyncMediaKey key;
  LocalMediaCopy copy;
};

// Linux's asynchronous OS boundary. Every callback runs on the owning UI
// context. An implementation cannot acquire media or persist an authorization.
class AsyncMediaPresentation {
 public:
  virtual ~AsyncMediaPresentation() = default;
  virtual std::uint64_t Now() const = 0;
  virtual bool Foreground() const = 0;
  virtual void Present(const AsyncMediaView& view, std::function<void()> stop,
                       MediaCompletion completion) = 0;
  virtual void Update(const AsyncMediaView& view, MediaCompletion completion) = 0;
  virtual void Exists(const AsyncMediaKey& key, MediaCompletion completion) = 0;
  virtual void Remove(const AsyncMediaKey& key, MediaCompletion completion) = 0;
  // OUTPUT is already closed. If an update is impossible, a platform may
  // confirm full removal instead and invoke the original immutable Stop.
  virtual void WithdrawRecording(const AsyncMediaView& view, MediaCompletion completion) {
    Update(view, std::move(completion));
  }
};

// One owner and at most one OS operation; no command queue. Release joins the
// outstanding operation, including a failed/retired late publication. The
// Flutter bridge separately fences replies and its own callback admission.
class LocalMediaAsyncHost final : public std::enable_shared_from_this<LocalMediaAsyncHost> {
 public:
  using Key = AsyncMediaKey;
  using Role = AsyncMediaOwner::Role;
  using Done = MediaCompletion;

  explicit LocalMediaAsyncHost(std::shared_ptr<AsyncMediaPresentation> platform)
      : platform_(std::move(platform)),
        owner_([this] { return platform_->Now(); },
               [this] { return normal_mode_ && !closing_ && !detached_; },
               [this] { return platform_->Foreground(); }) {
    if (!platform_) throw std::invalid_argument("Media presentation required");
  }

  std::function<void(const Key&)> on_stop;
  std::function<void()> on_drained;

  std::optional<AsyncMediaOwner::Claim> snapshot() const { return owner_.snapshot(); }
  bool has_owner() const noexcept { return owner_.has_owner(); }
  bool drained() const noexcept { return !owner_.has_owner() && !operation_ && !release_; }
  std::optional<Key> Binding(const std::string& token, const std::u16string& account) const {
    const auto value = owner_.snapshot();
    if (!value || value->key.token() != token || value->key.account() != account) return std::nullopt;
    return value->key;
  }

  static bool ValidLabel(const std::u16string& value) {
    return !value.empty() && value.size() <= 256 &&
        std::find(value.begin(), value.end(), char16_t(0)) == value.end() &&
        !std::all_of(value.begin(), value.end(), [](char16_t c) { return c <= u' '; });
  }

  void Present(std::string token, std::u16string account, Role role, bool normal_mode,
               LocalMediaCopy copy, Done completion) {
    const auto self = shared_from_this();
    if (detached_ || closing_ || owner_.has_owner() || operation_ || release_ ||
        !normal_mode || !ValidLabel(copy.title) || !ValidLabel(copy.body) ||
        !ValidLabel(copy.stop) || !ValidLabel(copy.stopping)) {
      Complete(std::move(completion), false); return;
    }
    normal_mode_ = normal_mode;
    std::optional<Key> reserved;
    try { reserved = owner_.Reserve(std::move(token), std::move(account), role); }
    catch (...) { Complete(std::move(completion), false); return; }
    if (!reserved) { Complete(std::move(completion), false); return; }
    const auto key = *reserved;
    base_.emplace(AsyncMediaView{key, std::move(copy)});
    const auto view = *base_;
    const std::weak_ptr<LocalMediaAsyncHost> weak = self;
    Run([self, view, weak](Done done) {
      self->platform_->Present(view, [weak, key = view.key] {
        if (const auto host = weak.lock()) host->Stop(key);
      }, std::move(done));
    }, [self, key, completion = std::move(completion)](bool shown) mutable {
      bool admitted = false;
      try { admitted = shown && !self->detached_ && !self->release_ &&
                             self->owner_.ConfirmPresentation(key); }
      catch (...) {}
      if (!admitted) self->Retire(key);
      Complete(std::move(completion), admitted);
    });
  }

  void Capture(const Key& key, Done completion) {
    const auto self = shared_from_this();
    Confirm(key, [self, key, completion = std::move(completion)](bool valid) mutable {
      bool admitted = false;
      try { admitted = valid && self->owner_.ReserveCapture(key); } catch (...) {}
      Complete(std::move(completion), admitted);
    });
  }

  void Active(const Key& key, Done completion) {
    const auto self = shared_from_this();
    Confirm(key, [self, key, completion = std::move(completion)](bool valid) mutable {
      bool admitted = false;
      try { admitted = valid && self->owner_.Activate(key); } catch (...) {}
      Complete(std::move(completion), admitted);
    });
  }

  void State(const Key& key, Done completion) {
    // Existing five-second maintenance observes the confirmed purpose while
    // its UI is updating; it grants no new capture or recording acknowledgement.
    if (operation_ && !release_ && !detached_ && Presented(key) && Current(key)) {
      Complete(std::move(completion), true); return;
    }
    Confirm(key, std::move(completion));
  }

  void ShowRecording(const Key& key, std::string nonce, std::u16string body, Done completion) {
    if (!ValidLabel(body)) { Complete(std::move(completion), false); return; }
    const auto self = shared_from_this();
    Confirm(key, [self, key, nonce = std::move(nonce), body = std::move(body),
                  completion = std::move(completion)](bool valid) mutable {
      bool reserved = false;
      try { reserved = valid && self->owner_.ReserveRecording(key, nonce); } catch (...) {}
      if (!reserved) { Complete(std::move(completion), false); return; }
      self->recording_body_ = std::move(body);
      const auto view = self->Render(key);
      if (!view) { Complete(std::move(completion), false); return; }
      self->Run([self, view = *view](Done done) {
        self->platform_->Update(view, std::move(done));
      }, [self, key, nonce, completion = std::move(completion)](bool shown) mutable {
        const bool admitted = shown && !self->detached_ && !self->release_ &&
            self->Current(key) && self->owner_.OwnsRecording(key, nonce);
        if (!admitted) self->Retire(key);
        Complete(std::move(completion), admitted);
      });
    });
  }

  // Called after the actual OUTPUT has closed, including after revocation.
  void HideRecording(const Key& key, std::string nonce, Done completion) {
    const auto self = shared_from_this();
    const auto view = Render(key, false);
    if (detached_ || !owner_.OwnsRecording(key, nonce) || !view) {
      Complete(std::move(completion), false); return;
    }
    Run([self, view = *view](Done done) {
      self->platform_->WithdrawRecording(view, std::move(done));
    }, [self, key, nonce = std::move(nonce), completion = std::move(completion)](bool hidden) mutable {
      const bool released = hidden && !self->detached_ &&
          self->owner_.ConfirmRecordingReleased(key, nonce);
      if (released) self->recording_body_.reset();
      Complete(std::move(completion), released);
    });
  }

  void Stop(const Key& key) noexcept {
    if (!owner_.RequestStop(key) || detached_ || notifying_id_ == key.native_id()) return;
    const auto previous = notifying_id_;
    notifying_id_ = key.native_id();
    try { const auto callback = on_stop; if (callback) callback(key); } catch (...) {}
    notifying_id_ = previous;
  }

  bool RequestClose() {
    closing_ = true;
    const auto value = owner_.snapshot(); if (value) Stop(value->key);
    return drained();
  }

  bool CancelClose() noexcept {
    if (detached_ || !drained()) return false;
    closing_ = false; return true;
  }

  void Release(const Key& key, Done completion) {
    if (detached_ || !owner_.Matches(key) || release_) {
      Complete(std::move(completion), false); return;
    }
    owner_.RequestStop(key);
    if (owner_.snapshot()->recording) { Complete(std::move(completion), false); return; }
    release_ = std::make_shared<ReleaseRequest>(key, std::move(completion));
    PumpRelease();
  }

  bool Detach() noexcept {
    if (!drained()) return false;
    detached_ = true; closing_ = true; on_stop = nullptr; on_drained = nullptr;
    return true;
  }

 private:
  struct Operation { bool completed = false; };
  struct ReleaseRequest {
    Key key;
    Done completion;
    ReleaseRequest(Key purpose, Done callback) : key(std::move(purpose)), completion(std::move(callback)) {}
  };
  using Action = std::function<void(Done)>;

  static void Complete(Done done, bool result) noexcept {
    try { if (done) done(result); } catch (...) {}
  }
  void Retire(const Key& key) noexcept {
    try {
      const auto value = owner_.snapshot();
      if (value && !value->retired) Stop(key);
    } catch (...) { owner_.RequestStop(key); }
  }
  bool Current(const Key& key) noexcept {
    try { return !detached_ && owner_.Current(key); } catch (...) { return false; }
  }
  bool Presented(const Key& key) const {
    const auto value = owner_.snapshot();
    return owner_.Matches(key) && value && value->presented;
  }
  std::optional<AsyncMediaView> Render(const Key& key, bool recording = true) const {
    if (!owner_.Matches(key) || !base_) return std::nullopt;
    auto view = *base_;
    if (recording && recording_body_) view.copy.body = *recording_body_;
    if (owner_.snapshot()->retired) {
      view.copy.body = recording && recording_body_ ?
          view.copy.body + u" \u2014 " + view.copy.stopping : view.copy.stopping;
    }
    return view;
  }

  void Confirm(const Key& key, Done completion) {
    if (!Presented(key) || !Current(key)) {
      Retire(key); Complete(std::move(completion), false); return;
    }
    const auto self = shared_from_this();
    Run([self, key](Done done) { self->platform_->Exists(key, std::move(done)); },
        [self, key, completion = std::move(completion)](bool exists) mutable {
      const bool valid = exists && !self->release_ && self->Current(key);
      if (!valid) self->Retire(key);
      Complete(std::move(completion), valid);
    });
  }

  void Run(Action action, Done completion, bool releasing = false) {
    if (operation_ || detached_ || (release_ && !releasing)) {
      Complete(std::move(completion), false); return;
    }
    const auto self = shared_from_this();
    const auto operation = std::make_shared<Operation>();
    operation_ = operation;
    auto finish = [self, operation, completion = std::move(completion)](bool result) mutable {
      if (operation->completed) return;
      operation->completed = true;
      if (self->operation_ == operation) self->operation_.reset();
      Complete(std::move(completion), result);
      self->PumpRelease();
    };
    try { action(finish); } catch (...) { finish(false); }
  }

  void PumpRelease() {
    if (operation_ || !release_ || detached_) return;
    const auto self = shared_from_this();
    const auto request = release_;
    Run([self, request](Done done) { self->platform_->Remove(request->key, std::move(done)); },
        [self, request](bool removed) mutable {
      if (self->release_ != request) return;
      const bool released = removed && !self->detached_ && self->owner_.ConfirmReleased(request->key);
      if (released) { self->base_.reset(); self->recording_body_.reset(); }
      self->release_.reset();
      Complete(std::move(request->completion), released);
      if (self->closing_ && self->drained()) {
        try { const auto callback = self->on_drained; if (callback) callback(); } catch (...) {}
      }
    }, true);
  }

  std::shared_ptr<AsyncMediaPresentation> platform_;
  bool normal_mode_ = false, closing_ = false, detached_ = false;
  std::optional<std::uint32_t> notifying_id_;
  AsyncMediaOwner owner_;
  std::optional<AsyncMediaView> base_;
  std::optional<std::u16string> recording_body_;
  std::shared_ptr<Operation> operation_;
  std::shared_ptr<ReleaseRequest> release_;
};

}  // namespace nonet
#endif  // NONET_LOCAL_MEDIA_ASYNC_HOST_H_
