#pragma once

// Bounded development diagnostics for the Stage 2.5 real-TSF shell. The log is
// observability only: it never changes product semantics, never gates behavior,
// and stays small (auto-truncating). Logging failures are silently ignored.

#include <cstddef>

namespace fusion::tsf::diag {

// Append one structured line: event name plus pre-formatted details.
// Thread-safe. First use opens the log file lazily; the path is taken from the
// FUSION_TSF_LOG environment variable (exact file path) and otherwise defaults
// to %LOCALAPPDATA%\fusion-tsf\fusion-tsf-<pid>.log.
void event(const char *name, const char *details);

// Convenience wrappers used by the TSF shell.
void info(const char *name, const char *details);
void warn(const char *name, const char *details);

// Test/driver support: upper bound honored for the auto-truncate behavior.
inline constexpr std::size_t kMaxFileBytes = 2u * 1024u * 1024u;

} // namespace fusion::tsf::diag
