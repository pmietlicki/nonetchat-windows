# bonsoir_windows 7.3.0 mutates resolved service properties while its queued
# Flutter events read them (observed Windows access violation in std::map).
# Build an isolated corrected copy: preserve immutable event snapshots and
# serialize discovery state. Never modify the shared pub cache.
# Nested locks always take servicesMutex before resolvingServicesCleanupMutex.
# Other paths release one before acquiring the other. Cancellation never holds
# either: Windows can wait for a callback during cancellation.
get_target_property(NONET_BONSOIR_DIR bonsoir_windows_plugin SOURCE_DIR)
foreach(NONET_BONSOIR_FILE bonsoir_discovery.cpp bonsoir_discovery.h)
  file(READ "${NONET_BONSOIR_DIR}/${NONET_BONSOIR_FILE}" NONET_BONSOIR_TEXT)
  string(REPLACE "\r\n" "\n" NONET_BONSOIR_TEXT "${NONET_BONSOIR_TEXT}")
  string(SHA256 NONET_BONSOIR_HASH "${NONET_BONSOIR_TEXT}")
  if(NONET_BONSOIR_FILE STREQUAL "bonsoir_discovery.cpp")
    set(NONET_BONSOIR_EXPECTED d6de69503471610956552ce2d7819c316a761bb34d4be241e2127050b689a233)
    set(NONET_BONSOIR_CPP "${NONET_BONSOIR_TEXT}")
  else()
    set(NONET_BONSOIR_EXPECTED 99bfe99b5c32b9db0a0e8423474e9699f09cefa0cc4c53d8ee143b421d8aeb53)
    set(NONET_BONSOIR_HEADER "${NONET_BONSOIR_TEXT}")
  endif()
  if(NOT NONET_BONSOIR_HASH STREQUAL NONET_BONSOIR_EXPECTED)
    message(FATAL_ERROR "Review Bonsoir Windows compatibility fix for this dependency version")
  endif()
endforeach()
function(nonet_bonsoir_replace NONET_VAR NONET_OLD NONET_NEW)
  string(FIND "${${NONET_VAR}}" "${NONET_OLD}" NONET_INDEX)
  if(NONET_INDEX LESS 0)
    message(FATAL_ERROR "Bonsoir Windows correction no longer matches its guarded source")
  endif()
  string(REPLACE "${NONET_OLD}" "${NONET_NEW}" NONET_REPLACED "${${NONET_VAR}}")
  set(${NONET_VAR} "${NONET_REPLACED}" PARENT_SCOPE)
endfunction()

# The registrar outlives FlutterWindowsEngine::Stop. Native event channels must
# not unregister through a messenger whose engine has already been destroyed.
foreach(NONET_BONSOIR_PLUGIN_FILE bonsoir_windows_plugin.cpp bonsoir_windows_plugin.h bonsoir_windows_plugin_c_api.cpp)
  file(READ "${NONET_BONSOIR_DIR}/${NONET_BONSOIR_PLUGIN_FILE}" NONET_BONSOIR_PLUGIN_TEXT)
  string(REPLACE "\r\n" "\n" NONET_BONSOIR_PLUGIN_TEXT "${NONET_BONSOIR_PLUGIN_TEXT}")
  string(SHA256 NONET_BONSOIR_PLUGIN_HASH "${NONET_BONSOIR_PLUGIN_TEXT}")
  if(NONET_BONSOIR_PLUGIN_FILE STREQUAL "bonsoir_windows_plugin.cpp")
    set(NONET_BONSOIR_PLUGIN_EXPECTED 8bf98c5eb1bf13bc650615fa5d0ba721cc364f2a9c01fef6eec57d4c1abfcf11)
    set(NONET_BONSOIR_PLUGIN_CPP "${NONET_BONSOIR_PLUGIN_TEXT}")
  elseif(NONET_BONSOIR_PLUGIN_FILE STREQUAL "bonsoir_windows_plugin.h")
    set(NONET_BONSOIR_PLUGIN_EXPECTED 2a9a51f87356f8eec863f28c859f793238c2599adba2d4489469a07378969736)
    set(NONET_BONSOIR_PLUGIN_HEADER "${NONET_BONSOIR_PLUGIN_TEXT}")
  else()
    set(NONET_BONSOIR_PLUGIN_EXPECTED 98337f8d4d94c72f06615b69d4a52aba1b1dddf217413149f619b4e126dfa242)
    set(NONET_BONSOIR_PLUGIN_C_API "${NONET_BONSOIR_PLUGIN_TEXT}")
  endif()
  if(NOT NONET_BONSOIR_PLUGIN_HASH STREQUAL NONET_BONSOIR_PLUGIN_EXPECTED)
    message(FATAL_ERROR "Review Bonsoir Windows compatibility fix for this dependency version")
  endif()
