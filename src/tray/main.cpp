// U7: the minimal per-user tray surface for the packaged product.
//
// Scope (accepted minimal design, docs/windows-daily-use.md §4):
//   - one tray icon per interactive user x session, reporting whether *this
//     user's* Host is running in *this* session;
//   - one balloon per running -> absent transition, never per key, and no
//     balloon for an intentional manual restart or a session end;
//   - a cancelled session end (WM_ENDSESSION(FALSE)) resumes normal reporting,
//     and a tray that already exited for a session end that was then cancelled is
//     restarted by its own task's cancellation trigger, like the Host;
//   - explicit Restart FusionHost and Open log folder actions, plus Exit;
//   - left click on the icon opens the settings window, or brings the window that is
//     already open to the foreground instead of stacking a duplicate; the right-click
//     menu keeps every action above and stays keyboard reachable;
//   - automatic restart and hang detection are NOT here. A running process is
//     not reported as healthy: the status is "running (pid N)" / "not running".
//
// It lives outside the Host and outside the application-local TSF DLL on
// purpose: a crashed Host cannot report itself, and one tray per application
// would be a second status authority. It never stops the Host - not even when
// it exits - and it never writes a log (diagnostics stay default-off and
// content-free by having no tray log at all).
//
// Usage:
//   FusionTray.exe            tray icon, message loop
//   FusionTray.exe --status   one-shot read-only probe, no window, no icon:
//                             exit 0 host=running, 1 host=absent, 2 unverified

#include "tray/host_watch.hpp"
#include "tray/settings_window.hpp"
#include "tray/tray_state.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

namespace {

// The window class is also the handle the installer uses to ask for an orderly
// exit (FindWindow + WM_CLOSE). It must be a top-level window: a message-only
// window receives neither the session-end broadcast nor FindWindow.
constexpr wchar_t tray_window_class[] = L"FusionTrayWindow";
constexpr UINT tray_callback_message = WM_APP + 1;
constexpr UINT_PTR poll_timer = 1;
constexpr UINT poll_interval_ms = 2000;
constexpr UINT command_restart = 1;
constexpr UINT command_open_logs = 2;
constexpr UINT command_exit = 3;
constexpr UINT command_settings = 4;
constexpr wchar_t restart_marker_name[] = L"restart-in-progress";

fusion::tray::Watch g_watch;
NOTIFYICONDATAW g_icon{};
HICON g_icon_running = nullptr;
HICON g_icon_absent = nullptr;
std::wstring g_version_dir;
bool g_session_ending = false;
bool g_icon_added = false;
UINT g_taskbar_created = 0;
unsigned long g_last_pid = 0;

std::wstring module_directory() {
  wchar_t path[MAX_PATH * 4];
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) {
    return {};
  }
  std::wstring full(path, length);
  const std::size_t slash = full.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return {};
  }
  return full.substr(0, slash);
}

std::wstring parent_directory(const std::wstring &path) {
  const std::size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return {};
  }
  return path.substr(0, slash);
}

std::wstring local_app_data() {
  wchar_t buffer[MAX_PATH * 4];
  const DWORD length =
      GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) {
    return {};
  }
  return std::wstring(buffer, length);
}

// The marker restart-host.ps1 writes for the duration of a manual restart (and
// that install/uninstall write around their own Host stop). It exists only to
// tell "the Host is being restarted on purpose" apart from "the Host died".
std::wstring restart_marker_path() {
  const std::wstring base = local_app_data();
  if (base.empty()) {
    return {};
  }
  return base + L"\\fusion-tray\\" + restart_marker_name;
}

bool restart_in_progress() {
  const std::wstring path = restart_marker_path();
  if (path.empty()) {
    return false;
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return false;
  }
  ULARGE_INTEGER written{};
  written.LowPart = data.ftLastWriteTime.dwLowDateTime;
  written.HighPart = data.ftLastWriteTime.dwHighDateTime;
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER current{};
  current.LowPart = now.dwLowDateTime;
  current.HighPart = now.dwHighDateTime;
  if (current.QuadPart <= written.QuadPart) {
    return true; // clock moved backwards: trust the marker rather than alarm
  }
  const ULONGLONG age_ms = (current.QuadPart - written.QuadPart) / 10000ull;
  return fusion::tray::marker_is_fresh(age_ms);
}

