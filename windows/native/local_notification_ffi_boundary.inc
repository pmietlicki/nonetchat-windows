// Appended to the pinned SDK translation unit, after renaming its C exports.
// Never allow a WinRT/C++ exception to unwind through Dart FFI.
NativePlugin* createPlugin() {
  try { return NonetUnchecked_createPlugin(); } catch (...) { return nullptr; }
}
void disposePlugin(NativePlugin* plugin) {
  try { NonetUnchecked_disposePlugin(plugin); } catch (...) {}
}
bool init(NativePlugin* plugin, char* appName, char* aumId, char* guid, char* iconPath, NativeNotificationCallback callback) {
  if (!plugin) return false;
  try {
    if (NonetUnchecked_init(plugin, appName, aumId, guid, iconPath, callback)) return true;
  } catch (...) {}
  // In this pinned Dart backend a failed initialization is never disposed and
  // retries allocate a new object. Retire the failed registration here.
  disposePlugin(plugin);
  return false;
}
bool isValidXml(char* xml) {
  try { return NonetUnchecked_isValidXml(xml); } catch (...) { return false; }
}
bool showNotification(NativePlugin* plugin, int id, char* xml, NativeStringMap bindings) {
  if (!plugin) return false;
  try { return NonetUnchecked_showNotification(plugin, id, xml, bindings); } catch (...) { return false; }
}
bool scheduleNotification(NativePlugin* plugin, int id, char* xml, int time) {
  if (!plugin) return false;
  try { return NonetUnchecked_scheduleNotification(plugin, id, xml, time); } catch (...) { return false; }
}
NativeUpdateResult updateNotification(NativePlugin* plugin, int id, NativeStringMap bindings) {
  if (!plugin) return NativeUpdateResult::failed;
  try { return NonetUnchecked_updateNotification(plugin, id, bindings); } catch (...) { return NativeUpdateResult::failed; }
}
void cancelAll(NativePlugin* plugin) {
  if (!plugin) return;
  try { NonetUnchecked_cancelAll(plugin); } catch (...) {}
}
void cancelNotification(NativePlugin* plugin, int id) {
  if (!plugin) return;
  try { NonetUnchecked_cancelNotification(plugin, id); } catch (...) {}
}
NativeNotificationDetails* getActiveNotifications(NativePlugin* plugin, int* size) {
  if (!size) return nullptr;
  *size = 0;
  if (!plugin) return nullptr;
  try { return NonetUnchecked_getActiveNotifications(plugin, size); } catch (...) { *size = 0; return nullptr; }
}
NativeNotificationDetails* getPendingNotifications(NativePlugin* plugin, int* size) {
  if (!size) return nullptr;
  *size = 0;
  if (!plugin) return nullptr;
  try { return NonetUnchecked_getPendingNotifications(plugin, size); } catch (...) { *size = 0; return nullptr; }
}
void freeDetailsArray(NativeNotificationDetails* ptr) {
  try { NonetUnchecked_freeDetailsArray(ptr); } catch (...) {}
}
void freeLaunchDetails(NativeLaunchDetails details) {
  try { NonetUnchecked_freeLaunchDetails(details); } catch (...) {}
}
extern "C" FFI_PLUGIN_EXPORT void nonet_close_notification_callbacks() {
  try { nonet::NotificationCallbackGate::CloseAll(); } catch (...) {}
}
