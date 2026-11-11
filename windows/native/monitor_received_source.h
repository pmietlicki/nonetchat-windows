#ifndef NONET_WINDOWS_MONITOR_RECEIVED_SOURCE_H_
#define NONET_WINDOWS_MONITOR_RECEIVED_SOURCE_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "monitor_received_media.h"
#include "monitor_received_registry.h"
#include "rtc_peerconnection.h"

namespace nonet {

using MonitorReceivedTarget = MonitorReceivedRegistry::Target;

// One selected RECEIVED source in one engine. Its resolver must borrow only
// from that engine, which keepalive retains through sink removal. Selection,
// Current, Attach and Close run on its original platform thread. No singleton,
// track mutation, volume change, microphone/camera acquisition or network IO.
// The local permit checks current consent AND the confirmed native indicator;
// a source identity alone never grants recording permission.
class MonitorReceivedSource final {
 public:
  using Permit = std::function<bool(bool first_admission)>;
  static std::unique_ptr<MonitorReceivedSource> Select(
      MonitorReceivedRegistry& registry, MonitorReceivedTarget target,
      Permit permit, std::shared_ptr<void> keepalive,
      std::shared_ptr<MonitorReceivedMedia> media) noexcept;
  ~MonitorReceivedSource();
  MonitorReceivedSource(const MonitorReceivedSource&) = delete;
  MonitorReceivedSource& operator=(const MonitorReceivedSource&) = delete;

  bool Current() noexcept;
  // Worker guards borrow this weak scope; voluntary Close is separate from
  // registry/account revocation. Host retains the source until programme drain.
  std::weak_ptr<MonitorReceivedRegistry::Loan> scope() const noexcept;
  bool Attach() noexcept;
  // Permanent retirement. False retains original handles and the engine;
  // a failed removal remains uncertain, never reselect a fresh track wrapper.
  // An uncertain Add cannot prove safe release by a later no-op Remove.
  bool Close() noexcept;

 private:
  struct Impl;
  explicit MonitorReceivedSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace nonet
#endif
