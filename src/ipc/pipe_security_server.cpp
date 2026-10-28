// The data pipe's security descriptor, server side only.
//
// It is a separate translation unit from pipe_security.cpp because deriving the search
// package's SID needs userenv.dll, and pipe_security.cpp is compiled into the TIP DLL as
// well. The TIP must not gain an import it never calls: it runs inside an AppContainer,
// where every module it loads is one more thing that can be denied.

#include "ipc/pipe_security.hpp"

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <cstdint>
#include <vector>

namespace fusion::ipc {
namespace {

// This process's TokenUser SID, pointing into `buffer`.
bool own_token_user(std::vector<std::uint8_t> &buffer, PSID &sid) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  DWORD length = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &length);
  if (length == 0) {
    CloseHandle(token);
    return false;
  }
  buffer.resize(length);
  const bool ok = GetTokenInformation(token, TokenUser, buffer.data(), length, &length) != FALSE;
  CloseHandle(token);
  if (!ok)
    return false;
  sid = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
  return true;
}

// The search package's AppContainer SID, or null when this OS derives something other
// than the constant this build was written against. A caller must treat null as "do not
// grant anything": granting a different principal would be worse than not starting.
PSID derived_search_package_sid() {
  PSID derived = nullptr;
  if (FAILED(DeriveAppContainerSidFromAppContainerName(search_package_family, &derived)) ||
      derived == nullptr)
    return nullptr;

  LPWSTR text = nullptr;
  const bool converted = ConvertSidToStringSidW(derived, &text) != FALSE;
  const bool matches = converted && text != nullptr && wcscmp(text, search_package_sid) == 0;
  if (converted)
    LocalFree(text);
  if (!matches) {
    FreeSid(derived);
    return nullptr;
  }
  return derived;
}

} // namespace

SECURITY_ATTRIBUTES make_data_pipe_security(bool allow_search_package) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);

  std::vector<std::uint8_t> user_buffer;
  PSID user_sid = nullptr;
  if (!own_token_user(user_buffer, user_sid))
    return sa; // fail closed: no SD, the caller treats pipe creation as failed

  std::vector<EXPLICIT_ACCESSW> entries;

  // The user's own ACE is what it has always been: the owner of the object may do what
  // it likes with it, and no normal client sees a change.
  EXPLICIT_ACCESSW user{};
  user.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
  user.grfAccessMode = SET_ACCESS;
  user.grfInheritance = NO_INHERITANCE;
  user.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  user.Trustee.ptstrName = static_cast<LPWSTR>(user_sid);
  entries.push_back(user);

  PSID package = nullptr;
  if (allow_search_package) {
    package = derived_search_package_sid();
    if (package == nullptr)
      return sa; // fail closed: the derived SID is not the one this build expects
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = kDataPipeClientAccess;
    entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(package);
    entries.push_back(entry);
  }

  PACL dacl = nullptr;
  const DWORD acl_status =
      SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &dacl);
  if (package != nullptr)
    FreeSid(package);
  if (acl_status != ERROR_SUCCESS)
    return sa; // fail closed: no DACL to free

  sa.lpSecurityDescriptor = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
  if (!sa.lpSecurityDescriptor) {
    LocalFree(dacl);
    return sa;
  }
  if (!InitializeSecurityDescriptor(sa.lpSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(sa.lpSecurityDescriptor, TRUE, dacl, FALSE)) {
    LocalFree(dacl);
    LocalFree(sa.lpSecurityDescriptor);
    sa.lpSecurityDescriptor = nullptr;
    return sa;
  }

  // The owner is set explicitly rather than left to the token default. The descriptor
  // keeps the pointer, not a copy, so the SID is duplicated into memory that lives as
  // long as the descriptor; free_pipe_security() releases it.
  const DWORD sid_length = GetLengthSid(user_sid);
  PSID owner = static_cast<PSID>(LocalAlloc(LPTR, sid_length));
  if (owner == nullptr || !CopySid(sid_length, owner, user_sid) ||
      !SetSecurityDescriptorOwner(sa.lpSecurityDescriptor, owner, FALSE)) {
    if (owner != nullptr)
      LocalFree(owner);
    free_pipe_security(sa);
    return sa;
  }
  return sa;
}

} // namespace fusion::ipc
