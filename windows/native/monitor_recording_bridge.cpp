#include "monitor_recording_bridge.h"

#include <mferror.h>
#include <pathcch.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include "local_pcm_queue.h"
#include "monitor_aac_inspector.h"
#include "monitor_av_inspector.h"
#include "monitor_av_index.h"
#include "monitor_preview_texture.h"
#include "monitor_recording_digest.h"
#include "monitor_pcm_pump.h"
#include "monitor_preview_player.h"
#include "monitor_recording_file.h"
#include "monitor_recording_stream.h"
#include "rtc_audio_track.h"
#include "rtc_rtp_receiver.h"

namespace nonet {
namespace {
using Value = flutter::EncodableValue;
using Map = flutter::EncodableMap;
using Result = flutter::MethodResult<Value>;
using libwebrtc::scoped_refptr;
constexpr wchar_t kWindowClass[] = L"NoNetChatMonitorRecordingDispatch";
constexpr UINT kCompletion =
    WM_APP + 1;  // Private message-only window, no pointers.

const Value* Field(const Map& map, const char* name) {
  const auto found = map.find(Value(name));
  return found == map.end() ? nullptr : &found->second;
}
std::string Text(const Map& map, const char* name) {
  const auto* value = Field(map, name);
  const auto* text = value ? std::get_if<std::string>(value) : nullptr;
  return text ? *text : std::string();
}
std::optional<std::int64_t> Integer(const Map& map, const char* name) {
  const auto* value = Field(map, name);
  if (value) {
    if (const auto* number = std::get_if<std::int32_t>(value)) return *number;
    if (const auto* number = std::get_if<std::int64_t>(value)) return *number;
  }
  return std::nullopt;
}
bool Token(const std::string& value) {
  return value.size() == 24 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
         });
}
std::wstring Wide(const std::string& value) {
  if (value.empty() || value.size() > PATHCCH_MAX_CCH * 4u) return {};
  const auto length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0 || length >= PATHCCH_MAX_CCH) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), result.data(),
                             length) == length
             ? result
             : std::wstring();
}
const char* CompletionName(fmp4::Stop stop) noexcept {
  switch (stop) {
    case fmp4::Stop::bytes:
      return "byteLimit";
    case fmp4::Stop::duration:
      return "durationLimit";
    case fmp4::Stop::queue:
      return "queueLimit";
    default:
      return nullptr;
  }
}
bool Identifier(const std::string& value) {
  // Same UTF-16 SDK identifier limit as LocalAudioPeer._targetFrom, not a new
  // transport threshold. Identifiers never leave this local native bridge.
  const auto wide = Wide(value);
  return !wide.empty() && wide.size() <= 256 &&
         std::none_of(wide.begin(), wide.end(),
                      [](wchar_t c) { return c <= 0x20 || c == 0x7f; });
}
struct Binding {
  std::int32_t id = 0;
  std::string token, file;
  bool inspection = false, preview = false;
  std::int32_t inspection_version = 1;
  bool video = false;
  bool operator==(const Binding& other) const {
    return id == other.id && token == other.token && file == other.file &&
           inspection == other.inspection && preview == other.preview &&
           inspection_version == other.inspection_version &&
           video == other.video;
  }
  Map Proof(bool released = false) const {
    if (preview) {
      Map proof{{Value("version"), Value(inspection_version)},
                {Value("playerToken"), Value(token)},
                {Value("fileId"), Value(file)},
                {Value("released"), Value(released)}};
      if (video) proof[Value("kind")] = Value("audioVideo");
      return proof;
    }
    if (inspection) {
      Map proof{{Value("version"), Value(inspection_version)},
                {Value("requestToken"), Value(token)},
                {Value("fileId"), Value(file)},
                {Value("released"), Value(released)}};
      if (inspection_version == 3)
        proof[Value("recordingKind")] = Value(video ? "audioVideo" : "audio");
      return proof;
    }
    return {{Value("version"), Value(4)},
            {Value("recorderId"), Value(id)},
            {Value("nonetToken"), Value(token)},
            {Value("fileId"), Value(file)},
            {Value("released"), Value(released)}};
  }
};
std::optional<Binding> ParseBinding(const Map& args, bool inspection,
                                    bool preview) {
  const auto version = Integer(args, "version"),
             id = inspection || preview ? std::optional<std::int64_t>(0)
                                        : Integer(args, "recorderId");
  const auto token = Text(args, preview      ? "playerToken"
                                : inspection ? "requestToken"
                                             : "nonetToken"),
             file = Text(args, "fileId");
  const bool typed = inspection && version == 3;
  const bool video_preview = preview && version == 2;
  const auto kind = Text(args, video_preview ? "kind" : "recordingKind");
  if (version != (inspection ? (typed ? 3 : 1)
                  : preview  ? (video_preview ? 2 : 1)
                             : 4) ||
      !id || *id < 0 || *id >= std::numeric_limits<std::int32_t>::max() ||
      !Token(token) || !Token(file) ||
      (typed && kind != "audio" && kind != "audioVideo") ||
      (video_preview && kind != "audioVideo"))
    return std::nullopt;
  return Binding{static_cast<std::int32_t>(*id),
                 token,
                 file,
                 inspection,
                 preview,
                 typed           ? 3
                 : video_preview ? 2
                                 : 1,
                 video_preview || (typed && kind == "audioVideo")};
}
void Error(std::unique_ptr<Result>& result, const Binding& binding,
           bool released) {
  if (!result) return;
  auto owned = std::move(result);
  owned->Error("nonet_recording_unavailable", "Local recording unavailable",
               Value(binding.Proof(released)));
}

