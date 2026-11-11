#include "monitor_recording_stream.h"
#include "monitor_media_stream.h"

#include <mfapi.h>
#include <mferror.h>
#include <objidl.h>
#include <wrl.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace nonet {
namespace {
HRESULT LastError() noexcept {
  const auto error = GetLastError();
  return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
}

struct FileState {
  FileState(HANDLE owned, std::uint64_t limit) : handle(owned), budget(limit) {}
  ~FileState() {
    // Last resort only. The owner retains this state on an uncertain Close.
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
  HANDLE handle;
  const std::uint64_t budget;
  std::mutex mutex;
  std::atomic<bool> retired{false};
  std::atomic<HRESULT> error{S_OK};

  HRESULT Fail(HRESULT status) noexcept {
    HRESULT expected = S_OK;
    if (FAILED(status)) error.compare_exchange_strong(expected, status);
    return status;
  }
  HRESULT Close() {
    retired.store(true);  // Fence queued aliases before waiting for active IO.
    std::lock_guard<std::mutex> lock(mutex);
    if (handle == INVALID_HANDLE_VALUE) return S_OK;
    if (!FlushFileBuffers(handle)) return LastError();
    if (!CloseHandle(handle)) return LastError();
    handle = INVALID_HANDLE_VALUE;
    return S_OK;
  }
};

// MFCreateMFByteStreamOnStream owns asynchronous dispatch. Its IStream calls
// reach this physical boundary, including Write and SetSize on cloned cursors.
// One HANDLE is shared; each COM stream has its independent logical position.
class LimitedStream final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IStream> {
 public:
  explicit LimitedStream(std::shared_ptr<FileState> state, std::uint64_t position = 0)
      : state_(std::move(state)), position_(position) {}

  STDMETHODIMP Read(void* bytes, ULONG count, ULONG* actual) override {
    if (actual) *actual = 0;
    if (count && !bytes) return STG_E_INVALIDPOINTER;
    return Locked([&]() -> HRESULT {
      const auto positioned = PositionHandle();
      if (FAILED(positioned)) return positioned;
      DWORD received = 0;
      if (!ReadFile(state_->handle, bytes, count, &received, nullptr))
        return state_->Fail(LastError());
      position_ += received;
      if (actual) *actual = received;
      return received == count ? S_OK : S_FALSE;
    });
  }
  STDMETHODIMP Write(const void* bytes, ULONG count, ULONG* actual) override {
    if (actual) *actual = 0;
    if (count && !bytes) return STG_E_INVALIDPOINTER;
    return Locked([&]() -> HRESULT {
      if (position_ > state_->budget || count > state_->budget - position_)
        return state_->Fail(STG_E_MEDIUMFULL);
      const auto positioned = PositionHandle();
      if (FAILED(positioned)) return positioned;
      DWORD written = 0;
      const bool complete = WriteFile(state_->handle, bytes, count, &written, nullptr) != FALSE;
      const auto status = complete ? S_OK : state_->Fail(LastError());
      position_ += written;
      if (actual) *actual = written;
      return status;
    });
  }
  STDMETHODIMP Seek(LARGE_INTEGER delta, DWORD origin, ULARGE_INTEGER* result) override {
    return Locked([&]() -> HRESULT {
      std::uint64_t base = 0;
      if (origin == STREAM_SEEK_CUR) base = position_;
      else if (origin == STREAM_SEEK_END) {
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(state_->handle, &length)) return state_->Fail(LastError());
        if (length.QuadPart < 0) return state_->Fail(E_UNEXPECTED);
        base = static_cast<std::uint64_t>(length.QuadPart);
      } else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
      std::uint64_t next;
      if (delta.QuadPart < 0) {
        // Avoid negating LLONG_MIN.
        const auto distance = static_cast<std::uint64_t>(-(delta.QuadPart + 1)) + 1;
        if (distance > base) return STG_E_INVALIDFUNCTION;
        next = base - distance;
      } else {
        const auto distance = static_cast<std::uint64_t>(delta.QuadPart);
        if (base > state_->budget || distance > state_->budget - base)
          return state_->Fail(STG_E_MEDIUMFULL);
        next = base + distance;
      }
      if (next > state_->budget) return state_->Fail(STG_E_MEDIUMFULL);
      position_ = next;
      if (result) result->QuadPart = next;
      return S_OK;
    });
  }
  STDMETHODIMP SetSize(ULARGE_INTEGER size) override {
    return Locked([&]() -> HRESULT {
      if (size.QuadPart > state_->budget) return state_->Fail(STG_E_MEDIUMFULL);
      LARGE_INTEGER position{};
      position.QuadPart = static_cast<LONGLONG>(size.QuadPart);
      if (!SetFilePointerEx(state_->handle, position, nullptr, FILE_BEGIN) ||
          !SetEndOfFile(state_->handle)) return state_->Fail(LastError());
      // IStream::SetSize does not change the logical cursor.
      return S_OK;
    });
  }
  STDMETHODIMP Commit(DWORD flags) override {
    return Locked([&]() -> HRESULT {
      if (flags != STGC_DEFAULT) return STG_E_INVALIDFLAG;
      return FlushFileBuffers(state_->handle) ? S_OK : state_->Fail(LastError());
    });
  }
  STDMETHODIMP Stat(STATSTG* stat, DWORD flags) override {
    if (!stat) return STG_E_INVALIDPOINTER;
    std::memset(stat, 0, sizeof(*stat));
    return Locked([&]() -> HRESULT {
      if (flags != STATFLAG_DEFAULT && flags != STATFLAG_NONAME) return STG_E_INVALIDFLAG;
      LARGE_INTEGER length{};
      if (!GetFileSizeEx(state_->handle, &length)) return state_->Fail(LastError());
      if (length.QuadPart < 0) return state_->Fail(E_UNEXPECTED);
      stat->type = STGTY_STREAM;
      stat->cbSize.QuadPart = static_cast<ULONGLONG>(length.QuadPart);
      stat->grfMode = STGM_READWRITE | STGM_SHARE_EXCLUSIVE;
      return S_OK;
    });
  }
  STDMETHODIMP Clone(IStream** result) override {
    if (!result) return STG_E_INVALIDPOINTER;
    *result = nullptr;
    return Locked([&]() -> HRESULT {
      auto copy = Microsoft::WRL::Make<LimitedStream>(state_, position_);
      if (!copy) return E_OUTOFMEMORY;
      *result = copy.Detach();
      return S_OK;
    });
  }
  STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
  STDMETHODIMP Revert() override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }

