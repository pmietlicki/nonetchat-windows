#include "monitor_preview_texture.h"
#include "local_pcm_queue.h"
#include <atomic>
#include <limits>
#include <mutex>
#include <utility>
namespace nonet {
struct MonitorPreviewTexture::Impl : std::enable_shared_from_this<Impl> {
  struct Ledger {
    std::atomic<std::size_t> bytes{0}, borrows{0};
    std::weak_ptr<Impl> owner;
    void Changed() noexcept {
      if (const auto state = owner.lock()) state->Changed();
    }
  };
  struct Pixel {
    std::atomic<std::size_t> references{1};
    const std::shared_ptr<Ledger> ledger;
    std::vector<std::uint8_t> bytes;
    FlutterDesktopPixelBuffer descriptor{};
    Pixel(std::shared_ptr<Ledger> l, std::vector<std::uint8_t>&& data)
        : ledger(std::move(l)), bytes(std::move(data)) {
      descriptor.buffer = bytes.data();
      descriptor.width = 640;
      descriptor.height = 480;
      descriptor.release_context = this;
      descriptor.release_callback = [](void* raw) {
        auto* pixel = static_cast<Pixel*>(raw);
        const auto held = pixel->ledger;
        held->borrows.fetch_sub(1);
        Drop(pixel);
        held->Changed();
      };
    }
    static void Drop(Pixel* value) noexcept {
      if (value && value->references.fetch_sub(1) == 1) {
        const auto held = value->ledger;
        const auto count = value->bytes.size();
        delete value;  // Physically free the pixels before publishing the
                       // drain.
        held->bytes.fetch_sub(count);
        held->Changed();
      }
    }
  };
  const DWORD ui = GetCurrentThreadId();
  mutable std::mutex mutex;
  flutter::TextureRegistrar* registrar;
  const Wake wake;
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<flutter::TextureVariant> texture;
  // Explicit ownership through an uncertain registration/engine retirement.
  std::shared_ptr<Impl> retained;
  Pixel* current = nullptr;
  std::int64_t id = -1;
  std::size_t preparing = 0;
  bool attempted = false, live = true, dirty = false, image = false,
       failed = false;
  bool native_call = false, unregister_started = false, unregistered = false,
       closed = false;
  Impl(flutter::TextureRegistrar* r, Wake w)
      : registrar(r), wake(std::move(w)) {}
  ~Impl() { Pixel::Drop(current); }
  void Notify() noexcept {
    try {
      if (wake) wake();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      failed = true;
    }
  }
  void Changed() noexcept {
    try {
      std::shared_ptr<Impl> release;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!live && unregistered && !native_call && !preparing && !current &&
            ledger->bytes.load() == 0 && ledger->borrows.load() == 0) {
          closed = true;
          release.swap(retained);
        }
      }
      Notify();
    } catch (...) { /* Uncertain cleanup never manufactures a positive proof. */
    }
  }
  const FlutterDesktopPixelBuffer* Copy() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      if (!live || !current || !attempted || failed ||
          current->references.load() ==
              std::numeric_limits<std::size_t>::max() ||
          ledger->borrows.load() == std::numeric_limits<std::size_t>::max())
        return nullptr;
      current->references.fetch_add(1);
      ledger->borrows.fetch_add(1);
      return &current->descriptor;
    } catch (...) {
      return nullptr;
    }
  }
};
MonitorPreviewTexture::MonitorPreviewTexture(
    flutter::TextureRegistrar* registrar, Wake wake)
    : impl_(std::make_shared<Impl>(registrar, std::move(wake))) {
  impl_->ledger->owner = impl_;
}
MonitorPreviewTexture::~MonitorPreviewTexture() { Retire(); }
HRESULT MonitorPreviewTexture::Register() noexcept {
  const auto s = impl_;
  if (GetCurrentThreadId() != s->ui) return RPC_E_WRONG_THREAD;
  bool called = false;
  try {
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      if (s->attempted || !s->live) return E_UNEXPECTED;
      s->attempted = true;
      if (!s->registrar) {
        s->failed = true;
        s->live = false;
        s->unregistered = true;
        s->closed = true;
        return E_POINTER;
      }
      s->native_call = true;
      s->retained = s;
    }
    const auto weak = std::weak_ptr<Impl>(s);
    s->texture = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([weak](std::size_t, std::size_t) {
          const auto state = weak.lock();
          return state ? state->Copy() : nullptr;
        }));
    called = true;
    const auto id = s->registrar->RegisterTexture(s->texture.get());
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->native_call = false;
      s->id = id;
      if (id < 0) {
        s->failed = true;
        s->live = false;
        s->unregistered = true;
      }
    }
    bool live = false;
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      live = s->live;
    }
    s->Changed();
    if (!live) ContinueClose();
    return id < 0 ? E_FAIL : live ? S_OK : E_ABORT;
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->native_call = false;
      s->live = false;
      s->failed = true;
      if (!called) s->unregistered = true;
    }
    s->Changed();
    return E_FAIL;
  }
}
HRESULT MonitorPreviewTexture::Publish(MonitorPreviewFrame&& frame) noexcept {
  const auto s = impl_;
  constexpr std::size_t count = 640 * 480 * 4;
  if (frame.width != 640 || frame.height != 480 || frame.rgba.size() != count)
    return E_INVALIDARG;
  try {
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      if (!s->live || s->failed || s->id < 0) return E_ABORT;
      const auto bytes = s->ledger->bytes.load();
      if (bytes > LocalPcmQueue::max_bytes ||
          count > LocalPcmQueue::max_bytes - bytes)
        return S_FALSE;
      s->ledger->bytes.fetch_add(count);
      ++s->preparing;
    }
    Impl::Pixel* next = nullptr;
    try {
      next = new Impl::Pixel(s->ledger, std::move(frame.rgba));
    } catch (...) {
      s->ledger->bytes.fetch_sub(count);
      {
        std::lock_guard<std::mutex> lock(s->mutex);
        --s->preparing;
      }
      s->Changed();
      return E_OUTOFMEMORY;
    }
    Impl::Pixel* old = nullptr;
    bool admitted = false;
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      --s->preparing;
      if (s->live && !s->failed) {
        old = std::exchange(s->current, next);
        s->dirty = true;
        admitted = true;
      }
    }
    if (admitted)
      Impl::Pixel::Drop(old);
    else
      Impl::Pixel::Drop(next);
    s->Changed();
    return admitted ? S_OK : E_ABORT;
  } catch (...) {
    return E_FAIL;
  }
}
HRESULT MonitorPreviewTexture::Update() noexcept {
  const auto s = impl_;
  if (GetCurrentThreadId() != s->ui) return RPC_E_WRONG_THREAD;
  flutter::TextureRegistrar* registrar = nullptr;
  std::int64_t id = -1;
  try {
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      if (!s->live || s->failed || s->id < 0 || !s->registrar) return E_ABORT;
      if (s->native_call || !s->dirty) return S_FALSE;
      s->native_call = true;
      s->dirty = false;
      registrar = s->registrar;
      id = s->id;
    }
    bool marked = false;
    try {
      marked = registrar->MarkTextureFrameAvailable(id);
    } catch (...) {
    }
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->native_call = false;
      if (!marked) {
        s->failed = true;
      } else if (s->live)
        s->image = true;
    }
    bool live = false;
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      live = s->live;
    }
    s->Changed();
    if (!live) ContinueClose();
    return marked ? S_OK : E_FAIL;
  } catch (...) {
    return E_FAIL;
  }
}
void MonitorPreviewTexture::Retire() noexcept {
  const auto s = impl_;
  if (!s) return;
  try {
    Impl::Pixel* old = nullptr;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      changed = s->live || s->dirty || s->image || s->current ||
                (!s->attempted && !s->unregistered);
      s->live = false;
      s->dirty = false;
      s->image = false;
      old = std::exchange(s->current, nullptr);
      if (!s->attempted) s->unregistered = true;
    }
    Impl::Pixel::Drop(old);
    if (changed) s->Changed();
  } catch (...) {
  }
}
HRESULT MonitorPreviewTexture::ContinueClose() noexcept {
  const auto s = impl_;
  if (GetCurrentThreadId() != s->ui) return RPC_E_WRONG_THREAD;
  Retire();
  flutter::TextureRegistrar* registrar = nullptr;
  std::int64_t id = -1;
  try {
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      if (s->closed) return S_OK;
      if (s->native_call || s->unregister_started || s->unregistered ||
          s->id < 0 || !s->registrar)
        return S_FALSE;
      s->unregister_started = true;
      s->native_call = true;
      registrar = s->registrar;
      id = s->id;
    }
    try {
      registrar->UnregisterTexture(id, [s] {
        {
          std::lock_guard<std::mutex> lock(s->mutex);
          s->unregistered = true;
        }
        s->Changed();
      });
    } catch (...) {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->failed = true;
    }
    {
      std::lock_guard<std::mutex> lock(s->mutex);
      s->native_call = false;
    }
    s->Changed();
    return State().closed ? S_OK : S_FALSE;
  } catch (...) {
    return E_FAIL;
  }
}
HRESULT MonitorPreviewTexture::EngineRetired() noexcept {
  const auto s = impl_;
  if (GetCurrentThreadId() != s->ui) return RPC_E_WRONG_THREAD;
  const auto hr = ContinueClose();
  {
    std::lock_guard<std::mutex> lock(s->mutex);
    s->registrar = nullptr;
  }
  return hr;
}
MonitorPreviewTextureState MonitorPreviewTexture::State() const noexcept {
  const auto s = impl_;
  try {
    std::lock_guard<std::mutex> lock(s->mutex);
    return {s->id,
            s->id >= 0 && !s->unregistered,
            s->image && s->live && !s->failed && s->current,
            !s->live,
            s->closed,
            s->failed,
            s->ledger->bytes.load(),
            s->ledger->borrows.load()};
  } catch (...) {
    return {-1, false, false, true, false, true, 0, 0};
  }
}
}  // namespace nonet
