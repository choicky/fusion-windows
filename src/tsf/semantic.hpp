#pragma once

// Stage 2.5 TSF<->Host adapter for one logical TSF context.
//
// This class is deliberately free of TSF/Win32 types so its routing, ownership
// admission, fallback queue and settlement bookkeeping are deterministically
// testable. It owns NO second ledger and NO semantic engine: every causal
// transition delegates to the existing fusion::Core through fusion::Host, and
// semantic verdicts come from the selected Mock or Fcitx Backend. The TSF
// shell supplies: the Windows hard-policy answer (policy_allows), real edit
// observations (fusion::EditResult), and projection notifications.
//
// Settlement follows D072 A': effect tickets are opened only after an edit
// session is granted and revalidated, immediately before the real mutation
// (see open_effect). Sync refusal never fakes success: the owned key stays in
// the fallback queue (enqueue_*) until a later session settles it, and a PASS
// key cannot overtake unresolved work (finish_pass).

#include "fusion/backend.hpp"
#include "fusion/core.hpp"
#include "fusion/host.hpp"
#include "mock_backend.hpp"
#include "tsf/key_map.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fusion::tsf {

struct FrontendWork {
  BackendOperation operation;
  Key key;
};

struct SemanticDecision {
  enum class Route {
    Pass,              // backend says genuine PASS; caller must run finish_pass
    State,             // owned STATE key; text is the full preedit projection
    Effect,            // owned EFFECT key; text is the immutable commit intent
    Reject,            // owned key failed after ownership; already settled here
    FailOpen,          // ownership was NOT admitted (policy/capacity): not eaten
    CancelledRecovery, // ownership admitted, then explicitly cancelled (§7.3)
  };
  Route route = Route::Pass;
  bool eaten = false;   // TSF pfEaten answer for the physical key
  std::string symbolic; // input handed to the Mock Backend (oracle trace)
  std::uint64_t backend_session_id = 0;
  std::uint64_t stimulus_id = 0;
  std::size_t operation_count = 0;
  Intent intent = Intent::Pass;
  std::string text; // STATE/EFFECT text to materialize, when routed
  std::optional<Key> key;
  std::vector<FrontendWork> frontend; // callback order, never folded
  bool forward = false;
};

// One queued execution opportunity after a refused synchronous session.
struct PendingWork {
  Kind kind = Kind::State;
  Key key{};
  std::string text; // Real frontend: immutable callback; Mock STATE: latest-wins
};

class SemanticController {
public:
  SemanticController(std::uint64_t context_id, std::size_t capacity,
                     fusion::test::MockBackend &backend, std::size_t recovery_reserve = 0);

  SemanticController(std::uint64_t context_id, std::size_t capacity, Backend &backend,
                     std::size_t recovery_reserve = 0);
  bool real_frontend() const { return real_backend_ != nullptr; }
  // T routing eligibility; real Fcitx consumption is determined only by send().
  bool routes_key(const KeyAction &action) const;

  // Key-callback entry point: classification -> hard policy -> ownership
  // admission -> Mock Backend response -> routing. See SemanticDecision.
  SemanticDecision begin_key(const KeyAction &action, bool policy_allows);

  // --- real edit observation settlement (called by TSF edit sessions) ---

  // EFFECT: propose at the watermark, then (after grant+revalidation) open the
  // single execution ticket immediately before the real mutation.
  Result propose_effect(Key key);
  std::optional<Ticket> open_effect(Key key);
  Result settle_effect(Ticket ticket, EditResult observed);

  // STATE: consume with the observed edit result.
  Result settle_state(Key key, EditResult observed);

  // Genuine-PASS barrier (D072 §7.2): Clear lets the key pass; anything else
  // requires the caller to drain (drain_work) or explicitly cancel.
  Pass pass_probe() const;

  // The unresolved effect to drain synchronously before allowing PASS, if any.
  std::optional<PendingWork> drain_work();

  // End of the PASS key path after a successful drain (or Clear probe).
  // Returns the pass verdict: true = genuine PASS (not eaten).
  bool finish_pass();

  // §7.3 recovery disposition: explicitly cancel the current key; never a fake
  // PASS. Returns the cancelled key.
  Key cancel_pass_recovery();

  // Fallback queue after a refused synchronous session (state or effect).
  void enqueue_state(Key key, std::string text);
  void enqueue_effect(Key key, std::string text);
  const std::vector<PendingWork> &pending() const { return pending_; }
  // Async execution: settles queued STATE work latest-wins — every pending
  // state key receives the same observed result in FIFO order.
  void settle_queued_states(EditResult observed);
  void clear_pending();
  void settle_front_state(Key key, EditResult observed);
  // Retire only observed terminals without uncertainty or queued references.
  void retire_terminals();
  bool state_applicable(Key key) const;

  // Effect settled Applied ends the composition projection: raw buffer resets.
  void note_composition_reset();
  // External composition termination (app-side): projection resets; queued
  // work is explicitly settled (cancel), never silently dropped.
  void note_composition_terminated();
  const std::string &raw_buffer() const { return buffer_; }

  // Discontinuity plumbing (context replacement), delegated to Core.
  Result replace_context(std::uint64_t context_id);
  void destroy();

  Core &core() { return core_; }
  Host &host() { return host_; }
  std::uint64_t context_id() const { return id_; }

private:
  std::uint64_t state_dependency_ = 0;
  std::size_t recovery_reserve_;
  std::uint64_t id_;
  std::uint64_t lifetime_;
  Core core_;
  Host host_;
  fusion::test::MockBackend *backend_ = nullptr;
  Backend *real_backend_ = nullptr;
  bool backend_ready_ = false;
  bool backend_reset_pending_ = false;
  std::uint64_t next_stimulus_ = 1;
  SemanticDecision begin_real_key(const KeyAction &action, bool policy_allows);
  std::string buffer_;
  std::vector<PendingWork> pending_;
};

} // namespace fusion::tsf
