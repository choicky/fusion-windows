#pragma once

// Stage 4A pipe security: per-user pipe name computation, server identity
// verification, and security descriptor construction.
//
// These functions require <windows.h> and are separated from ipc_protocol.hpp
// so that the protocol header remains platform-free.

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
inline constexpr const wchar_t *default_pipe_name_prefix = L"\\\\.\\pipe\\FusionHost-v1-";

// Compute a per-user per-session pipe name from the current process token SID
// and session ID. Returns empty on failure.
std::wstring compute_pipe_name();

// Verify that the server process at the other end of the pipe has the same
// user SID and session ID as the current process. Returns true only if both
// match.
bool verify_server_identity(HANDLE pipe_handle);

// Build a SECURITY_ATTRIBUTES with a DACL granting read/write only to the
// current user. Fails closed: if the DACL/SD cannot be built, the returned
// SECURITY_ATTRIBUTES has a null lpSecurityDescriptor and the caller must
// treat pipe creation as failed.
//
// The caller must call free_pipe_security() when done.
SECURITY_ATTRIBUTES make_pipe_security();

// Free the security descriptor and DACL allocated by make_pipe_security().
void free_pipe_security(SECURITY_ATTRIBUTES &sa);

} // namespace fusion::ipc
