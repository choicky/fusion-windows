#include "ipc/pipe_security.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <cwchar>
#include <vector>

namespace fusion::ipc {

namespace {

// Owns a SID that LocalFree() releases, so a failure path cannot leak it.
class OwnedSid {
public:
  OwnedSid() = default;
  OwnedSid(const OwnedSid &) = delete;
  OwnedSid &operator=(const OwnedSid &) = delete;
  ~OwnedSid() {
    if (sid_ != nullptr)
      LocalFree(sid_);
  }

  PSID *receive() { return &sid_; }
  PSID get() const { return sid_; }

private:
  PSID sid_ = nullptr;
};

// Build a pipe DACL that grants read/write to the current user and, when
// `extra_principals` is not empty, to each of those principals as well. Fails
// closed: a null lpSecurityDescriptor means the caller must not create the pipe.
SECURITY_ATTRIBUTES build_pipe_security(const std::vector<PSID> &extra_principals) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);

  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return sa; // fail closed: null SD
  DWORD len = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &len);
  if (len == 0) {
    CloseHandle(token);
    return sa; // fail closed
  }
  std::vector<std::uint8_t> buf(len);
  if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
    CloseHandle(token);
    return sa; // fail closed
  }
  CloseHandle(token);
  auto *user = reinterpret_cast<TOKEN_USER *>(buf.data());

  std::vector<EXPLICIT_ACCESSW> entries;
  entries.reserve(1 + extra_principals.size());
  auto add_allow = [&entries](PSID sid) {
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    entries.push_back(ea);
  };
  add_allow(user->User.Sid);
  for (PSID sid : extra_principals)
    add_allow(sid);

  PACL new_dacl = nullptr;
  if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &new_dacl) !=
      ERROR_SUCCESS)
    return sa; // fail closed: null SD, no DACL to free

  sa.lpSecurityDescriptor = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
  if (!sa.lpSecurityDescriptor) {
    LocalFree(new_dacl);
    return sa; // fail closed
  }
  if (!InitializeSecurityDescriptor(sa.lpSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION)) {
    LocalFree(new_dacl);
    LocalFree(sa.lpSecurityDescriptor);
    sa.lpSecurityDescriptor = nullptr;
    return sa; // fail closed
  }
  if (!SetSecurityDescriptorDacl(sa.lpSecurityDescriptor, TRUE, new_dacl, FALSE)) {
    LocalFree(new_dacl);
    LocalFree(sa.lpSecurityDescriptor);
    sa.lpSecurityDescriptor = nullptr;
    return sa; // fail closed
  }
  return sa;
}

} // namespace

std::wstring compute_pipe_name() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return {};
  DWORD len = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &len);
  if (len == 0) {
    CloseHandle(token);
    return {};
  }
  std::vector<std::uint8_t> buf(len);
  if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
    CloseHandle(token);
    return {};
  }
  CloseHandle(token);
  auto *user = reinterpret_cast<TOKEN_USER *>(buf.data());
  wchar_t *sid_str = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &sid_str))
    return {};
  DWORD session_id = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
  std::wstring name(default_pipe_name_prefix);
  name += sid_str;
  name += L'-';
  name += std::to_wstring(session_id);
  LocalFree(sid_str);
  return name;
}

std::wstring logical_pipe_name(const std::wstring &endpoint) {
  std::wstring name = endpoint;
  const std::wstring device(pipe_device_path_prefix);
  // Win32 object paths are case-insensitive, so accept the device path in any
  // case rather than reject a name that names the right pipe.
  if (name.size() > device.size() && _wcsnicmp(name.c_str(), device.c_str(), device.size()) == 0) {
    name.erase(0, device.size());
  }
  // A logical name is a single pipe name component. Anything that is empty,
  // whitespace only, a path separator, a character Win32 rejects in a pipe name
  // or a control character cannot name an endpoint: report it as invalid rather
  // than let a caller connect somewhere unintended.
  if (name.find_first_not_of(L' ') == std::wstring::npos) {
    return {};
  }
  for (const wchar_t character : name) {
    if (character < 0x20 || std::wcschr(L"\\/:*?\"<>|", character) != nullptr) {
      return {};
    }
  }
  return name;
}

