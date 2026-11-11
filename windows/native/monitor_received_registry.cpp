#include "monitor_received_registry.h"

#include <windows.h>

#include <algorithm>
#include <utility>
#include <vector>
namespace nonet {
namespace {
using libwebrtc::scoped_refptr;
bool Identifier(const std::string& value) {
  // Same 256 UTF16 units as LocalAudioPeer._targetFrom; byte precheck follows
  // Unicode's maximum four UTF8 bytes per code point, before native conversion.
  if (value.empty() || value.size() > 4 * 256 ||
      std::any_of(value.begin(), value.end(),
                  [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
    return false;
  const auto size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  return size > 0 && size <= 256;
}
std::string Id(libwebrtc::string value) {
  return value.size() <= 4 * 256 ? value.std_string() : std::string();
}
bool Live(const scoped_refptr<libwebrtc::RTCMediaTrack>& track,
          const std::string& id, const char* kind) {
  return track && Id(track->id()) == id && track->kind().std_string() == kind &&
         track->state() == libwebrtc::RTCMediaTrack::kLive && track->enabled();
}
}  // namespace
struct MonitorReceivedRegistry::State {
  const std::shared_ptr<void>
      runtime;  // Last destroyed, after all original SDK refs.
  const std::string id;
  const scoped_refptr<libwebrtc::RTCPeerConnection> peer;
  std::atomic<bool> current{true}, connected{false}, was_connected{false};
  scoped_refptr<libwebrtc::RTCRtpReceiver> audio_receiver, video_receiver;
  scoped_refptr<libwebrtc::RTCMediaTrack> audio, video;
  std::string audio_id, video_id, audio_track, video_track, audio_stream,
      video_stream;
  bool audio_ambiguous = false, video_ambiguous = false;
  std::mutex purpose_mutex;
  std::vector<std::weak_ptr<Loan>> purposes;
  State(std::shared_ptr<void> owner, std::string value,
        scoped_refptr<libwebrtc::RTCPeerConnection> pc)
      : runtime(std::move(owner)), id(std::move(value)), peer(std::move(pc)) {}
  void Stop() noexcept {
    current.store(false);
    connected.store(false);
    try {
      std::lock_guard<std::mutex> held(purpose_mutex);
      for (const auto& weak : purposes)
        if (const auto loan = weak.lock()) loan->RetireAdmission();
    } catch (...) {
    }  // Atomic revocation is permanent even on a failed wake.
  }
  void ClearVideo(scoped_refptr<libwebrtc::RTCRtpReceiver>& old_receiver,
                  scoped_refptr<libwebrtc::RTCMediaTrack>& old_track) {
    // A video-only mutation retires only loans that selected that video.
    // An unchanged original audio source remains usable without a new gesture.
    {
      std::lock_guard<std::mutex> held(purpose_mutex);
      for (const auto& weak : purposes)
        if (const auto loan = weak.lock())
          if (loan->video) loan->RetireAdmission();
    }
    // Callers retain the old SDK refs until AFTER releasing registry_mutex.
    old_receiver = std::move(video_receiver);
    old_track = std::move(video);
    video_id.clear();
    video_track.clear();
    video_stream.clear();
    video_ambiguous = false;
  }
  bool Bind(const std::shared_ptr<Loan>& loan) {
    std::lock_guard<std::mutex> held(purpose_mutex);
    if (!current.load() || !connected.load()) {
      loan->RetireAdmission();
      return false;
    }
    purposes.erase(std::remove_if(purposes.begin(), purposes.end(),
                                  [](const auto& p) { return p.expired(); }),
                   purposes.end());
    // One recorder purpose per PC; a retained callback/worker cannot be
    // replaced.
    if (!purposes.empty()) return false;
    purposes.push_back(loan);
    return true;
  }
};
MonitorReceivedRegistry::Loan::Loan(std::shared_ptr<State> state, Target target,
                                    scoped_refptr<libwebrtc::RTCAudioTrack> a,
                                    scoped_refptr<libwebrtc::RTCVideoTrack> v,
                                    std::shared_ptr<MonitorReceivedMedia> media)
    : state_(std::move(state)),
      target_(std::move(target)),
      media_(std::move(media)),
      peer(state_->peer),
      audio(std::move(a)),
      video(std::move(v)) {}
void MonitorReceivedRegistry::Loan::RetireAdmission() noexcept {
  retired_.store(true);
  media_->Seal();
}
bool MonitorReceivedRegistry::Loan::Present() const noexcept {
  return !retired_.load() && state_->current.load() && state_->connected.load();
}
bool MonitorReceivedRegistry::Loan::Current() const noexcept {
  try {
    return Present() && Live(audio, target_.audio_track, "audio") &&
           (target_.video_receiver.empty() ||
            Live(video, target_.video_track, "video")) &&
           Present();
  } catch (...) {
    return false;
  }
}
bool MonitorReceivedRegistry::InstallRuntime(
    std::shared_ptr<void> runtime) noexcept {
  try {
    std::lock_guard<std::mutex> held(mutex_);
    if (retired_ || runtime_ || !observers_.empty() || !runtime) return false;
    runtime_ = std::move(runtime);
    return true;
  } catch (...) {
    return false;
  }
}
bool MonitorReceivedRegistry::Register(
    const void* observer, std::string id,
    scoped_refptr<libwebrtc::RTCPeerConnection> peer) noexcept {
  try {
    if (!observer || !peer || !Identifier(id)) return false;
    std::lock_guard<std::mutex> held(mutex_);
    if (retired_ || !runtime_ || observers_.count(observer)) return false;
    auto state =
        std::make_shared<State>(runtime_, std::move(id), std::move(peer));
    const auto previous = peers_.find(state->id);
    if (previous != peers_.end())
      if (const auto old = previous->second.lock()) old->Stop();
    observers_.emplace(observer, state);
    peers_[state->id] = state;
    return true;
  } catch (...) {
    return false;
  }
}
void MonitorReceivedRegistry::Track(
    const void* observer, scoped_refptr<libwebrtc::RTCRtpReceiver> receiver,
    scoped_refptr<libwebrtc::RTCMediaTrack> track, std::string stream,
    bool ambiguous) noexcept {
  std::shared_ptr<State> selected;
  scoped_refptr<libwebrtc::RTCRtpReceiver> old_receiver;
  scoped_refptr<libwebrtc::RTCMediaTrack> old_track;
  try {
    // Read public SDK metadata before taking the registry lock: SDK getters
    // may marshal to a signaling thread that is also publishing an observer.
    const auto receiver_id = receiver ? Id(receiver->id()) : std::string();
    const auto track_id = track ? Id(track->id()) : std::string();
    const bool video =
        receiver && receiver->media_type() == libwebrtc::RTCMediaType::VIDEO;
    const bool audio =
        receiver && receiver->media_type() == libwebrtc::RTCMediaType::AUDIO;
    const bool valid = (audio || video) && Identifier(receiver_id) &&
                       Identifier(track_id) &&
                       Live(track, track_id, video ? "video" : "audio") &&
                       (stream.empty() || Identifier(stream));
    std::lock_guard<std::mutex> held(mutex_);
    const auto found = observers_.find(observer);
    if (found == observers_.end()) return;
    selected = found->second;
    if (!selected->current.load()) return;
    if (!valid) {
      if (video)
        selected->ClearVideo(old_receiver, old_track);
      else
        selected->Stop();
      return;
    }
    if (!video && selected->audio) {
      selected->Stop();
      return;
    }
    if (video) {
      if (selected->video) selected->ClearVideo(old_receiver, old_track);
      selected->video_receiver = std::move(receiver);
      selected->video = std::move(track);
      selected->video_id = receiver_id;
      selected->video_track = track_id;
      selected->video_stream = std::move(stream);
      selected->video_ambiguous = ambiguous;
    } else {
      selected->audio_receiver = std::move(receiver);
      selected->audio = std::move(track);
      selected->audio_id = receiver_id;
      selected->audio_track = track_id;
      selected->audio_stream = std::move(stream);
      selected->audio_ambiguous = ambiguous;
    }
  } catch (...) {
    if (selected)
      selected->Stop();
    else
      Removed(observer);
  }
}
void MonitorReceivedRegistry::Connection(
    const void* observer, libwebrtc::RTCPeerConnectionState state) noexcept {
  try {
    std::lock_guard<std::mutex> held(mutex_);
    const auto found = observers_.find(observer);
    if (found == observers_.end()) return;
    auto& selected = *found->second;
    if (!selected.current.load()) return;
    if (state == libwebrtc::RTCPeerConnectionStateConnected) {
      selected.was_connected.store(true);
      selected.connected.store(true);
    } else if (selected.was_connected.load() ||
               state == libwebrtc::RTCPeerConnectionStateClosed ||
               state == libwebrtc::RTCPeerConnectionStateFailed)
      selected.Stop();
  } catch (...) {
  }
}
void MonitorReceivedRegistry::Removed(const void* observer) noexcept {
  try {
    std::lock_guard<std::mutex> held(mutex_);
    const auto p = observers_.find(observer);
    if (p != observers_.end()) p->second->Stop();
  } catch (...) {
  }
}
void MonitorReceivedRegistry::Removed(
    const void* observer,
    scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) noexcept {
  scoped_refptr<libwebrtc::RTCRtpReceiver> old_receiver;
  scoped_refptr<libwebrtc::RTCMediaTrack> old_track;
  try {
    if (!receiver) {
      Removed(observer);
      return;
    }
    const auto id = Id(receiver->id());
    const auto kind = receiver->media_type();
    std::lock_guard<std::mutex> held(mutex_);
    const auto found = observers_.find(observer);
    if (found == observers_.end()) return;
    auto& selected = *found->second;
    if (kind == libwebrtc::RTCMediaType::VIDEO) {
      if (selected.video_id == id) selected.ClearVideo(old_receiver, old_track);
    } else if (kind == libwebrtc::RTCMediaType::AUDIO) {
      if (selected.audio_id == id) selected.Stop();
    } else
      selected.Stop();
  } catch (...) {
    Removed(observer);
  }
}
void MonitorReceivedRegistry::Retire(const void* observer) noexcept {
  std::shared_ptr<State> removed;
  try {
    std::lock_guard<std::mutex> held(mutex_);
    const auto found = observers_.find(observer);
    if (found == observers_.end()) return;
    removed = found->second;
    removed->Stop();
    const auto by_id = peers_.find(removed->id);
    if (by_id != peers_.end() && by_id->second.lock() == removed)
      peers_.erase(by_id);
    observers_.erase(found);
  } catch (...) {
  }
  // Last original SDK references are released outside the registry lock.
}
void MonitorReceivedRegistry::RetireAll() noexcept {
  std::shared_ptr<void> runtime;
  decltype(observers_) removed;
  decltype(peers_) peers;
  try {
    std::lock_guard<std::mutex> held(mutex_);
    retired_ = true;
    for (const auto& pair : observers_) pair.second->Stop();
    observers_.swap(removed);
    peers_.swap(peers);
    runtime = std::move(runtime_);
  } catch (...) {
  }
  // Maps/SDK wrappers drain outside the mutex; runtime is destroyed last.
}
std::shared_ptr<MonitorReceivedRegistry::Loan> MonitorReceivedRegistry::Borrow(
    const Target& target,
    const std::shared_ptr<MonitorReceivedMedia>& media) noexcept {
  try {
    const bool video =
        !target.video_receiver.empty() || !target.video_track.empty();
    if (!media || !Identifier(target.peer) ||
        !Identifier(target.audio_receiver) || !Identifier(target.audio_track) ||
        (video && (!Identifier(target.video_receiver) ||
                   !Identifier(target.video_track) ||
                   target.video_receiver == target.audio_receiver)))
      return {};
    std::shared_ptr<State> selected;
    scoped_refptr<libwebrtc::RTCAudioTrack> a;
    scoped_refptr<libwebrtc::RTCVideoTrack> v;
    {
      std::lock_guard<std::mutex> held(mutex_);
      if (retired_) return {};
      const auto found = peers_.find(target.peer);
      if (found == peers_.end() || !(selected = found->second.lock()))
        return {};
      if (!selected->current.load() || !selected->connected.load() ||
          selected->audio_id != target.audio_receiver ||
          selected->audio_track != target.audio_track)
        return {};
      if (video && (selected->video_id != target.video_receiver ||
                    selected->video_track != target.video_track ||
                    selected->audio_ambiguous || selected->video_ambiguous ||
                    selected->audio_stream.empty() ||
                    selected->audio_stream != selected->video_stream))
        return {};
      a = static_cast<libwebrtc::RTCAudioTrack*>(selected->audio.get());
      if (video)
        v = static_cast<libwebrtc::RTCVideoTrack*>(selected->video.get());
    }
    auto loan = std::shared_ptr<Loan>(
        new Loan(selected, target, std::move(a), std::move(v), media));
    if (!loan->Current() || !selected->Bind(loan) || !loan->Current())
      return {};
    return loan;
  } catch (...) {
    return {};
  }
}
}  // namespace nonet
