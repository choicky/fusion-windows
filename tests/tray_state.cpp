// U7: the tray's transition and notification rules, tested without a desktop
// session. The point of these checks is the policy the design fixed: one balloon
// per running -> absent transition, none for the first observation, none for a
// suppressed (intentional) transition, no deferred balloon afterwards, no balloon
// for a replacement Host, and a silent re-baseline after a cancelled session end.

#include "tray/tray_state.hpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

} // namespace

int main() {
  using fusion::tray::HostState;
  using fusion::tray::Observation;
  using fusion::tray::Watch;

  {
    Watch watch;
    const Observation step = watch.observe(false, 0, false);
    expect(step.changed, "a first absent observation establishes the baseline");
    expect(!step.notify_absent, "a first absent observation does not notify");
    expect(step.state == HostState::Absent, "a first absent observation is absent");
  }
  {
    Watch watch;
    const Observation step = watch.observe(true, 100, false);
    expect(step.changed, "a first running observation establishes the baseline");
    expect(!step.notify_absent, "a first running observation does not notify");
    expect(!step.identity_changed, "a first running observation is not an identity change");
    expect(step.state == HostState::Running, "a first running observation is running");
  }
  {
    Watch watch;
    (void)watch.observe(true, 100, false);
    const Observation step = watch.observe(false, 0, false);
    expect(step.changed, "running -> absent changes the state");
    expect(step.notify_absent, "running -> absent notifies once");
    expect(step.state == HostState::Absent, "running -> absent is absent");
  }
  {
    Watch watch;
    (void)watch.observe(true, 100, false);
    (void)watch.observe(false, 0, false);
    const Observation step = watch.observe(false, 0, false);
    expect(!step.changed, "a repeated absent observation does not change the state");
    expect(!step.notify_absent, "a repeated absent observation does not notify again");
  }
  {
    Watch watch;
    (void)watch.observe(true, 100, false);
    (void)watch.observe(false, 0, false);
    const Observation recovered = watch.observe(true, 200, false);
    expect(recovered.changed, "absent -> running changes the state");
    expect(!recovered.notify_absent, "recovery is silent");
    expect(!recovered.identity_changed, "recovery is a transition, not an identity change");
    const Observation again = watch.observe(false, 0, false);
    expect(again.notify_absent, "a later running -> absent transition notifies again");
  }
  {
    Watch watch;
    (void)watch.observe(true, 100, false);
    const Observation step = watch.observe(false, 0, true); // manual restart in progress
    expect(step.changed, "a suppressed transition still changes the state");
    expect(!step.notify_absent, "a suppressed transition does not notify");
    expect(step.state == HostState::Absent, "a suppressed transition is absent");
    const Observation after = watch.observe(false, 0, false);
    expect(!after.notify_absent, "the suppressed balloon is dropped, not deferred");
  }
  {
    Watch watch;
    (void)watch.observe(true, 100, false);
    (void)watch.observe(false, 0, true);
    (void)watch.observe(true, 300, false);
    const Observation step = watch.observe(false, 0, false);
    expect(step.notify_absent, "suppression does not disable later transitions");
  }
  {
    // A tray started during a restart has no baseline to lose, so it stays quiet.
    Watch watch;
    const Observation step = watch.observe(false, 0, true);
    expect(!step.notify_absent, "a suppressed first observation does not notify");
    expect(watch.state() == HostState::Absent, "a suppressed first observation is absent");
  }
  {
    // The marker bound: fresh at the boundary, stale one millisecond later, so a
    // marker left behind by a crash cannot silence a later real exit.
    expect(fusion::tray::marker_is_fresh(0), "a marker written now is fresh");
    expect(fusion::tray::marker_is_fresh(fusion::tray::restart_marker_fresh_ms),
           "a marker at the freshness bound is still fresh");
    expect(!fusion::tray::marker_is_fresh(fusion::tray::restart_marker_fresh_ms + 1),
           "a marker past the freshness bound is stale");
  }
  {
    // A replacement Host between two polls: same state, new identity. The tooltip
    // must refresh, and the policy must not invent an absence for it.
    Watch watch;
    (void)watch.observe(true, 100, false);
    const Observation replaced = watch.observe(true, 200, false);
    expect(!replaced.changed, "a new Host pid is not a state change");
    expect(replaced.identity_changed, "a new Host pid refreshes the identity");
    expect(!replaced.notify_absent, "a new Host pid does not fabricate an absence");
    const Observation same = watch.observe(true, 200, false);
    expect(!same.identity_changed, "the same pid is not an identity change");
  }
  {
    // A pid that could not be read (0) is not evidence of a new process.
    Watch watch;
    (void)watch.observe(true, 100, false);
    const Observation step = watch.observe(true, 0, false);
    expect(!step.identity_changed, "an unreadable pid is not an identity change");
  }
  {
    // A cancelled session end: the baseline is dropped, so an exit that happened
    // during the attempt is not replayed as a fresh failure, and the next real
    // exit still notifies.
    Watch watch;
    (void)watch.observe(true, 100, false);
    watch.reset();
    const Observation afterCancel = watch.observe(false, 0, false);
    expect(afterCancel.changed, "a re-baseline after cancellation establishes the state");
    expect(!afterCancel.notify_absent, "a cancelled session end does not replay an exit");
    (void)watch.observe(true, 300, false);
    const Observation real = watch.observe(false, 0, false);
    expect(real.notify_absent, "a real exit after cancellation notifies");
  }

  if (failures != 0) {
    std::printf("tray_state: %d check(s) failed\n", failures);
    return 1;
  }
  std::puts("tray_state: all transition checks passed");
  return 0;
}
