#include "monitor_preview_player.h"

#include <mferror.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>

namespace nonet {
namespace {
using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Devices::MediaDevice;
using winrt::Windows::Media::Playback::MediaPlaybackState;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Storage::Streams::IRandomAccessStream;
struct Runtime {
  explicit Runtime(bool probe = false) : thread(GetCurrentThreadId()) {
    status = RoInitialize(RO_INIT_MULTITHREADED);
    if (probe && status == RPC_E_CHANGED_MODE)
      status = RoInitialize(RO_INIT_SINGLETHREADED);
  }
  ~Runtime() {
    if (SUCCEEDED(status) && GetCurrentThreadId() == thread) RoUninitialize();
  }
  const DWORD thread;
  HRESULT status = E_FAIL;
};
struct Events {
  explicit Events(MonitorPreviewPlayer::Wake value) : wake(std::move(value)) {}
  const MonitorPreviewPlayer::Wake wake;
  std::atomic<bool> live{true}, opened{false}, ended{false}, failed{false},
      device_changed{false}, interrupted{false};
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t admitted = 0;
  std::atomic<std::uint64_t> frame_serial{0};
  void Notify() noexcept {
    try {
      {
        std::lock_guard<std::mutex> held(mutex);
        if (!live.load()) return;
        ++admitted;
      }
      try {
        if (wake) wake();
      } catch (...) {
        failed.store(true);
      }
      {
        std::lock_guard<std::mutex> held(mutex);
        --admitted;
        changed.notify_all();
      }
    } catch (...) {
      failed.store(true);
    }
  }
  void Retire() {
    std::unique_lock<std::mutex> held(mutex);
    live.store(false);
    changed.wait(held, [&] { return admitted == 0; });
  }
};
HRESULT Failure() noexcept {
  try {
    throw;
  } catch (const winrt::hresult_error& error) {
    return error.code();
  } catch (...) {
    return E_FAIL;
  }
}
}  // namespace

struct MonitorPreviewPlayer::Impl {
  Impl(Wake wake, CloseGate gate)
      : thread(GetCurrentThreadId()),
        events(std::make_shared<Events>(std::move(wake))),
        close_gate(std::move(gate)) {}
  const DWORD thread;
  Runtime runtime;  // Destroyed after every player/stream/callback member.
  const std::shared_ptr<Events> events;
  const CloseGate close_gate;
  MediaPlayer player{nullptr};
  MediaSource source{nullptr};
  IRandomAccessStream stream{nullptr};
  winrt::Windows::Media::Audio::AudioStateMonitor sound{nullptr};
  winrt::event_token opened{}, ended{}, failed{}, device{}, sound_changed{},
      frame_available{};
  std::unique_ptr<MonitorPreviewVideoSurface> video_surface;
  bool video = false, frame_registered = false;
  std::uint64_t copied_serial = 0;
  bool opened_registered = false, ended_registered = false,
       failed_registered = false, device_registered = false,
       sound_registered = false;
  bool attempted = false, retiring = false, released = false;
  HRESULT error = S_OK;
  bool OnThread() const noexcept { return GetCurrentThreadId() == thread; }
  bool Ready() const noexcept {
    return !retiring && player && events->opened.load() &&
           !events->failed.load() && !events->device_changed.load() &&
           !events->interrupted.load();
  }
  HRESULT Gate(MonitorPreviewResource resource) {
    return close_gate ? close_gate(resource) : S_OK;
  }
};

MonitorPreviewPlayer::MonitorPreviewPlayer(Wake wake, CloseGate gate)
    : impl_(std::make_unique<Impl>(std::move(wake), std::move(gate))) {}
MonitorPreviewPlayer::~MonitorPreviewPlayer() = default;
bool MonitorPreviewPlayer::Supported() noexcept {
  try {
    Runtime runtime(true);
    if (FAILED(runtime.status)) return false;
    return winrt::Windows::Foundation::Metadata::ApiInformation::
               IsPropertyPresent(L"Windows.Media.Playback.MediaPlayer",
                                 L"AudioStateMonitor") &&
           static_cast<bool>(winrt::get_activation_factory<MediaPlayer>());
  } catch (...) {
    return false;
  }
}
bool MonitorPreviewPlayer::VideoSupported() noexcept {
  try {
    Runtime runtime(true);
    if (FAILED(runtime.status) || !Supported()) return false;
    using winrt::Windows::Foundation::Metadata::ApiInformation;
    return ApiInformation::IsPropertyPresent(
               L"Windows.Media.Playback.MediaPlayer",
               L"IsVideoFrameServerEnabled") &&
           ApiInformation::IsMethodPresent(
               L"Windows.Media.Playback.MediaPlayer",
               L"CopyFrameToVideoSurface") &&
           ApiInformation::IsEventPresent(L"Windows.Media.Playback.MediaPlayer",
                                          L"VideoFrameAvailable");
  } catch (...) {
    return false;
  }
}
HRESULT MonitorPreviewPlayer::Open(IMFByteStream* file, bool video) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (self.attempted || self.retiring) return MF_E_INVALIDREQUEST;
  self.attempted = true;
  if (!file) return self.error = E_INVALIDARG;
  if (FAILED(self.runtime.status)) return self.error = self.runtime.status;
  try {
    // Microsoft: the adapter wraps this exact owned byte stream. It never
    // reopens a path, creates a network source or allocates the complete file.
    auto hr = MFCreateStreamOnMFByteStreamEx(
        file, winrt::guid_of<IRandomAccessStream>(),
        winrt::put_abi(self.stream));
    if (FAILED(hr)) return self.error = hr;
    self.player = MediaPlayer();
    self.player.AutoPlay(false);
    self.player.CommandManager().IsEnabled(false);
    self.player.SystemMediaTransportControls().IsEnabled(false);
    self.video = video;
    const auto events = self.events;
    if (video) {
      if (!VideoSupported()) return self.error = E_NOTIMPL;
      self.video_surface = std::make_unique<MonitorPreviewVideoSurface>();
      const auto prepared = self.video_surface->Open();
      if (FAILED(prepared)) return self.error = prepared;
      self.player.IsVideoFrameServerEnabled(true);
      if (!self.player.IsVideoFrameServerEnabled())
        return self.error = E_UNEXPECTED;
      self.frame_available =
          self.player.VideoFrameAvailable([events](auto&&, auto&&) noexcept {
            if (!events->live.load()) return;
            if (events->frame_serial.fetch_add(1) ==
                std::numeric_limits<std::uint64_t>::max())
              events->failed.store(true);
            events->Notify();
          });
      self.frame_registered = true;
    }
    self.opened = self.player.MediaOpened([events](auto&&, auto&&) noexcept {
      if (!events->live.load()) return;
      events->opened.store(true);
      events->Notify();
    });
    self.opened_registered = true;
    self.ended = self.player.MediaEnded([events](auto&&, auto&&) noexcept {
      if (!events->live.load()) return;
      events->ended.store(true);
      events->Notify();
    });
    self.ended_registered = true;
    self.failed = self.player.MediaFailed([events](auto&&, auto&&) noexcept {
      if (!events->live.load()) return;
      events->failed.store(true);
      events->Notify();
    });
    self.failed_registered = true;
    self.device = MediaDevice::DefaultAudioRenderDeviceChanged(
        [events](auto&&, auto&&) noexcept {
          if (!events->live.load()) return;
          events->device_changed.store(true);
          events->Notify();
        });
    self.device_registered = true;
    self.sound = self.player.AudioStateMonitor();
    self.sound_changed = self.sound.SoundLevelChanged([events](
                                                          const auto& sender,
                                                          auto&&) noexcept {
      if (!events->live.load()) return;
      try {
        if (sender.SoundLevel() != winrt::Windows::Media::SoundLevel::Full)
          events->interrupted.store(true);
      } catch (...) {
        events->failed.store(true);
      }
      events->Notify();  // A later Full event never resumes the retired player.
    });
    self.sound_registered = true;
    self.source = MediaSource::CreateFromStream(
        self.stream, video ? L"video/mp4" : L"audio/mp4");
    self.player.Source(self.source);
    return S_OK;
  } catch (...) {
    return self.error = Failure();
  }
}
HRESULT MonitorPreviewPlayer::Play() noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (!self.Ready()) return MF_E_INVALIDREQUEST;
  try {
    self.events->ended.store(false);
    self.player.Play();
    return S_OK;
  } catch (...) {
    return self.error = Failure();
  }
}
HRESULT MonitorPreviewPlayer::Pause() noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (self.retiring || !self.player) return MF_E_INVALIDREQUEST;
  try {
    self.player.Pause();
    return S_OK;
  } catch (...) {
    return self.error = Failure();
  }
}
HRESULT MonitorPreviewPlayer::Seek(std::int64_t position_us) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (!self.Ready() || position_us < 0 ||
      position_us > std::numeric_limits<std::int64_t>::max() / 10)
    return MF_E_INVALIDREQUEST;
  try {
    const auto session = self.player.PlaybackSession();
    const auto ticks = position_us * 10;
    if (ticks > session.NaturalDuration().count()) return E_INVALIDARG;
    session.Position(winrt::Windows::Foundation::TimeSpan(ticks));
    self.events->ended.store(false);
    return S_OK;
  } catch (...) {
    return self.error = Failure();
  }
}
HRESULT MonitorPreviewPlayer::State(MonitorPreviewState* result) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (!result || self.retiring || !self.player) return MF_E_INVALIDREQUEST;
  try {
    const auto session = self.player.PlaybackSession();
    result->failed = FAILED(self.error) || self.events->failed.load() ||
                     self.events->device_changed.load() ||
                     self.events->interrupted.load();
    result->duration_us = session.NaturalDuration().count() / 10;
    result->ready = self.Ready() && result->duration_us > 0;
    result->playing = !result->failed &&
                      session.PlaybackState() == MediaPlaybackState::Playing;
    result->completed = self.events->ended.load();
    result->position_us = session.Position().count() / 10;
    return S_OK;
  } catch (...) {
    return self.error = Failure();
  }
}
HRESULT MonitorPreviewPlayer::Frame(MonitorPreviewFrame* result) noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (!result) return E_POINTER;
  if (!self.Ready() || !self.video || !self.video_surface)
    return MF_E_INVALIDREQUEST;
  const auto serial = self.events->frame_serial.load();
  if (!serial || serial == self.copied_serial) return S_FALSE;
  const auto hr = self.video_surface->Copy(self.player, result);
  if (hr == S_OK)
    self.copied_serial = serial;
  else if (FAILED(hr))
    self.error = hr;
  return hr;
}
HRESULT MonitorPreviewPlayer::Close() noexcept {
  auto& self = *impl_;
  if (!self.OnThread()) return RPC_E_WRONG_THREAD;
  if (self.released) return S_OK;
  self.retiring = true;
  try {
    // Wake is only a nonblocking worker signal, never an engine/player call.
    // Drain admitted signals before their enclosing owner may be released.
    self.events->Retire();
    if (self.device_registered) {
      MediaDevice::DefaultAudioRenderDeviceChanged(self.device);
      self.device_registered = false;
    }
    if (self.sound_registered) {
      self.sound.SoundLevelChanged(self.sound_changed);
      self.sound_registered = false;
    }
    self.sound = nullptr;
    if (self.player) {
      if (self.frame_registered) {
        self.player.VideoFrameAvailable(self.frame_available);
        self.frame_registered = false;
      }
      if (self.opened_registered) {
        self.player.MediaOpened(self.opened);
        self.opened_registered = false;
      }
      if (self.ended_registered) {
        self.player.MediaEnded(self.ended);
        self.ended_registered = false;
      }
      if (self.failed_registered) {
        self.player.MediaFailed(self.failed);
        self.failed_registered = false;
      }
      // Pause failure must not prevent Close from releasing a broken decoder.
      try {
        self.player.Pause();
      } catch (...) {
      }
      auto hr = self.Gate(MonitorPreviewResource::player);
      if (FAILED(hr)) return hr;
      self.player.Close();
      self.player = nullptr;
    }
    if (self.video_surface) {
      const auto gate = self.Gate(MonitorPreviewResource::video_surface);
      if (FAILED(gate)) return gate;
      const auto closed = self.video_surface->Close();
      if (closed != S_OK) return closed;
      self.video_surface.reset();
    }
    if (self.source) {
      auto hr = self.Gate(MonitorPreviewResource::source);
      if (FAILED(hr)) return hr;
      self.source.Close();
      self.source = nullptr;
    }
    if (self.stream) {
      auto hr = self.Gate(MonitorPreviewResource::stream);
      if (FAILED(hr)) return hr;
      self.stream.Close();
      self.stream = nullptr;
    }
    self.released = true;
    return S_OK;
  } catch (...) {
    return Failure();
  }
}
bool MonitorPreviewPlayer::closed() const noexcept { return impl_->released; }

}  // namespace nonet
