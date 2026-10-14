#include "tsf/diag.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>

namespace fusion::tsf::diag {

namespace {

struct LogState {
  std::mutex mutex;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::size_t written = 0;
};

LogState &log_state() {
  static LogState state;
  return state;
}

void close_locked(LogState &state) {
  if (state.file != INVALID_HANDLE_VALUE) {
    CloseHandle(state.file);
    state.file = INVALID_HANDLE_VALUE;
  }
  state.written = 0;
}

// Bounded growth: once the file exceeds kMaxFileBytes it is recreated empty.
void ensure_open_locked(LogState &state) {
  if (state.file != INVALID_HANDLE_VALUE && state.written < kMaxFileBytes) {
    return;
  }
  close_locked(state);
  wchar_t path[MAX_PATH * 2];
  int length = 0;
  wchar_t requested[MAX_PATH * 2];
  DWORD requested_length = GetEnvironmentVariableW(L"FUSION_TSF_LOG", requested,
                                                   static_cast<DWORD>(std::size(requested)));
  if (requested_length > 0 && requested_length < std::size(requested)) {
    length =
        swprintf(path, std::size(path), L"%.*ls", static_cast<int>(requested_length), requested);
  } else {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
      return;
    }
    length = swprintf(path, std::size(path), L"%ls\\fusion-tsf\\fusion-tsf-%lu.log", base,
                      GetCurrentProcessId());
  }
  if (length <= 0 || static_cast<std::size_t>(length) >= std::size(path)) {
    return;
  }
  for (wchar_t *p = path; *p; ++p) {
    if (*p == L'\\') {
      const wchar_t saved = p[1];
      p[1] = L'\0';
      CreateDirectoryW(path, nullptr);
      p[1] = saved;
    }
  }
  state.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

void event(const char *name, const char *details) {
  LogState &state = log_state();
  std::lock_guard<std::mutex> guard(state.mutex);
  ensure_open_locked(state);
  if (state.file == INVALID_HANDLE_VALUE) {
    return;
  }
  char line[1024];
  int length =
      std::snprintf(line, sizeof(line), "ts=%llu pid=%lu tid=%lu %s %s\r\n",
                    static_cast<unsigned long long>(GetTickCount64()), GetCurrentProcessId(),
                    GetCurrentThreadId(), name, details ? details : "");
  if (length <= 0) {
    return;
  }
  DWORD appended = 0;
  WriteFile(state.file, line, static_cast<DWORD>(length), &appended, nullptr);
  state.written += appended;
}

void info(const char *name, const char *details) { event(name, details); }

void warn(const char *name, const char *details) { event(name, details); }

} // namespace fusion::tsf::diag
