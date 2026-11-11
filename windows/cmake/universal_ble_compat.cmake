# universal_ble 3.1.0 passes nullptr as FlutterError's EncodableValue details.
# It is converted to a null C string by Flutter's codec and crashes in strlen.
# Connection discovery also closes GATT services from a WinRT completion on the
# UI apartment, which can block that apartment inside Windows Bluetooth.
# Build a corrected copy; never mutate the shared pub cache. The digest forces
# review/removal of this workaround when upgrading the pinned dependency.
get_target_property(NONET_BLE_SOURCE_DIR universal_ble_plugin SOURCE_DIR)
set(NONET_BLE_SOURCE "${NONET_BLE_SOURCE_DIR}/src/universal_ble_plugin.cpp")
file(READ "${NONET_BLE_SOURCE}" NONET_BLE_TEXT)
string(REPLACE "\r\n" "\n" NONET_BLE_TEXT "${NONET_BLE_TEXT}")
string(SHA256 NONET_BLE_HASH "${NONET_BLE_TEXT}")
if(NOT NONET_BLE_HASH STREQUAL "e08f09b63122c617269b9f651149ae04b1405b12c913f437c1abbbaf915c799e")
  message(FATAL_ERROR "Review universal_ble Windows null-error compatibility patch for this dependency version")
endif()
# A C++ string may contain a semicolon. Bound calls by the closing parenthesis
# instead, under the pinned source digest, and require exactly six replacements.
set(NONET_BLE_ERROR_PATTERN "(return FlutterError\\([^)]*),[ \t\n]+nullptr\\)")
string(REGEX REPLACE "${NONET_BLE_ERROR_PATTERN}" "NONET_PATCHED_ERROR" NONET_BLE_MARKED "${NONET_BLE_TEXT}")
string(REGEX MATCHALL "NONET_PATCHED_ERROR" NONET_BLE_REPLACEMENTS "${NONET_BLE_MARKED}")
list(LENGTH NONET_BLE_REPLACEMENTS NONET_BLE_REPLACEMENT_COUNT)
if(NOT NONET_BLE_REPLACEMENT_COUNT EQUAL 6)
  message(FATAL_ERROR "Expected six universal_ble null error details")
endif()
string(REGEX REPLACE "${NONET_BLE_ERROR_PATTERN}" "\\1)" NONET_BLE_PATCHED "${NONET_BLE_TEXT}")
if(NONET_BLE_PATCHED MATCHES "${NONET_BLE_ERROR_PATTERN}")
  message(FATAL_ERROR "A universal_ble null error detail remains")
endif()
# GATT connection cleanup must leave the SDK completion before calling Close.
# Preserve FromBluetoothAddressAsync on the UI thread (consent may be shown).
# Both normal discovery-result paths hop to the pool before cleanup, including
# partial service discovery. Keep the callback lease and generation checks.
# https://learn.microsoft.com/windows/apps/develop/cpp-winrt/concurrency-2
set(NONET_BLE_CLEANUP_ANCHORS
  "    auto services_result_error =\n        gatt_communication_status_to_error(services_result.Status())\;"
  "    if (gatt_discovery_error.has_value()) {\n      DisposeGattMap(std::move(gatt_map))\;"
)
foreach(NONET_BLE_ANCHOR IN LISTS NONET_BLE_CLEANUP_ANCHORS)
  string(FIND "${NONET_BLE_PATCHED}" "${NONET_BLE_ANCHOR}" NONET_BLE_ANCHOR_AT)
  if(NONET_BLE_ANCHOR_AT LESS 0)
    message(FATAL_ERROR "Review universal_ble GATT completion cleanup patch")
  endif()
  string(REPLACE "${NONET_BLE_ANCHOR}"
    "    // NoNetChat: unwind the GATT completion before native cleanup.\n    co_await winrt::resume_background();\n${NONET_BLE_ANCHOR}"
    NONET_BLE_PATCHED "${NONET_BLE_PATCHED}")
