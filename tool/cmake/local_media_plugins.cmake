# flutter_webrtc's upstream CMake downloads into its own REALPATH (pub cache).
# Copy only pinned native inputs, then include a build-local plugin recipe.
# Keep Flutter's generated source recipe untouched and all other plugins intact.
if(CMAKE_VERSION VERSION_LESS "3.18")
  message(FATAL_ERROR "Local media requires CMake 3.18 or later for verified SDK extraction")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/webrtc_sources.cmake")

if(WIN32)
  set(NONET_WEBRTC_PLATFORM windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(NONET_WEBRTC_PLATFORM linux)
else()
  message(FATAL_ERROR "The local WebRTC copy is only for Windows/Linux")
endif()

set(NONET_WEBRTC_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/flutter/ephemeral/.plugin_symlinks/flutter_webrtc")
get_filename_component(NONET_WEBRTC_SOURCE "${NONET_WEBRTC_SOURCE}" REALPATH)
set(NONET_WEBRTC_COPY
  "${CMAKE_CURRENT_BINARY_DIR}/nonet_compat/flutter_webrtc/${NONET_WEBRTC_PACKAGE_VERSION}")
if(NONET_WEBRTC_SOURCE STREQUAL NONET_WEBRTC_COPY)
  message(FATAL_ERROR "Refuse to modify the WebRTC dependency source")
endif()

# Validate every input first. Unknown added source is never copied/compiled.
foreach(NONET_ENTRY IN LISTS NONET_WEBRTC_SOURCE_HASHES)
  string(REPLACE "|" ";" NONET_PARTS "${NONET_ENTRY}")
  list(GET NONET_PARTS 0 NONET_RELATIVE)
  list(GET NONET_PARTS 1 NONET_EXPECTED)
  if(NOT EXISTS "${NONET_WEBRTC_SOURCE}/${NONET_RELATIVE}")
    message(FATAL_ERROR "Missing pinned WebRTC native input: ${NONET_RELATIVE}")
  endif()
  file(READ "${NONET_WEBRTC_SOURCE}/${NONET_RELATIVE}" NONET_TEXT)
  string(REPLACE "\r\n" "\n" NONET_TEXT "${NONET_TEXT}")
  string(SHA256 NONET_ACTUAL "${NONET_TEXT}")
  if(NOT NONET_ACTUAL STREQUAL NONET_EXPECTED)
    message(FATAL_ERROR "Review WebRTC native source change: ${NONET_RELATIVE}")
  endif()
endforeach()

foreach(NONET_ENTRY IN LISTS NONET_WEBRTC_SOURCE_HASHES)
  string(REPLACE "|" ";" NONET_PARTS "${NONET_ENTRY}")
  list(GET NONET_PARTS 0 NONET_RELATIVE)
  if(NOT NONET_RELATIVE STREQUAL "third_party/CMakeLists.txt")
    get_filename_component(NONET_PARENT
      "${NONET_WEBRTC_COPY}/${NONET_RELATIVE}" DIRECTORY)
    file(MAKE_DIRECTORY "${NONET_PARENT}")
    configure_file("${NONET_WEBRTC_SOURCE}/${NONET_RELATIVE}"
      "${NONET_WEBRTC_COPY}/${NONET_RELATIVE}" COPYONLY)
  endif()
endforeach()
file(MAKE_DIRECTORY "${NONET_WEBRTC_COPY}/third_party")
configure_file("${CMAKE_CURRENT_LIST_DIR}/webrtc_sdk.cmake"
  "${NONET_WEBRTC_COPY}/third_party/CMakeLists.txt" COPYONLY)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/flutter/generated_plugins.cmake" NONET_RECIPE)
string(REPLACE "\r\n" "\n" NONET_RECIPE "${NONET_RECIPE}")
string(REGEX MATCHALL "\n  flutter_webrtc\n" NONET_MATCHES "${NONET_RECIPE}")
list(LENGTH NONET_MATCHES NONET_COUNT)
if(NOT NONET_COUNT EQUAL 1)
  message(FATAL_ERROR "Review Flutter generated WebRTC plugin recipe")
endif()
string(REPLACE "\n  flutter_webrtc\n" "\n" NONET_RECIPE "${NONET_RECIPE}")
set(NONET_RECIPE_FILE "${CMAKE_CURRENT_BINARY_DIR}/nonet_compat/generated_plugins.cmake")
file(WRITE "${NONET_RECIPE_FILE}" "${NONET_RECIPE}")
include("${NONET_RECIPE_FILE}")
if(WIN32)
  include("${CMAKE_CURRENT_LIST_DIR}/webrtc_windows_monitor.cmake")
else()
  include("${CMAKE_CURRENT_LIST_DIR}/webrtc_linux_scope.cmake")
endif()
add_subdirectory("${NONET_WEBRTC_COPY}/${NONET_WEBRTC_PLATFORM}" plugins/flutter_webrtc)
if(WIN32)
  target_sources(flutter_webrtc_plugin PRIVATE ${NONET_MONITOR_SOURCES})
  target_include_directories(flutter_webrtc_plugin PRIVATE
    "${NONET_MONITOR_ROOT}/windows/native" "${NONET_MONITOR_ROOT}/native")
  target_compile_features(flutter_webrtc_plugin PRIVATE cxx_std_17)
  target_compile_definitions(flutter_webrtc_plugin PRIVATE NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_link_libraries(flutter_webrtc_plugin PRIVATE mfreadwrite.lib mfplat.lib
    mfuuid.lib shell32.lib version.lib Synchronization.lib windowsapp.lib
    d3d11.lib dxgi.lib bcrypt.lib mincore.lib oleaut32.lib strmiids.lib
    wmcodecdspuuid.lib runtimeobject.lib)
else()
  add_dependencies(flutter_webrtc_plugin nonet_linux_webrtc_sdk)
  target_sources(flutter_webrtc_plugin PRIVATE ${NONET_LINUX_SCOPE_SOURCES})
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NONET_PREVIEW_EPOXY REQUIRED IMPORTED_TARGET epoxy)
  target_link_libraries(flutter_webrtc_plugin PRIVATE PkgConfig::NONET_PREVIEW_EPOXY)
  target_include_directories(flutter_webrtc_plugin PRIVATE
    "${NONET_LINUX_SCOPE_ROOT}/linux/native" "${NONET_LINUX_SCOPE_ROOT}")
  target_compile_features(flutter_webrtc_plugin PRIVATE cxx_std_17)
  find_package(Threads REQUIRED)
  # Only public headers are needed: optional libpulse.so.0 is resolved locally
  # at runtime. A headless installation must still start ordinary text chat.
  pkg_check_modules(NONET_PREVIEW_PULSE QUIET libpulse)
  if(NONET_PREVIEW_PULSE_FOUND)
    target_compile_definitions(flutter_webrtc_plugin PRIVATE NONET_MONITOR_PULSE=1)
    target_include_directories(flutter_webrtc_plugin PRIVATE ${NONET_PREVIEW_PULSE_INCLUDE_DIRS})
  endif()
  target_link_libraries(flutter_webrtc_plugin PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
target_link_libraries(${BINARY_NAME} PRIVATE flutter_webrtc_plugin)
list(APPEND PLUGIN_BUNDLED_LIBRARIES $<TARGET_FILE:flutter_webrtc_plugin>)
list(APPEND PLUGIN_BUNDLED_LIBRARIES ${flutter_webrtc_bundled_libraries})