void update_status(bool running, unsigned long pid) {
  g_last_pid = pid;
  wchar_t tip[128];
  if (running) {
    swprintf_s(tip, L"Fusion: Host running (pid %lu)", pid);
  } else {
    wcscpy_s(tip, L"Fusion: Host not running");
  }
  g_icon.uFlags = NIF_ICON | NIF_TIP;
  g_icon.hIcon = running ? g_icon_running : g_icon_absent;
  wcscpy_s(g_icon.szTip, tip);
  if (g_icon_added) {
    Shell_NotifyIconW(NIM_MODIFY, &g_icon);
  }
}

// Adds the icon with every field it needs. Called at startup and again when
// Explorer restarts: the previous icon belonged to the old shell, and the flags
// left behind by a balloon (NIF_INFO) are not the ones NIM_ADD needs.
void add_icon(HWND window) {
  const bool running = g_watch.state() == fusion::tray::HostState::Running;
  g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_icon.uCallbackMessage = tray_callback_message;
  g_icon.hIcon = running ? g_icon_running : g_icon_absent;
  if (running && g_last_pid != 0) {
    swprintf_s(g_icon.szTip, L"Fusion: Host running (pid %lu)", g_last_pid);
  } else if (running) {
    wcscpy_s(g_icon.szTip, L"Fusion: Host running");
  } else if (g_watch.state() == fusion::tray::HostState::Absent) {
    wcscpy_s(g_icon.szTip, L"Fusion: Host not running");
  } else {
    wcscpy_s(g_icon.szTip, L"Fusion: checking the Host");
  }
  g_icon.hWnd = window;
  g_icon_added = Shell_NotifyIconW(NIM_ADD, &g_icon) != FALSE;
}

void notify_host_stopped() {
  if (!g_icon_added) {
    return;
  }
  g_icon.uFlags = NIF_INFO;
  wcscpy_s(g_icon.szInfoTitle, L"Fusion Host stopped");
  wcscpy_s(g_icon.szInfo,
           L"The Fusion Host is not running, so the input method is unavailable. Use "
           L"\"Restart FusionHost\" in this menu to start it again.");
  g_icon.dwInfoFlags = NIIF_WARNING;
  Shell_NotifyIconW(NIM_MODIFY, &g_icon);
}