bool verify_server_identity(HANDLE pipe_handle, ConnectFailure *failure) {
  const auto refuse = [failure](IdentityFailure why, unsigned long error) {
    if (failure) {
      failure->identity = why;
      failure->win32_error = error;
    }
    return false;
  };

  DWORD server_pid = 0;
  if (!GetNamedPipeServerProcessId(pipe_handle, &server_pid))
    return refuse(IdentityFailure::ServerProcessId, GetLastError());

  // Check session ID first (cheap).
  DWORD server_session = 0;
  if (!ProcessIdToSessionId(server_pid, &server_session))
    return refuse(IdentityFailure::ServerSession, GetLastError());
  DWORD my_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &my_session))
    return refuse(IdentityFailure::MySession, GetLastError());
  if (server_session != my_session)
    return refuse(IdentityFailure::SessionMismatch, 0);

  // Check user SID. An API that fails here is not a mismatch: it is the access
  // check refusing this process a handle to the server, which is a different
  // fact about the same refusal.
  HANDLE server_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_pid);
  if (!server_proc)
    return refuse(IdentityFailure::OpenProcess, GetLastError());
  HANDLE server_token = nullptr;
  bool ok = false;
  if (OpenProcessToken(server_proc, TOKEN_QUERY, &server_token)) {
    DWORD len = 0;
    GetTokenInformation(server_token, TokenUser, nullptr, 0, &len);
    if (len > 0) {
      std::vector<std::uint8_t> buf(len);
      if (GetTokenInformation(server_token, TokenUser, buf.data(), len, &len)) {
        auto *server_user = reinterpret_cast<TOKEN_USER *>(buf.data());
        HANDLE my_token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &my_token)) {
          DWORD my_len = 0;
          GetTokenInformation(my_token, TokenUser, nullptr, 0, &my_len);
          if (my_len > 0) {
            std::vector<std::uint8_t> my_buf(my_len);
            if (GetTokenInformation(my_token, TokenUser, my_buf.data(), my_len, &my_len)) {
              auto *my_user = reinterpret_cast<TOKEN_USER *>(my_buf.data());
              ok = EqualSid(server_user->User.Sid, my_user->User.Sid) != FALSE;
              if (!ok)
                refuse(IdentityFailure::UserMismatch, 0);
            } else {
              refuse(IdentityFailure::TokenUser, GetLastError());
            }
          } else {
            refuse(IdentityFailure::TokenUser, GetLastError());
          }
          CloseHandle(my_token);
        } else {
          refuse(IdentityFailure::TokenUser, GetLastError());
        }
      } else {
        refuse(IdentityFailure::TokenUser, GetLastError());
      }
    } else {
      refuse(IdentityFailure::TokenUser, GetLastError());
    }
    CloseHandle(server_token);
  } else {
    refuse(IdentityFailure::OpenToken, GetLastError());
  }
  CloseHandle(server_proc);
  return ok;
}

SECURITY_ATTRIBUTES make_pipe_security() {
  return build_pipe_security({});
}

SECURITY_ATTRIBUTES make_data_pipe_security() {
  // The search package is not present on every Windows edition this Host may run
  // on. When the SID cannot be built the pipe keeps exactly the DACL it had
  // before — a packaged client is refused, as it is today — rather than failing
  // closed and leaving the ordinary clients without a Host.
  OwnedSid package;
  if (!ConvertStringSidToSidW(search_package_sid, package.receive()) || package.get() == nullptr)
    return build_pipe_security({});
  return build_pipe_security({package.get()});
}

void free_pipe_security(SECURITY_ATTRIBUTES &sa) {
  if (sa.lpSecurityDescriptor) {
    // Retrieve and free the DACL that was set on the SD.
    BOOL dacl_present = FALSE;
    PACL dacl = nullptr;
    BOOL dacl_defaulted = FALSE;
    if (GetSecurityDescriptorDacl(sa.lpSecurityDescriptor, &dacl_present, &dacl, &dacl_defaulted) &&
        dacl_present && dacl != nullptr) {
      LocalFree(dacl);
    }
    LocalFree(sa.lpSecurityDescriptor);
    sa.lpSecurityDescriptor = nullptr;
  }
}

} // namespace fusion::ipc