 private:
  template <typename Call> HRESULT Locked(Call&& call) noexcept {
    try {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->retired.load() || state_->handle == INVALID_HANDLE_VALUE) return STG_E_REVERTED;
      return call();
    } catch (...) { return state_->Fail(E_FAIL); }
  }
  HRESULT PositionHandle() {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(position_);
    return SetFilePointerEx(state_->handle, position, nullptr, FILE_BEGIN)
        ? S_OK : state_->Fail(LastError());
  }
  std::shared_ptr<FileState> state_;
  std::uint64_t position_;
};
}  // namespace

struct MonitorRecordingStream::Impl {
  const DWORD thread = GetCurrentThreadId();
  std::shared_ptr<FileState> file;
  Microsoft::WRL::ComPtr<IStream> storage;
  Microsoft::WRL::ComPtr<IMFByteStream> stream;
  bool attempted = false, stream_closed = false, closed = false;
};

MonitorRecordingStream::MonitorRecordingStream() : impl_(std::make_unique<Impl>()) {}
MonitorRecordingStream::~MonitorRecordingStream() {
  if (GetCurrentThreadId() == impl_->thread) { const auto ignored = Close(); (void)ignored; }
}
HRESULT MonitorRecordingStream::Create(const std::wstring& path, std::uint64_t budget) noexcept {
  return Open(path, budget, true);
}
HRESULT MonitorRecordingStream::CreateStorage(const std::wstring& path, std::uint64_t budget) noexcept {
  return Open(path, budget, false);
}
IStream* MonitorRecordingStream::storage() const noexcept { return impl_->storage.Get(); }
HRESULT MonitorRecordingStream::Open(const std::wstring& path, std::uint64_t budget, bool adapt) noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return RPC_E_WRONG_THREAD;
  if (self.attempted || self.closed) return MF_E_INVALIDREQUEST;
  self.attempted = true;
  if (!budget || budget > max_bytes) return E_INVALIDARG;
  try {
    // Allocate ownership before acquisition: even an allocation exception must
    // not leave an acquired HANDLE with only best-effort cleanup.
    self.file = std::make_shared<FileState>(INVALID_HANDLE_VALUE, budget);
    self.file->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (self.file->handle == INVALID_HANDLE_VALUE) return LastError();
    self.storage = Microsoft::WRL::Make<LimitedStream>(self.file);
    if (!self.storage) return E_OUTOFMEMORY;
    if (!adapt) return S_OK;
    const auto opened = MFCreateMFByteStreamOnStream(self.storage.Get(), &self.stream);
    if (FAILED(opened)) return opened;
    auto confirmed = Microsoft::WRL::Make<ConfirmedMonitorByteStream>(self.stream.Get());
    if (!confirmed) return E_OUTOFMEMORY;
    self.stream = confirmed;
    return S_OK;
  } catch (...) { return E_FAIL; }
}
IMFByteStream* MonitorRecordingStream::stream() const noexcept { return impl_->stream.Get(); }
HRESULT MonitorRecordingStream::failure() const noexcept { return impl_->file ? impl_->file->error.load() : S_OK; }
HRESULT MonitorRecordingStream::Close() noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return RPC_E_WRONG_THREAD;
  if (self.closed) return S_OK;
  try {
    if (self.stream && !self.stream_closed) {
      const auto closed = self.stream->Close();
      if (closed != S_OK) return closed;
      self.stream_closed = true;
      self.stream.Reset();
    }
    if (self.file) { const auto closed = self.file->Close(); if (closed != S_OK) return closed; }
    self.storage.Reset();
    self.closed = true;
    return S_OK;
  } catch (...) { return E_FAIL; }
}
}  // namespace nonet