endforeach()
# Separate dependency layouts so cached native objects can never be reused
# against another version's headers (file(COPY) preserves upstream timestamps).
set(NONET_BLE_COPY "${CMAKE_CURRENT_BINARY_DIR}/nonet_compat/universal_ble/${NONET_BLE_HASH}")
# Notification failures must be caught inside their native completion: Flutter
# disables STL exception capture, so std::async + future.get can terminate the
# process and also blocks the Flutter apartment. Preserve the Pigeon wire reply.
function(nonet_ble_replace NONET_VAR NONET_OLD NONET_NEW)
  string(FIND "${${NONET_VAR}}" "${NONET_OLD}" NONET_INDEX)
  if(NONET_INDEX LESS 0)
    message(FATAL_ERROR "Review universal_ble asynchronous notification patch")
  endif()
  # Full normalized source hashes prevent stale or ambiguous anchors.
  string(REPLACE "${NONET_OLD}" "${NONET_NEW}" NONET_REPLACED "${${NONET_VAR}}")
  set(${NONET_VAR} "${NONET_REPLACED}" PARENT_SCOPE)
endfunction()
function(nonet_ble_read NONET_FILE NONET_HASH NONET_VAR)
  file(READ "${NONET_BLE_SOURCE_DIR}/${NONET_FILE}" NONET_TEXT)
  string(REPLACE "\r\n" "\n" NONET_TEXT "${NONET_TEXT}")
  string(SHA256 NONET_ACTUAL "${NONET_TEXT}")
  if(NOT NONET_ACTUAL STREQUAL NONET_HASH)
    message(FATAL_ERROR "Review universal_ble asynchronous notification source: ${NONET_FILE}")
  endif()
  set(${NONET_VAR} "${NONET_TEXT}" PARENT_SCOPE)
endfunction()
nonet_ble_read(src/universal_ble_plugin.h
  10f5854aebf42dca6363c5e42249317e25e13c06480f9d6266d037e2af8673d2 NONET_BLE_HEADER)
nonet_ble_read(src/generated/universal_ble.g.h
  e79b917bfc30c209f2b84a64c82ecf2dc24d10b965b0fc99db00e81c1c50b1a6 NONET_BLE_API_HEADER)
nonet_ble_read(src/generated/universal_ble.g.cpp
  63fe589fe5d27588559ed4485008e8430c5ad2696b7477b356547ef18b2194ca NONET_BLE_API_CPP)
# Shutdown must close the UI publication queue before waiting for leases held
# by its callbacks. Review that ownership contract when this header changes.
nonet_ble_read(src/ui_thread_handler.hpp
  75bde23e11d3b9637e672bca0d567f62a4009f5a3c6376b72e3bfa1d7b3a1a05 NONET_BLE_UI_HEADER)
nonet_ble_replace(NONET_BLE_HEADER [=[  std::optional<FlutterError> UpdateCharacteristic(
      const std::string &characteristic_id, const std::vector<uint8_t> &value,
      const std::string *device_id) override;
]=] [=[  void UpdateCharacteristic(
      const std::string &characteristic_id, const std::vector<uint8_t> &value,
      const std::string *device_id,
      std::function<void(std::optional<FlutterError>)> result) override;
]=])
nonet_ble_replace(NONET_BLE_HEADER [=[  std::mutex peripheral_mutex_;]=] [=[  std::mutex peripheral_mutex_;
  // Completed callbacks can run on a worker. Cancel without holding this lock.
  std::mutex peripheral_notify_mutex_;
  uint64_t peripheral_notify_next_ = 0;
  std::map<uint64_t, PendingNotification<winrt::Windows::Foundation::IAsyncInfo>> peripheral_notify_operations_;
  void BeginNotificationShutdown();
  void CancelPeripheralNotifications(bool retire = false);]=])