endforeach()
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_HEADER [=[#include "bonsoir_broadcast.h"]=] [=[#include "bonsoir_broadcast.h"
#include "engine_lifetime_messenger.h"]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_HEADER [=[static void RegisterWithRegistrar(PluginRegistrarWindows *registrar);]=] [=[static void RegisterWithRegistrar(PluginRegistrarWindows *registrar, FlutterDesktopMessengerRef core_messenger);]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_HEADER [=[BonsoirWindowsPlugin(BinaryMessenger *messenger);]=] [=[BonsoirWindowsPlugin(BinaryMessenger *messenger, FlutterDesktopMessengerRef core_messenger);]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_HEADER [=[    BinaryMessenger *messenger;]=] [=[    std::unique_ptr<nonet::EngineLifetimeMessenger> engine_messenger_;
    BinaryMessenger *messenger;]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_CPP [=[RegisterWithRegistrar(PluginRegistrarWindows *registrar)]=] [=[RegisterWithRegistrar(PluginRegistrarWindows *registrar, FlutterDesktopMessengerRef core_messenger)]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_CPP [=[    auto channel = std::make_unique<MethodChannel<EncodableValue>>(
      messenger, "fr.skyost.bonsoir", &StandardMethodCodec::GetInstance()
    );
    auto plugin = std::make_unique<BonsoirWindowsPlugin>(messenger);]=] [=[    auto plugin = std::make_unique<BonsoirWindowsPlugin>(messenger, core_messenger);
    auto channel = std::make_unique<MethodChannel<EncodableValue>>(
      plugin->messenger, "fr.skyost.bonsoir", &StandardMethodCodec::GetInstance()
    );]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_CPP [=[BonsoirWindowsPlugin::BonsoirWindowsPlugin(BinaryMessenger *messenger)
    : messenger(messenger) {}]=] [=[BonsoirWindowsPlugin::BonsoirWindowsPlugin(BinaryMessenger *messenger, FlutterDesktopMessengerRef core_messenger)
    : engine_messenger_(std::make_unique<nonet::EngineLifetimeMessenger>(messenger, core_messenger)),
      messenger(engine_messenger_.get()) {}]=])
nonet_bonsoir_replace(NONET_BONSOIR_PLUGIN_C_API [=[      ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar)
]=] [=[      ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar),
    FlutterDesktopPluginRegistrarGetMessenger(registrar)
]=])
nonet_bonsoir_replace(NONET_BONSOIR_HEADER [=[    std::mutex resolvingServicesCleanupMutex;]=] [=[    std::mutex resolvingServicesCleanupMutex;
    std::recursive_mutex servicesMutex;]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    {
      const std::lock_guard<std::mutex> lock(resolvingServicesCleanupMutex);
      for (auto const &[key, value] : resolvingServices) {
        DnsServiceResolveCancel(value);
        delete value; // Allocated in resolveService()
      }
      resolvingServices.clear();
    }
]=] [=[    std::map<std::shared_ptr<BonsoirService>, PDNS_SERVICE_CANCEL> cancelled;
    {
      const std::lock_guard<std::mutex> lock(resolvingServicesCleanupMutex);
      cancelled.swap(resolvingServices);
    }
    // Cancellation may wait for a callback. Never hold either state lock here.
    for (const auto& [service, handle] : cancelled) {
      DnsServiceResolveCancel(handle);
      delete handle;
    }
]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    services.clear();]=] [=[    {
      const std::lock_guard<std::recursive_mutex> lock(servicesMutex);
      services.clear();
    }]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    for (auto &found_service : this->services) {]=] [=[    const std::lock_guard<std::recursive_mutex> lock(servicesMutex);
    for (auto &found_service : this->services) {]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    DNS_STATUS status = DnsServiceResolve(&resolveRequest, resolveCancelHandle);
    if (status == DNS_REQUEST_PENDING) {
      resolvingServices[servicePtr] = resolveCancelHandle;
    } else {
      onSuccess(Generated::discoveryServiceResolveFailed, servicePtr, std::list<std::string>{std::to_string(status)});
    }]=] [=[    DNS_STATUS status;
    {
      const std::lock_guard<std::mutex> lock(resolvingServicesCleanupMutex);
      for (const auto& [pending, handle] : resolvingServices) {
        if (pending->name == serviceName && pending->type == serviceType) {
          delete resolveCancelHandle;
          return; // Coalesce an already pending resolution of this service.
        }
      }
      resolvingServices[servicePtr] = resolveCancelHandle;
      status = DnsServiceResolve(&resolveRequest, resolveCancelHandle);
    }
    if (status != DNS_REQUEST_PENDING) {
      {
        const std::lock_guard<std::mutex> lock(resolvingServicesCleanupMutex);
        auto pending = resolvingServices.find(servicePtr);
        if (pending != resolvingServices.end() && pending->second == resolveCancelHandle) {
          resolvingServices.erase(pending);
          delete resolveCancelHandle;
        }
      }
      onSuccess(Generated::discoveryServiceResolveFailed, servicePtr, std::list<std::string>{std::to_string(status)});
    }]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    auto discovery = (BonsoirDiscovery *)context;
    if (status == ERROR_CANCELLED) {]=] [=[    auto discovery = (BonsoirDiscovery *)context;
    if (!discovery->isRunning() || dnsRecord == nullptr) {
      if (dnsRecord) DnsRecordListFree(dnsRecord, DnsFreeRecordList);
      return;
    }
    if (status == ERROR_CANCELLED) {]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    auto serviceData = parseBonjourFqdn(toUtf8(dnsRecord->Data.PTR.pNameHost));]=] [=[    const std::lock_guard<std::recursive_mutex> stateLock(discovery->servicesMutex);
    PDNS_RECORD pointerRecord = dnsRecord;
    while (pointerRecord && pointerRecord->wType != DNS_TYPE_PTR) pointerRecord = pointerRecord->pNext;
    if (!pointerRecord || !pointerRecord->Data.PTR.pNameHost) {
      DnsRecordListFree(dnsRecord, DnsFreeRecordList);
      return;
    }
    auto serviceData = parseBonjourFqdn(toUtf8(pointerRecord->Data.PTR.pNameHost));]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    if (dnsRecord->dwTtl <= 0) {]=] [=[    if (pointerRecord->dwTtl <= 0) {]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[        discovery->services.remove(servicePtr);
      }
      return;]=] [=[        discovery->services.remove(servicePtr);
      }
      DnsRecordListFree(dnsRecord, DnsFreeRecordList);
      return;]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    auto discovery = (BonsoirDiscovery *)context;
    std::shared_ptr<BonsoirService> servicePtr = nullptr;]=] [=[    auto discovery = (BonsoirDiscovery *)context;
    const std::lock_guard<std::recursive_mutex> stateLock(discovery->servicesMutex);
    if (!discovery->isRunning()) {
      if (serviceInstance) DnsServiceFreeInstance(serviceInstance);
      return;
    }
    std::shared_ptr<BonsoirService> servicePtr = nullptr;]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    if(servicePtr) {
      const std::lock_guard<std::mutex> lock(discovery->resolvingServicesCleanupMutex);
      DNS_SERVICE_CANCEL* resolveCancelHandle = discovery->resolvingServices[servicePtr];
      delete resolveCancelHandle; // Allocated in resolveService()
	    discovery->resolvingServices.erase(servicePtr);
    }]=] [=[    {
      const std::lock_guard<std::mutex> lock(discovery->resolvingServicesCleanupMutex);
      for (auto pending = discovery->resolvingServices.begin(); pending != discovery->resolvingServices.end(); ++pending) {
        if (pending->first->name == std::get<0>(serviceData) && pending->first->type == std::get<1>(serviceData)) {
          delete pending->second;
          discovery->resolvingServices.erase(pending);
          break;
        }
      }
    }]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    if (serviceInstance) {
      servicePtr->hostAddresses = serviceInstanceIpAddresses(serviceInstance);]=] [=[    if (serviceInstance) {
      // Queued Flutter events retain the old immutable snapshot. Never mutate
      // its std::map while the platform thread serializes it.
      auto previous = servicePtr;
      servicePtr = std::make_shared<BonsoirService>(*previous);
      discovery->services.remove(previous);
      servicePtr->hostAddresses = serviceInstanceIpAddresses(serviceInstance);]=])
