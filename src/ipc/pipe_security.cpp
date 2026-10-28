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

// The mandatory label's RID from a SACL, if the object carries one. `false` means the
// object has no label, which reads as Medium.
bool mandatory_label_rid(PACL sacl, DWORD &rid) {
  if (sacl == nullptr)
    return false;
  for (WORD index = 0; index < sacl->AceCount; ++index) {
    LPVOID ace = nullptr;
    if (!GetAce(sacl, index, &ace))
      continue;
    const auto *header = static_cast<const ACE_HEADER *>(ace);
    if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
      continue;
    auto *label = static_cast<SYSTEM_MANDATORY_LABEL_ACE *>(ace);
    // This SDK spells the label's SID as SidStart: the SID structure begins there.
    PSID label_sid = reinterpret_cast<PSID>(&label->SidStart);
    if (!IsValidSid(label_sid))
      return false;
    const DWORD count = *GetSidSubAuthorityCount(label_sid);
    if (count == 0)
      return false;
    rid = *GetSidSubAuthority(label_sid, count - 1);
    return true;
  }
  return false;
}

} // namespace

IdentityFailure judge_appcontainer_pipe(const AppContainerPipeFacts &facts) {
  if (!facts.have_own_session || !facts.have_server_session)
    return facts.have_own_session ? IdentityFailure::PipeServerSession
                                  : IdentityFailure::TokenSession;
  if (facts.own_session != facts.server_session)
    return IdentityFailure::SessionMismatch;
  if (facts.own_user == nullptr || facts.pipe_owner == nullptr)
    return IdentityFailure::PipeOwner;
  if (!EqualSid(facts.own_user, facts.pipe_owner))
    return IdentityFailure::PipeOwnerMismatch;
  // No label is Medium. Anything below Medium was created by a low-integrity or
  // AppContainer process, which is what another package posing as the Host would be.
  if (facts.have_label && facts.label_rid < SECURITY_MANDATORY_MEDIUM_RID)
    return IdentityFailure::PipeIntegrityLow;
  return IdentityFailure::None;
}


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

  // Which check applies is a property of *this* token, and it has to be decided before
  // anything touches the server process. An AppContainer client — the TIP inside the
  // taskbar search host — cannot open the server process at all, so the process-based
  // check below would fail on an access denial that says nothing about the peer. It
  // reads the pipe object instead, and that check is only sound because the pipe grants
  // the package client rights and not FILE_CREATE_PIPE_INSTANCE: another package cannot
  // create this name, and a low-integrity object is refused by its label.
  HANDLE own_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_token))
    return refuse(IdentityFailure::OpenToken, GetLastError());
  BOOL appcontainer = FALSE;
  DWORD token_length = 0;
  if (!GetTokenInformation(own_token, TokenIsAppContainer, &appcontainer, sizeof(appcontainer),
                           &token_length)) {
    const unsigned long error = GetLastError();
    CloseHandle(own_token);
    return refuse(IdentityFailure::TokenIsAppContainer, error);
  }

  if (appcontainer) {
    AppContainerPipeFacts facts;

    std::vector<std::uint8_t> user_buffer;
    DWORD user_length = 0;
    GetTokenInformation(own_token, TokenUser, nullptr, 0, &user_length);
    if (user_length == 0) {
      const unsigned long error = GetLastError();
      CloseHandle(own_token);
      return refuse(IdentityFailure::TokenUser, error);
    }
    user_buffer.resize(user_length);
    if (!GetTokenInformation(own_token, TokenUser, user_buffer.data(), user_length,
                             &user_length)) {
      const unsigned long error = GetLastError();
      CloseHandle(own_token);
      return refuse(IdentityFailure::TokenUser, error);
    }
    facts.own_user = reinterpret_cast<TOKEN_USER *>(user_buffer.data())->User.Sid;

    DWORD own_session = 0;
    if (GetTokenInformation(own_token, TokenSessionId, &own_session, sizeof(own_session),
                            &token_length)) {
      facts.have_own_session = true;
      facts.own_session = own_session;
    }
    CloseHandle(own_token);

    DWORD server_session = 0;
    if (GetNamedPipeServerSessionId(pipe_handle, &server_session)) {
      facts.have_server_session = true;
      facts.server_session = server_session;
    }

    PSID owner = nullptr;
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_status =
        GetSecurityInfo(pipe_handle, SE_KERNEL_OBJECT,
                        OWNER_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &owner, nullptr,
                        nullptr, &sacl, &descriptor);
    if (security_status != ERROR_SUCCESS) {
      if (descriptor != nullptr)
        LocalFree(descriptor);
      return refuse(IdentityFailure::PipeOwner, security_status);
    }
    facts.pipe_owner = owner;
    facts.have_label = mandatory_label_rid(sacl, facts.label_rid);

    const IdentityFailure why = judge_appcontainer_pipe(facts);
    LocalFree(descriptor);
    if (why != IdentityFailure::None)
      return refuse(why, 0);
    return true;
  }
  CloseHandle(own_token);

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

  EXPLICIT_ACCESSW ea{};
  ea.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
  ea.grfAccessMode = SET_ACCESS;
  ea.grfInheritance = NO_INHERITANCE;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);

  PACL new_dacl = nullptr;
  if (SetEntriesInAclW(1, &ea, nullptr, &new_dacl) != ERROR_SUCCESS)
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
    // The data-pipe descriptor sets the owner explicitly, and that SID is a copy this
    // side allocated (see make_data_pipe_security). A descriptor that never set one
    // reports none, so nothing else is freed here.
    PSID owner = nullptr;
    BOOL owner_defaulted = FALSE;
    if (GetSecurityDescriptorOwner(sa.lpSecurityDescriptor, &owner, &owner_defaulted) &&
        owner != nullptr) {
      LocalFree(owner);
    }
    LocalFree(sa.lpSecurityDescriptor);
    sa.lpSecurityDescriptor = nullptr;
  }
}

} // namespace fusion::ipc