struct Work;
enum class CompletionKind { ready, closed, command, texture };
enum class PreviewAction { play, pause, seek };
struct PreviewCommand {
  PreviewAction action;
  std::int64_t position_us = 0;
};
struct Completion {
  std::shared_ptr<Work> work;
  CompletionKind kind;
};
struct Dispatcher {
  std::mutex mutex;
  std::deque<Completion> incoming;
  HWND window = nullptr;
  DWORD ui_thread = 0;
  bool live = false;
  bool Post(Completion event) noexcept {
    try {
      std::lock_guard<std::mutex> held(mutex);
      if (!live || !window) return false;
      incoming.push_back(std::move(event));
      // Inert nonblocking UI dispatch, including a texture release from UI.
      // Keep the window pinned under admission; no engine pointer crosses.
      return PostMessageW(window, kCompletion, 0, 0) ||
             (GetCurrentThreadId() != ui_thread &&
              SendNotifyMessageW(window, kCompletion, 0, 0));
    } catch (...) {
      return false;
    }
  }
};
// WaitOnAddress compares before sleeping and permits spurious wakes. Its state
// lives inside the shared worker; no settle timer or fallible event allocation.
LONG64 ReadCounter(volatile LONG64* address) noexcept {
  return InterlockedCompareExchange64(address, 0, 0);
}
void SignalCounter(volatile LONG64* address) noexcept {
  InterlockedIncrement64(address);
  WakeByAddressAll(const_cast<LONG64*>(address));
}
void WaitCounter(volatile LONG64* address, LONG64 observed) noexcept {
  while (ReadCounter(address) == observed)
    WaitOnAddress(address, &observed, sizeof(observed), INFINITE);
}
struct Work : std::enable_shared_from_this<Work> {
  struct Status {
    bool opened, closed, finalized;
    HRESULT error, cleanup;
  };
  Binding binding;
  std::wstring path;
  std::uint64_t byte_budget = 0;
  MonitorRecordingBridge::SupportDirectory support;
  std::weak_ptr<Dispatcher> dispatcher;
  LocalPcmQueue queue;
  std::shared_ptr<MonitorPreviewTexture> texture;
  std::atomic<bool> texture_pending{false};
  std::uint64_t expected_bytes = 0;
  std::string expected_sha;
  std::atomic<bool> preview_attested{
      false};  // Published through opened's release/acquire.
  std::int64_t indexed_duration_us = 0;
  std::atomic<bool> retired{false}, decided{false}, attached{false};
  std::atomic<bool> opened{false}, closed{false}, finalized{false};
  // Published before closed; only an original, positively closed prefix sets
  // it.
  fmp4::Stop completion = fmp4::Stop::none;
  std::atomic<HRESULT> error{S_OK}, cleanup{S_OK};
  std::atomic<bool> callback_failed{false}, accepting_callbacks{true};
  // Written by the worker before closed=true, read on UI only after that proof.
  std::optional<MonitorAacMetadata> metadata;
  std::optional<MonitorAvMetadata> av_metadata;
  std::mutex preview_mutex;
  std::condition_variable preview_changed;
  std::optional<PreviewCommand> preview_command;
  MonitorPreviewState preview_state;
  std::uint64_t preview_generation = 0;
  bool command_completed = false;
  HRESULT command_status = S_OK;
  alignas(8) volatile LONG64 decision_generation = 0;
  alignas(8) volatile LONG64 close_generation = 0;
  alignas(8) volatile LONG64 callbacks = 0;

