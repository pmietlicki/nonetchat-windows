#include "monitor_av_fragment_stream.h"

#include <mfapi.h>
#include <mferror.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "monitor_media_stream.h"
#include "monitor_recording_stream.h"
namespace nonet {
namespace {
using fmp4::Bytes;
using fmp4::Stop;
using fmp4::Tag;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
struct State {
  State(QWORD bound, std::uint64_t audio_start, std::uint64_t video_start,
        MonitorAvFragmentStream::LimitCallback notify)
      : budget(bound),
        admission(bound, fmp4::av::Admission::metadata_limit, audio_start,
                  video_start),
        on_limit(std::move(notify)) {}
  const QWORD budget;
  ComPtr<IStream> raw;
  fmp4::av::Admission admission;
  std::mutex mutex;
  std::atomic<bool> retired{false};
  std::atomic<HRESULT> failure{S_OK};
  MonitorAvFragmentStream::LimitCallback on_limit;
  std::vector<BYTE> pending, moof;
  std::optional<fmp4::av::Plan> plan;
  std::optional<fmp4::av::Initialization> init;
  std::optional<QWORD> trailer_begin;
  std::size_t movie_offset = 0, movie_bytes = 0;
  QWORD physical_end = 0, body_left = 0, box_size = 0;
  std::uint32_t box_kind = 0;
  std::size_t header_size = 8;
  bool initialized = false, closed = false, prefix = false, hard_error = false,
       finished = false;