void open_log_folder() {
  const std::wstring base = local_app_data();
  if (base.empty()) {
    return;
  }
  const std::wstring folder = base + L"\\fusion-host";
  // Creating the directory does not enable logging: the Host writes only under
  // FUSION_HOST_DIAGNOSTICS, and host-diagnostics.ps1 -Status reports the real
  // state of the running process.
  CreateDirectoryW(folder.c_str(), nullptr);
  ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void restart_host() {
  if (g_version_dir.empty()) {
    return;
  }
  const std::wstring script = g_version_dir + L"\\restart-host.ps1";
  wchar_t system_root[MAX_PATH];
  const DWORD length =
      GetEnvironmentVariableW(L"SystemRoot", system_root, static_cast<DWORD>(std::size(system_root)));
  std::wstring powershell = L"powershell.exe";
  if (length > 0 && length < std::size(system_root)) {
    powershell.assign(system_root, length);
    powershell += L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
  }
  // The restart tool owns the lifecycle (verified stop, bounded wait, start
  // through the logon task). The tray only launches it, exactly as the Start
  // Menu shortcut does, and keeps its console visible for the outcome.
  const std::wstring arguments =
      L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Pause";
  ShellExecuteW(nullptr, L"open", powershell.c_str(), arguments.c_str(), g_version_dir.c_str(),
                SW_SHOWNORMAL);
}

// The WinUI settings client lives in bin\settings\ because it carries its own .NET and
// Windows App SDK runtimes; the Qt client stays in bin\ as the rollback path.
std::wstring settings_executable() { return g_version_dir + L"\\bin\\settings\\FusionSettings.exe"; }

// One in-flight launch at a time: a second click while the process is still starting must
// not stack another window, and a launch that failed or exited must not lock the action.
fusion::tray::LaunchGuard g_settings_launch;

void open_settings() {
  if (g_version_dir.empty()) {
    MessageBoxW(nullptr, L"无法确定 Fusion 安装版本目录。", L"Fusion 设置",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return;
  }
  const ULONGLONG now = GetTickCount64();
  const std::wstring executable = settings_executable();
  DWORD session = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &session) != 0) {
    // The intended instance is this exact executable, in this session, on the production
    // endpoint. An older version directory, a development copy or a test instance started
    // with --pipe-name is not it, and is never raised as if it were.
    const fusion::tray::SettingsInstance instance =
        fusion::tray::find_settings_instance(executable, session);
    const bool found = instance.window != nullptr;
    const bool activated = found && fusion::tray::activate_window(instance.window);
    switch (fusion::tray::decide_settings_action(found, activated)) {
    case fusion::tray::SettingsAction::Activate:
      g_settings_launch.clear();
      return;
    case fusion::tray::SettingsAction::NotifyOnly:
      // The window exists but Windows refused the foreground change. Launching again would
      // only add a second window, so ask for attention instead.
      g_settings_launch.clear();
      fusion::tray::request_attention(instance.window);
      return;
    case fusion::tray::SettingsAction::Launch:
      break;
    }
  }
  // Only the launch path is guarded: a click while the previous process is still starting
  // must not stack another window.
  if (g_settings_launch.pending(now)) {
    return;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"open";
  info.lpFile = executable.c_str();
  info.lpDirectory = g_version_dir.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) {
    MessageBoxW(nullptr,
                L"无法启动 Fusion 设置。请确认当前安装包包含 bin\\settings\\FusionSettings.exe，"
                L"且 FusionHost 正在运行。",
                L"Fusion 设置", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return;
  }
  g_settings_launch.started(info.hProcess, now);
}

void exit_tray(HWND window) {
  if (g_icon_added) {
    Shell_NotifyIconW(NIM_DELETE, &g_icon);
    g_icon_added = false;
  }
  DestroyWindow(window);
}

void poll() {
  const fusion::tray::HostObservation observation = fusion::tray::observe_host();
  if (!observation.verified) {
    // The probe could not prove anything (no token SID, no snapshot). Keep the
    // previous state: a failed measurement is not an absence.
    return;
  }
  const bool suppressed = g_session_ending || restart_in_progress();
  const fusion::tray::Observation step =
      g_watch.observe(observation.running, observation.pid, suppressed);
  if (step.changed || step.identity_changed) {
    // A replacement Host is a new identity to show, not a transition to report.
    update_status(observation.running, observation.pid);
  }
  if (step.notify_absent) {
    notify_host_stopped();
  }
}

void show_menu(HWND window, POINT point) {
  wchar_t status[128];
  if (g_watch.state() == fusion::tray::HostState::Running) {
    swprintf_s(status, L"Fusion: Host running (pid %lu)", g_last_pid);
  } else if (g_watch.state() == fusion::tray::HostState::Absent) {
    wcscpy_s(status, L"Fusion: Host not running");
  } else {
    wcscpy_s(status, L"Fusion: checking the Host");
  }
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, command_settings, L"Settings");
  AppendMenuW(menu, MF_STRING, command_restart, L"Restart FusionHost");
  AppendMenuW(menu, MF_STRING, command_open_logs, L"Open log folder");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, command_exit, L"Exit Fusion tray");
  // Without this pair the menu can stay open after the click that dismissed it.
  SetForegroundWindow(window);
  (void)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, window,
                       nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

