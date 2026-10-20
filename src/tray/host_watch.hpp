#pragma once

// U7: read-only observation of "this user's Host is running in this session".
//
// The tray must report existence only. A running process says nothing about
// responsiveness or backend health, so nothing here probes the pipe, the
// backend or the engine.
//
// Verification is the same one the manual restart tool applies: the process is
// named FusionHost.exe, its session id matches this process's session, and its
// token user SID matches this process's token user SID. An owner that cannot be
// read is never assumed to be ours.

#include <string>

namespace fusion::tray {

struct HostObservation {
  // False when the probe itself could not be trusted (no token SID, no process
  // snapshot). The caller must then keep its previous state instead of reporting
  // an absence it cannot prove.
  bool verified = false;
  bool running = false;
  unsigned long pid = 0;
};

// One look at the verified Host set. Read-only: no write access to the process,
// no signal, no termination.
HostObservation observe_host();

// "S-1-5-21-..." of the current process token, or empty when unreadable.
std::wstring current_user_sid();

// The interactive session this process runs in (0 when unreadable).
unsigned long current_session_id();

} // namespace fusion::tray
