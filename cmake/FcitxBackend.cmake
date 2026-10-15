if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Stage 3B native Fcitx PoC currently requires Windows/MSVC")
endif()

set(FUSION_FCITX5_ROOT "" CACHE PATH "Prepared matching Debug/Release native Fcitx runtime prefix")
if(NOT FUSION_FCITX5_ROOT)
  message(FATAL_ERROR "Set FUSION_FCITX5_ROOT to the prepared native runtime prefix")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${FUSION_FCITX5_ROOT}")
find_package(Fcitx5Core CONFIG REQUIRED)
find_package(LibIMEPinyin CONFIG REQUIRED)
find_package(PThreads4W CONFIG REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_library(FusionFcitxHost SHARED src/fcitx/backend.cpp)
target_include_directories(FusionFcitxHost PRIVATE include
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fcitx-native/compat"
  "${PThreads4W_INCLUDE_DIR}")
target_compile_features(FusionFcitxHost PRIVATE cxx_std_20)
target_compile_definitions(FusionFcitxHost PRIVATE FUSION_FCITX_HOST_BUILD NOMINMAX WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(FusionFcitxHost PRIVATE /W4 /WX /utf-8 /Zc:preprocessor /Zc:__cplusplus
  "/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/fcitx-native/compat/unistd.h")
target_link_libraries(FusionFcitxHost PRIVATE Fcitx5::Core LibIME::Pinyin PThreads4W::PThreads4W)
foreach(addon pinyin pinyinhelper punctuation spell quickphrase clipboard)
  add_library(FusionAddon_${addon} STATIC IMPORTED)
  set_target_properties(FusionAddon_${addon} PROPERTIES
    IMPORTED_LOCATION "${FUSION_FCITX5_ROOT}/lib/fcitx5/${addon}.lib")
  target_link_libraries(FusionFcitxHost PRIVATE FusionAddon_${addon})
endforeach()

# Runtime is a separate Host module. The TSF DLL does not link Fcitx / LibIME.
# Same-directory staging is a reversible local fixture, not release packaging.
file(GLOB FUSION_FCITX5_DLLS "${FUSION_FCITX5_ROOT}/bin/*.dll")
add_custom_command(TARGET FusionFcitxHost POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${FUSION_FCITX5_DLLS} "$<TARGET_FILE_DIR:FusionFcitxHost>"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${FUSION_FCITX5_ROOT}/data" "${CMAKE_BINARY_DIR}/data"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${FUSION_FCITX5_ROOT}/lib/libime" "${CMAKE_BINARY_DIR}/lib/libime")
