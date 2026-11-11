#pragma once

#include <memory>
#include <mutex>
#include <utility>

#include "async_operation_tracker.h"

namespace universal_ble {

// Copies of an OS delegate share one claim. Retiring an uncalled delegate
// makes a later invocation harmless; an already claimed callback keeps its
// independent lease until its work and queued reply are finished.
class NotificationCompletionLease {
 public:
  explicit NotificationCompletionLease(AsyncOperationTracker::Lease lease)
      : value_(std::move(lease)) {}

  AsyncOperationTracker::Lease Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(value_, {});
  }

 private:
  std::mutex mutex_;
  AsyncOperationTracker::Lease value_;
};

template <typename Operation>
struct PendingNotification {
  Operation operation;
  std::shared_ptr<NotificationCompletionLease> completion;
  bool cancel_requested = false;
};

}  // namespace universal_ble
