#pragma once

#include <windows.h>
#include <objbase.h>
#include <chrono>
#include "async_operation_tracker.h"

namespace universal_ble {

// MTA GATT destruction can marshal its final COM Release back to the STA.
// Wait for every lease, dispatching only COM's default restricted messages;
// never pump arbitrary Flutter/window messages during engine destruction.
inline bool WaitForBleCleanup(
    const AsyncOperationTracker &operations,
    std::chrono::milliseconds budget = std::chrono::seconds(10)) noexcept {
  // Same budget as the existing BLE write queue; use local monotonic time.
  const auto deadline = std::chrono::steady_clock::now() + budget;
  const HANDLE sentinel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // Use a real handle: COM rejects a thread pseudo handle for this API.
  // On allocation failure retain the lease barrier; never free live objects.
  HANDLE wait_target = sentinel;
  while (!operations.WaitUntilIdleFor(std::chrono::milliseconds(10))) {
    if (std::chrono::steady_clock::now() >= deadline) {
      if (sentinel) CloseHandle(sentinel);
      // The static cleanup coroutine owns its agent and tracker state. Its
      // closed callback gates reject entry before dereferencing the plugin.
      // Leave those objects alive, possibly until process exit; never free a
      // GATT object while a native operation still uses it.
      return false;
    }
    if (sentinel) {
      DWORD index = 0;
      CoWaitForMultipleHandles(COWAIT_DEFAULT, 10, 1, &wait_target, &index);
    }
  }
  if (sentinel) CloseHandle(sentinel);
  return true;
}

} // namespace universal_ble