  Status Snapshot() noexcept {
    const auto done = closed.load();
    return {opened.load(), done, finalized.load(), error.load(),
            cleanup.load()};
  }
  void Fail(HRESULT reason) noexcept {
    HRESULT expected = S_OK;
    if (FAILED(reason)) error.compare_exchange_strong(expected, reason);
  }
  bool Retired() const noexcept { return retired.load(); }
  void Decide(bool value) noexcept {
    attached.store(value);
    if (!value) {
      Fail(E_ABORT);
      try {
        queue.Seal();
      } catch (...) {
        callback_failed.store(true);
      }
    }
    decided.store(true);
    SignalCounter(&decision_generation);
  }
  void StopAdmission() noexcept {
    accepting_callbacks.store(false);
    try {
      queue.Seal();
    } catch (...) {
      callback_failed.store(true);
    }
  }
  void Stop() noexcept {
    retired.store(true);
    StopAdmission();
    SignalCounter(&close_generation);
    SignalCounter(&decision_generation);
    if (binding.preview) {
      if (texture) {
        texture->Retire();
        Post(CompletionKind::texture);
      }
      WakePreview();
    }
  }
  void WakePreview() noexcept {
    try {
      std::lock_guard<std::mutex> held(preview_mutex);
      ++preview_generation;
      preview_changed.notify_all();
    } catch (...) {
      callback_failed.store(true);
    }
  }
  bool Command(PreviewCommand value) {
    std::lock_guard<std::mutex> held(preview_mutex);
    if (Retired() || preview_command || command_completed) return false;
    preview_command = value;
    preview_changed.notify_all();
    return true;
  }
  MonitorPreviewState PreviewSnapshot() {
    std::lock_guard<std::mutex> held(preview_mutex);
    return preview_state;
  }
  void WaitCallbacks() noexcept {
    for (;;) {
      const auto count = ReadCounter(&callbacks);
      if (!count) return;
      WaitCounter(&callbacks, count);
    }
  }
  void Post(CompletionKind kind) noexcept {
    if (kind == CompletionKind::texture && texture_pending.exchange(true))
      return;
    bool posted = false;
    try {
      const auto target = dispatcher.lock();
      posted = target && target->Post({shared_from_this(), kind});
    } catch (...) {
    }
    if (!posted && kind == CompletionKind::texture)
      texture_pending.store(false);
    // A lost terminal wake must not increment the close generation and spin
    // retrying failed IO after engine retirement.
    if (!posted && kind == CompletionKind::ready) Stop();
  }
  void Run() noexcept {
    if (binding.preview) {
      RunPreview();
      return;
    }
    if (binding.inspection) {
      RunInspection();
      return;
    }
    MonitorMediaContext media;
    std::unique_ptr<MonitorRecordingFile> file;
    std::unique_ptr<MonitorPcmPump> pump;
    HRESULT status = E_FAIL;
    bool run = false;
    try {
      if (media.ready() && !Retired()) {
        const auto weak = weak_from_this();
        file = std::make_unique<MonitorRecordingFile>(support(), [weak] {
          if (const auto work = weak.lock()) work->StopAdmission();
        });
        pump = std::make_unique<MonitorPcmPump>(queue);
        status =
            Retired() ? E_ABORT : file->Create(path, binding.file, byte_budget);
      }
      if (SUCCEEDED(status)) {
        opened.store(true);
        Post(CompletionKind::ready);
        for (;;) {
          const auto generation = ReadCounter(&decision_generation);
          if (Retired() || decided.load()) break;
          WaitCounter(&decision_generation, generation);
        }
      }
      run = SUCCEEDED(status) && attached.load();
      if (run) {
        status = pump->Run(file->stream());
      } else {
        if (SUCCEEDED(status)) status = E_ABORT;
        queue.Seal();
        while (queue.WaitTake()) {
        }  // Only a visible terminal start failure.
      }
    } catch (...) {
      status = E_FAIL;
      StopAdmission();
    }
    if (status != kMonitorRecordingLimit) Fail(status);
    // All operations after acquisition are nonthrowing; the actual file and
    // encoder stay on this worker until positive Close, including on failure.
    for (;;) {
      const auto generation = ReadCounter(&close_generation);
      const auto done =
          run ? pump->Close() : MonitorAacClose{true, false, status, S_OK};
      const auto release = done.resources_closed ? (file ? file->Close() : S_OK)
                                                 : done.cleanup_status;
      const bool released = done.resources_closed && release == S_OK;
      const auto physical = file ? file->failure() : S_OK;
      const bool quota = released && file && file->quota_prefix() &&
                         (status == S_OK || status == kMonitorRecordingLimit) &&
                         (done.encoding_status == S_OK ||
                          done.encoding_status == kMonitorRecordingLimit) &&
                         physical == kMonitorRecordingLimit &&
                         SUCCEEDED(error.load()) && !callback_failed.load();
      if (!quota) {
        Fail(done.encoding_status);
        Fail(physical);
      }
      if (quota) completion = file->limit();
      cleanup.store(released ? S_OK : FAILED(release) ? release : E_FAIL);
      finalized.store(released && (done.content_finalized || quota) &&
                      SUCCEEDED(error.load()) && !callback_failed.load());
      closed.store(released);  // Publish final metadata before the close proof.
      Post(CompletionKind::closed);
      if (released) return;
      WaitCounter(&close_generation, generation);
    }
  }
  void RunInspection() noexcept {
    MonitorMediaContext media;
    std::unique_ptr<MonitorRecordingFile> file;
    std::unique_ptr<MonitorAacInspector> audio;
    std::unique_ptr<MonitorAvInspector> video;
    HRESULT status = E_FAIL;
    try {
      if (media.ready() && !Retired()) {
        file = std::make_unique<MonitorRecordingFile>(support());
        const auto container = binding.video
                                   ? MonitorRecordingContainer::audio_video_mp4
                                   : MonitorRecordingContainer::audio_m4a;
        if (binding.video) {
          const auto weak = weak_from_this();
          video = std::make_unique<MonitorAvInspector>([weak] {
            const auto work = weak.lock();
            return work && !work->Retired();
          });
        } else
          audio = std::make_unique<MonitorAacInspector>();
        status =
            Retired() ? E_ABORT : file->Read(path, binding.file, container);
        if (SUCCEEDED(status)) {
          opened.store(true);
          status = Retired()       ? E_ABORT
                   : binding.video ? video->Inspect(*file)
                                   : audio->Inspect(file->stream());
        }
      }
    } catch (...) {
      status = E_FAIL;
    }
    Fail(status);
    for (;;) {
      const auto generation = ReadCounter(&close_generation);
      const auto audio_done =
          audio ? audio->Close()
                : MonitorAacInspectedClose{true, status, S_OK, {}};
      const auto video_done =
          video ? video->Close()
                : MonitorAvInspectedClose{true, status, S_OK, {}};
      const bool readers_closed =
          audio_done.resources_closed && video_done.resources_closed;
      const auto reader_error = !audio_done.resources_closed
                                    ? audio_done.cleanup_status
                                    : video_done.cleanup_status;
      const auto release =
          readers_closed ? (file ? file->Close() : S_OK) : reader_error;
      const bool released = readers_closed && release == S_OK;
      Fail(audio_done.inspection_status);
      Fail(video_done.inspection_status);
      cleanup.store(released ? S_OK : FAILED(release) ? release : E_FAIL);
      if (released) {
        if (Retired()) Fail(E_ABORT);
        if (SUCCEEDED(error.load())) {
          if (binding.video && video_done.media) {
            av_metadata = video_done.media;
            metadata = video_done.media->audio;
          } else if (!binding.video)
            metadata = audio_done.media;
        }
        closed.store(true);
      }
      Post(CompletionKind::closed);
      if (released) return;
      WaitCounter(&close_generation, generation);
    }
  }
  HRESULT VerifyPreview(MonitorRecordingFile& file) {
    if (!expected_bytes) return binding.video ? E_INVALIDARG : S_OK;
    std::uint64_t length = 0;
    auto hr = file.ReadPinnedLength(&length);
    if (FAILED(hr)) return hr;
    if (length != expected_bytes) return E_INVALIDARG;
    if (binding.video) {
      const auto index = av_inspection::Inspect(
          length, [this, &file](std::uint64_t at, std::uint8_t* data,
                                std::size_t count) {
            return !Retired() && file.ReadPinnedAt(at, data, count) == S_OK;
          });
      if (!index) return Retired() ? E_ABORT : MF_E_INVALIDMEDIATYPE;
      indexed_duration_us = static_cast<std::int64_t>(index->duration_us);
    }
    std::string digest;
    hr = MonitorRecordingDigest(
        file, length, [this] { return !Retired(); }, digest);
    if (FAILED(hr)) return hr;
    if (digest != expected_sha) return E_INVALIDARG;
    preview_attested = true;
    return S_OK;
  }
  void RunPreview() noexcept {
    MonitorMediaContext media;
    std::unique_ptr<MonitorRecordingFile> file;
    std::unique_ptr<MonitorPreviewPlayer> player;
    HRESULT status = E_FAIL;
    try {
      if (media.ready() && !Retired()) {
        file = std::make_unique<MonitorRecordingFile>(support());
        const auto weak = weak_from_this();
        player = std::make_unique<MonitorPreviewPlayer>([weak] {
          if (const auto work = weak.lock()) work->WakePreview();
        });
        status = error.load();
        if (SUCCEEDED(status))
          status =
              Retired()
                  ? E_ABORT
                  : file->Read(path, binding.file,
                               binding.video
                                   ? MonitorRecordingContainer::audio_video_mp4
                                   : MonitorRecordingContainer::audio_m4a);
        if (SUCCEEDED(status))
          status = Retired() ? E_ABORT : VerifyPreview(*file);
        if (SUCCEEDED(status))
          status =
              Retired() ? E_ABORT : player->Open(file->stream(), binding.video);
      }
      while (SUCCEEDED(status) && !Retired()) {
        std::optional<PreviewCommand> command;
        std::uint64_t generation;
        {
          std::lock_guard<std::mutex> held(preview_mutex);
          generation = preview_generation;
          command.swap(preview_command);
        }
        HRESULT command_result = S_OK;
        if (command && !Retired()) {
          switch (command->action) {
            case PreviewAction::play:
              command_result = player->Play();
              break;
            case PreviewAction::pause:
              command_result = player->Pause();
              break;
            case PreviewAction::seek:
              command_result = player->Seek(command->position_us);
              break;
          }
        }
        MonitorPreviewState state;
        status = player->State(&state);
        if (SUCCEEDED(status) && state.failed) status = E_FAIL;
        if (SUCCEEDED(status) && state.ready && binding.video && !Retired()) {
          MonitorPreviewFrame frame;
          const auto copied = player->Frame(&frame);
          if (FAILED(copied))
            status = copied;
          else if (copied == S_OK) {
            const auto published = texture->Publish(std::move(frame));
            if (FAILED(published)) status = published;
          }
        }
        {
          std::lock_guard<std::mutex> held(preview_mutex);
          preview_state = state;
          if (command) {
            command_status = command_result;
            command_completed = true;
          }
        }
        if (command) Post(CompletionKind::command);
        if (SUCCEEDED(status) && state.ready && !opened.exchange(true))
          Post(CompletionKind::ready);
        if (FAILED(status) || Retired()) break;
        std::unique_lock<std::mutex> held(preview_mutex);
        preview_changed.wait(held, [&] {
          return Retired() || preview_generation != generation ||
                 preview_command.has_value();
        });
      }
    } catch (...) {
      status = E_FAIL;
    }
    Fail(status);
    if (!opened.load() && Retired()) Fail(E_ABORT);
    if (texture) {
      texture->Retire();
      Post(CompletionKind::texture);
    }
    for (;;) {
      const auto generation = ReadCounter(&close_generation);
      const auto player_close = player ? player->Close() : S_OK;
      const bool pixels_closed = !texture || texture->State().closed;
      const auto release = player_close == S_OK && pixels_closed
                               ? (file ? file->Close() : S_OK)
                           : player_close != S_OK ? player_close
                                                  : S_FALSE;
      const bool released =
          player_close == S_OK && pixels_closed && release == S_OK;
      cleanup.store(released ? S_OK : FAILED(release) ? release : E_FAIL);
      closed.store(released);
      // Normal asynchronous raster retirement is pending work, not a failed
      // Stop. Its original callbacks wake the worker; retain the file
      // meanwhile.
      if (released || player_close != S_OK || release != S_FALSE ||
          (texture && texture->State().failed))
        Post(CompletionKind::closed);
      if (released) return;
      WaitCounter(&close_generation, generation);
    }
  }
};
unsigned __stdcall RunWork(void* payload) noexcept {
  std::unique_ptr<std::shared_ptr<Work>> owned(
      static_cast<std::shared_ptr<Work>*>(payload));
  const auto work = *owned;
  owned.reset();
  work->Run();
  return 0;
}
class Sink final : public libwebrtc::AudioTrackSink {
 public:
  explicit Sink(std::shared_ptr<Work> work) : work_(std::move(work)) {}
  void OnData(const void* data, int bits, int rate, size_t channels,
              size_t frames) noexcept override {
    const auto work = work_;
    if (!work->accepting_callbacks.load()) return;
    InterlockedIncrement64(&work->callbacks);
    try {
      if (work->accepting_callbacks.load())
        work->queue.Push(data, bits, rate, channels, frames);
    } catch (...) {
      work->callback_failed.store(true);
      try {
        work->queue.Seal();
      } catch (...) {
      }
    }
    InterlockedDecrement64(&work->callbacks);
    WakeByAddressAll(const_cast<LONG64*>(&work->callbacks));
  }

