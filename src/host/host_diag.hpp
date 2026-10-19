#pragma once

// R30: opt-in, technical-only diagnostic log for the packaged (windowless) Host.
//
// The packaged Host has no console, so its stderr only exists when a caller
// redirects it. This module gives it a file to fall back on, under the same
// policy as the TSF log (src/tsf/diag): explicit opt-in, bounded size, no key
// or text content, and every failure ignored. It is observability only — it
// never gates behaviour, never holds a lock across the backend, and never
// changes HostClaim, the stop ordering or the exit code.
//
// Opt-in: FUSION_HOST_DIAGNOSTICS = 1 / true / on (case-insensitive), read once
// by initialize(). Unset or any other value: no directory, no file, no work.
//
// Sink: %LOCALAPPDATA%\fusion-host\fusion-host-<pid>.log, one line per event:
//   ts=<tick> pid=<pid> tid=<tid> <event> <details>
// Bounded at kMaxFileBytes: once a line would pass the bound the file is
// recreated empty (the same behaviour as the TSF log). At startup the newest
// kRetentionFiles-1 existing fusion-host-*.log files are kept and the older
// ones deleted, so the directory holds at most kRetentionFiles including this
// process's own file. Nothing else in the directory is ever touched.
//
// Content policy (U3): technical fields only — no key codes, pinyin, preedit,
// candidate or commit text, and no per-key lines. A detail that accidentally
// carries one of the privacy markers is replaced by "privacy=redacted".

#include <cstddef>
#include <string>

namespace fusion::host::diag {

// Reads the opt-in once, applies retention and creates this process's file.
// Safe to call more than once (later calls do nothing). Called at Host startup.
void initialize();

// True when the log is enabled (reads the opt-in if initialize() has not run).
bool enabled();

// Append one structured line: event name plus pre-formatted technical details.
// Thread-safe; a no-op while disabled; any failure is silently ignored.
void event(const char *name, const char *details);

// Upper bound honored for the auto-truncate behaviour (same as the TSF log).
inline constexpr std::size_t kMaxFileBytes = 2u * 1024u * 1024u;

// Files kept in the directory, including this process's own.
inline constexpr int kRetentionFiles = 5;

// Converts a wide technical detail (a pipe or event name) to UTF-8 for event().
// Returns "(unavailable)" when the conversion fails.
std::string utf8(const std::wstring &text);

// Test/driver support: closes the file and forgets the opt-in so one test
// process can exercise the off/on/redaction/cap/retention paths. Never called
// by the Host.
void reset_for_tests();

} // namespace fusion::host::diag
