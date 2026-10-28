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

// Verify that the server process at the other end of the pipe has the same
// user SID and session ID as the current process. Returns true only if both
// match. `failure` (optional, diagnostics only) receives which step refused —
// an API that failed, with its Win32 code, or a real mismatch.
bool verify_server_identity(HANDLE pipe_handle, ConnectFailure *failure = nullptr);

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
