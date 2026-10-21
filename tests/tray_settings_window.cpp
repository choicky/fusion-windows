// Focused checks for the tray's settings lookup, activation decision and launch guard.
//
// Three layers are covered separately, because they fail independently:
//   - lookup/decision: pure functions over a command line and a found/activated pair;
//   - instance identification: a real child process with a window is found by full path and
//     session, while a child started with --pipe-name is not mistaken for it;
//   - launch guard: repeated clicks during startup launch once, and a dead or timed-out
//     launch can be retried.
//
// The lookup runs against child processes on purpose: the tray must never treat its own
// window as the settings window, so the in-process case is excluded by design. Nothing here
// touches the owner's Host, tray or settings window.
#include "tray/settings_window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::wstring own_image_path() {
  wchar_t path[MAX_PATH * 2]{};
  require(GetModuleFileNameW(nullptr, path, MAX_PATH * 2) != 0, "GetModuleFileNameW");
  return std::wstring(path);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

// Child mode: hold one titled window open so the parent can look for it.
int run_child(const wchar_t *title) {
  const wchar_t *class_name = L"FusionTraySettingsWindowProbe";
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = class_name;
  if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 2;
  }
  HWND window =
      CreateWindowExW(0, class_name, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320,
                      200, nullptr, nullptr, window_class.hInstance, nullptr);
  if (window == nullptr) {
    return 2;
  }
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  const ULONGLONG deadline = GetTickCount64() + 20000;
  MSG message{};
  while (GetTickCount64() < deadline) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(20);
  }
  DestroyWindow(window);
  return 0;
}

struct Child {
  PROCESS_INFORMATION info{};
  HWND window = nullptr;

  Child() = default;
  Child(const Child &) = delete;
  Child &operator=(const Child &) = delete;
  ~Child() { stop(); }

  bool start(const std::wstring &command_line, const wchar_t *title) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring mutable_command = command_line;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &info)) {
      return false;
    }
    for (int attempt = 0; attempt < 60 && window == nullptr; ++attempt) {
      Sleep(100);
      window = FindWindowW(nullptr, title);
    }
    return window != nullptr;
  }

  void stop() {
    if (info.hProcess != nullptr) {
      TerminateProcess(info.hProcess, 0);
      WaitForSingleObject(info.hProcess, 3000);
      CloseHandle(info.hThread);
      CloseHandle(info.hProcess);
      info = PROCESS_INFORMATION{};
    }
    // Let the window disappear before the next lookup.
    for (int attempt = 0; attempt < 30 && window != nullptr; ++attempt) {
      if (!IsWindow(window)) {
        window = nullptr;
        break;
      }
      Sleep(50);
    }
    window = nullptr;
  }
};

void check_endpoint_classification() {
  using fusion::tray::EndpointKind;
  require(fusion::tray::classify_endpoint(
              L"\"C:\\Program Files\\Fcitx5 Fusion\\x\\bin\\FusionSettings.exe\"") ==
              EndpointKind::Production,
          "a plain launch is the production endpoint");
  require(fusion::tray::classify_endpoint(L"FusionSettings.exe --pipe-name \\\\.\\pipe\\test") ==
              EndpointKind::Isolated,
          "--pipe-name is an isolated instance");
  require(fusion::tray::classify_endpoint(L"FusionSettings.exe --self-test") ==
              EndpointKind::Isolated,
          "--self-test is an isolated instance");
  require(fusion::tray::classify_endpoint(L"FusionSettings.exe --verify-only") ==
              EndpointKind::Isolated,
          "--verify-only is an isolated instance");
  require(fusion::tray::classify_endpoint(L"FusionSettings.exe --PIPE-NAME x") ==
              EndpointKind::Isolated,
          "the switch is matched case-insensitively");
  require(fusion::tray::classify_endpoint(L"") == EndpointKind::Production,
          "an empty command line is the production endpoint");
}

