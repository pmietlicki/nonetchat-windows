#ifndef NONET_WINDOWS_MONITOR_PREVIEW_FRAME_H_
#define NONET_WINDOWS_MONITOR_PREVIEW_FRAME_H_
#include <cstdint>
#include <vector>
namespace nonet {
struct MonitorPreviewFrame {
  std::vector<std::uint8_t> rgba;
  std::uint32_t width = 0, height = 0;
};
}  // namespace nonet
#endif
