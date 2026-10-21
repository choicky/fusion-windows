#include "tray/settings_window.hpp"

#include <winternl.h>

#include <cstring>

namespace fusion::tray {

namespace {

bool equals_ordinal(const std::wstring &left, const std::wstring &right) {
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring process_image_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    return {};
  }
  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD size = static_cast<DWORD>(std::size(buffer));
  std::wstring result;
  if (QueryFullProcessImageNameW(process, 0, buffer, &size) != 0) {
    result.assign(buffer, size);
  }
  CloseHandle(process);
  return result;
}

// NtQueryInformationProcess(ProcessCommandLineInformation) is the only way to read another
// process's command line without WMI or COM. It is resolved dynamically: when it is absent
// the identity is simply unknown, and the caller treats that as "not the intended
// instance" rather than guessing.
using NtQueryInformationProcessFn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationProcessFn nt_query_information() {
  static NtQueryInformationProcessFn function = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
      return static_cast<NtQueryInformationProcessFn>(nullptr);
    }
    return reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
  }();
  return function;
}

constexpr ULONG kProcessCommandLineInformation = 60;

struct Search {
  const std::wstring *expected_path = nullptr;
  DWORD session_id = 0;
  SettingsInstance found;
};

BOOL CALLBACK visit(HWND window, LPARAM param) {
  auto *search = reinterpret_cast<Search *>(param);
  if (!IsWindowVisible(window) || GetWindowTextLengthW(window) == 0) {
    return TRUE;
  }
  if (GetWindow(window, GW_OWNER) != nullptr) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == 0 || pid == GetCurrentProcessId()) {
    return TRUE;
  }
  DWORD session = 0;
  if (ProcessIdToSessionId(pid, &session) == 0 || session != search->session_id) {
    return TRUE;
  }
  if (!equals_ordinal(process_image_path(pid), *search->expected_path)) {
    return TRUE;
  }
  // Same executable and session: only the production endpoint counts. An isolated or
  // unreadable instance is left alone, so a test run is never raised as the real window.
  const std::optional<std::wstring> command_line = query_command_line(pid);
  if (!command_line || classify_endpoint(*command_line) != EndpointKind::Production) {
    return TRUE;
  }
  search->found.window = window;
  search->found.pid = pid;
  return FALSE;
}

} // namespace

EndpointKind classify_endpoint(const std::wstring &command_line) {
  // Case-insensitive search for the switches the settings executable understands that make
  // it something other than the production instance. A plain launch has none of them.
  std::wstring lowered;
  lowered.reserve(command_line.size());
  for (wchar_t character : command_line) {
    lowered.push_back(static_cast<wchar_t>(towlower(character)));
  }
  for (const wchar_t *marker : {L"--pipe-name", L"--self-test", L"--verify-only"}) {
    if (lowered.find(marker) != std::wstring::npos) {
      return EndpointKind::Isolated;
    }
  }
  return EndpointKind::Production;
}

std::optional<std::wstring> query_command_line(DWORD pid) {
  const NtQueryInformationProcessFn query = nt_query_information();
  if (query == nullptr) {
    return std::nullopt;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    return std::nullopt;
  }
  // One fixed-size buffer instead of a sizing round trip: the structure is a UNICODE_STRING
  // followed by the text, and the text points inside this buffer.
  std::wstring storage(32 * 1024 / sizeof(wchar_t), L'\0');
  ULONG needed = 0;
  std::optional<std::wstring> result;
  const LONG status = query(process, kProcessCommandLineInformation, storage.data(),
                            static_cast<ULONG>(storage.size() * sizeof(wchar_t)), &needed);
  if (status >= 0) {
    const auto *string = reinterpret_cast<const UNICODE_STRING *>(storage.data());
    if (string->Length > 0 && string->Buffer != nullptr) {
      const wchar_t *text = string->Buffer;
      const std::size_t length = string->Length / sizeof(wchar_t);
      if (text >= storage.data() && text + length <= storage.data() + storage.size()) {
        result = std::wstring(text, length);
      }
    }
  }
  CloseHandle(process);
  return result;
}

SettingsInstance find_settings_instance(const std::wstring &expected_executable_path,
                                        DWORD session_id) {
  if (expected_executable_path.empty()) {
    return {};
  }
  Search search;
  search.expected_path = &expected_executable_path;
  search.session_id = session_id;
  EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

SettingsAction decide_settings_action(bool instance_found, bool activated) {
  if (!instance_found) {
    return SettingsAction::Launch;
  }
  return activated ? SettingsAction::Activate : SettingsAction::NotifyOnly;
}

bool activate_window(HWND window) {
  if (window == nullptr || !IsWindow(window)) {
    return false;
  }
  if (IsIconic(window)) {
    ShowWindow(window, SW_RESTORE);
  }
  if (GetForegroundWindow() == window) {
    return true;
  }
  // AttachThreadInput is not a guaranteed permission fix; it is one attempt. The result is
  // measured afterwards and reported honestly.
  const HWND current = GetForegroundWindow();
  const DWORD current_thread = current ? GetWindowThreadProcessId(current, nullptr) : 0;
  const DWORD our_thread = GetCurrentThreadId();
  const bool attached = current_thread != 0 && current_thread != our_thread &&
                        AttachThreadInput(our_thread, current_thread, TRUE) != 0;
  BringWindowToTop(window);
  SetForegroundWindow(window);
  if (attached) {
    AttachThreadInput(our_thread, current_thread, FALSE);
  }
  return GetForegroundWindow() == window;
}

void request_attention(HWND window) {
  if (window == nullptr || !IsWindow(window)) {
    return;
  }
  FLASHWINFO info{};
  info.cbSize = sizeof(info);
  info.hwnd = window;
  info.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
  info.uCount = 0;
  info.dwTimeout = 0;
  FlashWindowEx(&info);
}

LaunchGuard::LaunchGuard(AliveCheck alive) : alive_(std::move(alive)) {}

LaunchGuard::~LaunchGuard() { clear(); }

bool LaunchGuard::process_alive() const {
  if (alive_) {
    return alive_();
  }
  if (process_ == nullptr) {
    return false;
  }
  DWORD code = 0;
  return GetExitCodeProcess(process_, &code) != 0 && code == STILL_ACTIVE;
}

bool LaunchGuard::pending(ULONGLONG now) const {
  if (!started_) {
    return false;
  }
  if (now - since_ >= kStartupTimeoutMs) {
    return false;
  }
  return process_alive();
}

void LaunchGuard::started(HANDLE process, ULONGLONG now) {
  clear();
  process_ = process;
  since_ = now;
  started_ = true;
}

void LaunchGuard::clear() {
  if (process_ != nullptr) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  since_ = 0;
  started_ = false;
}

} // namespace fusion::tray
