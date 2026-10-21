// Focused check for the tray's settings-window lookup (U7 left-click activation).
//
// The tray must activate the settings window that is already open instead of launching a
// duplicate. The lookup is by process image name and session, so this test creates its
// own window and asks for the lookup to find it, without touching any real settings
// process, the Host, or the owner's desktop.
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

std::wstring own_image_name() {
  wchar_t path[MAX_PATH]{};
  require(GetModuleFileNameW(nullptr, path, MAX_PATH) != 0, "GetModuleFileNameW");
  const std::wstring full(path);
  const std::wstring::size_type slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int main() {
  try {
    const wchar_t *class_name = L"FusionTraySettingsWindowProbe";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    require(RegisterClassW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "RegisterClassW");

    HWND window =
        CreateWindowExW(0, class_name, L"Fusion settings probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                        CW_USEDEFAULT, 320, 200, nullptr, nullptr, window_class.hInstance, nullptr);
    require(window != nullptr, "CreateWindowExW");
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    DWORD session = 0;
    require(ProcessIdToSessionId(GetCurrentProcessId(), &session) != 0, "ProcessIdToSessionId");
    const std::wstring image = own_image_name();

    require(fusion::tray::find_window_of_process_image(image.c_str(), session, 0) == window,
            "the window is found by its own image name");
    require(fusion::tray::find_window_of_process_image(L"not-a-real-settings-image.exe", session,
                                                       0) == nullptr,
            "an unrelated image name finds nothing");
    require(fusion::tray::find_window_of_process_image(image.c_str(), session,
                                                       GetCurrentProcessId()) == nullptr,
            "the caller's own process can be excluded");
    require(fusion::tray::find_window_of_process_image(image.c_str(), session + 1, 0) == nullptr,
            "another session finds nothing");
    require(fusion::tray::find_window_of_process_image(nullptr, session, 0) == nullptr,
            "a null image name finds nothing");
    require(fusion::tray::find_window_of_process_image(L"", session, 0) == nullptr,
            "an empty image name finds nothing");
    require(!fusion::tray::activate_window(nullptr), "a null window cannot be activated");

    DestroyWindow(window);
    require(fusion::tray::find_window_of_process_image(image.c_str(), session, 0) == nullptr,
            "a destroyed window is no longer found");
    std::cout << "PASS tray settings window lookup\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
