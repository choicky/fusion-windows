#pragma once

// U7: the tray's transition rules, kept free of Win32 so that the notification
// policy can be tested without a desktop session.
//
// The accepted minimal design (docs/windows-daily-use.md §4) says: one balloon
// per running -> absent transition, never per key, and no misleading balloon
// during an intentional manual restart or a session end.
//
// Consequences encoded here:
//   - the first observation only establishes the baseline, so a tray that starts
//     while the Host is absent does not report an exit this process never saw;
//   - a suppressed transition still updates the state, so the same absence is not
//     reported later when the suppression ends (the balloon is dropped, not
//     deferred);
//   - recovery (absent -> running) is silent: the design asks for one balloon per
//     transition, and the useful direction is the loss of the Host.

namespace fusion::tray {

// How long a restart marker stays meaningful. A restart that takes longer than
// this is no longer "in progress", so a marker left behind by a crash cannot
// silence a real exit for the rest of the session.
constexpr unsigned long long restart_marker_fresh_ms = 5ull * 60ull * 1000ull;

// True when a marker written `age_ms` ago still describes an in-progress
// restart. Pure so the boundary is testable without touching the file system.
constexpr bool marker_is_fresh(unsigned long long age_ms) {
  return age_ms <= restart_marker_fresh_ms;
}

enum class HostState { Unknown, Running, Absent };

struct Observation {
  // The observed state differs from the previous one.
  bool changed = false;
  // A running -> absent transition that should raise a balloon.
  bool notify_absent = false;
  HostState state = HostState::Unknown;
};

class Watch {
 public:
  // `running` is the verified observation; `suppressed` means an intentional
  // lifecycle event is in progress (manual restart, session end).
  Observation observe(bool running, bool suppressed) {
    const HostState next = running ? HostState::Running : HostState::Absent;
    Observation result;
    result.state = next;
    result.changed = next != state_;
    if (state_ == HostState::Running && next == HostState::Absent) {
      result.notify_absent = !suppressed;
    }
    state_ = next;
    return result;
  }

  HostState state() const { return state_; }

 private:
  HostState state_ = HostState::Unknown;
};

} // namespace fusion::tray
