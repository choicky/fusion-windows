#pragma once
#include "fusion/core.hpp"
#include <cstdint>
#include <optional>

namespace fusion::tsf {
// This decision is shared by the real K callback and its regression test.
// Draining prior work never disposes of the current physical key.
template <typename Probe, typename Drain, typename Cancel>
bool continue_current_key(Probe probe, Drain drain, Cancel cancel) {
  if (probe() == Pass::DrainRequired)
    drain();
  if (probe() == Pass::Clear)
    return true;
  cancel();
  return false;
}

// One context-local projection cleanup, not a semantic effect or scheduler.
// Admission refusal retains intent. An attempted but unsuccessful edit is held
// for external reconciliation, never blindly retried.
class CleanupIntent {
public:
  void retain(std::uint64_t generation, const void *identity) {
    if (pending_)
      return;
    generation_ = generation;
    identity_ = identity;
    ++serial_;
    pending_ = true;
    scheduled_ = attempted_ = false;
  }
  bool pending() const { return pending_; }
  std::uint64_t serial() const { return serial_; }
  bool request() {
    if (!pending_ || scheduled_ || attempted_)
      return false;
    scheduled_ = true;
    return true;
  }
  void refused(std::uint64_t serial) {
    if (serial == serial_)
      scheduled_ = false;
  }
  template <typename Edit>
  std::optional<bool> execute(std::uint64_t serial, std::uint64_t generation, const void *identity,
                              Edit edit) {
    if (!pending_ || attempted_ || serial != serial_ || generation != generation_ ||
        identity != identity_)
      return std::nullopt;
    attempted_ = true; // before the callback: duplicate/reentrant grants are no-ops
    scheduled_ = false;
    const bool success = edit();
    if (serial == serial_ && pending_)
      pending_ = !success; // do not undo reentrant external reconciliation
    return success;
  }
  void reconciled() { pending_ = scheduled_ = false; }

private:
  std::uint64_t generation_ = 0, serial_ = 0;
  const void *identity_ = nullptr;
  bool pending_ = false, scheduled_ = false, attempted_ = false;
};

// Active only across the service's own EndComposition call. After successful
// return that composition is detached, so a delayed callback cannot match a
// newer live composition. A failed end leaves later external callbacks enabled.
struct SelfEnd {
  std::uint64_t generation = 0;
  const void *identity = nullptr;
  template <typename External>
  bool terminated(std::uint64_t live_generation, const void *composition, External external) const {
    if (identity && generation == live_generation && identity == composition)
      return true;
    external();
    return false;
  }
};
} // namespace fusion::tsf