LRESULT CALLBACK tray_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (g_taskbar_created != 0 && message == g_taskbar_created) {
    // Explorer restarted: the previous icon belonged to the old shell.
    add_icon(window);
    return 0;
  }
  switch (message) {
  case WM_TIMER:
    if (wparam == poll_timer) {
      poll();
    }
    return 0;
  case tray_callback_message:
    switch (LOWORD(lparam)) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
      // Left click is the settings entry point; the context menu stays on the right
      // button so every existing action keeps its place.
      open_settings();
      return 0;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU: {
      POINT point{};
      if (GetCursorPos(&point)) {
        show_menu(window, point);
      }
      return 0;
    }
    default:
      return 0;
    }
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case command_settings:
      open_settings();
      return 0;
    case command_restart:
      restart_host();
      return 0;
    case command_open_logs:
      open_log_folder();
      return 0;
    case command_exit:
      exit_tray(window);
      return 0;
    default:
      return 0;
    }
  case WM_QUERYENDSESSION:
    // Ending the session is not a Host failure: suppress the balloon and let the
    // session end proceed.
    g_session_ending = true;
    return TRUE;
  case WM_ENDSESSION:
    if (wparam != 0) {
      g_session_ending = true;
      exit_tray(window);
      return 0;
    }
    // The session end was cancelled - another application vetoed it, or Windows
    // aborted it after this process had already been told the session was ending.
    // Stop suppressing and drop the baseline: an exit that happened during the
    // attempt is not a fresh failure to report, and the Host's own cancellation
    // trigger starts it again. A later real exit notifies normally.
    g_session_ending = false;
    g_watch.reset();
    poll();
    return 0;
  case WM_CLOSE:
    exit_tray(window);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

int print_status() {
  const fusion::tray::HostObservation observation = fusion::tray::observe_host();
  if (!observation.verified) {
    std::fputs("host=unknown\n", stdout);
    return 2;
  }
  if (observation.running) {
    std::printf("host=running pid=%lu\n", observation.pid);
    return 0;
  }
  std::fputs("host=absent\n", stdout);
  return 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  bool status_only = false;
  int argument_count = 0;
  LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments != nullptr) {
    for (int index = 1; index < argument_count; ++index) {
      if (_wcsicmp(arguments[index], L"--status") == 0) {
        status_only = true;
      }
    }
    LocalFree(arguments);
  }
  if (status_only) {
    return print_status();
  }

  // Single instance per user x session. The name carries both so the object is
  // unambiguous; `Local\` is already per-session. A second instance exits
  // quietly (0) - the logon task may start one while another is alive.
  const std::wstring sid = fusion::tray::current_user_sid();
  if (sid.empty()) {
    return 1; // fail closed: without an identity there is nothing to scope to
  }
  const std::wstring mutex_name = L"Local\\FusionTray-" + sid + L"-" +
                                  std::to_wstring(fusion::tray::current_session_id());
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (mutex == nullptr) {
    return 1; // an existing object this principal may not open: do not run blind
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return 0;
  }

  const std::wstring exe_directory = module_directory();
  g_version_dir = parent_directory(exe_directory); // <version>\bin -> <version>
  g_icon_running = LoadIconW(nullptr, IDI_APPLICATION);
  g_icon_absent = LoadIconW(nullptr, IDI_WARNING);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = tray_window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = tray_window_class;
  if (RegisterClassExW(&window_class) == 0) {
    CloseHandle(mutex);
    return 1;
  }
  HWND window = CreateWindowExW(0, tray_window_class, L"Fusion", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                nullptr, instance, nullptr);
  if (window == nullptr) {
    CloseHandle(mutex);
    return 1;
  }

  g_icon.cbSize = sizeof(g_icon);
  g_icon.hWnd = window;
  g_icon.uID = 1;
  g_icon.uCallbackMessage = tray_callback_message;
  g_icon.hIcon = g_icon_absent;
  wcscpy_s(g_icon.szTip, L"Fusion: checking the Host");
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  add_icon(window);

  // Establish the baseline immediately, then poll on a low-frequency timer: the
  // notification is bounded, not per key, and 2 s is well inside "noticeable".
  poll();
  SetTimer(window, poll_timer, poll_interval_ms, nullptr);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (g_icon_added) {
    Shell_NotifyIconW(NIM_DELETE, &g_icon);
  }
  CloseHandle(mutex);
  return 0;
}
