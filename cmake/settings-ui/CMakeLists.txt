# Upstream-derived settings process; Qt is deliberately linked only here.
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "FusionSettings.exe currently requires Windows/MSVC")
endif()
if(NOT FUSION_WITH_FCITX5)
  message(FATAL_ERROR "FusionSettings.exe requires the production Fcitx Host")
endif()
if(NOT FUSION_SETTINGS_SOURCE_ROOT)
  message(FATAL_ERROR "Set FUSION_SETTINGS_SOURCE_ROOT to the pinned, patch-applied upstream source root")
endif()

set(_settings_configtool "${FUSION_SETTINGS_SOURCE_ROOT}/fcitx5-configtool")
set(_settings_fcitxqt "${FUSION_SETTINGS_SOURCE_ROOT}/fcitx5-qt")
if(NOT EXISTS "${_settings_configtool}/src/lib/configlib/dbusprovider.cpp" OR
   NOT EXISTS "${_settings_fcitxqt}/qt6/dbusaddons/CMakeLists.txt")
  message(FATAL_ERROR "Pinned configtool/fcitx5-qt source checkouts are missing")
endif()

find_package(ECM 6.30.0 EXACT CONFIG REQUIRED)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(GNUInstallDirs)
include(GenerateExportHeader)
include(ECMSetupVersion)
include(CMakePackageConfigHelpers)
find_package(Fcitx5Core 5.1.13 CONFIG REQUIRED)
find_package(Fcitx5Config CONFIG REQUIRED)
find_package(Fcitx5Utils CONFIG REQUIRED)
find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS Core Gui Widgets Network DBus)

set(FUSION_SETTINGS_SOURCE_ROOT "${FUSION_SETTINGS_SOURCE_ROOT}" CACHE PATH "Pinned, patch-applied fcitx5-configtool/fcitx5-qt checkouts")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
add_compile_options("/FI${CMAKE_CURRENT_LIST_DIR}/../fcitx-native/compat/unistd.h")
set(QT_MAJOR_VERSION 6)
set(FCITX5_QT_GUI_WRAPPER "fcitx5-qt6-gui-wrapper")
set(LIBRARY_TYPE SHARED)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/fcitxqt" settings-fcitxqt)
add_subdirectory("${_settings_configtool}/src/lib/configlib" settings-configlib)
add_subdirectory("${_settings_configtool}/src/lib/configwidgetslib" settings-configwidgetslib)

target_include_directories(configlib BEFORE PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../fcitx-native/compat")
target_include_directories(configwidgetslib BEFORE PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../fcitx-native/compat")
target_compile_definitions(configlib PRIVATE FCITX_GETTEXT_DOMAIN=\"fcitx5-configtool\" QT_NO_KEYWORDS)
target_compile_definitions(configwidgetslib PRIVATE FCITX_GETTEXT_DOMAIN="fcitx5-configtool" QT_NO_KEYWORDS)
target_compile_options(configlib PRIVATE /W4 /utf-8)
target_compile_options(configwidgetslib PRIVATE /W4 /utf-8)

add_executable(FusionSettingsExe "${CMAKE_CURRENT_LIST_DIR}/../../src/settings/main.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/settings/dbusprovider.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../src/ipc/pipe_security.cpp"
  "${_settings_configtool}/src/lib/configlib/dbusprovider.h")
set_target_properties(FusionSettingsExe PROPERTIES
  OUTPUT_NAME FusionSettings
  WIN32_EXECUTABLE TRUE
  CXX_EXTENSIONS OFF)
target_include_directories(FusionSettingsExe PRIVATE
  "${_settings_configtool}/src/lib/configlib"
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../../src")
target_compile_features(FusionSettingsExe PRIVATE cxx_std_20)
target_compile_definitions(FusionSettingsExe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE QT_NO_KEYWORDS)
target_compile_options(FusionSettingsExe PRIVATE /W4 /utf-8)
target_link_libraries(FusionSettingsExe PRIVATE
  configwidgetslib configlib
  Fcitx5Qt6::DBusAddons Fcitx5Qt6::WidgetsAddons
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus
  advapi32 user32)

# A portable owner-test directory must launch without the developer Qt prefix on
# PATH. Stage only the linked Qt runtime and the Windows platform plugin.
get_filename_component(_settings_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
set(_settings_bin "$<TARGET_FILE_DIR:FusionSettingsExe>")
foreach(_qt_target IN ITEMS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus)
  add_custom_command(TARGET FusionSettingsExe POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${_qt_target}>" "${_settings_bin}"
    VERBATIM)
endforeach()
add_custom_command(TARGET FusionSettingsExe POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_settings_bin}/plugins/platforms"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${_settings_qt_root}/plugins/platforms/qwindows.dll" "${_settings_bin}/plugins/platforms/qwindows.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/qt.conf" "${_settings_bin}/qt.conf"
  VERBATIM)
