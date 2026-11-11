// Inserted into the pinned universal_ble source by universal_ble_compat.cmake.
// Keep the Pigeon wire response unchanged, but never wait on the UI thread.
void UniversalBlePlugin::BeginNotificationShutdown() {
  callback_operations_.Close();
  // The Flutter window is already being destroyed. Its delegated messages can
  // no longer execute these replies, and queued replies own callback leases.
  // Drop them before the lease barrier; late Post calls must also be rejected.
  ui_thread_handler_.Shutdown();
  CancelPeripheralNotifications(true);
}

void UniversalBlePlugin::CancelPeripheralNotifications(bool retire) {
  std::map<uint64_t, PendingNotification<IAsyncInfo>> pending;
  {
    std::lock_guard<std::mutex> lock(peripheral_notify_mutex_);
    if (retire) {
      pending.swap(peripheral_notify_operations_);
    } else {
      for (auto &[id, entry] : peripheral_notify_operations_) {
        if (!entry.cancel_requested) {
          entry.cancel_requested = true;
          pending.emplace(id, entry);
        }
      }
    }
  }
  // Cancellation can synchronously enter Completed. Never hold its mutex.
  for (const auto &[id, entry] : pending) {
    try {
      entry.operation.Cancel();
    } catch (...) {
      log_and_swallow_unknown("Cancel peripheral notification");
    }
    // Cancel need not invoke Completed. Only retirement may revoke an
    // unclaimed callback; a callback already executing owns its own lease.
    if (retire) entry.completion->Take();
  }
}

void UniversalBlePlugin::UpdateCharacteristic(
    const std::string &characteristic_id, const std::vector<uint8_t> &value,
    const std::string *device_id,
    std::function<void(std::optional<FlutterError>)> result) {
  const auto callback = callback_operations_.TryAcquire();
  const auto replied = std::make_shared<std::atomic_bool>(false);
  const auto reply = [result = std::move(result), replied](
                         std::optional<FlutterError> error) {
    if (replied->exchange(true)) return;
    // Explicitly catch here: Flutter builds the STL with _HAS_EXCEPTIONS=0.
    try {
      result(std::move(error));
    } catch (...) {
      log_and_swallow_unknown("Peripheral notification reply");
    }
  };
  if (!callback.has_value()) {
    reply(FlutterError("failed", "Bluetooth is stopping"));
    return;
  }
  try {
    GattLocalCharacteristic characteristic{nullptr};
    IBuffer buffer{nullptr};
    {
      std::lock_guard<std::mutex> lock(peripheral_mutex_);
      bool ambiguous = false;
      auto *object = FindPeripheralGattCharacteristicObject(characteristic_id,
                                                           &ambiguous);
      if (ambiguous) {
        reply(FlutterError("ambiguous-characteristic",
                          "Characteristic UUID exists in multiple services"));
        return;
      }
      if (object == nullptr) {
        reply(FlutterError("not-found", "Characteristic not found"));
        return;
      }
      DataWriter writer;
      writer.ByteOrder(ByteOrder::LittleEndian);
      writer.WriteBuffer(from_bytevc(value));
      characteristic = object->obj;
      buffer = writer.DetachBuffer();
    }
    GattSubscribedClient target{nullptr};
    if (device_id != nullptr) {
      const auto clients = characteristic.SubscribedClients();
      for (uint32_t index = 0; index < clients.Size(); ++index) {
        const auto candidate = clients.GetAt(index);
        const auto candidate_id = ParsePeripheralBluetoothClientId(
            candidate.Session().DeviceId().Id());
        if (to_lower_case(candidate_id) == to_lower_case(*device_id)) {
          target = candidate;
          break;
        }
      }
      if (!target) {
        // A departed client must never turn a private link write into a flood.
        reply(FlutterError("device-not-found", "Subscribed client not found"));
        return;
      }
    }
    // The native call returns an operation; neither get() nor a future wait is
    // allowed here. Both WinRT overloads share cancellation and UI ownership.
    // Copies of the characteristic, buffer and client outlive service removal.
    const auto observe = [this, callback, reply, characteristic, buffer, target](
                             auto operation, auto result_error) {
      // Completed delegates may outlive their invocation. Share a one-shot
      // lease slot across delegate copies, rather than tying shutdown to the
      // operating system releasing the delegate object.
      const auto completion_lease =
          std::make_shared<NotificationCompletionLease>(*callback);
      uint64_t id;
      {
        std::lock_guard<std::mutex> lock(peripheral_notify_mutex_);
        id = ++peripheral_notify_next_;
        peripheral_notify_operations_.emplace(
            id, PendingNotification<IAsyncInfo>{
                    operation.template as<IAsyncInfo>(), completion_lease});
      }
      try {
        operation.Completed(
            [this, id, completion_lease, reply, characteristic, buffer, target,
             result_error](
                const auto &sender, AsyncStatus status) {
              // Retain the native buffer until this completion delegate ends.
              // Its lifetime matters even when no result reads its contents.
              static_cast<void>(buffer);
              auto lease = completion_lease->Take();
              // A duplicate native invocation must not dereference a plugin
              // whose first completion already released its ownership.
              if (!lease) return;
              std::optional<FlutterError> error;
              try {
                if (status != AsyncStatus::Completed) {
                  error = FlutterError("failed", "Bluetooth notification interrupted");
                } else {
                  error = result_error(sender.GetResults());
                }
              } catch (...) {
                error = FlutterError("failed", "Bluetooth notification failed");
              }
              {
                std::lock_guard<std::mutex> lock(peripheral_notify_mutex_);
                peripheral_notify_operations_.erase(id);
              }
              // Transfer ownership to the queued reply. Shutdown drops that
              // queue before draining callbacks; rejected posts release it too.
              ui_thread_handler_.Post(
                  [lease = std::move(lease), reply,
                   error = std::move(error)]() mutable {
                    reply(std::move(error));
                  });
            });
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(peripheral_notify_mutex_);
          peripheral_notify_operations_.erase(id);
        }
        try {
          operation.Cancel();
        } catch (...) {
        }
        throw;
      }
    };
    if (target) {
      // WinRT's subscribed-client overload avoids broadcasting every fragment
      // to all neighbours; it also retains that client's own negotiated MTU.
      observe(characteristic.NotifyValueAsync(buffer, target),
              [](const auto &notification) -> std::optional<FlutterError> {
                const auto error =
                    gatt_communication_status_to_error(notification.Status());
                if (error.has_value()) return FlutterError("failed", *error);
                return std::nullopt;
              });
    } else {
      // Preserve the explicit broadcast API. SDK success alone never proves
      // destination storage; the mesh still requires its signed final receipt.
      observe(characteristic.NotifyValueAsync(buffer),
              [](const auto &) -> std::optional<FlutterError> {
                return std::nullopt;
              });
    }
  } catch (...) {
    reply(FlutterError("failed", "Bluetooth notification failed"));
  }
}