 private:
  const std::shared_ptr<Work> work_;
};
struct Purpose {
  Binding binding;
  std::string peer_id, receiver_id, track_id;
  scoped_refptr<libwebrtc::RTCPeerConnection> peer;
  scoped_refptr<libwebrtc::RTCRtpReceiver> receiver;
  scoped_refptr<libwebrtc::RTCAudioTrack> track;
  std::shared_ptr<Work> work;
  std::unique_ptr<Sink> sink;
  std::unique_ptr<Result> start, stop, command;
  bool attached = false, attaching = false;
  HANDLE worker_handle = nullptr;
  bool worker_launched = false;
  // Only an uncertain SDK RemoveSink retains itself after unexpected engine
  // teardown. The normal explicit retry clears it after a positive removal.
  std::shared_ptr<Purpose> retained;
  bool CloseThreadHandle() noexcept {
    if (!worker_handle) return true;
    if (!CloseHandle(worker_handle)) return false;
    worker_handle = nullptr;
    return true;
  }
  bool Detach() noexcept {
    try {
      work->StopAdmission();
      if (attaching)
        return false;  // AddSink's reentrant stack must finish first.
      if (attached) {
        track->RemoveSink(sink.get());
        work->WaitCallbacks();
        attached = false;
      }
      if (!CloseThreadHandle()) return false;
      retained.reset();
      return true;
    } catch (...) {
      return false;
    }
  }
};
}  // namespace

struct MonitorRecordingBridge::Impl : std::enable_shared_from_this<Impl> {
  Impl(PeerForId get_peer, bool ready, SupportDirectory directory,
       flutter::TextureRegistrar* texture_registrar)
      : resolver(std::move(get_peer)),
        support(directory ? std::move(directory)
                          : MonitorRecordingSupportDirectory),
        software_ready(ready),
        textures(texture_registrar),
        ui_thread(GetCurrentThreadId()) {}
  PeerForId resolver;
  SupportDirectory support;
  const bool software_ready;
  flutter::TextureRegistrar* const textures;
  const DWORD ui_thread;
  bool retired = false;
  HWND window = nullptr;
  std::shared_ptr<Dispatcher> dispatcher = std::make_shared<Dispatcher>();
  std::shared_ptr<Purpose> purpose;
  std::shared_ptr<Purpose> inspection;
  std::shared_ptr<Purpose> preview;

