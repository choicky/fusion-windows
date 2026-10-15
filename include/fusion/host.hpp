#pragma once

#include "fusion/core.hpp"

#include <optional>

namespace fusion {

// Minimal semantic-intent categories a backend can express about an already
// admitted key. This is the Host-side boundary contract, not a wire schema and
// not an ownership authority: Windows-side admission has already decided the key
// is owned (or genuinely unowned) before a response is routed here.
enum class Intent {
  Preedit, // consumed input produces STATE / preedit
  Commit,  // consumed input produces an EFFECT / irreversible commit intent
  Pass,    // genuine PASS: the input is unowned and materializes nothing
  Reject,  // backend rejection after ownership: a failure input, never PASS
};

// Single-owner deterministic translation from backend semantic intent to
// existing FusionCore operations. It adds no second ledger, no reliability state
// machine, no document materialization and no correctness policy: every method
// delegates one transition to Core and returns Core's verdict. The document
// authority still performs the real edit and reports the observed EditResult /
// Readback between an opened effect and its completion.
class Host {
public:
  explicit Host(Core &core) : core_(core) {}
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;
  Host(Host &&) = delete;
  Host &operator=(Host &&) = delete;

  // STATE path: apply a preedit observation for an owned State key.
  Result preedit(Key key, EditResult observed) { return core_.consume_state(key, observed); }
  Result classify(Key key, std::optional<std::size_t> state_index,
                  const std::vector<std::size_t> &effect_indices) {
    return core_.classify(key, state_index, effect_indices);
  }

  // EFFECT path: propose the commit against the current consumed-STATE watermark,
  // open the single execution opportunity, then settle it with the observed edit.
  Result commit(Key key, std::size_t operation_index = 0) {
    return core_.propose_effect(key, core_.consumed_state(), operation_index);
  }
  std::optional<Ticket> open(Key key, std::size_t operation_index = SIZE_MAX) {
    return core_.begin_effect(key, operation_index);
  }
  Result settle(const Ticket &ticket, EditResult observed) {
    return core_.complete_effect(ticket, observed);
  }

  // Failure input after ownership: reaches an explicit no-effect terminal, never
  // a PASS. Cancellation keeps its own distinct terminal value.
  Result reject(Key key) { return core_.reject(key); }
  Result cancel(Key key) { return core_.cancel(key); }

  // Indeterminate irreversible mutation: readback accounting only, no retry.
  Result reconcile(const Ticket &ticket, Readback observed) {
    return core_.reconcile(ticket, observed);
  }

  // PASS admission barrier for a genuine unowned input.
  Pass pass_barrier() const { return core_.probe_pass(); }

  std::uint64_t watermark() const { return core_.consumed_state(); }

private:
  Core &core_;
};

} // namespace fusion