  QWORD Length() const { return physical_end + moof.size() + pending.size(); }
  HRESULT Invalid() {
    prefix = false;
    hard_error = true;
    admission.Retire();
    failure.store(MF_E_INVALID_FILE_FORMAT);
    return MF_E_INVALID_FILE_FORMAT;
  }
  HRESULT Io(HRESULT status) {
    prefix = false;
    hard_error = true;
    admission.IoFailed();
    failure.store(status);
    return status;
  }
  HRESULT Limit(Stop kind = Stop::bytes) {
    admission.ExternalLimit(kind);
    if (admission.stop() != kind) return Invalid();
    failure.store(kMonitorAvRecordingLimit);
    if (on_limit) on_limit();
    return kMonitorAvRecordingLimit;
  }
  HRESULT RawWrite(QWORD at, const BYTE* bytes, ULONG count) {
    if (at > budget || count > budget - at) return Io(STG_E_MEDIUMFULL);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(at);
    auto hr = raw->Seek(position, STREAM_SEEK_SET, nullptr);
    ULONG actual = 0;
    if (SUCCEEDED(hr)) hr = raw->Write(bytes, count, &actual);
    if (FAILED(hr)) return Io(hr);
    if (actual != count) return Io(STG_E_WRITEFAULT);
    physical_end = (std::max)(physical_end, at + count);
    return S_OK;
  }
  HRESULT FullBox() {
    const auto at = physical_end;
    if (box_kind == Tag('m', 'o', 'o', 'f')) {
      if (!initialized || !moof.empty()) return Invalid();
      if (pending.size() > budget - physical_end ||
          8 > budget - physical_end - pending.size())
        return Limit();
      moof = std::move(pending);
      pending.clear();
      box_size = 0;
      return S_OK;
    }
    if (!moof.empty()) return Invalid();
    if (pending.size() > budget - physical_end) return Limit();
    if (box_kind == Tag('m', 'o', 'o', 'v')) {
      if (initialized) return Invalid();
      const fmp4::Bytes metadata{pending.data(), pending.size()};
      init = fmp4::av::Parse(metadata, at);
      const auto root = fmp4::At(metadata, 0, metadata.size);
      const auto movie =
          root ? fmp4::av::Child(metadata, *root, Tag('m', 'v', 'h', 'd'))
               : std::nullopt;
      if (!init || !movie) return Invalid();
      movie_offset = static_cast<std::size_t>(at) + movie->start;
      movie_bytes = movie->size;
    } else if (box_kind == Tag('m', 'f', 'r', 'a')) {
      if (trailer_begin || !admission.Trailer(at, pending.size()))
        return admission.stop() == Stop::bytes ? Limit(Stop::bytes) : Invalid();
      trailer_begin = at;
    } else if (initialized || (box_kind != Tag('f', 't', 'y', 'p') &&
                               box_kind != Tag('u', 'u', 'i', 'd') &&
                               box_kind != Tag('p', 'd', 'i', 'n')))
      return Invalid();
    const auto written =
        RawWrite(at, pending.data(), static_cast<ULONG>(pending.size()));
    if (FAILED(written)) return written;
    if (box_kind == Tag('m', 'o', 'o', 'v')) {
      initialized = admission.Initialize({pending.data(), pending.size()}, at,
                                         physical_end);
      if (!initialized) return Invalid();
    }
    pending.clear();
    box_size = 0;
    return S_OK;
  }
  HRESULT Mdat() {
    if (moof.empty() || !initialized) return Invalid();
    plan = admission.Reserve(moof.data(), moof.size(),
                             {pending.data(), pending.size()}, physical_end);
    if (!plan) {
      const auto reason = admission.stop();
      if (reason == Stop::bytes || reason == Stop::duration ||
          reason == Stop::queue)
        return Limit(reason);
      return Invalid();
    }
    auto hr =
        RawWrite(physical_end, moof.data(), static_cast<ULONG>(moof.size()));
    if (SUCCEEDED(hr))
      hr = RawWrite(physical_end, pending.data(),
                    static_cast<ULONG>(pending.size()));
    if (FAILED(hr)) return hr;
    body_left = box_size - header_size;
    moof.clear();
    pending.clear();
    box_size = 0;
    if (!body_left) return Invalid();
    return S_OK;
  }
  HRESULT Append(const BYTE* data, ULONG count, ULONG* actual) {
    ULONG consumed = 0;
    *actual = 0;
    while (consumed < count) {
      if (FAILED(failure.load())) return failure.load();
      if (body_left) {
        const auto part =
            static_cast<ULONG>((std::min)(body_left, QWORD(count - consumed)));
        const auto hr = RawWrite(physical_end, data + consumed, part);
        if (FAILED(hr)) return hr;
        body_left -= part;
        consumed += part;
        *actual = consumed;
        if (!body_left) {
          if (!plan || !admission.Commit(*plan, physical_end)) return Invalid();
          plan.reset();
        }
        continue;
      }
      if (box_size && box_kind != Tag('m', 'd', 'a', 't') &&
          pending.size() == box_size) {
        const auto hr = FullBox();
        if (FAILED(hr)) return hr;
        continue;
      }
      if (pending.empty() && !box_size) header_size = 8;
      auto target = box_size && box_kind != Tag('m', 'd', 'a', 't')
                        ? box_size
                        : QWORD(header_size);
      if (target < pending.size()) return Invalid();
      const auto part = static_cast<ULONG>(
          (std::min)(QWORD(count - consumed), target - pending.size()));
      const auto limit = fmp4::av::Admission::metadata_limit;
      if (moof.size() > limit || pending.size() > limit - moof.size() ||
          part > limit - moof.size() - pending.size())
        return Invalid();
      if (Length() > budget || part > budget - Length()) return Limit();
      if (!part) return Invalid();
      pending.insert(pending.end(), data + consumed, data + consumed + part);
      consumed += part;
      *actual = consumed;
      if (pending.size() < header_size) continue;
      if (!box_size) {
        const Bytes header{pending.data(), pending.size()};
        const auto size = header.U32(0), kind = header.U32(4);
        if (!size || !kind) return Invalid();
        box_kind = *kind;
        if (box_kind != Tag('f', 't', 'y', 'p') &&
            box_kind != Tag('u', 'u', 'i', 'd') &&
            box_kind != Tag('p', 'd', 'i', 'n') &&
            box_kind != Tag('m', 'o', 'o', 'v') &&
            box_kind != Tag('m', 'o', 'o', 'f') &&
            box_kind != Tag('m', 'd', 'a', 't') &&
            box_kind != Tag('m', 'f', 'r', 'a'))
          return Invalid();
        if (*size == 1 && header_size == 8) {
          header_size = 16;
          continue;
        }
        if (*size == 1) {
          const auto large = header.U64(8);
          if (!large) return Invalid();
          box_size = *large;
        } else
          box_size = *size;
        if (box_size < header_size) return Invalid();
        if (box_kind != Tag('m', 'd', 'a', 't') &&
            box_size > limit - moof.size())
          return Invalid();
      }
      if (box_kind == Tag('m', 'd', 'a', 't')) {
        const auto hr = Mdat();
        if (FAILED(hr)) return hr;
      } else if (pending.size() == box_size) {
        const auto hr = FullBox();
        if (FAILED(hr)) return hr;
      }
    }
    return S_OK;
  }
  HRESULT Finish() {
    retired.store(true);
    std::lock_guard<std::mutex> held(mutex);
    if (finished) return S_OK;
    if (!raw) {
      finished = true;
      return S_OK;
    }
    if (SUCCEEDED(failure.load()) &&
        (!initialized || body_left || !pending.empty() || !moof.empty()))
      Invalid();
    const bool quota = failure.load() == kMonitorAvRecordingLimit;
    if (!hard_error && (SUCCEEDED(failure.load()) || quota)) {
      // MF may choose the video movie clock initially, then replace it with
      // the audio clock during Finalize. Read that closed header from the same
      // physical stream, preserving its exact original location/version.
      std::array<BYTE, 16 + 4 + 8 + 8 + 4 + 8> prefix_bytes{};
      const auto count =
          static_cast<ULONG>((std::min)(movie_bytes, prefix_bytes.size()));
      LARGE_INTEGER at{};
      at.QuadPart = static_cast<LONGLONG>(movie_offset);
      auto status = raw->Seek(at, STREAM_SEEK_SET, nullptr);
      ULONG actual = 0;
      if (SUCCEEDED(status))
        status = raw->Read(prefix_bytes.data(), count, &actual);
      if (FAILED(status) || actual != count)
        return Io(FAILED(status) ? status : STG_E_READFAULT);
      const fmp4::Bytes view{prefix_bytes.data(), count};
      const auto compact = view.U32(0), kind = view.U32(4);
      if (!compact || kind != Tag('m', 'v', 'h', 'd')) return Invalid();
      const std::size_t header = *compact == 1 ? 16 : 8;
      const auto length =
          *compact == 1 ? view.U64(8) : std::optional<std::uint64_t>(*compact);
      if (!length || *length != movie_bytes || !view.Range(header, 4))
        return Invalid();
      const auto version = view.data[header];
      if (version > 1) return Invalid();
      const auto scale_at = header + (version ? 20 : 12);
      const auto scale = view.U32(scale_at);
      if (!init || !scale ||
          movie_offset + scale_at + 4 != init->movie_duration.offset ||
          (version ? 8u : 4u) != init->movie_duration.width)
        return Invalid();
      const auto patches = admission.ClosedPatches(true, quota, *scale);
      if (!patches)
        Invalid();
      else
        for (const auto& patch : *patches) {
          BYTE bytes[8]{};
          auto value = patch.value;
          for (std::size_t n = patch.width; n > 0; --n) {
            bytes[n - 1] = static_cast<BYTE>(value);
            value >>= 8;
          }
          const auto hr =
              RawWrite(patch.offset, bytes, static_cast<ULONG>(patch.width));
          if (FAILED(hr)) return hr;
        }
      if (quota && patches && !hard_error) prefix = true;
    }
    if (!hard_error && trailer_begin &&
        (SUCCEEDED(failure.load()) || quota)) {
      // MF's optional random-access footer can retain different times from
      // the validated fragments for irregular live video. Finalize this NEW
      // file without that optional index (FFmpeg's skip_trailer profile).
      // The synchronous writer has already retired; keep every media byte and
      // the strict reader's timeline checks. Never alter an existing archive.
      ULARGE_INTEGER end{};
      end.QuadPart = *trailer_begin;
      const auto status = raw->SetSize(end);
      if (status != S_OK) return Io(FAILED(status) ? status : E_FAIL);
      physical_end = *trailer_begin;
    }
    const auto hr = raw ? raw->Commit(STGC_DEFAULT) : S_OK;
    if (hr != S_OK) return Io(FAILED(hr) ? hr : E_FAIL);
    finished = true;
    pending.clear();
    moof.clear();
    return S_OK;
  }
};
class Stream final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IStream> {
 public:
  explicit Stream(std::shared_ptr<State> owned_state, QWORD position = 0)
      : state_(std::move(owned_state)), position_(position) {}
  template <class Call>
  HRESULT Locked(Call&& call) noexcept {
    try {
      std::lock_guard<std::mutex> held(state_->mutex);
      if (state_->retired.load() || state_->closed) return STG_E_REVERTED;
      try {
        return call();
      } catch (const std::bad_alloc&) {
        return state_->Io(E_OUTOFMEMORY);
      } catch (...) {
        return state_->Io(E_FAIL);
      }
    } catch (...) {
      state_->retired.store(true);
      state_->failure.store(E_FAIL);
      return E_FAIL;
    }
  }
  STDMETHODIMP Read(void* bytes, ULONG count, ULONG* actual) override {
    if (actual) *actual = 0;
    if (count && !bytes) return STG_E_INVALIDPOINTER;
    return Locked([&]() -> HRESULT {
      if (FAILED(state_->failure.load())) return state_->failure.load();
      ULONG copied = 0;
      auto* out = static_cast<BYTE*>(bytes);
      if (position_ < state_->physical_end) {
        LARGE_INTEGER p{};
        p.QuadPart = static_cast<LONGLONG>(position_);
        auto hr = state_->raw->Seek(p, STREAM_SEEK_SET, nullptr);
        const auto part = static_cast<ULONG>(
            (std::min)(QWORD(count), state_->physical_end - position_));
        if (SUCCEEDED(hr)) hr = state_->raw->Read(out, part, &copied);
        if (FAILED(hr) || copied != part)
          return state_->Io(FAILED(hr) ? hr : STG_E_READFAULT);
        position_ += copied;
      }
      if (copied < count && position_ >= state_->physical_end) {
        auto offset = position_ - state_->physical_end;
        for (const auto* buffer : {&state_->moof, &state_->pending}) {
          if (offset >= buffer->size()) {
            offset -= buffer->size();
            continue;
          }
          const auto part = static_cast<ULONG>(
              (std::min)(std::size_t(count - copied),
                         buffer->size() - static_cast<std::size_t>(offset)));
          std::memcpy(out + copied,
                      buffer->data() + static_cast<std::size_t>(offset), part);
          copied += part;
          position_ += part;
          offset = 0;
        }
      }
      if (actual) *actual = copied;
      return copied == count ? S_OK : S_FALSE;
    });
  }
  STDMETHODIMP Write(const void* bytes, ULONG count, ULONG* actual) override {
    if (actual) *actual = 0;
    if (count && !bytes) return STG_E_INVALIDPOINTER;
    return Locked([&]() -> HRESULT {
      if (FAILED(state_->failure.load())) return state_->failure.load();
      const auto* data = static_cast<const BYTE*>(bytes);
      ULONG consumed = 0;
      HRESULT hr;
      if (position_ == state_->Length())
        hr = state_->Append(data, count, &consumed);
      else if (state_->init && state_->moof.empty() &&
               state_->pending.empty() && !state_->body_left &&
               position_ >= state_->movie_offset &&
               position_ <= state_->movie_offset + state_->movie_bytes &&
               count <=
                   state_->movie_offset + state_->movie_bytes - position_) {
        hr = state_->RawWrite(position_, data, count);
        if (SUCCEEDED(hr)) consumed = count;
      } else
        hr = state_->Invalid();
      position_ += consumed;
      if (actual) *actual = consumed;
      return hr;
    });
  }
  STDMETHODIMP Seek(LARGE_INTEGER delta, DWORD origin,
                    ULARGE_INTEGER* actual) override {
    return Locked([&]() -> HRESULT {
      QWORD base = 0;
      if (origin == STREAM_SEEK_CUR)
        base = position_;
      else if (origin == STREAM_SEEK_END)
        base = state_->Length();
      else if (origin != STREAM_SEEK_SET)
        return STG_E_INVALIDFUNCTION;
      QWORD target = 0;
      if (delta.QuadPart < 0) {
        const auto distance = QWORD(-(delta.QuadPart + 1)) + 1;
        if (distance > base) return STG_E_INVALIDFUNCTION;
        target = base - distance;
      } else {
        const auto distance = QWORD(delta.QuadPart);
        if (base > state_->budget || distance > state_->budget - base)
          return STG_E_MEDIUMFULL;
        target = base + distance;
      }
      if (target > state_->budget) return STG_E_MEDIUMFULL;
      position_ = target;
      if (actual) actual->QuadPart = target;
      return S_OK;
    });
  }
  STDMETHODIMP SetSize(ULARGE_INTEGER size) override {
    return Locked([&]() -> HRESULT {
      return size.QuadPart == state_->Length() ? S_OK : STG_E_INVALIDFUNCTION;
    });
  }
  STDMETHODIMP Commit(DWORD flags) override {
    return Locked([&]() -> HRESULT {
      if (flags != STGC_DEFAULT) return STG_E_INVALIDFLAG;
      if (FAILED(state_->failure.load())) return state_->failure.load();
      if (!state_->pending.empty() || !state_->moof.empty() ||
          state_->body_left)
        return E_PENDING;
      const auto hr = state_->raw->Commit(STGC_DEFAULT);
      return hr == S_OK ? S_OK : state_->Io(FAILED(hr) ? hr : E_FAIL);
    });
  }
  STDMETHODIMP Stat(STATSTG* output, DWORD flags) override {
    if (!output) return STG_E_INVALIDPOINTER;
    std::memset(output, 0, sizeof(*output));
    return Locked([&]() -> HRESULT {
      if (flags != STATFLAG_DEFAULT && flags != STATFLAG_NONAME)
        return STG_E_INVALIDFLAG;
      output->type = STGTY_STREAM;
      output->cbSize.QuadPart = state_->Length();
      output->grfMode = STGM_READWRITE | STGM_SHARE_EXCLUSIVE;
      return S_OK;
    });
  }
  STDMETHODIMP Clone(IStream** result) override {
    if (!result) return STG_E_INVALIDPOINTER;
    *result = nullptr;
    return Locked([&]() -> HRESULT {
      const auto copy = Make<Stream>(state_, position_);
      if (!copy) return E_OUTOFMEMORY;
      *result = copy.Get();
      (*result)->AddRef();
      return S_OK;
    });
  }
  STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                      ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP Revert() override { return STG_E_INVALIDFUNCTION; }
  STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }

