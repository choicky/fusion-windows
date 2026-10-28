#pragma once

// Stage 4A pipe security: per-user pipe name computation, server identity
// verification, and security descriptor construction.
//
// These functions require <windows.h> and are separated from ipc_protocol.hpp
// so that the protocol header remains platform-free.

#include "fusion/backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fusion::ipc {

// UNFROZEN engineering constant (P0-D OPEN).
// The device path and the product identity are one string here because
// compute_pipe_name() needs both at once. They are not interchangeable: the
// device path is a Win32 transport detail, the identity is part of the name.
inline constexpr const wchar_t *default_pipe_name_prefix = L"\\\\.\\pipe\\FusionHost-v1-";

// The Win32 named-pipe device path. CreateNamedPipeW()/CreateFileW() take it,
// while Qt's QLocalServer/QLocalSocket prepend it themselves, so it is never
// part of a pipe's logical identity.
inline constexpr const wchar_t *pipe_device_path_prefix = L"\\\\.\\pipe\\";

// Compute a per-user per-session pipe name from the current process token SID
// and session ID. Returns empty on failure.
std::wstring compute_pipe_name();

// Reduce a full Win32 pipe path (`\\.\pipe\FusionHost-v1-<sid>-<session>`) or a
// bare logical pipe name (`FusionHost-v1-<sid>-<session>`, `fusion-test`) to the
// logical name QLocalSocket::connectToServer() expects, which Qt completes with
// the device path. The product identity is preserved: only the device path is
// removed. Returns empty when the endpoint is empty, whitespace only, or not a
// single pipe name component, so a caller can fail closed on a malformed
// --pipe-name instead of silently using another endpoint.
std::wstring logical_pipe_name(const std::wstring &endpoint);

// The taskbar search host's package family. It is the one AppContainer principal the
// data pipe admits (D089), and only with kDataPipeClientAccess.
inline constexpr const wchar_t *search_package_family =
    L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy";

// The AppContainer SID that family name derives to. DeriveAppContainerSidFromAppContainerName()
// is deterministic, and this is the answer on the Windows build this product ships on; a
// derivation that disagrees with it means the constant and the OS disagree, and the
// descriptor is then not built at all rather than granting the wrong principal.
inline constexpr const wchar_t *search_package_sid =
    L"S-1-15-2-283421221-3183566570-1718213290-751554359-3541592344-2312209569-3374928651";

// The rights a client needs on the data pipe — and the exact mask the search package is
// granted, so the two cannot drift apart: the client opens with this, the package ACE
// carries it.
//
//   FILE_READ_DATA       read a frame
//   FILE_WRITE_DATA      write a frame
//   FILE_READ_ATTRIBUTES  GetNamedPipeServerProcessId / GetNamedPipeServerSessionId
//   FILE_WRITE_ATTRIBUTES SetNamedPipeHandleState (byte mode)
//   READ_CONTROL          GetSecurityInfo: the AppContainer owner and integrity check
//   SYNCHRONIZE           the overlapped waits on the handle
//
// Deliberately absent: FILE_APPEND_DATA (which is FILE_CREATE_PIPE_INSTANCE for a named
// pipe, so a grantee could create a second instance of this name and pose as the Host),
// WRITE_DAC, WRITE_OWNER, DELETE, the extended-attribute rights, and every generic right.
inline constexpr DWORD kDataPipeClientAccess = FILE_READ_DATA | FILE_WRITE_DATA |
                                               FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                                               READ_CONTROL | SYNCHRONIZE;

// Verify that the server process at the other end of the pipe has the same
// user SID and session ID as the current process. Returns true only if both
// match. `failure` (optional, diagnostics only) receives which step refused —
// an API that failed, with its Win32 code, or a real mismatch.
//
// A client whose own token is an AppContainer token uses the pipe-object check
// instead (see judge_appcontainer_pipe): it cannot open the server process at all,
// and the object's owner, session and integrity label are what separate the real
// Host from another package that created the same name.
bool verify_server_identity(HANDLE pipe_handle, ConnectFailure *failure = nullptr);

// What the AppContainer client read from the pipe object and its own token. Pure
// input, so every branch is testable without a second process.
struct AppContainerPipeFacts {
  PSID own_user = nullptr;   // TokenUser of this process
  PSID pipe_owner = nullptr; // OWNER_SECURITY_INFORMATION of the pipe object
  bool have_own_session = false;
  DWORD own_session = 0;
  bool have_server_session = false;
  DWORD server_session = 0;
  // A pipe with no mandatory label reads as Medium: the absence of a label is the
  // normal case for an object created by a medium-IL process.
  bool have_label = false;
  DWORD label_rid = 0;
};

// The decision, and the reason when it refuses. IdentityFailure::None means the pipe
// may be trusted. Order: session, then owner, then integrity.
IdentityFailure judge_appcontainer_pipe(const AppContainerPipeFacts &facts);

// Build a SECURITY_ATTRIBUTES with a DACL granting read/write only to the
// current user. Fails closed: if the DACL/SD cannot be built, the returned
// SECURITY_ATTRIBUTES has a null lpSecurityDescriptor and the caller must
// treat pipe creation as failed.
//
// This is the descriptor for the objects that stay user-only: the Host claim event and
// the Host stop event. A packaged client that could signal the stop event could end the
// Host, so neither is ever created with make_data_pipe_security().
//
// The caller must call free_pipe_security() when done.
SECURITY_ATTRIBUTES make_pipe_security();

// The data pipe's descriptor. The user ACE keeps the rights it always had; with
// `allow_search_package` one further ACE grants kDataPipeClientAccess to the search
// package's AppContainer SID, and nothing else. The owner is set explicitly to this
// process's TokenUser SID instead of being left to the token default.
//
// This lives in pipe_security_server.cpp because only the Host builds it: the client
// half of this file must not gain a userenv import, which the TIP would then carry
// into an AppContainer.
SECURITY_ATTRIBUTES make_data_pipe_security(bool allow_search_package);

// Free the security descriptor and DACL allocated by make_pipe_security() or
// make_data_pipe_security().
void free_pipe_security(SECURITY_ATTRIBUTES &sa);

} // namespace fusion::ipc
