#ifndef NONET_WINDOWS_MONITOR_MEDIA_STREAM_H_
#define NONET_WINDOWS_MONITOR_MEDIA_STREAM_H_

#include <mfidl.h>
#include <wrl.h>

#include <atomic>

namespace nonet {

// Media Foundation may itself close a stream. Preserve that positive proof,
// including across a later redundant Close, without mistaking Finalize for it.
// The enclosing worker must keep this capability until closure is confirmed.
class ConfirmedMonitorByteStream final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFByteStream> {
 public:
  explicit ConfirmedMonitorByteStream(IMFByteStream* stream) : stream_(stream) {}
  STDMETHODIMP GetCapabilities(DWORD* v) override { return Safe([&] { return stream_->GetCapabilities(v); }); }
  STDMETHODIMP GetLength(QWORD* v) override { return Safe([&] { return stream_->GetLength(v); }); }
  STDMETHODIMP SetLength(QWORD v) override { return Safe([&] { return stream_->SetLength(v); }); }
  STDMETHODIMP GetCurrentPosition(QWORD* v) override { return Safe([&] { return stream_->GetCurrentPosition(v); }); }
  STDMETHODIMP SetCurrentPosition(QWORD v) override { return Safe([&] { return stream_->SetCurrentPosition(v); }); }
  STDMETHODIMP IsEndOfStream(BOOL* v) override { return Safe([&] { return stream_->IsEndOfStream(v); }); }
  STDMETHODIMP Read(BYTE* b, ULONG n, ULONG* v) override { return Safe([&] { return stream_->Read(b, n, v); }); }
  STDMETHODIMP BeginRead(BYTE* b, ULONG n, IMFAsyncCallback* cb, IUnknown* s) override { return Safe([&] { return stream_->BeginRead(b, n, cb, s); }); }
  STDMETHODIMP EndRead(IMFAsyncResult* r, ULONG* n) override { return Safe([&] {
    if (!r) return E_POINTER;
    const auto status = r->GetStatus();
    const auto completed = stream_->EndRead(r, n);
    return FAILED(status) ? status : completed;
  }); }
  STDMETHODIMP Write(const BYTE* b, ULONG n, ULONG* v) override { return Safe([&] { return stream_->Write(b, n, v); }); }
  STDMETHODIMP BeginWrite(const BYTE* b, ULONG n, IMFAsyncCallback* cb, IUnknown* s) override { return Safe([&] { return stream_->BeginWrite(b, n, cb, s); }); }
  STDMETHODIMP EndWrite(IMFAsyncResult* r, ULONG* n) override { return Safe([&] {
    if (!r) return E_POINTER;
    const auto status = r->GetStatus();
    const auto completed = stream_->EndWrite(r, n);
    return FAILED(status) ? status : completed;
  }); }
  STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN o, LONGLONG p, DWORD f, QWORD* v) override { return Safe([&] { return stream_->Seek(o, p, f, v); }); }
  STDMETHODIMP Flush() override { return Safe([&] { return stream_->Flush(); }); }
  STDMETHODIMP Close() override {
    if (closed_.load()) return S_OK;
    const auto status = Safe([&] { return stream_->Close(); });
    if (status == S_OK) closed_.store(true);
    return closed_.load() ? S_OK : status;
  }

 private:
  template <typename Call>
  static HRESULT Safe(Call&& call) noexcept {
    try { return call(); } catch (...) { return E_FAIL; }
  }
  Microsoft::WRL::ComPtr<IMFByteStream> stream_;
  std::atomic<bool> closed_{false};
};

}  // namespace nonet
#endif