nonet_bonsoir_replace(NONET_BONSOIR_CPP [=[    discovery->onSuccess(Generated::discoveryServiceResolved, servicePtr);]=] [=[    discovery->services.push_back(servicePtr);
    discovery->onSuccess(Generated::discoveryServiceResolved, servicePtr);]=])
string(REGEX MATCHALL "onError\\(nullptr," NONET_NULL_ERRORS "${NONET_BONSOIR_CPP}")
list(LENGTH NONET_NULL_ERRORS NONET_NULL_ERROR_COUNT)
if(NOT NONET_NULL_ERROR_COUNT EQUAL 2)
  message(FATAL_ERROR "Expected two Bonsoir null error details")
endif()
string(REPLACE "onError(nullptr," "onError(EncodableValue()," NONET_BONSOIR_CPP "${NONET_BONSOIR_CPP}")

set(NONET_BONSOIR_COPY "${CMAKE_CURRENT_BINARY_DIR}/nonet_compat/bonsoir")
# All translation units need the same class layout and copied local headers.
file(COPY "${NONET_BONSOIR_DIR}/" DESTINATION "${NONET_BONSOIR_COPY}")
file(WRITE "${NONET_BONSOIR_COPY}/bonsoir_discovery.cpp" "${NONET_BONSOIR_CPP}")
file(WRITE "${NONET_BONSOIR_COPY}/bonsoir_discovery.h" "${NONET_BONSOIR_HEADER}")
file(WRITE "${NONET_BONSOIR_COPY}/bonsoir_windows_plugin.cpp" "${NONET_BONSOIR_PLUGIN_CPP}")
file(WRITE "${NONET_BONSOIR_COPY}/bonsoir_windows_plugin.h" "${NONET_BONSOIR_PLUGIN_HEADER}")
file(WRITE "${NONET_BONSOIR_COPY}/bonsoir_windows_plugin_c_api.cpp" "${NONET_BONSOIR_PLUGIN_C_API}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../native/engine_lifetime_messenger.h"
  "${NONET_BONSOIR_COPY}/engine_lifetime_messenger.h" COPYONLY)
get_target_property(NONET_BONSOIR_SOURCES bonsoir_windows_plugin SOURCES)
set(NONET_BONSOIR_COPIED_SOURCES)
foreach(NONET_SOURCE IN LISTS NONET_BONSOIR_SOURCES)
  if(IS_ABSOLUTE "${NONET_SOURCE}")
    list(APPEND NONET_BONSOIR_COPIED_SOURCES "${NONET_SOURCE}")
  else()
    list(APPEND NONET_BONSOIR_COPIED_SOURCES "${NONET_BONSOIR_COPY}/${NONET_SOURCE}")
  endif()
endforeach()
set_property(TARGET bonsoir_windows_plugin PROPERTY SOURCES "${NONET_BONSOIR_COPIED_SOURCES}")
target_include_directories(bonsoir_windows_plugin BEFORE PRIVATE "${NONET_BONSOIR_COPY}" "${NONET_BONSOIR_COPY}/include")
