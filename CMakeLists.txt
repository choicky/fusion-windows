cmake_minimum_required(VERSION 3.20)
project(FusionWindowsStage1 LANGUAGES CXX)

option(FUSION_WITH_FCITX5 "Enable the Stage 4A out-of-process Fcitx Host (FusionHost.exe + IPC)" OFF)
if(FUSION_WITH_FCITX5)
  include(cmake/FcitxBackend.cmake)
endif()

add_library(FusionCore src/core.cpp)
target_include_directories(FusionCore PUBLIC include)
target_compile_features(FusionCore PUBLIC cxx_std_20)
set_target_properties(FusionCore PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(FusionCore PRIVATE /W4 /WX)
else()
  target_compile_options(FusionCore PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(FusionHost INTERFACE)
target_include_directories(FusionHost INTERFACE include)
target_link_libraries(FusionHost INTERFACE FusionCore)
target_compile_features(FusionHost INTERFACE cxx_std_20)

# Stage 2 semantic Mock Backend. It stays a test abstraction; the Stage 2.5 TSF
# shell links the same source as its semantic oracle (no second backend).
add_library(MockBackend tests/mock_backend.cpp)
target_link_libraries(MockBackend PUBLIC FusionHost)
target_include_directories(MockBackend PUBLIC tests)

if(WIN32 AND MSVC)
  add_executable(FusionTsfProfile tools/tsf/profile.cpp)
  target_link_libraries(FusionTsfProfile PRIVATE ole32)
  # Stage 2.5 real TSF/COM development shell (x64; see README for registration).
  # Stage 4A: FusionTsf.dll no longer links Fcitx runtime directly.
  # When FUSION_WITH_FCITX5 is ON, it uses the IPC backend to talk to FusionHost.exe.
  add_library(FusionTsf SHARED
    src/tsf/guids.cpp
    src/tsf/key_map.cpp
    src/tsf/diag.cpp
    src/tsf/semantic.cpp
    src/tsf/candidate_view.cpp
    src/tsf/service.cpp
    src/tsf/register.cpp
    src/tsf/dll.cpp
    src/tsf/fusiontsf.def)
  target_include_directories(FusionTsf PRIVATE include src tests "${CMAKE_CURRENT_BINARY_DIR}/generated")
  configure_file(src/tsf/build_identity.hpp.in generated/tsf/build_identity.hpp @ONLY)
  target_compile_features(FusionTsf PRIVATE cxx_std_20)
  set_target_properties(FusionTsf PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options(FusionTsf PRIVATE /W4 /WX /utf-8)
  target_compile_definitions(FusionTsf PRIVATE UNICODE _UNICODE)
  target_link_libraries(FusionTsf PRIVATE FusionCore MockBackend ole32 advapi32 user32 gdi32)
  if(FUSION_WITH_FCITX5)
    target_compile_definitions(FusionTsf PRIVATE FUSION_WITH_FCITX5)
    # Client side only: the TSF DLL never hosts a pipe server.
    target_sources(FusionTsf PRIVATE
      src/ipc/pipe_client.cpp
      src/ipc/pipe_security.cpp
      src/ipc/ipc_backend.cpp)
  endif()

  # Stage 4A: out-of-process Fusion Host executable.
  # Links the real Fcitx backend and serves IPC requests from FusionTsf.dll.
  if(FUSION_WITH_FCITX5)
    add_executable(FusionHostExe
      src/host/main.cpp
      src/host/host_server.cpp
      src/ipc/pipe_server.cpp
      src/ipc/pipe_security.cpp)
    target_include_directories(FusionHostExe PRIVATE include src)
    target_compile_features(FusionHostExe PRIVATE cxx_std_20)
    set_target_properties(FusionHostExe PROPERTIES CXX_EXTENSIONS OFF OUTPUT_NAME FusionHost)
    target_compile_definitions(FusionHostExe PRIVATE FUSION_WITH_FCITX5 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(FusionHostExe PRIVATE /W4 /WX /utf-8)
    target_link_libraries(FusionHostExe PRIVATE FusionFcitxHost FusionCore)
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  if(FUSION_WITH_FCITX5)
    add_executable(stage3b tests/stage3b.cpp src/tsf/key_map.cpp src/tsf/semantic.cpp)
    target_include_directories(stage3b PRIVATE src)
    target_compile_definitions(stage3b PRIVATE FUSION_WITH_FCITX5)
    target_compile_options(stage3b PRIVATE /W4 /WX /utf-8)
    target_link_libraries(stage3b PRIVATE FusionFcitxHost MockBackend)
    add_test(NAME stage3b_real_backend COMMAND stage3b)

    add_executable(stage3c tests/stage3c.cpp src/tsf/key_map.cpp src/tsf/semantic.cpp)
    target_include_directories(stage3c PRIVATE src)
    target_compile_definitions(stage3c PRIVATE FUSION_WITH_FCITX5)
    target_compile_options(stage3c PRIVATE /W4 /WX /utf-8)
    target_link_libraries(stage3c PRIVATE FusionFcitxHost MockBackend)
    add_test(NAME stage3c_faithful_frontend COMMAND stage3c)

    # Dependency portability regression for the two MSVC empty-range guards in
    # cmake/patches/fcitx5-msvc.patch (fcitx-utils/utf8.h). Drives the real Fcitx
    # engine directly; it does not exercise or change TSF key admission.
    add_executable(fcitx_msvc_empty_range_guards tests/fcitx_msvc_empty_range_guards.cpp)
    target_include_directories(fcitx_msvc_empty_range_guards PRIVATE include)
    target_compile_features(fcitx_msvc_empty_range_guards PRIVATE cxx_std_20)
    target_compile_options(fcitx_msvc_empty_range_guards PRIVATE /W4 /WX /utf-8)
    target_link_libraries(fcitx_msvc_empty_range_guards PRIVATE FusionFcitxHost)
    add_test(NAME fcitx_msvc_empty_range_guards COMMAND fcitx_msvc_empty_range_guards)
  endif()

  if(WIN32 AND MSVC)
    # Stage 4A IPC tests: the protocol, the IpcBackend client and the HostServer
    # transport are independent of Fcitx, so they compile and run in every
    # Windows/MSVC build (Mock-only included). Only the end-to-end
    # FusionHost.exe + real Fcitx mode needs FUSION_WITH_FCITX5.
    add_executable(stage4a
      tests/stage4a_ipc.cpp
      src/ipc/pipe_client.cpp
      src/ipc/pipe_server.cpp
      src/ipc/pipe_security.cpp
      src/ipc/ipc_backend.cpp
      src/host/host_server.cpp
      src/tsf/key_map.cpp
      src/tsf/semantic.cpp)
    target_include_directories(stage4a PRIVATE src)
    target_compile_features(stage4a PRIVATE cxx_std_20)
    # std::getenv in the integration mode; same convention as FusionFcitxHost.
    target_compile_definitions(stage4a PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(stage4a PRIVATE /W4 /WX /utf-8)
    target_link_libraries(stage4a PRIVATE MockBackend)
    add_test(NAME stage4a_ipc_protocol COMMAND stage4a protocol)
    add_test(NAME stage4a_ipc_loopback COMMAND stage4a loopback)
    add_test(NAME stage4a_ipc_hostserver COMMAND stage4a hostserver)
    if(FUSION_WITH_FCITX5)
      target_compile_definitions(stage4a PRIVATE FUSION_WITH_FCITX5)
      target_link_libraries(stage4a PRIVATE FusionFcitxHost)
      add_test(NAME stage4a_ipc_integration COMMAND stage4a integration)
    endif()

    # Stage 4B reconnect / semantic-readiness / incarnation-fencing tests. They
    # drive the real product classes (IpcBackend + HostServer + the Stage 2.5/3C
    # semantic controller) against a deterministic Host-side test double, so they
    # build and run in every Windows/MSVC build (Mock-only included).
    add_executable(stage4b
      tests/stage4b_reconnect.cpp
      src/ipc/pipe_client.cpp
      src/ipc/pipe_server.cpp
      src/ipc/pipe_security.cpp
      src/ipc/ipc_backend.cpp
      src/host/host_server.cpp
      src/tsf/key_map.cpp
      src/tsf/semantic.cpp)
    target_include_directories(stage4b PRIVATE src)
    target_compile_features(stage4b PRIVATE cxx_std_20)
    # std::getenv in the real-Host mode; same convention as stage4a.
    target_compile_definitions(stage4b PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(stage4b PRIVATE /W4 /WX /utf-8)
    target_link_libraries(stage4b PRIVATE MockBackend)
    foreach(mode link failover cycles fencing contexts semantic)
      add_test(NAME stage4b_${mode} COMMAND stage4b ${mode})
    endforeach()
    if(FUSION_WITH_FCITX5)
      # Real FusionHost.exe process failover: needs the built Host executable and
      # the Fcitx runtime on PATH. FUSION_STAGE4B_SKIP_HOST_PROCESS skips them.
      target_compile_definitions(stage4b PRIVATE FUSION_WITH_FCITX5)
      target_link_libraries(stage4b PRIVATE FusionFcitxHost)
      add_test(NAME stage4b_hostprocess COMMAND stage4b hostprocess)
      add_test(NAME stage4b_silentdeath COMMAND stage4b silentdeath)
    endif()
  endif()
  add_library(FusionMock tests/mock.cpp)
  target_link_libraries(FusionMock PUBLIC FusionCore)
  target_include_directories(FusionMock PUBLIC tests)
  add_executable(rg0 tests/rg0.cpp)
  target_link_libraries(rg0 PRIVATE FusionMock)

  add_executable(stage2 tests/stage2.cpp)
  target_link_libraries(stage2 PRIVATE MockBackend FusionMock)

  # Stage 2.5 adapter logic (key map + semantic controller) is platform-free.
  add_executable(tsf tests/tsf_semantic.cpp src/tsf/key_map.cpp src/tsf/semantic.cpp)
  target_link_libraries(tsf PRIVATE MockBackend FusionMock)
  target_include_directories(tsf PRIVATE src)
  if(MSVC)
    target_compile_options(tsf PRIVATE /utf-8)
  endif()

  if(NOT MSVC)
    target_compile_options(FusionMock PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(rg0 PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(MockBackend PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(stage2 PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(tsf PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_executable(tsf_orchestration tests/tsf_orchestration.cpp src/tsf/key_map.cpp src/tsf/semantic.cpp)
  target_link_libraries(tsf_orchestration PRIVATE MockBackend FusionMock)
  target_include_directories(tsf_orchestration PRIVATE src)
  if(MSVC)
    target_compile_options(tsf_orchestration PRIVATE /W4 /WX /utf-8)
  else()
    target_compile_options(tsf_orchestration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  foreach(test routing cleanup termination)
    add_test(NAME tsf_orchestration_${test} COMMAND tsf_orchestration ${test})
  endforeach()
  # Stage 4C lifecycle obligations and teardown accounting are platform-free
  # decisions, so they are tested with the same portable sources.
  add_executable(tsf_lifecycle tests/tsf_lifecycle.cpp src/tsf/key_map.cpp src/tsf/semantic.cpp)
  target_link_libraries(tsf_lifecycle PRIVATE MockBackend FusionMock)
  target_include_directories(tsf_lifecycle PRIVATE src)
  if(MSVC)
    target_compile_options(tsf_lifecycle PRIVATE /W4 /WX /utf-8)
  else()
    target_compile_options(tsf_lifecycle PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  foreach(test obligation teardown_clean teardown_unresolved composition_terminated
               identity_across_focus context_isolation)
    add_test(NAME tsf_lifecycle_${test} COMMAND tsf_lifecycle ${test})
  endforeach()
  # Stage 5 candidate projection/action decisions are platform-free controller
  # logic (the Win32 surface is smoke-tested on the real product path), so they
  # build with the same portable sources as tsf_lifecycle.
  add_executable(stage5_candidates tests/stage5_candidates.cpp src/tsf/key_map.cpp src/tsf/semantic.cpp)
  target_link_libraries(stage5_candidates PRIVATE MockBackend FusionMock)
  target_include_directories(stage5_candidates PRIVATE src)
  if(MSVC)
    target_compile_options(stage5_candidates PRIVATE /W4 /WX /utf-8)
  else()
    target_compile_options(stage5_candidates PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  foreach(test page select refuse paging truncation focus)
    add_test(NAME stage5_candidates_${test} COMMAND stage5_candidates ${test})
  endforeach()
  # Blocker-1 regression: the extracted candidate projection driven against the
  # real Fcitx5 engine (system libraries). Non-Windows only, and registered
  # only when Fcitx5 development libraries are found; the Windows build already
  # compiles the same source into FusionHost.exe and smoke-tests it on the
  # product path, so the pinned Windows topology is not perturbed here.
  if(NOT WIN32)
    find_package(Fcitx5Core QUIET CONFIG)
    if(Fcitx5Core_FOUND)
      add_executable(fcitx_candidate_filter
        tests/fcitx_candidate_filter.cpp src/fcitx/candidate_projection.cpp)
      target_include_directories(fcitx_candidate_filter PRIVATE src include)
      target_link_libraries(fcitx_candidate_filter PRIVATE Fcitx5::Core)
      target_compile_features(fcitx_candidate_filter PRIVATE cxx_std_20)
      target_compile_options(fcitx_candidate_filter PRIVATE -Wall -Wextra -Wpedantic -Werror)
      add_test(NAME fcitx_candidate_filter COMMAND fcitx_candidate_filter)
    endif()
  endif()
  foreach(test loss duplicate context stale order bounds pass terminal cancelled retry reference)
    add_test(NAME rg0_${test} COMMAND rg0 ${test})
  endforeach()
  foreach(test state_path effect_path pass pass_barrier delay reorder duplicate stale restart
               reject indeterminate bound)
    add_test(NAME stage2_${test} COMMAND stage2 ${test})
  endforeach()
  foreach(test classification typing pass_paths fail_open reject backspace async_fallback
               drain_before_pass cancelled_recovery composition_terminated context_fence ordering repaired_boundaries)
    add_test(NAME tsf_${test} COMMAND tsf ${test})
  endforeach()
endif()
