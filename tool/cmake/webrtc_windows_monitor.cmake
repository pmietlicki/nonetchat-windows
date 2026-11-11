# Called only after all original upstream source hashes have been verified and
# copied to the build tree. Never modify the dependency cache or SDK globals.
function(nonet_monitor_replace variable needle replacement)
  string(FIND "${${variable}}" "${needle}" first)
  if(first LESS 0)
    message(FATAL_ERROR "Review WebRTC Windows monitor bridge anchor")
  endif()
  string(LENGTH "${needle}" needle_length)
  math(EXPR after "${first} + ${needle_length}")
  string(SUBSTRING "${${variable}}" ${after} -1 tail)
  string(FIND "${tail}" "${needle}" duplicate)
  if(NOT duplicate LESS 0)
    message(FATAL_ERROR "Review duplicate WebRTC Windows monitor bridge anchor")
  endif()
  string(REPLACE "${needle}" "${replacement}" patched "${${variable}}")
  set(${variable} "${patched}" PARENT_SCOPE)
endfunction()

set(NONET_MONITOR_PLUGIN "${NONET_WEBRTC_COPY}/windows/flutter_webrtc_plugin.cc")
file(READ "${NONET_MONITOR_PLUGIN}" NONET_MONITOR_TEXT)
string(REPLACE "\r\n" "\n" NONET_MONITOR_TEXT "${NONET_MONITOR_TEXT}")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "#include \"task_runner_windows.h\""
  "#include \"task_runner_windows.h\"\n#include \"monitor_recording_bridge.h\"\n#include \"monitor_selected_recording_bridge.h\"\n#include \"monitor_recording_file.h\"\n#include \"engine_lifetime_messenger.h\"")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "  static void RegisterWithRegistrar(PluginRegistrar* registrar) {"
  [=[  static void RegisterWithRegistrar(PluginRegistrar* registrar,
                                    FlutterDesktopMessengerRef host_messenger) {
    auto nonet_messenger = std::make_unique<nonet::EngineLifetimeMessenger>(
        registrar->messenger(), host_messenger);]=])
nonet_monitor_replace(NONET_MONITOR_TEXT
  "        registrar->messenger(), kChannelName,"
  "        nonet_messenger.get(), kChannelName,")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "        new FlutterWebRTCPluginImpl(registrar, std::move(channel)));"
  "        new FlutterWebRTCPluginImpl(registrar, std::move(channel), std::move(nonet_messenger)));")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "                          std::unique_ptr<MethodChannel> channel)"
  "                          std::unique_ptr<MethodChannel> channel,\n                          std::unique_ptr<nonet::EngineLifetimeMessenger> nonet_messenger)")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "      : channel_(std::move(channel)),"
  "      : nonet_messenger_(std::move(nonet_messenger)),\n        channel_(std::move(channel)),")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "  std::unique_ptr<MethodChannel> channel_;"
  "  std::unique_ptr<nonet::EngineLifetimeMessenger> nonet_messenger_;\n  std::unique_ptr<MethodChannel> channel_;")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));"
  "          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar),\n      FlutterDesktopPluginRegistrarGetMessenger(registrar));")
nonet_monitor_replace(NONET_MONITOR_TEXT
  "  virtual ~FlutterWebRTCPluginImpl() {}"
  [=[  virtual ~FlutterWebRTCPluginImpl() {
    if (nonet_selected_recorder_) nonet_selected_recorder_->Retire();
    if (nonet_recorder_) nonet_recorder_->Retire();
    nonet_recorder_.reset();
    // EngineLifetimeMessenger skips withdrawal after the engine detaches.
    channel_->SetMethodCallHandler(nullptr);
    // SDK media/event owners close while their TaskRunner is still alive.
    webrtc_.reset();
    if (nonet_selected_recorder_) nonet_selected_recorder_->GraphClosed();
    nonet_selected_recorder_.reset();
  }]=])