 private:
  std::shared_ptr<State> state_;
  QWORD position_;
};
}  // namespace
struct MonitorAvFragmentStream::Impl {
  explicit Impl(LimitCallback notify) : callback(std::move(notify)) {}
  const DWORD thread = GetCurrentThreadId();
  LimitCallback callback;
  MonitorRecordingStream file;
  std::shared_ptr<State> state;
  ComPtr<IMFByteStream> stream;
  bool attempted = false, native_closed = false, closed = false;
};
MonitorAvFragmentStream::MonitorAvFragmentStream(LimitCallback callback)
    : impl_(std::make_unique<Impl>(std::move(callback))) {}
MonitorAvFragmentStream::~MonitorAvFragmentStream() {
  if (GetCurrentThreadId() == impl_->thread) {
    const auto ignored = Close();
    (void)ignored;
  }
}
HRESULT MonitorAvFragmentStream::Create(
    const std::wstring& path, std::uint64_t budget,
    std::uint64_t audio_start_hns, std::uint64_t video_start_hns) noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return RPC_E_WRONG_THREAD;
  if (self.attempted || self.closed) return MF_E_INVALIDREQUEST;
  self.attempted = true;
  if (!budget || budget > fmp4::av::Admission::max_bytes) return E_INVALIDARG;
  try {
    // Both owners are registered before any file or async adapter acquisition.
    self.state = std::make_shared<State>(
        budget, audio_start_hns, video_start_hns, std::move(self.callback));
    if (self.state->admission.stop() != fmp4::Stop::none) return E_INVALIDARG;
    auto hr = self.file.CreateStorage(path, budget);
    if (FAILED(hr)) return hr;
    self.state->raw = self.file.storage();
    auto stream = Make<Stream>(self.state);
    if (!stream) return E_OUTOFMEMORY;
    hr = MFCreateMFByteStreamOnStream(stream.Get(), &self.stream);
    if (FAILED(hr)) return hr;
    auto confirmed = Make<ConfirmedMonitorByteStream>(self.stream.Get());
    if (!confirmed) return E_OUTOFMEMORY;
    self.stream = confirmed;
    return S_OK;
  } catch (...) {
    return E_FAIL;
  }
}
IMFByteStream* MonitorAvFragmentStream::stream() const noexcept {
  return impl_->stream.Get();
}
HRESULT MonitorAvFragmentStream::failure() const noexcept {
  const auto physical = impl_->file.failure();
  if (FAILED(physical)) return physical;
  return impl_->state ? impl_->state->failure.load() : S_OK;
}
HRESULT MonitorAvFragmentStream::Close() noexcept {
  auto& self = *impl_;
  if (GetCurrentThreadId() != self.thread) return RPC_E_WRONG_THREAD;
  if (self.closed) return S_OK;
  try {
    if (self.stream && !self.native_closed) {
      const auto hr = self.stream->Close();
      if (hr != S_OK) return hr;
      self.native_closed = true;
    }
    if (self.state) {
      // MF work is positively closed before patching. Close the HANDLE only
      // on this owning worker; failed close retains the complete capability.
      const auto hr = self.state->Finish();
      if (hr != S_OK) return hr;
    }
    const auto hr = self.file.Close();
    if (hr != S_OK) return hr;
    if (self.state) self.state->closed = true;
    self.stream.Reset();
    self.closed = true;
    return S_OK;
  } catch (...) {
    return E_FAIL;
  }
}
bool MonitorAvFragmentStream::quota_prefix() const noexcept {
  return GetCurrentThreadId() == impl_->thread && impl_->closed &&
         impl_->state && impl_->state->prefix && !impl_->state->hard_error;
}
fmp4::Stop MonitorAvFragmentStream::limit() const noexcept {
  return GetCurrentThreadId() == impl_->thread && impl_->state
             ? impl_->state->admission.stop()
             : fmp4::Stop::none;
}
}  // namespace nonet
