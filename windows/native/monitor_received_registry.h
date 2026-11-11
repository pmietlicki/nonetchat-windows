#ifndef NONET_WINDOWS_MONITOR_RECEIVED_REGISTRY_H_
#define NONET_WINDOWS_MONITOR_RECEIVED_REGISTRY_H_
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "monitor_received_media.h"
#include "rtc_audio_track.h"
#include "rtc_peerconnection.h"
#include "rtc_rtp_receiver.h"
#include "rtc_video_track.h"
namespace nonet {
// Native callback index, scoped to one actual engine. Source identity only:
// caller must separately prove current account/Listener indication/permission.
// Neither enumeration of fresh SDK envelopes nor a singleton is used here.
class MonitorReceivedRegistry final {
  struct State;

 public:
  ~MonitorReceivedRegistry() { RetireAll(); }
  bool InstallRuntime(std::shared_ptr<void> original_runtime) noexcept;
  struct Target {
    std::string peer, audio_receiver, audio_track, video_receiver, video_track;
  };
  class Loan final {
   private:
    // Declared before public SDK refs: the runtime outlives every one of them.
    const std::shared_ptr<State> state_;
    const Target target_;
    const std::shared_ptr<MonitorReceivedMedia> media_;
    std::atomic<bool> retired_{false};

   public:
    bool Current() const noexcept;
    bool Present() const noexcept;
    void RetireAdmission() noexcept;
    const libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer;
    const libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> audio;
    const libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> video;

   private:
    friend class MonitorReceivedRegistry;
    Loan(std::shared_ptr<State>, Target,
         libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack>,
         libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>,
         std::shared_ptr<MonitorReceivedMedia>);
  };
  bool Register(const void* original_observer, std::string peer_id,
                libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection>
                    original_peer) noexcept;
  // Called before the ordinary onTrack event, using the SAME received objects.
  // stream is the already-read unique stream; ambiguous means no AV adoption.
  void Track(const void* observer,
             libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>,
             libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack>,
             std::string stream, bool ambiguous_stream) noexcept;
  void Connection(const void* observer,
                  libwebrtc::RTCPeerConnectionState) noexcept;
  void Removed(const void* observer) noexcept;
  void Removed(const void* observer,
               libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) noexcept;
  void Retire(const void* observer) noexcept;
  void RetireAll() noexcept;
  std::shared_ptr<Loan> Borrow(
      const Target&, const std::shared_ptr<MonitorReceivedMedia>&) noexcept;

 private:
  std::mutex mutex_;
  bool retired_ = false;
  std::shared_ptr<void> runtime_;
  std::map<const void*, std::shared_ptr<State>> observers_;
  std::map<std::string, std::weak_ptr<State>> peers_;
};
}  // namespace nonet
#endif
