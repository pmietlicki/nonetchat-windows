#include "monitor_recording_file.h"

#include <mfapi.h>
#include <mferror.h>
#include <pathcch.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <utility>

#include "monitor_fragment_stream.h"
#include "monitor_media_stream.h"

namespace nonet {
namespace {
HRESULT LastError() noexcept {
  const auto error = GetLastError();
  return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_DATA);
}

bool LowerHex(const std::string& value) noexcept {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
bool FileId(const std::string& value) noexcept {
  // InventoryIds.idBytes=18, the existing local recording UID/token format.
  return value.size() == 24 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
         });
}
bool LocalPath(const std::wstring& value) {
  if (value.size() < 3 || value.size() >= PATHCCH_MAX_CCH || value[1] != L':' ||
      value[2] != L'\\' || value.find(L'\0') != std::wstring::npos ||
      value.find(L':', 2) != std::wstring::npos ||
      value.find(L'/') != std::wstring::npos)
    return false;
  const auto drive = GetDriveTypeW(value.substr(0, 3).c_str());
  if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE &&
      drive != DRIVE_RAMDISK)
    return false;
  for (const auto& part : std::filesystem::path(value))
    if (part == L"." || part == L"..") return false;
  return true;
}
std::wstring FullPath(const std::wstring& value) {
  if (!LocalPath(value)) return {};
  std::vector<wchar_t> buffer(PATHCCH_MAX_CCH);
  const auto length = GetFullPathNameW(
      value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  return length && length < buffer.size() ? std::wstring(buffer.data(), length)
                                          : std::wstring();
}
std::wstring FinalPath(HANDLE handle) {
  std::vector<wchar_t> buffer(PATHCCH_MAX_CCH);
  const auto length = GetFinalPathNameByHandleW(
      handle, buffer.data(), static_cast<DWORD>(buffer.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= buffer.size()) return {};
  std::wstring value(buffer.data(), length);
  if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
  return LocalPath(value) ? value : std::wstring();
}
std::wstring VersionString(const std::vector<BYTE>& bytes, const wchar_t* key) {
  for (const auto prefix :
       {L"\\StringFileInfo\\040904e4\\", L"\\StringFileInfo\\040904b0\\"}) {
    const auto query = std::wstring(prefix) + key;
    wchar_t* value = nullptr;
    UINT length = 0;
    if (VerQueryValueW(bytes.data(), query.c_str(),
                       reinterpret_cast<void**>(&value), &length) &&
        value && length)
      return std::wstring(value, length - 1);
  }
  return {};
}
}  // namespace

std::wstring MonitorRecordingSupportDirectory() noexcept {
  PWSTR roaming = nullptr;
  try {
    std::vector<wchar_t> module(PATHCCH_MAX_CCH);
    const auto length = GetModuleFileNameW(nullptr, module.data(),
                                           static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return {};
    DWORD unused = 0;
    const auto size = GetFileVersionInfoSizeW(module.data(), &unused);
    if (!size) return {};
    std::vector<BYTE> info(size);
    if (!GetFileVersionInfoW(module.data(), 0, size, info.data())) return {};
    const auto company = VersionString(info, L"CompanyName");
    const auto product = VersionString(info, L"ProductName");
    // Match the repository's production and isolated fieldtest identities.
    // Unknown/renamed resource roots need review, never adopt another profile.
    if ((company != L"com.nonetchat" &&
         company != L"fr.pmietlicki.fieldtest_v4") ||
        product != L"nonetchat")
      return {};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,
                                    KF_FLAG_DONT_VERIFY, nullptr, &roaming)))
      return {};
    auto result =
        (std::filesystem::path(roaming) / company / product).wstring();
    CoTaskMemFree(roaming);
    roaming = nullptr;
    return FullPath(result);
  } catch (...) {
    if (roaming) CoTaskMemFree(roaming);
    return {};
  }
}