  std::shared_ptr<Purpose> Selected(const Binding& binding) const {
    return binding.preview      ? preview
           : binding.inspection ? inspection
                                : purpose;
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM first,
                                     LPARAM second) noexcept {
    if (message == WM_NCCREATE) {
      auto* created = reinterpret_cast<CREATESTRUCTW*>(second);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    }
    auto* raw = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    try {
      if (raw && message == kCompletion) {
        const auto owned = raw->shared_from_this();
        owned->Drain();
        return 0;
      }
    } catch (...) { /* Never throw through a Windows callback. */
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return DefWindowProcW(hwnd, message, first, second);
  }
  void InitializeWindow() {
    WNDCLASSW definition{};
    definition.lpfnWndProc = WindowProc;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpszClassName = kWindowClass;
    if (!RegisterClassW(&definition)) {
      WNDCLASSW existing{};
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
          !GetClassInfoW(definition.hInstance, kWindowClass, &existing) ||
          existing.lpfnWndProc != WindowProc)
        return;
    }
    window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, definition.hInstance, this);
    std::lock_guard<std::mutex> held(dispatcher->mutex);
    dispatcher->window = window;
    dispatcher->ui_thread = ui_thread;
    dispatcher->live = window != nullptr;
  }
  bool CurrentSource(const std::shared_ptr<Purpose>& selected) {
    const auto peer = resolver(selected->peer_id);
    if (!peer || peer.get() != selected->peer.get() ||
        peer->peer_connection_state() !=
            libwebrtc::RTCPeerConnectionStateConnected ||
        selected->track->state() != libwebrtc::RTCMediaTrack::kLive ||
        !selected->track->enabled())
      return false;
    // libwebrtc 070aa6d returns fresh receiver/track envelopes on each read.
    // Compare their public identities inside this exact peer, rejecting even
    // ambiguous receiver IDs (remote SDP can reuse them). Never replace the
    // retained track: AddSink/RemoveSink always address the original source.
    bool found = false;
    for (const auto& receiver : peer->receivers().std_vector()) {
      if (!receiver || receiver->id().std_string() != selected->receiver_id)
        continue;
      if (found || receiver->media_type() != libwebrtc::RTCMediaType::AUDIO)
        return false;
      const auto track = receiver->track();
      if (!track || track->id().std_string() != selected->track_id ||
          track->kind().std_string() != "audio" ||
          track->state() != libwebrtc::RTCMediaTrack::kLive ||
          !track->enabled())
        return false;
      found = true;
    }
    return found;
  }
  bool Released(const std::shared_ptr<Purpose>& selected) {
    const bool closed = !selected->attaching && !selected->attached &&
                        selected->work->Snapshot().closed &&
                        selected->CloseThreadHandle();
    if (closed) selected->retained.reset();
    return closed;
  }
  void CompleteStop(const std::shared_ptr<Purpose>& selected) {
    const auto status = selected->work->Snapshot();
    if (!selected->stop) return;
    const bool released = Released(selected);
    if (!released) {
      Error(selected->stop, selected->binding, false);
      return;
    }
    auto map = selected->binding.Proof(true);
    map[Value("finalized")] = Value(status.finalized);
    if (status.closed && status.finalized) {
      if (const auto completion = CompletionName(selected->work->completion))
        map[Value("completion")] = Value(completion);
    }
    auto reply = std::move(selected->stop);
    reply->Success(Value(map));
  }
  void CompleteInspection(const std::shared_ptr<Purpose>& selected) {
    if (!selected->start) return;
    const auto status = selected->work->Snapshot();
    const bool released = Released(selected);
    const auto& data = selected->work->metadata;
    if (!released || FAILED(status.error) || FAILED(status.cleanup) || !data ||
        (selected->binding.video && !selected->work->av_metadata)) {
      Error(selected->start, selected->binding, released);
      return;
    }
    auto proof = selected->binding.Proof(true);
    proof[Value("codec")] = Value("audio/mp4a-latm");
    proof[Value("bytes")] = Value(static_cast<std::int64_t>(data->bytes));
    proof[Value("durationUs")] = Value(data->duration_us);
    proof[Value("sampleRate")] = Value(static_cast<std::int64_t>(data->rate));
    proof[Value("channels")] = Value(static_cast<std::int64_t>(data->channels));
    proof[Value("firstSampleUs")] = Value(data->first_sample_us);
    proof[Value("secondSampleUs")] = Value(data->second_sample_us);
    proof[Value("endSampleUs")] = Value(data->end_sample_us);
    if (selected->binding.video) {
      const auto& video = *selected->work->av_metadata;
      proof[Value("videoCodec")] = Value("video/avc");
      proof[Value("videoFrames")] =
          Value(static_cast<std::int64_t>(video.video_frames));
      proof[Value("firstFrameUs")] = Value(video.first_frame_us);
      proof[Value("secondFrameUs")] = Value(video.second_frame_us);
      proof[Value("endFrameUs")] = Value(video.end_frame_us);
      proof[Value("decodedAudioFrames")] =
          Value(static_cast<std::int64_t>(video.decoded_audio_frames));
      proof[Value("sha256")] = Value(video.sha256);
    }
    auto reply = std::move(selected->start);
    reply->Success(Value(proof));
  }
  Map PreviewProof(const std::shared_ptr<Purpose>& selected) {
    const auto state = selected->work->PreviewSnapshot();
    const auto status = selected->work->Snapshot();
    auto proof = selected->binding.Proof(Released(selected));
    proof[Value("ready")] =
        Value(state.ready && !status.closed && !selected->work->Retired());
    proof[Value("playing")] =
        Value(state.playing && !status.closed && !selected->work->Retired());
    proof[Value("completed")] = Value(state.completed);
    proof[Value("failed")] =
        Value(state.failed || FAILED(status.error) || FAILED(status.cleanup));
    proof[Value("positionUs")] = Value(state.position_us);
    proof[Value("durationUs")] = Value(state.duration_us);
    if (selected->work->preview_attested) {
      proof[Value("bytes")] =
          Value(static_cast<std::int64_t>(selected->work->expected_bytes));
      proof[Value("sha256")] = Value(selected->work->expected_sha);
    }
    if (selected->binding.video && selected->work->texture) {
      const auto pixels = selected->work->texture->State();
      proof[Value("textureId")] = Value(pixels.id);
      proof[Value("width")] = Value(640);
      proof[Value("height")] = Value(480);
      proof[Value("imageReady")] =
          Value(pixels.image_ready && !selected->work->Retired());
    }
    return proof;
  }
  void CompletePreview(const std::shared_ptr<Purpose>& selected,
                       CompletionKind kind) {
    const auto status = selected->work->Snapshot();
    if (kind == CompletionKind::ready && selected->start &&
        !selected->work->Retired() && status.opened && !status.closed &&
        SUCCEEDED(status.error)) {
      auto proof = PreviewProof(selected);
      auto reply = std::move(selected->start);
      reply->Success(Value(proof));
    }
    if (kind == CompletionKind::command || kind == CompletionKind::closed) {
      HRESULT command_result = E_ABORT;
      {
        std::lock_guard<std::mutex> held(selected->work->preview_mutex);
        command_result = selected->work->command_status;
        selected->work->command_completed = false;
      }
      if (selected->command) {
        if (FAILED(command_result) || FAILED(status.error) ||
            FAILED(status.cleanup) || status.closed ||
            selected->work->Retired())
          Error(selected->command, selected->binding, Released(selected));
        else {
          auto proof = PreviewProof(selected);
          auto reply = std::move(selected->command);
          reply->Success(Value(proof));
        }
      }
    }
    if (kind == CompletionKind::closed) {
      Error(selected->start, selected->binding, Released(selected));
      if (selected->stop) CompleteStop(selected);
    }
  }
  void Drain() {
    if (retired || GetCurrentThreadId() != ui_thread) return;
    std::deque<Completion> events;
    {
      std::lock_guard<std::mutex> held(dispatcher->mutex);
      events.swap(dispatcher->incoming);
    }
    for (const auto& event : events) {
      if (retired) return;
      if (event.kind == CompletionKind::texture)
        event.work->texture_pending.store(false);
      const auto selected = Selected(event.work->binding);
      if (!selected || selected->work != event.work) continue;
      if (selected->binding.preview) {
        const auto texture = selected->work->texture;
        if (texture) {
          const auto pixels = texture->State();
          if (pixels.closing)
            texture->ContinueClose();
          else if (FAILED(texture->Update()))
            selected->work->Stop();
        }
        if (!selected->worker_launched && selected->work->Retired() &&
            (!texture || texture->State().closed)) {
          selected->work->closed.store(true);
          CompletePreview(selected, CompletionKind::closed);
        } else
          CompletePreview(selected, event.kind);
        continue;
      }
      if (selected->binding.inspection) {
        CompleteInspection(selected);
        if (selected->stop) CompleteStop(selected);
        continue;
      }
      try {
        if (event.kind == CompletionKind::ready) {
          if (event.work->Retired() || !CurrentSource(selected)) {
            event.work->Decide(false);
            event.work->Stop();
          } else {
            selected->attached =
                true;  // Even a partially throwing AddSink owns.
            selected->attaching = true;
            selected->track->AddSink(selected->sink.get());
            selected->attaching = false;
            const auto admitted = event.work->queue.state();
            if (retired || event.work->Retired() ||
                event.work->callback_failed.load() ||
                admitted.failure != LocalPcmAdmission::accepted) {
              event.work->Decide(false);
              event.work->Stop();
              if (!selected->Detach()) selected->retained = selected;
              if (event.work->Snapshot().closed) {
                Error(selected->start, selected->binding, Released(selected));
                CompleteStop(selected);
              }
              continue;
            }
            event.work->Decide(true);
            if (selected->start) {
              auto map = selected->binding.Proof();
              map[Value("started")] = Value(true);
              auto reply = std::move(selected->start);
              reply->Success(Value(map));
            }
          }
        } else {
          const auto status = event.work->Snapshot();
          if (selected->attaching) continue;
          if (selected->start)
            Error(selected->start, selected->binding, Released(selected));
          if (selected->stop) CompleteStop(selected);
          if (!status.closed && selected->attached && event.work->Retired())
            selected->retained = selected;
        }
      } catch (...) {
        selected->attaching = false;
        event.work->Decide(false);
        event.work->Stop();
        if (!selected->Detach()) selected->retained = selected;
        Error(selected->start, selected->binding, Released(selected));
        if (selected->stop) CompleteStop(selected);
      }
    }
  }
  void Start(const Map& args, const Binding& binding,
             std::unique_ptr<Result>& reply) {
    if (!software_ready || retired || !window) {
      Error(reply, binding, true);
      return;
    }
    if (inspection && !Released(inspection)) {
      Error(reply, binding, true);
      return;
    }
    if (preview && !Released(preview)) {
      Error(reply, binding, true);
      return;
    }
    if (purpose) {
      if (purpose->binding == binding) {
        Error(reply, binding, Released(purpose));
        return;
      }
      if (!Released(purpose)) {
        Error(reply, binding, true);
        return;
      }  // No acquisition for this rejected new tuple.
    }
    const auto peer_id = Text(args, "peerConnectionId"),
               receiver_id = Text(args, "receiverId"),
               track_id = Text(args, "trackId");
    const auto path = Wide(Text(args, "path"));
    const auto budget = Integer(args, "fileBudget");
    if (!Identifier(peer_id) || !Identifier(receiver_id) ||
        !Identifier(track_id) || path.empty() || !budget || *budget <= 0 ||
        static_cast<std::uint64_t>(*budget) >
            MonitorRecordingStream::max_bytes) {
      Error(reply, binding, true);
      return;
    }
    auto selected = std::make_shared<Purpose>();
    selected->binding = binding;
    selected->peer_id = peer_id;
    selected->receiver_id = receiver_id;
    selected->track_id = track_id;
    selected->peer = resolver(peer_id);
    if (selected->peer) {
      for (const auto& receiver : selected->peer->receivers().std_vector()) {
        if (receiver && receiver->id().std_string() == receiver_id &&
            receiver->media_type() == libwebrtc::RTCMediaType::AUDIO) {
          const auto track = receiver->track();
          if (track && track->id().std_string() == track_id &&
              track->kind().std_string() == "audio") {
            selected->receiver = receiver;
            selected->track =
                static_cast<libwebrtc::RTCAudioTrack*>(track.get());
          }
        }
      }
    }
    if (!selected->track || !CurrentSource(selected)) {
      Error(reply, binding, true);
      return;
    }
    selected->work = std::make_shared<Work>();
    selected->work->binding = binding;
    selected->work->path = path;
    selected->work->byte_budget = static_cast<std::uint64_t>(*budget);
    selected->work->support = support;
    selected->work->dispatcher = dispatcher;
    selected->sink = std::make_unique<Sink>(selected->work);
    auto payload = std::make_unique<std::shared_ptr<Work>>(selected->work);
    purpose = selected;  // Index before any asynchronous native acquisition.
    selected->start = std::move(reply);
    selected->worker_handle = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, RunWork, payload.get(), 0, nullptr));
    if (!selected->worker_handle) {
      selected->work->Fail(E_FAIL);
      selected->work->closed.store(true);
      selected->work->Stop();
      Error(selected->start, binding, true);
    } else {
      payload.release();  // The CRT worker now owns this shared reference.
      if (!selected->CloseThreadHandle()) {
        selected->work->Stop();
        Error(selected->start, binding, false);
      }
    }
  }
  void Inspect(const Map& args, const Binding& binding,
               std::unique_ptr<Result>& reply) {
    if (!software_ready || retired || !window ||
        (purpose && !Released(purpose))) {
      Error(reply, binding, true);
      return;
    }
    if (preview && !Released(preview)) {
      Error(reply, binding, true);
      return;
    }
    if (inspection) {
      if (inspection->binding == binding) {
        Error(reply, binding, Released(inspection));
        return;
      }
      if (!Released(inspection)) {
        Error(reply, binding, true);
        return;
      }
    }
    const auto path = Wide(Text(args, "path"));
    if (path.empty()) {
      Error(reply, binding, true);
      return;
    }
    auto selected = std::make_shared<Purpose>();
    selected->binding = binding;
    selected->work = std::make_shared<Work>();
    selected->work->binding = binding;
    selected->work->path = path;
    selected->work->support = support;
    selected->work->dispatcher = dispatcher;
    auto payload = std::make_unique<std::shared_ptr<Work>>(selected->work);
    inspection = selected;
    selected->start = std::move(reply);
    selected->worker_handle = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, RunWork, payload.get(), 0, nullptr));
    if (!selected->worker_handle) {
      selected->work->Fail(E_FAIL);
      selected->work->closed.store(true);
      selected->work->Stop();
      Error(selected->start, binding, true);
    } else {
      payload.release();
      if (!selected->CloseThreadHandle()) {
        selected->work->Stop();
        Error(selected->start, binding, false);
      }
    }
  }
  void OpenPreview(const Map& args, const Binding& binding,
                   std::unique_ptr<Result>& reply) {
    if (!software_ready || retired || !window ||
        (purpose && !Released(purpose)) ||
        (inspection && !Released(inspection))) {
      Error(reply, binding, true);
      return;
    }
    if (preview) {
      if (preview->binding == binding) {
        Error(reply, binding, Released(preview));
        return;
      }
      if (!Released(preview)) {
        Error(reply, binding, true);
        return;
      }
    }
    const auto path = Wide(Text(args, "path"));
    const auto bytes = Integer(args, "bytes");
    const auto sha = Text(args, "sha256");
    const bool attested =
        bytes && *bytes > 0 &&
        static_cast<std::uint64_t>(*bytes) <=
            MonitorRecordingStream::max_bytes &&
        sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (path.empty() ||
        (binding.video &&
         (!attested || !textures || !MonitorPreviewPlayer::VideoSupported())) ||
        ((!sha.empty() || bytes.has_value()) && !attested)) {
      Error(reply, binding, true);
      return;
    }
    auto selected = std::make_shared<Purpose>();
    selected->binding = binding;
    selected->work = std::make_shared<Work>();
    selected->work->binding = binding;
    selected->work->path = path;
    selected->work->support = support;
    selected->work->dispatcher = dispatcher;
    if (attested) {
      selected->work->expected_bytes = static_cast<std::uint64_t>(*bytes);
      selected->work->expected_sha = sha;
    }
    if (binding.video) {
      const auto weak = std::weak_ptr<Work>(selected->work);
      selected->work->texture =
          std::make_shared<MonitorPreviewTexture>(textures, [weak] {
            if (const auto work = weak.lock()) {
              SignalCounter(&work->close_generation);
              work->Post(CompletionKind::texture);
            }
          });
    }
    auto payload = std::make_unique<std::shared_ptr<Work>>(selected->work);
    preview = selected;  // Index before the first actual registrar/native call.
    selected->start = std::move(reply);
    if (selected->work->texture) {
      const auto registered = selected->work->texture->Register();
      if (FAILED(registered)) selected->work->Fail(registered);
    }
    selected->worker_handle = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, RunWork, payload.get(), 0, nullptr));
    if (!selected->worker_handle) {
      selected->work->Fail(E_FAIL);
      selected->work->Stop();
      if (selected->work->texture) selected->work->texture->ContinueClose();
      const bool closed =
          !selected->work->texture || selected->work->texture->State().closed;
      selected->work->closed.store(closed);
      if (!closed) selected->retained = selected;
      Error(selected->start, binding, closed);
    } else {
      selected->worker_launched = true;
      payload.release();
      if (!selected->CloseThreadHandle()) {
        selected->work->Stop();
        Error(selected->start, binding, false);
      }
    }
  }
  void PreviewControl(const Binding& binding,
                      std::optional<PreviewCommand> command,
                      std::unique_ptr<Result>& reply) {
    const auto selected = preview;
    if (!selected || !(selected->binding == binding)) {
      Error(reply, binding, false);
      return;
    }
    if (!command) {
      selected->work->WakePreview();  // All SDK state reads stay on the worker.
      reply->Success(Value(PreviewProof(selected)));
      return;
    }
    const auto status = selected->work->Snapshot();
    if (selected->command || !status.opened || status.closed ||
        selected->work->Retired()) {
      Error(reply, binding, Released(selected));
      return;
    }
    selected->command = std::move(reply);
    if (!selected->work->Command(*command))
      Error(selected->command, binding, Released(selected));
  }
  void Stop(const Binding& binding, std::unique_ptr<Result>& reply) {
    const auto selected = Selected(binding);
    if (!selected || !(selected->binding == binding)) {
      Error(reply, binding, false);
      return;
    }
    if (selected->stop) {
      Error(reply, binding, false);
      return;
    }
    selected->stop = std::move(reply);
    if (selected->attaching) {
      selected->work->Stop();
      return;
    }
    const bool detached = selected->Detach();
    if (!detached) selected->retained = selected;
    selected->work->Stop();
    const auto status = selected->work->Snapshot();
    if (!detached || status.closed) {
      if (selected->start && status.closed)
        Error(selected->start, binding, Released(selected));
      CompleteStop(selected);
    }
  }
  void Retire() noexcept {
    if (retired || GetCurrentThreadId() != ui_thread) return;
    retired = true;
    try {
      {
        std::lock_guard<std::mutex> held(dispatcher->mutex);
        dispatcher->live = false;
        dispatcher->window = nullptr;
        dispatcher->incoming.clear();
      }
      if (purpose) {
        if (!purpose->attaching && !purpose->Detach())
          purpose->retained = purpose;
        purpose->work->Stop();
        purpose->start.reset();
        purpose->stop.reset();
      }
      if (inspection) {
        if (!inspection->Detach()) inspection->retained = inspection;
        inspection->work->Stop();
        inspection->start.reset();
        inspection->stop.reset();
      }
      if (preview) {
        if (preview->work->texture) preview->work->texture->EngineRetired();
        if (!preview->Detach()) preview->retained = preview;
        preview->work->Stop();
        preview->start.reset();
        preview->stop.reset();
        preview->command.reset();
      }
      if (window) {
        DestroyWindow(window);
        window = nullptr;
      }
      UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
    } catch (...) { /* No release proof is produced by engine retirement. */
    }
  }
};

