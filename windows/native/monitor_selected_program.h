#ifndef NONET_WINDOWS_MONITOR_SELECTED_PROGRAM_H_
#define NONET_WINDOWS_MONITOR_SELECTED_PROGRAM_H_
#include <memory>

#include "monitor_av_program.h"
#include "monitor_pcm_pump.h"
namespace nonet {
// One selected purpose, confined to its original worker. The audio branch
// reuses the existing pump/profile/file checks; AV uses the two-track
// programme. Neither branch has a SDK source, a UI endpoint or authority to
// start capture.
class MonitorSelectedProgram final {
 public:
  MonitorSelectedProgram(MonitorReceivedMedia&, bool video,
                         std::wstring support, MonitorAvProgram::Revoked);
  HRESULT Prepare(const std::wstring&, const std::string&,
                  std::uint64_t) noexcept;
  HRESULT Run() noexcept;
  MonitorAvProgramClosed Close() noexcept;

 private:
  bool Live() noexcept;
  HRESULT Fail(HRESULT) noexcept;
  const DWORD thread_ = GetCurrentThreadId();
  MonitorReceivedMedia& media_;
  MonitorAvProgram::Revoked revoked_;
  MonitorMediaContext context_;
  std::unique_ptr<MonitorAvProgram> video_;
  std::unique_ptr<MonitorRecordingFile> file_;
  std::unique_ptr<MonitorPcmPump> pump_;
  HRESULT error_ = S_OK, run_ = E_PENDING;
  bool prepared_ = false, attempted_ = false, closing_ = false;
};
}  // namespace nonet
#endif