nonet_monitor_replace(NONET_MONITOR_TEXT
  "    webrtc_ = std::make_unique<FlutterWebRTC>(this);"
  [=[    webrtc_ = std::make_unique<FlutterWebRTC>(this);
    nonet_recorder_ = std::make_unique<nonet::MonitorRecordingBridge>(
        [this](const std::string& id) {
          return libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection>(
              webrtc_ ? webrtc_->PeerConnectionForId(id) : nullptr);
        }, true, nonet::MonitorRecordingBridge::SupportDirectory{}, textures_); // Original engine owns preview textures.

    webrtc_->nonet_received_registry_ = std::make_shared<nonet::MonitorReceivedRegistry>();
    nonet_selected_recorder_ = std::make_unique<nonet::MonitorSelectedRecordingBridge>(
        webrtc_->nonet_received_registry_, nonet_messenger_->core(),
        nonet::MonitorRecordingSupportDirectory, true);]=])
nonet_monitor_replace(NONET_MONITOR_TEXT
  "    // handle method call and forward to webrtc native sdk."
  [=[    // Probe/stop must not initialize an unrelated capture factory.
    if (nonet_selected_recorder_->Handle(method_call, result)) return;
    if (nonet_recorder_->Handle(method_call, result)) return;
    // handle method call and forward to webrtc native sdk.]=])
nonet_monitor_replace(NONET_MONITOR_TEXT
  "  std::unique_ptr<TaskRunner> task_runner_;"
  "  std::unique_ptr<nonet::MonitorSelectedRecordingBridge> nonet_selected_recorder_;\n  std::unique_ptr<nonet::MonitorRecordingBridge> nonet_recorder_;\n  std::unique_ptr<TaskRunner> task_runner_;")
file(WRITE "${NONET_MONITOR_PLUGIN}" "${NONET_MONITOR_TEXT}")

# Attach first-party code after upstream creates its plugin target. This list
# is consumed by local_media_plugins.cmake, not the vendor's pub-cache recipe.
get_filename_component(NONET_MONITOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NONET_MONITOR_SOURCES
  "${NONET_MONITOR_ROOT}/windows/native/monitor_recording_bridge.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_recording_file.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_recording_stream.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_fragment_stream.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_aac_sink.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_aac_inspector.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_preview_player.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_pcm_pump.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_av_inspector.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_av_program.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_av_sink.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_preview_texture.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_preview_video_surface.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_received_media.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_received_registry.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_received_source.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_selected_program.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_selected_recording_bridge.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_video_normalizer.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/webrtc_runtime_owner.cpp"
  "${NONET_MONITOR_ROOT}/windows/native/monitor_av_fragment_stream.cpp")

# Engine-scoped original sources and shared native runtime: common/cpp/include/flutter_webrtc_base.h
set(NONET_MONITOR_FILE "${NONET_WEBRTC_COPY}/common/cpp/include/flutter_webrtc_base.h")
file(READ "${NONET_MONITOR_FILE}" NONET_MONITOR_TEXT)
string(REPLACE "\r\n" "\n" NONET_MONITOR_TEXT "${NONET_MONITOR_TEXT}")
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[#include "flutter_common.h"]==]
  [==[#include "flutter_common.h"
#include "monitor_received_registry.h"
#include "webrtc_runtime_owner.h"]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[class FlutterWebRTCBase {
 public:]==]
  [==[class FlutterWebRTCBase {
 public:
  std::shared_ptr<nonet::MonitorReceivedRegistry> nonet_received_registry_;
  std::shared_ptr<nonet::WindowsWebRtcRuntimeOwner> nonet_runtime_owner_;]==])
file(WRITE "${NONET_MONITOR_FILE}" "${NONET_MONITOR_TEXT}")

# Engine-scoped original sources and shared native runtime: common/cpp/src/flutter_webrtc_base.cc
set(NONET_MONITOR_FILE "${NONET_WEBRTC_COPY}/common/cpp/src/flutter_webrtc_base.cc")
file(READ "${NONET_MONITOR_FILE}" NONET_MONITOR_TEXT)
string(REPLACE "\r\n" "\n" NONET_MONITOR_TEXT "${NONET_MONITOR_TEXT}")
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[#include "flutter_webrtc_base.h"]==]
  [==[#include "flutter_webrtc_base.h"
#include "webrtc_runtime_owner.h"
#include "flutter_video_renderer.h"]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  webrtc_initialized_ = true;
  warp_enabled_]==]
  [==[  warp_enabled_]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  if (field_trials.empty()) {
    LibWebRTC::Initialize();
  } else {
    LibWebRTC::InitializeWithFieldTrials(
        libwebrtc::vector<libwebrtc::string>(field_trials));
  }

  factory_ = LibWebRTC::CreateRTCPeerConnectionFactory();
  factory_->Initialize();]==]
  [==[  nonet_runtime_owner_ = nonet::WindowsWebRtcRuntimeOwner::Acquire(
      libwebrtc::vector<libwebrtc::string>(field_trials));
  factory_ = nonet_runtime_owner_->factory();
  if (nonet_received_registry_) nonet_received_registry_->InstallRuntime(nonet_runtime_owner_);
  webrtc_initialized_ = true;]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  if (webrtc_initialized_) {
    LibWebRTC::Terminate();
  }]==]
  [==[  if (nonet_received_registry_) nonet_received_registry_->RetireAll();
  if (webrtc_initialized_) {
    if (audio_device_) audio_device_->OnDeviceChange({});
    for (auto& renderer : renders_) renderer.second->SetVideoTrack(nullptr);
    for (auto& entry : video_capturers_) {
      if (entry.second->CaptureStarted()) entry.second->StopCapture();
    }
    for (auto& entry : peerconnections_) {
      entry.second->Close();
      entry.second->DeRegisterRTCPeerConnectionObserver();
      factory_->Delete(entry.second);
    }
    peerconnections_.clear();
    peerconnection_observers_.clear();
    data_channel_observers_.clear();
    renders_.clear();local_streams_.clear();local_tracks_.clear();
    video_capturers_.clear();key_providers_.clear();
    audio_device_=nullptr;video_device_=nullptr;desktop_device_=nullptr;audio_processing_=nullptr;
    factory_=nullptr;
    // A selected source retains only native factory/runtime refs through drain,
    // never this Flutter graph, its registrar or its TaskRunner.
    nonet_runtime_owner_.reset();
  }]==])
file(WRITE "${NONET_MONITOR_FILE}" "${NONET_MONITOR_TEXT}")

# Engine-scoped original sources and shared native runtime: common/cpp/include/flutter_peerconnection.h
set(NONET_MONITOR_FILE "${NONET_WEBRTC_COPY}/common/cpp/include/flutter_peerconnection.h")
file(READ "${NONET_MONITOR_FILE}" NONET_MONITOR_TEXT)
string(REPLACE "\r\n" "\n" NONET_MONITOR_TEXT "${NONET_MONITOR_TEXT}")
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  virtual void OnSignalingState(RTCSignalingState state) override;]==]
  [==[  ~FlutterPeerConnectionObserver() override {
    if (base_->nonet_received_registry_) base_->nonet_received_registry_->Retire(this);
  }

  virtual void OnSignalingState(RTCSignalingState state) override;]==])
