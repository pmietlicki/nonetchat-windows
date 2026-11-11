#ifndef NONET_WINDOWS_MONITOR_RECORDING_DIGEST_H_
#define NONET_WINDOWS_MONITOR_RECORDING_DIGEST_H_
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <functional>
#include <string>
#include "monitor_recording_file.h"
#include "monitor_recording_stream.h"
namespace nonet {
// Reads only the original pinned payload. No cursor change, reopen or authority
// after retirement. The existing 64 KiB inspection chunk bounds each read.
inline HRESULT MonitorRecordingDigest(MonitorRecordingFile& file,
                                      std::uint64_t bytes,
                                      const std::function<bool()>& current,
                                      std::string& result) {
  struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    ~Hash() {
      if (value) BCryptDestroyHash(value);
      if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
  } hash;
  if (!bytes || bytes > MonitorRecordingStream::max_bytes || !current)
    return E_INVALIDARG;
  if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM,
                                  nullptr, 0) < 0 ||
      BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) <
          0)
    return E_FAIL;
  std::array<std::uint8_t, 64 * 1024> block{};
  for (std::uint64_t at = 0; at < bytes;) {
    if (!current()) return E_ABORT;
    const auto count = static_cast<std::size_t>(
        (std::min)(std::uint64_t(block.size()), bytes - at));
    const auto hr = file.ReadPinnedAt(at, block.data(), count);
    if (FAILED(hr)) return hr;
    if (BCryptHashData(hash.value, block.data(), static_cast<ULONG>(count), 0) <
        0)
      return E_FAIL;
    at += count;
  }
  std::uint8_t digest[32]{};
  if (!current()) return E_ABORT;
  if (BCryptFinishHash(hash.value, digest, sizeof(digest), 0) < 0)
    return E_FAIL;
  constexpr char hex[] = "0123456789abcdef";
  std::string text(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    text[2 * i] = hex[digest[i] >> 4];
    text[2 * i + 1] = hex[digest[i] & 15];
  }
  result = std::move(text);
  return S_OK;
}
}  // namespace nonet
#endif
