#include "host/host_diag.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

namespace fusion::host::diag {

namespace {

constexpr wchar_t kDirectoryName[] = L"fusion-host";
constexpr wchar_t kFilePattern[] = L"fusion-host-*.log";
constexpr wchar_t kOptInVariable[] = L"FUSION_HOST_DIAGNOSTICS";

struct LogState {
  std::mutex mutex;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::size_t written = 0;
  bool enabled = false;
  bool initialized = false;
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

// %LOCALAPPDATA%\fusion-host (empty when LOCALAPPDATA is unavailable).
std::wstring directory_path() {
  wchar_t base[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::wstring(base) + L'\\' + kDirectoryName;
}

std::wstring file_path() {
  const std::wstring directory = directory_path();
  if (directory.empty()) {
    return {};
  }
  return directory + L"\\fusion-host-" + std::to_wstring(GetCurrentProcessId()) + L".log";
}

void create_directory(const std::wstring &directory) {
  if (directory.empty()) {
    return;
  }
  // Create every component; an existing one is fine.
  for (std::size_t i = 0; i < directory.size(); ++i) {
    if (directory[i] != L'\\' || i == 0) {
      continue;
    }
    CreateDirectoryW(directory.substr(0, i).c_str(), nullptr);
  }
  CreateDirectoryW(directory.c_str(), nullptr);
}

// Keeps the newest kRetentionFiles-1 existing files: this process is about to
// create its own, so the directory ends up holding at most kRetentionFiles.
// Only the exact fusion-host-*.log pattern is considered, directories are
// skipped, and a failed delete is ignored (another Host may hold the file).
void apply_retention() {
  const std::wstring directory = directory_path();
  if (directory.empty()) {
    return;
  }
  const std::wstring pattern = directory + L'\\' + kFilePattern;
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  struct Entry {
    std::wstring name;
    unsigned long long last_write;
  };
  std::vector<Entry> entries;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    entries.push_back(Entry{data.cFileName, stamp.QuadPart});
  } while (FindNextFileW(find, &data) != 0);
  FindClose(find);

  std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
    if (left.last_write != right.last_write) {
      return left.last_write > right.last_write;
    }
    return left.name > right.name; // deterministic when two files share a stamp
  });
  const std::size_t keep = kRetentionFiles > 1 ? static_cast<std::size_t>(kRetentionFiles - 1) : 0;
  for (std::size_t i = keep; i < entries.size(); ++i) {
    DeleteFileW((directory + L'\\' + entries[i].name).c_str());
  }
}

// Bounded growth: once the file has reached kMaxFileBytes it is recreated empty.
void open_locked(LogState &state) {
  close_locked(state);
  const std::wstring path = file_path();
  if (path.empty()) {
    return;
  }
  create_directory(directory_path());
  state.file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void initialize_locked(LogState &state) {
  if (state.initialized) {
    return;
  }
  state.initialized = true;
  wchar_t value[16] = {};
  const DWORD length =
      GetEnvironmentVariableW(kOptInVariable, value, static_cast<DWORD>(std::size(value)));
  state.enabled =
      length > 0 && length < std::size(value) &&
      (_wcsicmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"on") == 0);
  if (!state.enabled) {
    return;
  }
  apply_retention();
  open_locked(state);
}

} // namespace

void initialize() {
  LogState &state = log_state();
  std::lock_guard<std::mutex> guard(state.mutex);
  initialize_locked(state);
}

bool enabled() {
  LogState &state = log_state();
  std::lock_guard<std::mutex> guard(state.mutex);
  initialize_locked(state);
  return state.enabled;
}

void event(const char *name, const char *details) {
  LogState &state = log_state();
  std::lock_guard<std::mutex> guard(state.mutex);
  initialize_locked(state);
  if (!state.enabled) {
    return;
  }
  if (state.file == INVALID_HANDLE_VALUE || state.written >= kMaxFileBytes) {
    open_locked(state);
  }
  if (state.file == INVALID_HANDLE_VALUE) {
    return;
  }
  // U3: technical fields only. A caller that accidentally passes a key/text
  // bearing field is reduced to a technical marker instead of writing it.
  const char *safe_details = details != nullptr ? details : "";
  for (const char *marker : {"vk=", "symbolic=", "text=", "preedit=", "commit=", "candidate="}) {
    if (std::strstr(safe_details, marker) != nullptr) {
      safe_details = "privacy=redacted";
      break;
    }
  }
  char line[1024];
  const int length = std::snprintf(line, sizeof(line), "ts=%llu pid=%lu tid=%lu %s %s\r\n",
                                   static_cast<unsigned long long>(GetTickCount64()),
                                   GetCurrentProcessId(), GetCurrentThreadId(), name, safe_details);
  if (length <= 0) {
    return;
  }
  DWORD appended = 0;
  WriteFile(state.file, line, static_cast<DWORD>(length), &appended, nullptr);
  state.written += appended;
}

std::string utf8(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return "(unavailable)";
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

void reset_for_tests() {
  LogState &state = log_state();
  std::lock_guard<std::mutex> guard(state.mutex);
  close_locked(state);
  state.enabled = false;
  state.initialized = false;
}

} // namespace fusion::host::diag
