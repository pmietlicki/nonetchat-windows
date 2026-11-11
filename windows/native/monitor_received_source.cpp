#include "monitor_received_source.h"

#include <utility>
namespace nonet {
using libwebrtc::scoped_refptr;
struct MonitorReceivedSource::Impl {
  struct Audio final : libwebrtc::AudioTrackSink {
    std::shared_ptr<MonitorReceivedMedia> media;
    explicit Audio(std::shared_ptr<MonitorReceivedMedia> value)
        : media(std::move(value)) {}
    void OnData(const void* pcm, int bits, int rate, std::size_t channels,
                std::size_t frames) override {
      media->Audio(pcm, bits, rate, channels, frames);
    }
  };
  struct Video final
      : libwebrtc::RTCVideoRenderer<scoped_refptr<libwebrtc::RTCVideoFrame>> {
    std::shared_ptr<MonitorReceivedMedia> media;
    explicit Video(std::shared_ptr<MonitorReceivedMedia> value)
        : media(std::move(value)) {}
    void OnFrame(scoped_refptr<libwebrtc::RTCVideoFrame> frame) override {
      // Getters describe this borrowed frame only; all owned copies are made
      // before returning. No SDK/policy queries, encoding, file IO or wait.
      try {
        if (frame)
          media->Video({frame->width(), frame->height(),
                        static_cast<int>(frame->rotation()), frame->DataY(),
                        frame->DataU(), frame->DataV(), frame->StrideY(),
                        frame->StrideU(), frame->StrideV(), 0});
        else
          media->Video({0, 0, 0, nullptr, nullptr, nullptr, 0, 0, 0, 0});
      } catch (...) {
        media->Video({0, 0, 0, nullptr, nullptr, nullptr, 0, 0, 0, 0});
      }
    }
  };
  const std::thread::id thread = std::this_thread::get_id();
  MonitorReceivedTarget target;
  Permit permit;
  std::shared_ptr<void> keepalive;
  std::shared_ptr<MonitorReceivedMedia> media;
  std::shared_ptr<MonitorReceivedRegistry::Loan> loan;
  Audio audio_sink;
  Video video_sink;
  std::atomic<bool> retired{false};
  bool attempted = false, audio_attached = false, video_attached = false;
  bool audio_admitting = false, video_admitting = false;
  bool uncertain_add = false, audio_remove_failed = false,
       video_remove_failed = false;
  Impl(MonitorReceivedTarget selection, Permit permission,
       std::shared_ptr<void> owner,
       std::shared_ptr<MonitorReceivedMedia> ingress,
       std::shared_ptr<MonitorReceivedRegistry::Loan> source)
      : target(std::move(selection)),
        permit(std::move(permission)),
        keepalive(std::move(owner)),
        media(std::move(ingress)),
        loan(std::move(source)),
        audio_sink(media),
        video_sink(media) {}
  bool Current() {
    return std::this_thread::get_id() == thread && !retired && permit &&
           permit(false) && !retired && loan && loan->Current();
  }
  bool Close() noexcept {
    media->Seal();  // Even a wrong-thread disposal permanently stops copying.
    retired.store(true);
    if (std::this_thread::get_id() != thread) return false;
    // AddSink/AddRenderer may reenter local stop before the SDK wrapper has
    // indexed its own sink table. Removing through that incomplete wrapper
    // could do nothing and falsely clear our ownership. The outer admission
    // finishes the exact wrapper first, then retries its terminal removal.
    if (video_attached && !video_admitting && !video_remove_failed) {
      try {
        loan->video->RemoveRenderer(&video_sink);
        video_attached = false;
      } catch (...) {
        video_remove_failed = true;
      }
    }
    if (audio_attached && !audio_admitting && !audio_remove_failed) {
      try {
        loan->audio->RemoveSink(&audio_sink);
        audio_attached = false;
      } catch (...) {
        audio_remove_failed = true;
      }
    }
    // The pinned SDK serializes original-wrapper removal with delivery (see
    // remote_audio_source sink_lock_ and VideoSinkAdapter::crt_sec_).
    // Thus successful removals, not a timer/queue count, drain admitted calls.
    return !audio_attached && !video_attached && !uncertain_add &&
           !audio_remove_failed && !video_remove_failed;
  }
};

MonitorReceivedSource::MonitorReceivedSource(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
MonitorReceivedSource::~MonitorReceivedSource() {
  if (impl_ && !impl_->Close()) {
    // Retain the exact source, sink, media and engine for process lifetime.
    // A failed removal must never free a callback target or select a successor.
    (void)impl_.release();
  }
}
std::unique_ptr<MonitorReceivedSource> MonitorReceivedSource::Select(
    MonitorReceivedRegistry& registry, MonitorReceivedTarget target,
    Permit permit, std::shared_ptr<void> keepalive,
    std::shared_ptr<MonitorReceivedMedia> media) noexcept {
  try {
    if (!permit || !keepalive || !media || !permit(true)) return {};
    auto loan = registry.Borrow(target, media);
    if (!loan) return {};
    auto state = std::make_unique<Impl>(std::move(target), std::move(permit),
                                        std::move(keepalive), std::move(media),
                                        std::move(loan));
    if (!state->Current() || !state->permit(true)) return {};
    return std::unique_ptr<MonitorReceivedSource>(
        new MonitorReceivedSource(std::move(state)));
  } catch (...) {
    return {};
  }
}
bool MonitorReceivedSource::Current() noexcept {
  try {
    return impl_ && impl_->Current();
  } catch (...) {
    return false;
  }
}
bool MonitorReceivedSource::Attach() noexcept {
  try {
    if (!impl_ || impl_->attempted || !impl_->Current() || !impl_->permit(true))
      return false;
    impl_->attempted = true;
    if (!impl_->media->Activate()) return false;
    // Index each attempt before entering SDK code, which can callback inline.
    impl_->audio_attached = impl_->audio_admitting = true;
    try {
      impl_->loan->audio->AddSink(&impl_->audio_sink);
    } catch (...) {
      impl_->audio_admitting = false;
      impl_->uncertain_add = true;
      throw;
    }
    impl_->audio_admitting = false;
    if (!impl_->Current() || !impl_->permit(true) ||
        !impl_->media->state().accepting) {
      impl_->Close();
      return false;
    }
    if (!impl_->target.video_receiver.empty()) {
      impl_->video_attached = impl_->video_admitting = true;
      try {
        impl_->loan->video->AddRenderer(&impl_->video_sink);
      } catch (...) {
        impl_->video_admitting = false;
        impl_->uncertain_add = true;
        throw;
      }
      impl_->video_admitting = false;
    }
    if (impl_->Current() && impl_->permit(true) &&
        impl_->media->state().accepting)
      return true;
  } catch (...) {
  }
  if (impl_) impl_->Close();
  return false;
}
std::weak_ptr<MonitorReceivedRegistry::Loan> MonitorReceivedSource::scope()
    const noexcept {
  return impl_ ? impl_->loan : std::weak_ptr<MonitorReceivedRegistry::Loan>();
}
bool MonitorReceivedSource::Close() noexcept {
  return !impl_ || impl_->Close();
}

}  // namespace nonet