file(WRITE "${NONET_MONITOR_FILE}" "${NONET_MONITOR_TEXT}")

# Engine-scoped original sources and shared native runtime: common/cpp/src/flutter_peerconnection.cc
set(NONET_MONITOR_FILE "${NONET_WEBRTC_COPY}/common/cpp/src/flutter_peerconnection.cc")
file(READ "${NONET_MONITOR_FILE}" NONET_MONITOR_TEXT)
string(REPLACE "\r\n" "\n" NONET_MONITOR_TEXT "${NONET_MONITOR_TEXT}")
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  peerconnection->RegisterRTCPeerConnectionObserver(this);]==]
  [==[  if (base_->nonet_received_registry_) base_->nonet_received_registry_->Register(this, id_, peerconnection_);
  peerconnection->RegisterRTCPeerConnectionObserver(this);]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[    RTCPeerConnectionState state) {
  EncodableMap params;]==]
  [==[    RTCPeerConnectionState state) {
  if (base_->nonet_received_registry_) base_->nonet_received_registry_->Connection(this, state);
  EncodableMap params;]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  auto streams = receiver->streams();
  for (scoped_refptr<RTCMediaStream> item : streams.std_vector()) {]==]
  [==[  auto streams = receiver->streams();
  if (base_->nonet_received_registry_) {
    std::string nonet_stream;
    bool nonet_ambiguous = streams.size() != 1;
    if (streams.size() == 1 && streams.data()[0]) {
      auto label = streams.data()[0]->label();
      if (label.size() <= 4 * 256) nonet_stream = label.std_string();
      else nonet_ambiguous = true;
    }
    base_->nonet_received_registry_->Track(this, receiver, track, std::move(nonet_stream), nonet_ambiguous);
  }
  for (scoped_refptr<RTCMediaStream> item : streams.std_vector()) {]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[void FlutterPeerConnectionObserver::OnRemoveTrack(
    scoped_refptr<RTCRtpReceiver> receiver) {]==]
  [==[void FlutterPeerConnectionObserver::OnRemoveTrack(
    scoped_refptr<RTCRtpReceiver> receiver) {
  if (base_->nonet_received_registry_) base_->nonet_received_registry_->Removed(this, receiver);]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[void FlutterPeerConnectionObserver::OnRemoveStream(
    scoped_refptr<RTCMediaStream> stream) {]==]
  [==[void FlutterPeerConnectionObserver::OnRemoveStream(
    scoped_refptr<RTCMediaStream> stream) {
  if (base_->nonet_received_registry_) base_->nonet_received_registry_->Removed(this);]==])
file(WRITE "${NONET_MONITOR_FILE}" "${NONET_MONITOR_TEXT}")

# Engine-scoped original sources and shared native runtime: common/cpp/src/flutter_webrtc.cc
set(NONET_MONITOR_FILE "${NONET_WEBRTC_COPY}/common/cpp/src/flutter_webrtc.cc")
file(READ "${NONET_MONITOR_FILE}" NONET_MONITOR_TEXT)
string(REPLACE "\r\n" "\n" NONET_MONITOR_TEXT "${NONET_MONITOR_TEXT}")
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[#include "flutter_webrtc.h"]==]
  [==[#include "flutter_webrtc.h"
#include "webrtc_runtime_owner.h"]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[    std::unique_ptr<MethodResultProxy> result) {
  if (method_call.method_name().compare("initialize") == 0) {]==]
  [==[    std::unique_ptr<MethodResultProxy> result) {
  const auto ensure_ready = [&](bool warp = false, bool zero_delay = false) {
    try { EnsureWebRTCInitialized(warp,zero_delay);return true; }
    catch (const nonet::WindowsWebRtcUnavailable&) {
      result->Error("audio_unavailable","Local audio is unavailable");return false;
    }
  };
  if (method_call.method_name().compare("initialize") == 0) {]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[    EnsureWebRTCInitialized(findBoolean(options, "enableWARP"),
                            findBoolean(options, "zeroPlayoutDelay"));]==]
  [==[    if (!ensure_ready(findBoolean(options, "enableWARP"),
                      findBoolean(options, "zeroPlayoutDelay"))) return;]==])
nonet_monitor_replace(NONET_MONITOR_TEXT
  [==[  EnsureWebRTCInitialized();]==]
  [==[  if (!ensure_ready()) return;]==])
file(WRITE "${NONET_MONITOR_FILE}" "${NONET_MONITOR_TEXT}")