MonitorRecordingBridge::MonitorRecordingBridge(
    PeerForId resolver, bool ready, SupportDirectory support,
    flutter::TextureRegistrar* textures) {
  impl_ = std::make_shared<Impl>(std::move(resolver), ready, std::move(support),
                                 textures);
  impl_->InitializeWindow();
}
MonitorRecordingBridge::~MonitorRecordingBridge() { Retire(); }
void MonitorRecordingBridge::Retire() noexcept {
  if (impl_) impl_->Retire();
}
bool MonitorRecordingBridge::Handle(const flutter::MethodCall<Value>& call,
                                    std::unique_ptr<Result>& result) noexcept {
  const auto& name = call.method_name();
  const bool inspect = name == "nonetWindowsInspectCapabilities" ||
                       name == "nonetWindowsInspectAudio" ||
                       name == "nonetWindowsInspectMedia" ||
                       name == "nonetWindowsInspectStop";
  const bool preview =
      name == "nonetWindowsPreviewCapabilities" ||
      name == "nonetWindowsPreviewOpen" || name == "nonetWindowsPreviewState" ||
      name == "nonetWindowsPreviewPlay" || name == "nonetWindowsPreviewPause" ||
      name == "nonetWindowsPreviewSeek" || name == "nonetWindowsPreviewStop";
  if (!inspect && !preview && name != "nonetWindowsRecordingCapabilities" &&
      name != "nonetWindowsRecordingStart" &&
      name != "nonetWindowsRecordingState" &&
      name != "nonetWindowsRecordingStop")
    return false;
  const auto self = impl_;
  if (!self || self->retired || GetCurrentThreadId() != self->ui_thread)
    return true;
  std::optional<Binding> binding;
  try {
    self->Drain();
    if (self->retired) return true;
    if (name == "nonetWindowsRecordingCapabilities") {
      result->Success(Value(self->software_ready && !self->retired &&
                                    self->window &&
                                    MonitorPreviewPlayer::Supported()
                                ? 4
                                : 0));
      return true;
    }
    if (name == "nonetWindowsInspectCapabilities") {
      result->Success(Value(
          self->software_ready && !self->retired && self->window ? 3 : 0));
      return true;
    }
    if (name == "nonetWindowsPreviewCapabilities") {
      const bool audio = self->software_ready && !self->retired &&
                         self->window && MonitorPreviewPlayer::Supported();
      result->Success(Value(
          audio ? (self->textures && MonitorPreviewPlayer::VideoSupported() ? 3
                                                                            : 1)
                : 0));
      return true;
    }
    const auto* args =
        call.arguments() ? std::get_if<Map>(call.arguments()) : nullptr;
    if (!args || !(binding = ParseBinding(*args, inspect, preview))) {
      result->Error("invalid_arguments", "Invalid recording purpose");
      return true;
    }
    if ((name == "nonetWindowsInspectMedia" &&
         binding->inspection_version != 3) ||
        (name == "nonetWindowsInspectAudio" &&
         (binding->inspection_version != 1 || binding->video))) {
      Error(result, *binding, true);
      return true;
    }
    if (name == "nonetWindowsRecordingStart")
      self->Start(*args, *binding, result);
    else if (name == "nonetWindowsRecordingStop")
      self->Stop(*binding, result);
    else if (name == "nonetWindowsInspectAudio" ||
             name == "nonetWindowsInspectMedia")
      self->Inspect(*args, *binding, result);
    else if (name == "nonetWindowsInspectStop")
      self->Stop(*binding, result);
    else if (name == "nonetWindowsPreviewOpen")
      self->OpenPreview(*args, *binding, result);
    else if (name == "nonetWindowsPreviewStop")
      self->Stop(*binding, result);
    else if (preview) {
      std::optional<PreviewCommand> command;
      if (name == "nonetWindowsPreviewPlay")
        command = PreviewCommand{PreviewAction::play};
      if (name == "nonetWindowsPreviewPause")
        command = PreviewCommand{PreviewAction::pause};
      if (name == "nonetWindowsPreviewSeek") {
        const auto position = Integer(*args, "positionUs");
        if (!position || *position < 0) {
          Error(result, *binding, false);
          return true;
        }
        command = PreviewCommand{PreviewAction::seek, *position};
      }
      self->PreviewControl(*binding, command, result);
    } else {
      const auto selected = self->purpose;
      if (!selected || !(selected->binding == *binding)) {
        Error(result, *binding, false);
        return true;
      }
      const auto status = selected->work->Snapshot();
      const auto queue = selected->work->queue.state();
      const bool failed = FAILED(status.error) || FAILED(status.cleanup) ||
                          selected->work->callback_failed.load() ||
                          (queue.failure != LocalPcmAdmission::accepted &&
                           queue.failure != LocalPcmAdmission::retired) ||
                          !self->CurrentSource(selected);
      auto map = binding->Proof();
      map[Value("active")] =
          Value(status.opened && !status.closed && !selected->work->Retired() &&
                selected->attached && !failed);
      map[Value("failed")] = Value(failed);
      if (status.closed && status.finalized && !failed) {
        if (const auto completion =
                CompletionName(selected->work->completion)) {
          map[Value("finalized")] = Value(true);
          map[Value("completion")] = Value(completion);
        }
      }
      result->Success(Value(map));
    }
  } catch (...) {
    try {
      if (binding) {
        const auto selected = self->Selected(*binding);
        Error(result, *binding,
              !selected || !(selected->binding == *binding) ||
                  self->Released(selected));
      } else if (result)
        result->Error("native_failure", "Local recording unavailable");
    } catch (...) {
    }
  }
  return true;
}
}  // namespace nonet
