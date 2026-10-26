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

// R49: the package whose AppContainer is the taskbar search box's SearchHost.exe.
// The grant is derived from this package family name (not from an executable
// path), so it names the package and survives a process rename. Measured
// 2026-10-26 on the 7700: the running SearchHost's TokenPackageFamilyName is this
// name, and its TokenAppContainerSid is `search_package_sid` below.
inline constexpr const wchar_t *search_package_family_name =
    L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy";

// The AppContainer SID of that package family. It is a property of the family
// name (publisher hash included), not of this machine, so it is written here
// rather than derived at run time: the derivation (userenv's
// DeriveAppContainerSidFromAppContainerName) initialises a library the Host does
// not otherwise use, and the value is pinned by a test that derives it and
// compares (`stage4a pipesecurity`), so the constant cannot drift silently.
inline constexpr const wchar_t *search_package_sid =
    L"S-1-15-2-283421221-3183566570-1718213290-751554359-3541592344-2312209569-3374928651";

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
// This is the descriptor for the objects that must stay user-only: the Host
// claim event and the Host stop event. A packaged client that could signal the
// stop event could end the Host, so it is deliberately not used there.
//
// The caller must call free_pipe_security() when done.
SECURITY_ATTRIBUTES make_pipe_security();

// The descriptor for the IPC data pipe: the same current-user grant, plus one
// more allow ACE naming the AppContainer package SID of the taskbar search box,
// so SearchHost.exe can open the pipe at all. The rights are the ones the
// reviewed connection path already uses (read and write on the pipe), and no
// other principal is named: not ALL APPLICATION PACKAGES, not ALL RESTRICTED
// APPLICATION PACKAGES, not Everyone, not anonymous.
//
// What this does NOT do: it does not admit the client past anything else. The
// server identity check, the Hello handshake and every message remain exactly
// as they were, and the settings messages travel on this same pipe, so the
// grant is also a statement about how much configuration access that package
// gets (R49, D072 §14 P0-D).
//
// When the package SID cannot be built (a Windows edition without the
// package), the result is the user-only descriptor above.
//
// The caller must call free_pipe_security() when done.
SECURITY_ATTRIBUTES make_data_pipe_security();

// Free the security descriptor and DACL allocated by make_pipe_security().
void free_pipe_security(SECURITY_ATTRIBUTES &sa);

} // namespace fusion::ipc
