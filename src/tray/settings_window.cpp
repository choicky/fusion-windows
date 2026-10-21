#include "tray/settings_window.hpp"

#include <string>

namespace fusion::tray {

namespace {

bool same_image_name(const std::wstring &path, const wchar_t *image_name) {
  const std::wstring::size_type slash = path.find_last_of(L"\\/");
  const std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
  return CompareStringOrdinal(base.c_str(), static_cast<int>(base.size()), image_name, -1, TRUE) ==
         CSTR_EQUAL;
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

struct Search {
  const wchar_t *image_name = nullptr;
  DWORD session_id = 0;
  DWORD exclude_pid = 0;
  HWND found = nullptr;
};

BOOL CALLBACK visit(HWND window, LPARAM param) {
  auto *search = reinterpret_cast<Search *>(param);
  if (!IsWindowVisible(window) || GetWindowTextLengthW(window) == 0) {
    return TRUE;
  }
  if (GetWindow(window, GW_OWNER) != nullptr) {
    return TRUE; // owned dialogs belong to their owner's process anyway
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == 0 || pid == search->exclude_pid) {
    return TRUE;
  }
  DWORD session = 0;
  if (ProcessIdToSessionId(pid, &session) == 0 || session != search->session_id) {
    return TRUE;
  }
  const std::wstring path = process_image_path(pid);
  if (path.empty() || !same_image_name(path, search->image_name)) {
    return TRUE;
  }
  search->found = window;
  return FALSE;
}

} // namespace

HWND find_window_of_process_image(const wchar_t *image_name, DWORD session_id, DWORD exclude_pid) {
  if (image_name == nullptr || image_name[0] == L'\0') {
    return nullptr;
  }
  Search search;
  search.image_name = image_name;
  search.session_id = session_id;
  search.exclude_pid = exclude_pid;
  EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
  return search.found;
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
  // A background process may not raise a window on its own; attaching to the current
  // foreground thread lifts that restriction for the duration of the call.
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

} // namespace fusion::tray
