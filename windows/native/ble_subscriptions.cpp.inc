// Adapted from the pinned universal_ble Windows subscription callback.
fire_and_forget UniversalBlePlugin::PeripheralSubscribedClientsChanged(
    GattLocalCharacteristic const &local_char, IInspectable const &) {
  const auto callback = callback_operations_.TryAcquire();
  if (!callback.has_value())
    co_return;
  const auto characteristic_id = guid_to_uuid(local_char.Uuid());
  IVectorView<GattSubscribedClient> current_clients = nullptr;
  IVectorView<GattSubscribedClient> old_clients = nullptr;
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex_);
    auto *characteristic_object =
        FindPeripheralGattCharacteristicObject(characteristic_id);
    if (characteristic_object == nullptr) {
      co_return;
    }
    current_clients = local_char.SubscribedClients();
    old_clients = characteristic_object->stored_clients;
    characteristic_object->stored_clients = current_clients;
  }

  for (uint32_t i = 0; i < current_clients.Size(); ++i) {
    auto client = current_clients.GetAt(i);
    bool found = false;
    for (uint32_t j = 0; j < old_clients.Size(); ++j) {
      if (old_clients.GetAt(j) == client) {
        found = true;
        break;
      }
    }
    if (!found) {
      const auto device_id =
          ParsePeripheralBluetoothClientId(client.Session().DeviceId().Id());
      // NoNetChat never uses the OS client name. An optional Windows device
      // information query can remain pending while valid GATT writes arrive;
      // publish subscription readiness immediately, under the existing lease.
      PostPeripheralReport(
          [device_id, characteristic_id](
              UniversalBlePeripheralCallback *peripheral_channel) {
            peripheral_channel->OnCharacteristicSubscriptionChange(
                device_id, characteristic_id, true, nullptr, SuccessCallback,
                ErrorCallback);
          });
      const int64_t mtu = client.Session().MaxPduSize();
      PostPeripheralReport(
          [device_id, mtu](UniversalBlePeripheralCallback *peripheral_channel) {
            peripheral_channel->OnMtuChange(device_id, mtu, SuccessCallback,
                                            ErrorCallback);
          });
    }
  }

  for (uint32_t i = 0; i < old_clients.Size(); ++i) {
    auto client = old_clients.GetAt(i);
    bool found = false;
    for (uint32_t j = 0; j < current_clients.Size(); ++j) {
      if (current_clients.GetAt(j) == client) {
        found = true;
        break;
      }
    }
    if (!found) {
      const auto device_id =
          ParsePeripheralBluetoothClientId(client.Session().DeviceId().Id());
      PostPeripheralReport(
          [device_id, characteristic_id](
              UniversalBlePeripheralCallback *peripheral_channel) {
            peripheral_channel->OnCharacteristicSubscriptionChange(
                device_id, characteristic_id, false, nullptr, SuccessCallback,
                ErrorCallback);
          });
    }
  }
}