MonitorRecordingFile::MonitorRecordingFile(
    std::wstring support, MonitorFragmentStream::LimitCallback on_limit)
    : thread_(GetCurrentThreadId()),
      support_(std::move(support)),
      on_limit_(std::move(on_limit)) {}
MonitorRecordingFile::~MonitorRecordingFile() {
  // Last-resort cleanup never substitutes for an explicit positive Close.
  if (GetCurrentThreadId() == thread_) {
    const auto ignored = Close();
    (void)ignored;
  }
}

HRESULT MonitorRecordingFile::PinDirectory(const std::wstring& path) {
  const auto handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return LastError();
  try {
    handles_.push_back(handle);
  } catch (...) {
    CloseHandle(handle);
    throw;
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                    sizeof(attributes)))
    return LastError();
  if (!(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return E_ACCESSDENIED;
  return S_OK;
}

HRESULT MonitorRecordingFile::PinFile(const std::wstring& path, bool marker) {
  const auto handle =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return LastError();
  try {
    handles_.push_back(handle);
  } catch (...) {
    CloseHandle(handle);
    throw;
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                    sizeof(attributes)))
    return LastError();
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      GetFileType(handle) != FILE_TYPE_DISK)
    return E_ACCESSDENIED;
  BY_HANDLE_FILE_INFORMATION identity{};
  if (!GetFileInformationByHandle(handle, &identity)) return LastError();
  if (identity.nNumberOfLinks != 1) return E_ACCESSDENIED;
  if (!marker) {
    if (FinalPath(handle).empty()) return E_ACCESSDENIED;
    read_pin_ = handle;
    read_bytes_ =
        (std::uint64_t(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow;
  }
  return S_OK;
}

HRESULT MonitorRecordingFile::CheckMarker(const std::string& account,
                                          const std::string& id) {
  const bool video = container_ == MonitorRecordingContainer::audio_video_mp4;
  const auto prefix =
      std::string("{\"version\":") + (video ? "2" : "1") + ",\"account\":\"" +
      account + "\",\"id\":\"" + id +
      (video ? "\",\"kind\":\"audioVideo\",\"container\":\"mp4\",\"binding\":\""
             : "\",\"binding\":\"");
  const std::string suffix = "\"}";
  const auto expected = prefix.size() + 64 + suffix.size();
  LARGE_INTEGER length{};
  if (!GetFileSizeEx(handles_.back(), &length)) return LastError();
  if (length.QuadPart != static_cast<LONGLONG>(expected)) return E_ACCESSDENIED;
  std::string bytes(expected, '\0');
  DWORD read = 0;
  if (!ReadFile(handles_.back(), bytes.data(), static_cast<DWORD>(bytes.size()),
                &read, nullptr))
    return LastError();
  if (read != bytes.size() || bytes.compare(0, prefix.size(), prefix) != 0 ||
      bytes.compare(prefix.size() + 64, suffix.size(), suffix) != 0 ||
      !LowerHex(bytes.substr(prefix.size(), 64)))
    return E_ACCESSDENIED;
  return S_OK;
}

HRESULT MonitorRecordingFile::Open(
    const std::wstring& requested, const std::string& id, bool read,
    bool stream, std::uint64_t byte_budget,
    MonitorRecordingContainer container) noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (attempted_ || closed_) return MF_E_INVALIDREQUEST;
  attempted_ = true;
  if (container != MonitorRecordingContainer::audio_m4a &&
      container != MonitorRecordingContainer::audio_video_mp4)
    return E_INVALIDARG;
  container_ = container;
  if (!read && (!byte_budget || byte_budget > fmp4::AudioAdmission::max_bytes))
    return E_INVALIDARG;
  try {
    // App root + three private directories + marker + optional read pin.
    // Allocate before acquiring any HANDLE so insertion cannot lose an owner.
    handles_.reserve(6);
    if (!FileId(id)) return E_INVALIDARG;
    const auto supplied = FullPath(requested);
    const auto support = FullPath(support_);
    if (supplied.empty() || support.empty()) return E_ACCESSDENIED;
    // Pin the trusted app root before resolving redirects such as a local
    // profile junction. Only the three private descendants refuse reparses.
    const auto base =
        CreateFileW(support.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (base == INVALID_HANDLE_VALUE) return LastError();
    try {
      handles_.push_back(base);
    } catch (...) {
      CloseHandle(base);
      throw;
    }
    const auto canonical = FinalPath(base);
    if (canonical.empty()) return E_ACCESSDENIED;
    const auto file = std::filesystem::path(supplied);
    const auto account = file.parent_path().parent_path().filename().string();
    if (!LowerHex(account)) return E_ACCESSDENIED;
    const auto root = std::filesystem::path(canonical) / L"NoNetChatRecordings";
    const auto account_root =
        root / std::wstring(account.begin(), account.end());
    const auto directory = account_root / std::wstring(id.begin(), id.end());
    const auto filename = container_ == MonitorRecordingContainer::audio_m4a
                              ? L"audio.m4a"
                              : L"video.mp4";
    path_ = (directory / filename).wstring();
    // Compare the lexical app root as well as its pinned canonical target.
    const auto original =
        (std::filesystem::path(support) / L"NoNetChatRecordings" /
         std::wstring(account.begin(), account.end()) /
         std::wstring(id.begin(), id.end()) / filename)
            .wstring();
    if (_wcsicmp(supplied.c_str(), path_.c_str()) &&
        _wcsicmp(supplied.c_str(), original.c_str()))
      return E_ACCESSDENIED;
    for (const auto& value : {root, account_root, directory}) {
      const auto hr = PinDirectory(value.wstring());
      if (FAILED(hr)) return hr;
    }
    auto hr = PinFile((directory / L".owner").wstring(), true);
    if (FAILED(hr)) return hr;
    hr = CheckMarker(account, id);
    if (FAILED(hr)) return hr;
    if (read) {
      hr = PinFile(path_, false);
      if (FAILED(hr)) return hr;
    }
    if (!stream) {
      if (read) read_ready_ = true;
      if (!read && container_ == MonitorRecordingContainer::audio_video_mp4) {
        prepared_av_ = true;
        byte_budget_ = byte_budget;
      }
      return S_OK;
    }
    if (!read) {
      writer_ = std::make_unique<MonitorFragmentStream>(std::move(on_limit_));
      return writer_->Create(path_, byte_budget);
    }
    Microsoft::WRL::ComPtr<IMFByteStream> opened;
    hr = MFCreateFile(
        read ? MF_ACCESSMODE_READ : MF_ACCESSMODE_READWRITE,
        read ? MF_OPENMODE_FAIL_IF_NOT_EXIST : MF_OPENMODE_FAIL_IF_EXIST,
        MF_FILEFLAGS_NONE, path_.c_str(), &opened);
    // Preserve any partially acquired stream even on a native failure.
    if (opened) {
      stream_ = opened;
      auto confirmed =
          Microsoft::WRL::Make<ConfirmedMonitorByteStream>(opened.Get());
      if (!confirmed) return E_OUTOFMEMORY;
      stream_ = confirmed;
    }
    if (SUCCEEDED(hr) && !stream_) return E_UNEXPECTED;
    if (SUCCEEDED(hr) && read) read_ready_ = true;
    return hr;
  } catch (...) {
    return E_FAIL;
  }
}

HRESULT MonitorRecordingFile::Create(const std::wstring& path,
                                     const std::string& id,
                                     std::uint64_t byte_budget) noexcept {
  return Open(path, id, false, true, byte_budget);
}
HRESULT MonitorRecordingFile::PrepareAv(const std::wstring& path,
                                        const std::string& id,
                                        std::uint64_t budget) noexcept {
  return Open(path, id, false, false, budget,
              MonitorRecordingContainer::audio_video_mp4);
}
HRESULT MonitorRecordingFile::StartAv(std::uint64_t audio_start,
                                      std::uint64_t video_start) noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (!prepared_av_ || closed_ || av_attempted_) return MF_E_INVALIDREQUEST;
  av_attempted_ = true;
  try {
    av_writer_ =
        std::make_unique<MonitorAvFragmentStream>(std::move(on_limit_));
    return av_writer_->Create(path_, byte_budget_, audio_start, video_start);
  } catch (...) {
    return E_FAIL;
  }
}
HRESULT MonitorRecordingFile::Read(const std::wstring& path,
                                   const std::string& id,
                                   MonitorRecordingContainer kind) noexcept {
  return Open(path, id, true, true, 0, kind);
}
HRESULT MonitorRecordingFile::PinRead(const std::wstring& path,
                                      const std::string& id,
                                      MonitorRecordingContainer kind) noexcept {
  return Open(path, id, true, false, 0, kind);
}
IMFByteStream* MonitorRecordingFile::stream() const noexcept {
  return av_writer_ ? av_writer_->stream()
         : writer_  ? writer_->stream()
                    : stream_.Get();
}
HRESULT MonitorRecordingFile::failure() const noexcept {
  return av_writer_ ? av_writer_->failure()
         : writer_  ? writer_->failure()
                    : S_OK;
}
bool MonitorRecordingFile::quota_prefix() const noexcept {
  return av_writer_ ? av_writer_->quota_prefix()
                    : writer_ && writer_->quota_prefix();
}
fmp4::Stop MonitorRecordingFile::limit() const noexcept {
  return av_writer_ ? av_writer_->limit()
         : writer_  ? writer_->limit()
                    : fmp4::Stop::none;
}
HRESULT MonitorRecordingFile::ReadPinnedLength(std::uint64_t* result) noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (!result) return E_POINTER;
  if (!read_ready_ || closed_ || read_pin_ == INVALID_HANDLE_VALUE)
    return E_ACCESSDENIED;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(read_pin_, &size)) return LastError();
  if (size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) != read_bytes_)
    return E_ACCESSDENIED;
  *result = read_bytes_;
  return S_OK;
}
HRESULT MonitorRecordingFile::ReadPinnedAt(std::uint64_t offset,
                                           std::uint8_t* output,
                                           std::size_t count) noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (!read_ready_ || closed_ || read_pin_ == INVALID_HANDLE_VALUE)
    return E_ACCESSDENIED;
  if (!output || !count || count > fmp4::AudioAdmission::max_metadata_bytes ||
      offset > read_bytes_ || count > read_bytes_ - offset ||
      offset > INT64_MAX)
    return E_INVALIDARG;
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(read_pin_, position, nullptr, FILE_BEGIN))
    return LastError();
  DWORD actual = 0;
  if (!ReadFile(read_pin_, output, static_cast<DWORD>(count), &actual, nullptr))
    return LastError();
  return actual == count ? S_OK : HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
}
HRESULT MonitorRecordingFile::Close() noexcept {
  if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
  if (closed_) return S_OK;
  read_ready_ = false;  // A partial/uncertain close never admits a later read.
  try {
    if (av_writer_) {
      const auto hr = av_writer_->Close();
      if (hr != S_OK) return hr;
    }
    if (writer_) {
      const auto hr = writer_->Close();
      if (hr != S_OK) return hr;
    }
    if (stream_ && !stream_closed_) {
      const auto hr = stream_->Close();
      if (hr != S_OK) return hr;
      stream_closed_ = true;
      stream_.Reset();
    }
    while (!handles_.empty()) {
      if (!CloseHandle(handles_.back())) return LastError();
      handles_.pop_back();
    }
    closed_ = true;
    return S_OK;
  } catch (...) {
    return E_FAIL;
  }
}
}  // namespace nonet
