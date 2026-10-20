#include "tray/host_watch.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// ConvertSidToStringSidW lives in advapi32 and is declared by <sddl.h>; it is not
// pulled in by <windows.h>. advapi32 is linked explicitly in CMakeLists.txt.
#include <sddl.h>
#include <tlhelp32.h>

#include <vector>

namespace fusion::tray {

namespace {

constexpr wchar_t host_image_name[] = L"FusionHost.exe";

// The token user SID of an already-open process, as text. Empty on any failure:
// the caller treats "unreadable" as "not verified", never as "someone else".
std::wstring token_user_sid(HANDLE process) {
  HANDLE token = nullptr;
  // PROCESS_QUERY_LIMITED_INFORMATION is enough for OpenProcessToken (Vista+).
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    return {};
  }
  std::wstring result;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size > 0) {
    std::vector<unsigned char> buffer(size);
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
      auto *user = reinterpret_cast<TOKEN_USER *>(buffer.data());
      wchar_t *text = nullptr;
      if (ConvertSidToStringSidW(user->User.Sid, &text)) {
        result.assign(text);
        LocalFree(text);
      }
    }
  }
  CloseHandle(token);
  return result;
}

} // namespace

std::wstring current_user_sid() {
  return token_user_sid(GetCurrentProcess());
}

unsigned long current_session_id() {
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
    return 0;
  }
  return session;
}

HostObservation observe_host() {
  HostObservation result;
  const std::wstring me = current_user_sid();
  if (me.empty()) {
    return result; // unverified: no way to prove ownership
  }
  const DWORD session = static_cast<DWORD>(current_session_id());

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return result;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return result;
  }
  // A process named FusionHost.exe that this process cannot judge - it cannot be
  // opened, its session cannot be read, its token user cannot be read, or the
  // enumeration itself stopped early - makes the whole look inconclusive. "We
  // could not tell" must never be reported as "it is gone": that would raise a
  // stopped notification for a Host that may still be running.
  bool unresolved = false;
  bool complete = false;
  do {
    if (_wcsicmp(entry.szExeFile, host_image_name) != 0) {
      continue;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (process == nullptr) {
      // ERROR_INVALID_PARAMETER is what a process that has already exited
      // reports; anything else (access denied, for example) is a process whose
      // identity cannot be established.
      if (GetLastError() != ERROR_INVALID_PARAMETER) {
        unresolved = true;
      }
      continue;
    }
    DWORD process_session = 0;
    if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session)) {
      unresolved = true;
      CloseHandle(process);
      continue;
    }
    if (process_session != session) {
      CloseHandle(process); // another session: not the Host this tray reports
      continue;
    }
    const std::wstring owner = token_user_sid(process);
    CloseHandle(process);
    if (owner.empty()) {
      unresolved = true;
      continue;
    }
    if (_wcsicmp(owner.c_str(), me.c_str()) == 0) {
      result.running = true;
      result.pid = entry.th32ProcessID;
      complete = true;
      break;
    }
    // Readable and someone else's: resolved, and not this tray's Host.
  } while (Process32NextW(snapshot, &entry));
  if (!complete) {
    // Normal completion reports ERROR_NO_MORE_FILES; anything else means the walk
    // stopped before the process list was exhausted.
    complete = GetLastError() == ERROR_NO_MORE_FILES;
  }
  CloseHandle(snapshot);

  // A matching Host is a positive measurement whatever else could not be read.
  if (result.running) {
    result.verified = true;
    return result;
  }
  if (unresolved || !complete) {
    return result; // inconclusive: keep the caller's previous state
  }
  result.verified = true;
  return result;
}

} // namespace fusion::tray