void check_action_decision() {
  using fusion::tray::SettingsAction;
  require(fusion::tray::decide_settings_action(false, false) == SettingsAction::Launch,
          "no instance launches");
  require(fusion::tray::decide_settings_action(true, true) == SettingsAction::Activate,
          "a raised instance needs no launch");
  require(fusion::tray::decide_settings_action(true, false) == SettingsAction::NotifyOnly,
          "a found instance whose activation was refused must not launch again");
}

void check_launch_guard() {
  bool alive = true;
  fusion::tray::LaunchGuard guard([&alive] { return alive; });
  require(!guard.pending(1000), "nothing is pending before a launch");

  guard.started(nullptr, 1000);
  require(guard.pending(1000), "a launch is pending immediately after starting");
  require(guard.pending(1000 + fusion::tray::LaunchGuard::kStartupTimeoutMs - 1),
          "a launch is still pending inside the startup window");

  alive = false;
  require(!guard.pending(1200), "an exited process is no longer pending");
  alive = true;
  require(!guard.pending(1000 + fusion::tray::LaunchGuard::kStartupTimeoutMs),
          "the startup window expires instead of locking the action out");

  guard.clear();
  require(!guard.pending(2000), "a cleared guard allows a new launch");
}

void check_command_line_of_self() {
  const std::optional<std::wstring> command_line =
      fusion::tray::query_command_line(GetCurrentProcessId());
  if (!command_line) {
    std::cout << "NOTE command line of this process is not readable; lookup treats an "
                 "unreadable instance as not intended\n";
    return;
  }
  require(fusion::tray::classify_endpoint(*command_line) == fusion::tray::EndpointKind::Production,
          "this test process runs on the production endpoint");
}

void check_instance_lookup() {
  const std::wstring path = own_image_path();
  DWORD session = 0;
  require(ProcessIdToSessionId(GetCurrentProcessId(), &session) != 0, "ProcessIdToSessionId");
  const std::wstring quoted = L"\"" + path + L"\"";
  const wchar_t *isolated_title = L"Fusion settings isolated probe";
  const wchar_t *production_title = L"Fusion settings production probe";

  // 1. An isolated instance on the same executable path is not the intended instance.
  Child isolated;
  require(isolated.start(quoted + L" --pipe-name \\\\.\\pipe\\fusion-tray-test --child-window \"" +
                             isolated_title + L"\"",
                         isolated_title),
          "the isolated child created a window");
  require(fusion::tray::find_settings_instance(path, session).window == nullptr,
          "an isolated instance on the same path is not the intended instance");
  isolated.stop();
  Sleep(200);

  // 2. A plain instance on the same path and session is the intended instance.
  Child production;
  require(
      production.start(quoted + L" --child-window \"" + production_title + L"\"", production_title),
      "the production child created a window");
  const fusion::tray::SettingsInstance found = fusion::tray::find_settings_instance(path, session);
  require(found.window == production.window, "the production instance is found");
  require(found.pid == production.info.dwProcessId, "the found pid is the child process");

  // 3. Wrong path and wrong session are not the intended instance.
  require(
      fusion::tray::find_settings_instance(L"C:\\nowhere\\FusionSettings.exe", session).window ==
          nullptr,
      "another executable path finds nothing");
  require(fusion::tray::find_settings_instance(path, session + 1).window == nullptr,
          "another session finds nothing");
  require(fusion::tray::find_settings_instance(L"", session).window == nullptr,
          "an empty path finds nothing");
  production.stop();
}

} // namespace

int main(int argc, char **argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--child-window") {
        const char *title = i + 1 < argc ? argv[i + 1] : "Fusion settings probe";
        wchar_t wide[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wide, 256);
        return run_child(wide);
      }
    }
    check_endpoint_classification();
    check_action_decision();
    check_launch_guard();
    check_command_line_of_self();
    check_instance_lookup();
    std::cout << "PASS tray settings lookup, activation decision and launch guard\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