nonet_ble_replace(NONET_BLE_HEADER [=[#include "async_operation_tracker.h"]=] [=[#include "async_operation_tracker.h"
#include "ble_notification_lifetime.h"]=])
nonet_ble_replace(NONET_BLE_API_HEADER [=[  virtual std::optional<FlutterError> UpdateCharacteristic(
    const std::string& characteristic_id,
    const std::vector<uint8_t>& value,
    const std::string* device_id) = 0;
]=] [=[  virtual void UpdateCharacteristic(
    const std::string& characteristic_id,
    const std::vector<uint8_t>& value,
    const std::string* device_id,
    std::function<void(std::optional<FlutterError>)> result) = 0;
]=])
nonet_ble_replace(NONET_BLE_API_CPP [=[          std::optional<FlutterError> output = api->UpdateCharacteristic(characteristic_id_arg, value_arg, device_id_arg);
          if (output.has_value()) {
            reply(WrapError(output.value()));
            return;
          }
          EncodableList wrapped;
          wrapped.push_back(EncodableValue());
          reply(EncodableValue(std::move(wrapped)));
]=] [=[          api->UpdateCharacteristic(characteristic_id_arg, value_arg, device_id_arg,
              [reply](std::optional<FlutterError> error) {
                if (error.has_value()) {
                  reply(WrapError(error.value()));
                  return;
                }
                EncodableList wrapped;
                wrapped.push_back(EncodableValue());
                reply(EncodableValue(std::move(wrapped)));
              });
]=])
set(NONET_BLE_NOTIFY_START "std::optional<FlutterError> UniversalBlePlugin::UpdateCharacteristic(")
set(NONET_BLE_NOTIFY_END "fire_and_forget UniversalBlePlugin::PeripheralAddServiceAsync(")
string(FIND "${NONET_BLE_PATCHED}" "${NONET_BLE_NOTIFY_START}" NONET_BLE_START)
string(FIND "${NONET_BLE_PATCHED}" "${NONET_BLE_NOTIFY_END}" NONET_BLE_END)
if(NONET_BLE_START LESS 0 OR NONET_BLE_END LESS NONET_BLE_START)
  message(FATAL_ERROR "Review universal_ble notification implementation anchors")
endif()
string(SUBSTRING "${NONET_BLE_PATCHED}" 0 ${NONET_BLE_START} NONET_BLE_PREFIX)
string(SUBSTRING "${NONET_BLE_PATCHED}" ${NONET_BLE_END} -1 NONET_BLE_SUFFIX)
set(NONET_BLE_NOTIFY_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../native/ble_notify.cpp.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NONET_BLE_NOTIFY_SOURCE}")
file(READ "${NONET_BLE_NOTIFY_SOURCE}" NONET_BLE_NOTIFY)
set(NONET_BLE_PATCHED "${NONET_BLE_PREFIX}${NONET_BLE_NOTIFY}\n${NONET_BLE_SUFFIX}")
# Keep the actual subscription callback available to the native fault fixture.
set(NONET_BLE_SUBSCRIPTIONS_START "fire_and_forget UniversalBlePlugin::PeripheralSubscribedClientsChanged(")
set(NONET_BLE_SUBSCRIPTIONS_END "fire_and_forget UniversalBlePlugin::PeripheralReadRequestedAsync(")
string(FIND "${NONET_BLE_PATCHED}" "${NONET_BLE_SUBSCRIPTIONS_START}" NONET_BLE_START)
string(FIND "${NONET_BLE_PATCHED}" "${NONET_BLE_SUBSCRIPTIONS_END}" NONET_BLE_END)
if(NONET_BLE_START LESS 0 OR NONET_BLE_END LESS NONET_BLE_START)
  message(FATAL_ERROR "Review universal_ble subscription implementation anchors")
endif()
string(SUBSTRING "${NONET_BLE_PATCHED}" 0 ${NONET_BLE_START} NONET_BLE_PREFIX)
string(SUBSTRING "${NONET_BLE_PATCHED}" ${NONET_BLE_END} -1 NONET_BLE_SUFFIX)
set(NONET_BLE_SUBSCRIPTIONS_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../native/ble_subscriptions.cpp.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NONET_BLE_SUBSCRIPTIONS_SOURCE}")
file(READ "${NONET_BLE_SUBSCRIPTIONS_SOURCE}" NONET_BLE_SUBSCRIPTIONS)
set(NONET_BLE_PATCHED "${NONET_BLE_PREFIX}${NONET_BLE_SUBSCRIPTIONS}\n${NONET_BLE_SUFFIX}")
nonet_ble_replace(NONET_BLE_PATCHED [=[  callback_operations_.Close();
  initialization_operations_.Close();]=] [=[  BeginNotificationShutdown();
  initialization_operations_.Close();]=])
nonet_ble_replace(NONET_BLE_PATCHED [=[#include "callback_drain.h"]=] [=[#include "callback_drain.h"
#include "ble_cleanup.h"]=])
nonet_ble_replace(NONET_BLE_PATCHED [=[  // Disposal owns its agent/lease and finishes on MTA without a UI
  // continuation. Pumping here would re-enter application code during engine
  // teardown.
  WaitForCallbacksWithoutMessagePump(cleanup_operations_);]=] [=[  // GATT COM Release on MTA may marshal to this STA. Dispatch only COM's
  // restricted default messages, without re-entering Flutter/window callbacks.
  if (!WaitForBleCleanup(cleanup_operations_)) {
    UniversalBleLogger::LogWarning(
        "BLE cleanup exceeded its shutdown budget; native cleanup retains its objects");
  }]=])
nonet_ble_replace(NONET_BLE_PATCHED [=[std::optional<FlutterError> UniversalBlePlugin::StopAdvertising() {]=] [=[std::optional<FlutterError> UniversalBlePlugin::StopAdvertising() {
  CancelPeripheralNotifications();]=])
nonet_ble_replace(NONET_BLE_PATCHED [=[std::optional<FlutterError> UniversalBlePlugin::RemoveService(
    const std::string &service_id) {]=] [=[std::optional<FlutterError> UniversalBlePlugin::RemoveService(
    const std::string &service_id) {
  CancelPeripheralNotifications();]=])
nonet_ble_replace(NONET_BLE_PATCHED [=[std::optional<FlutterError> UniversalBlePlugin::ClearServices() {]=] [=[std::optional<FlutterError> UniversalBlePlugin::ClearServices() {
  CancelPeripheralNotifications();]=])
# Copy the complete local include graph: the exported C API translation unit
# includes this plugin class too and must see the same virtual table/layout.
# Recreate our generated copy: files removed upstream must not survive a
# dependency change and enter the plugin's source glob or include search.
file(REMOVE_RECURSE "${NONET_BLE_COPY}")
file(COPY "${NONET_BLE_SOURCE_DIR}/src" "${NONET_BLE_SOURCE_DIR}/include"
  DESTINATION "${NONET_BLE_COPY}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../native/ble_cleanup.h"
  "${NONET_BLE_COPY}/src/ble_cleanup.h" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../native/ble_notification_lifetime.h"
  "${NONET_BLE_COPY}/src/ble_notification_lifetime.h" COPYONLY)
configure_file("${NONET_BLE_SOURCE_DIR}/universal_ble_plugin_c_api.cpp"
  "${NONET_BLE_COPY}/universal_ble_plugin_c_api.cpp" COPYONLY)
file(WRITE "${NONET_BLE_COPY}/src/universal_ble_plugin.cpp" "${NONET_BLE_PATCHED}")
file(WRITE "${NONET_BLE_COPY}/src/universal_ble_plugin.h" "${NONET_BLE_HEADER}")
file(WRITE "${NONET_BLE_COPY}/src/generated/universal_ble.g.h" "${NONET_BLE_API_HEADER}")
file(WRITE "${NONET_BLE_COPY}/src/generated/universal_ble.g.cpp" "${NONET_BLE_API_CPP}")
get_target_property(NONET_BLE_SOURCES universal_ble_plugin SOURCES)
set(NONET_BLE_COPIED_SOURCES)
foreach(NONET_SOURCE IN LISTS NONET_BLE_SOURCES)
  if(IS_ABSOLUTE "${NONET_SOURCE}")
    file(RELATIVE_PATH NONET_RELATIVE "${NONET_BLE_SOURCE_DIR}" "${NONET_SOURCE}")
  else()
    set(NONET_RELATIVE "${NONET_SOURCE}")
  endif()
  if(NONET_RELATIVE MATCHES "^\\.\\./" OR NOT EXISTS "${NONET_BLE_COPY}/${NONET_RELATIVE}")
    message(FATAL_ERROR "Review universal_ble translation unit: ${NONET_SOURCE}")
  endif()
  list(APPEND NONET_BLE_COPIED_SOURCES "${NONET_BLE_COPY}/${NONET_RELATIVE}")
endforeach()
set_property(TARGET universal_ble_plugin PROPERTY SOURCES "${NONET_BLE_COPIED_SOURCES}")
target_include_directories(universal_ble_plugin BEFORE PRIVATE
  "${NONET_BLE_COPY}" "${NONET_BLE_COPY}/src" "${NONET_BLE_COPY}/include")
