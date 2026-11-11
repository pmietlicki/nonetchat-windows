#include "monitor_selected_recording_bridge.h"

#include <bcrypt.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include "monitor_received_source.h"
#include "monitor_selected_program.h"
namespace nonet {
namespace {
using V = flutter::EncodableValue;
using Map = flutter::EncodableMap;
using Reply = flutter::MethodResult<V>;
constexpr UINT kReady = WM_APP + 1;
constexpr wchar_t kClass[] = L"NoNetChatSelectedRecordingDispatchV5";
const V* Field(const Map& map, const char* name) {
  const auto p = map.find(V(name));
  return p == map.end() ? nullptr : &p->second;
}
std::string Text(const Map& map, const char* name) {
  const auto p = Field(map, name);
  const auto s = p ? std::get_if<std::string>(p) : nullptr;
  return s ? *s : std::string();
}
std::optional<std::int64_t> Integer(const Map& map, const char* name) {
  const auto p = Field(map, name);
  if (!p) return {};
  if (const auto n = std::get_if<std::int32_t>(p)) return *n;
  if (const auto n = std::get_if<std::int64_t>(p)) return *n;
  return {};
}
bool Token(const std::string& s) {
  return s.size() == 24 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
         });
}
std::wstring Wide(const std::string& s, std::size_t maximum) {
  if (s.empty() || s.size() > 4 * maximum || s.find('\0') != std::string::npos)
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0 || static_cast<std::size_t>(n) > maximum) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                          static_cast<int>(s.size()), out.data(), n) != n)
    return {};
  return out;
}
std::string AccountHash(const std::string& value) {
  // Same SHA256 of exact account UTF8 as NativeMonitorRecordingFiles. This
  // binds the independently rooted marker/path to this native disclosure.
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string result;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0)
    return {};
  unsigned char bytes[32]{};
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
      BCryptHashData(hash,
                     reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                     static_cast<ULONG>(value.size()), 0) >= 0 &&
      BCryptFinishHash(hash, bytes, sizeof(bytes), 0) >= 0) {
    constexpr char hex[] = "0123456789abcdef";
    result.resize(64);
    for (std::size_t i = 0; i < 32; ++i) {
      result[2 * i] = hex[bytes[i] >> 4];
      result[2 * i + 1] = hex[bytes[i] & 15];
    }
  }
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return result;
}
struct Binding {
  std::int32_t id = 0;
  std::string token, file;
  WindowsRecordingDisclosure disclosure;
  bool operator==(const Binding& other) const noexcept {
    return id == other.id && token == other.token && file == other.file &&
           disclosure == other.disclosure;
  }
  Map Proof(bool released = false) const {
    return {{V("version"), V(5)},
            {V("recorderId"), V(id)},
            {V("nonetToken"), V(token)},
            {V("fileId"), V(file)},
            {V("recordingKind"), V(disclosure.kind)},
            {V("localMediaToken"), V(disclosure.token)},
            {V("localMediaAccount"), V(disclosure.account)},
            {V("localRecordingToken"), V(disclosure.recording)},
            {V("released"), V(released)}};
  }
};
std::optional<Binding> Parse(const Map& args) {
  const auto id = Integer(args, "recorderId");
  Binding b;
  b.token = Text(args, "nonetToken");
  b.file = Text(args, "fileId");
  b.disclosure = {
      Text(args, "localMediaToken"), Text(args, "localMediaAccount"),
      Text(args, "localRecordingToken"), Text(args, "recordingKind")};
  if (Integer(args, "version") != 5 || !id || *id < 0 || *id >= INT32_MAX ||
      !Token(b.token) || !Token(b.file) || !Token(b.disclosure.token) ||
      !Token(b.disclosure.recording) ||
      Wide(b.disclosure.account, 256).empty() ||
      (b.disclosure.kind != "audio" && b.disclosure.kind != "audioVideo"))
    return {};
  b.id = static_cast<std::int32_t>(*id);
  return b;
}
void Error(std::unique_ptr<Reply>& reply, const Binding& b, bool released) {
  if (!reply) return;
  auto owned = std::move(reply);
  owned->Error("nonet_recording_unavailable", "Local recording unavailable",
               V(b.Proof(released)));
}
const char* Completion(fmp4::Stop why) noexcept {
  switch (why) {
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
struct Work;
struct Event {
  std::shared_ptr<Work> work;
  bool ready;
};
struct Dispatch {
  std::mutex mutex;
  std::deque<Event> events;
  HWND window = nullptr;
  bool Post(Event event) noexcept {
    try {
      std::lock_guard<std::mutex> held(mutex);
      if (!window) return false;
      events.push_back(std::move(event));
      return PostMessageW(window, kReady, 0, 0) ||
             SendNotifyMessageW(window, kReady, 0, 0);
    } catch (...) {
      return false;
    }
  }
};
struct Work : std::enable_shared_from_this<Work> {
  std::shared_ptr<MonitorReceivedMedia> media =
      std::make_shared<MonitorReceivedMedia>();
  std::weak_ptr<MonitorReceivedRegistry::Loan> loan;
  std::weak_ptr<Dispatch> dispatch;
  std::wstring path, support;
  std::string file;
  bool video = false;
  std::uint64_t budget = 0;
  std::atomic<bool> revoked{false}, stopping{false}, started{false},
      opened{false};
  std::atomic<bool> closed{false}, native_done{false}, finalized{false},
      failed{false}, closing{false}, cleanup_failed{false};
  fmp4::Stop completion = fmp4::Stop::none;  // Publish before native_done.
  std::mutex mutex;
  std::condition_variable changed;
  bool decided = false, attached = false;
  std::uint64_t commands = 0;
  bool Revoked() const noexcept {
    const auto scope = loan.lock();
    return revoked.load() || !scope || !scope->Present();
  }
  void Decide(bool yes) noexcept {
    try {
      std::lock_guard<std::mutex> held(mutex);
      decided = true;
      attached = yes;
      ++commands;
      changed.notify_all();
    } catch (...) {
      failed.store(true);
      revoked.store(true);
      media->Seal();
    }
  }
  void Stop(bool invalid) noexcept {
    if (invalid) {
      revoked.store(true);
      failed.store(true);
    }
    stopping.store(true);
    media->Seal();
    try {
      std::lock_guard<std::mutex> held(mutex);
      ++commands;
      changed.notify_all();
    } catch (...) {
      failed.store(true);
    }
  }
  void Post(bool ready) noexcept {
    const auto queue = dispatch.lock();
    if (!queue || !queue->Post({shared_from_this(), ready})) Stop(true);
  }
  void Run() noexcept {
    std::unique_ptr<MonitorSelectedProgram> program;
    try {
      {
        program = std::make_unique<MonitorSelectedProgram>(
            *media, video, support, [this] { return Revoked(); });
        const auto prepared = program->Prepare(path, file, budget);
        opened.store(SUCCEEDED(prepared));
        if (FAILED(prepared)) failed.store(true);
        Post(true);
        bool run = false;
        if (SUCCEEDED(prepared)) {
          std::unique_lock<std::mutex> held(mutex);
          changed.wait(held, [&] {
            return decided || stopping.load() || revoked.load();
          });
          run = decided && attached && !revoked.load();
        }
        if (run) {
          const auto hr = program->Run();
          if (FAILED(hr) && hr != kMonitorRecordingLimit &&
              hr != kMonitorAvRecordingLimit)
            failed.store(true);
        }
        closing.store(true);
        for (;;) {
          std::uint64_t seen = 0;
          {
            std::lock_guard<std::mutex> held(mutex);
            seen = commands;
          }
          const auto result = program->Close();
          if (result.resources_closed && !result.content_finalized &&
              started.load())
            failed.store(true);
          finalized.store(result.content_finalized && !failed.load() &&
                          !Revoked());
          if (result.resources_closed) {
            if (finalized.load()) completion = result.completion;
            cleanup_failed.store(false);
            closed.store(true);
            break;
          }
          cleanup_failed.store(true);
          Post(false);
          std::unique_lock<std::mutex> held(mutex);
          changed.wait(held, [&] { return commands != seen; });
        }
      }
      program
          .reset();  // Programme/COM/MF destruction drains before native_done.
      native_done.store(true);
      Post(false);
    } catch (...) {
      failed.store(true);
      revoked.store(true);
      cleanup_failed.store(true);
      media->Seal();
      // Preserve any uncertain original programme for process lifetime. It may
      // own native work; a destructor/no-op must never manufacture release.
      if (program) (void)program.release();
      Post(false);
    }
  }
};
unsigned __stdcall RunWork(void* data) {
  std::unique_ptr<std::shared_ptr<Work>> work(
      static_cast<std::shared_ptr<Work>*>(data));
  (*work)->Run();
  return 0;
}
struct Job {
  Binding binding;
  std::shared_ptr<Work> work = std::make_shared<Work>();
  std::unique_ptr<MonitorReceivedSource> source;
  std::unique_ptr<Reply> start, stop;
  HANDLE thread = nullptr;
  bool attaching = false, source_closed = false, stopping = false,
       normal_end = false;
  bool CloseSource() noexcept {
    return source_closed = (!source || source->Close());
  }
  bool Released() noexcept {
    if (attaching || !source_closed || !work->closed.load() ||
        !work->native_done.load())
      return false;
    if (thread) {
      if (!CloseHandle(thread)) return false;
      thread = nullptr;
    }
    return true;
  }
};
}  // namespace
struct MonitorSelectedRecordingBridge::Impl
    : std::enable_shared_from_this<Impl> {
  std::shared_ptr<MonitorReceivedRegistry> registry;
  const FlutterDesktopMessengerRef core;
  SupportDirectory support;
  const DWORD thread = GetCurrentThreadId();
  const bool software_ready;
  std::shared_ptr<WindowsRecordingPermission> permission;
  std::shared_ptr<Dispatch> dispatch = std::make_shared<Dispatch>();
  std::shared_ptr<Job> job;
  std::shared_ptr<Impl> retained;
  HWND window = nullptr;
  bool retired = false, graph_closed = false;
  Impl(std::shared_ptr<MonitorReceivedRegistry> r, FlutterDesktopMessengerRef c,
       SupportDirectory s, bool ready)
      : registry(std::move(r)),
        core(c),
        support(std::move(s)),
        software_ready(ready) {}
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp,
                                     LPARAM lp) noexcept {
    if (message == WM_NCCREATE) {
      const auto create = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto raw =
        reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (raw && message == kReady) {
      try {
        const auto self = raw->shared_from_this();
        self->Drain();
      } catch (...) {
      }
      return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }
  void Initialize() noexcept {
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kClass;
    type.lpfnWndProc = WindowProc;
    if (!RegisterClassW(&type)) {
      WNDCLASSW existing{};
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
          !GetClassInfoW(type.hInstance, kClass, &existing) ||
          existing.lpfnWndProc != WindowProc)
        return;
    }
    window = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, type.hInstance, this);
    std::lock_guard<std::mutex> held(dispatch->mutex);
    dispatch->window = window;
  }
  bool Allowed(const std::shared_ptr<Job>& selected, bool first) noexcept {
    return GetCurrentThreadId() == thread && !retired && job == selected &&
           permission && !selected->work->revoked.load() &&
           !selected->stopping &&
           permission->Current(selected->binding.disclosure, first);
  }
  bool Current(const std::shared_ptr<Job>& selected) noexcept {
    return Allowed(selected, false) && selected->source &&
           selected->source->Current() && Allowed(selected, false);
  }
  bool BindPermission() {
    if (permission) return true;
    auto candidate = WindowsRecordingPermissionFor(core);
    if (!candidate || !candidate->ClaimEngine()) return false;
    permission = std::move(candidate);
    const std::weak_ptr<Impl> weak = shared_from_this();
    permission->on_changed = [weak] {
      if (const auto self = weak.lock()) self->Changed();
    };
    return true;
  }
  void Changed() noexcept {
    if (GetCurrentThreadId() != thread) return;
    const auto selected = job;
    if (selected && !selected->Released() &&
        (!permission || !permission->Current(selected->binding.disclosure)))
      Stop(selected, true);
  }
  void Stop(const std::shared_ptr<Job>& selected, bool invalid) noexcept {
    selected->stopping = true;
    selected->work->Stop(invalid);
    if (!selected->attaching) selected->CloseSource();
    Complete(selected);
  }
  Map State(const std::shared_ptr<Job>& selected) {
    const auto work = selected->work;
    if (!selected->stopping && !selected->normal_end && !Current(selected))
      Stop(selected, true);
    const bool done = work->native_done.load();
    const bool normal =
        done && work->finalized.load() && Completion(work->completion);
    const auto ingress = work->media->state();
    const bool failed = work->failed.load() || work->revoked.load() ||
                        ingress.failure != MonitorReceivedFailure::none;
    const bool closing = work->started.load() && !selected->stopping && !done &&
                         !failed &&
                         (work->closing.load() || ingress.codec_stopped);
    auto out = selected->binding.Proof(selected->Released());
    out[V("started")] = V(work->started.load());
    out[V("failed")] = V(failed);
    out[V("closing")] = V(closing);
    out[V("active")] = V(work->started.load() && !selected->stopping &&
                         !closing && !done && !failed);
    out[V("finalized")] = V(done && work->finalized.load() && !failed);
    if (normal && !failed)
      out[V("completion")] = V(Completion(work->completion));
    return out;
  }
  void Complete(const std::shared_ptr<Job>& selected) noexcept {
    try {
      if (selected->work->native_done.load() && !selected->attaching) {
        selected->normal_end = selected->work->finalized.load() &&
                               Completion(selected->work->completion);
        selected->CloseSource();
      }
      const bool released = selected->Released();
      if (selected->start &&
          (selected->stopping || selected->work->native_done.load()))
        Error(selected->start, selected->binding, released);
      if (selected->stop && (released || !selected->source_closed ||
                             selected->work->cleanup_failed.load())) {
        if (!released)
          Error(selected->stop, selected->binding, false);
        else {
          auto out = selected->binding.Proof(true);
          const auto work = selected->work;
          const bool valid = work->finalized.load() && !work->failed.load() &&
                             !work->revoked.load();
          out[V("finalized")] = V(valid);
          if (valid)
            if (const auto why = Completion(work->completion))
              out[V("completion")] = V(why);
          auto reply = std::move(selected->stop);
          reply->Success(V(std::move(out)));
        }
      }
      FinishGraph();
    } catch (...) {
      selected->work->Stop(true);
    }
  }
  void Ready(const std::shared_ptr<Job>& selected) {
    if (retired || job != selected || !selected->work->opened.load() ||
        !Allowed(selected, true) || !selected->source ||
        !selected->source->Current()) {
      selected->work->Decide(false);
      Stop(selected, true);
      return;
    }
    const std::weak_ptr<Impl> weak = shared_from_this();
    permission->ConfirmCurrent(
        selected->binding.disclosure, [weak, selected](bool yes) {
          const auto self = weak.lock();
          if (!self) {
            selected->work->Stop(true);
            return;
          }
          bool attached = false;
          if (yes && self->Allowed(selected, true) && selected->source &&
              selected->source->Current()) {
            selected->attaching = true;
            attached = selected->source->Attach();
            selected->attaching = false;
            attached = attached && self->Allowed(selected, true) &&
                       selected->source->Current();
          }
          selected->work->started.store(attached);
          selected->work->Decide(attached);
          if (!attached) {
            self->Stop(selected, true);
            return;
          }
          if (selected->start) {
            auto out = selected->binding.Proof();
            out[V("started")] = V(true);
            auto reply = std::move(selected->start);
            reply->Success(V(std::move(out)));
          }
        });
  }
  void Drain() {
    if (GetCurrentThreadId() != thread) return;
    std::deque<Event> events;
    {
      std::lock_guard<std::mutex> held(dispatch->mutex);
      events.swap(dispatch->events);
    }
    for (const auto& event : events) {
      const auto selected = job;
      if (!selected || selected->work != event.work) continue;
      if (event.ready && !retired)
        Ready(selected);
      else
        Complete(selected);
    }
    FinishGraph();
  }
  void Start(const Map& args, const Binding& binding,
             std::unique_ptr<Reply>& reply) {
    if (!software_ready || !window || retired || !registry ||
        !BindPermission() || !permission->Current(binding.disclosure, true)) {
      Error(reply, binding, true);
      return;
    }
    if (job) {
      if (job->binding == binding) {
        Error(reply, binding, job->Released());
        return;
      }
      if (!job->Released()) {
        Error(reply, binding, true);
        return;
      }
      job.reset();  // Release old native wrappers before a new explicit borrow.
    }
    const auto path = Wide(Text(args, "path"), 32767);
    const auto budget = Integer(args, "fileBudget");
    const bool video = binding.disclosure.kind == "audioVideo";
    const auto account = AccountHash(binding.disclosure.account);
    if (path.empty() || account.empty() || !budget || *budget <= 0 ||
        *budget > 2000000000 ||
        std::filesystem::path(path)
                .parent_path()
                .parent_path()
                .filename()
                .wstring() != std::wstring(account.begin(), account.end())) {
      Error(reply, binding, true);
      return;
    }
    MonitorReceivedTarget target{
        Text(args, "peerConnectionId"), Text(args, "receiverId"),
        Text(args, "trackId"),
        video ? Text(args, "videoReceiverId") : std::string(),
        video ? Text(args, "videoTrackId") : std::string()};
    if (!video &&
        (Field(args, "videoReceiverId") || Field(args, "videoTrackId"))) {
      Error(reply, binding, true);
      return;
    }
    auto selected = std::make_shared<Job>();
    selected->binding = binding;
    selected->work->path = path;
    selected->work->file = binding.file;
    selected->work->video = video;
    selected->work->budget = static_cast<std::uint64_t>(*budget);
    selected->work->support = support();
    selected->work->dispatch = dispatch;
    job =
        selected;  // Before source/file loans and every asynchronous operation.
    const std::weak_ptr<Impl> weak = shared_from_this();
    const std::weak_ptr<Job> purpose = selected;
    selected->source = MonitorReceivedSource::Select(
        *registry, std::move(target),
        [weak, purpose](bool first) {
          const auto self = weak.lock();
          const auto current = purpose.lock();
          return self && current && self->Allowed(current, first);
        },
        registry, selected->work->media);
    if (!selected->source) {
      selected->source_closed = true;
      selected->work->closed.store(true);
      selected->work->native_done.store(true);
      selected->work->failed.store(true);
      Error(reply, binding, true);
      return;
    }
    selected->work->loan = selected->source->scope();
    auto payload = std::make_unique<std::shared_ptr<Work>>(selected->work);
    selected->start = std::move(reply);
    selected->thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, RunWork, payload.get(), 0, nullptr));
    if (!selected->thread) {
      selected->work->failed.store(true);
      selected->work->closed.store(true);
      selected->work->native_done.store(true);
      Stop(selected, true);
    } else
      payload.release();
  }
  void FinishGraph() noexcept {
    if (!retired || !graph_closed || (job && !job->Released())) return;
    // Destroy originals/runtime on their UI thread before the host's drain
    // callback is allowed to remove the selected recording indication.
    job.reset();
    registry.reset();
    if (permission) {
      permission->on_changed = {};
      permission->EngineDrained();
    }
    {
      std::lock_guard<std::mutex> held(dispatch->mutex);
      dispatch->window = nullptr;
      dispatch->events.clear();
    }
    if (window) {
      DestroyWindow(window);
      window = nullptr;
    }
    retained.reset();
  }
  void Retire() noexcept {
    if (GetCurrentThreadId() != thread || retired) return;
    retired = true;
    retained = shared_from_this();
    if (job) {
      job->start.reset();
      job->stop.reset();
      Stop(job, true);
    }
    FinishGraph();
  }
};
MonitorSelectedRecordingBridge::MonitorSelectedRecordingBridge(
    std::shared_ptr<MonitorReceivedRegistry> registry,
    FlutterDesktopMessengerRef core, SupportDirectory support, bool ready)
    : impl_(std::make_shared<Impl>(std::move(registry), core,
                                   std::move(support), ready)) {
  impl_->Initialize();
}
MonitorSelectedRecordingBridge::~MonitorSelectedRecordingBridge() { Retire(); }
void MonitorSelectedRecordingBridge::Retire() noexcept {
  if (impl_) impl_->Retire();
}
void MonitorSelectedRecordingBridge::GraphClosed() noexcept {
  const auto self = impl_;
  if (!self || GetCurrentThreadId() != self->thread) return;
  self->Retire();
  self->graph_closed = true;
  self->registry.reset();
  self->Drain();
}
bool MonitorSelectedRecordingBridge::Handle(
    const flutter::MethodCall<V>& call,
    std::unique_ptr<Reply>& reply) noexcept {
  const auto& name = call.method_name();
  if (name != "nonetWindowsRecordingCapabilities" &&
      name != "nonetWindowsRecordingStart" &&
      name != "nonetWindowsRecordingState" &&
      name != "nonetWindowsRecordingStop")
    return false;
  const auto self = impl_;
  if (!self || GetCurrentThreadId() != self->thread || self->retired)
    return true;
  try {
    self->Drain();
    if (name == "nonetWindowsRecordingCapabilities") {
      auto owned = std::move(reply);
      owned->Success(
          V(self->software_ready && self->window && self->core && self->registry
                ? 5
                : 0));
      return true;
    }
    const auto args =
        call.arguments() ? std::get_if<Map>(call.arguments()) : nullptr;
    const auto binding = args ? Parse(*args) : std::optional<Binding>();
    if (!binding) {
      auto owned = std::move(reply);
      owned->Error("nonet_recording_unavailable",
                   "Local recording unavailable");
      return true;
    }
    if (name == "nonetWindowsRecordingStart") {
      self->Start(*args, *binding, reply);
      return true;
    }
    const auto selected = self->job;
    if (!selected || !(selected->binding == *binding)) {
      Error(reply, *binding, true);
      return true;
    }
    if (name == "nonetWindowsRecordingState") {
      auto out = self->State(selected);
      auto owned = std::move(reply);
      owned->Success(V(std::move(out)));
    } else {
      if (selected->stop) {
        Error(reply, *binding, false);
        return true;
      }
      selected->stop = std::move(reply);
      const bool invalid = !selected->stopping && !selected->normal_end &&
                           !self->Current(selected);
      self->Stop(selected, invalid);
    }
  } catch (...) {
    if (self->job) self->Stop(self->job, true);
    if (reply) {
      auto owned = std::move(reply);
      try {
        owned->Error("nonet_recording_unavailable",
                     "Local recording unavailable");
      } catch (...) {
      }
    }
  }
  return true;
}
}  // namespace nonet
