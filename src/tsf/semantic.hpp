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
  // Candidate page projection carried by this dispatch, present whenever the
  // Backend batch contained a CandidateSnapshot. An empty page is the authoritative
  // hide signal, not "no change". Never a second copy of engine candidate state.
  std::optional<CandidatePage> page;
};

// A candidate UI operation. It is a non-key semantic action: the caller asks the
// engine to select a page-local candidate or turn the page. Selection must never
// be simulated as a numeric key (D072 §7.4, D082).
enum class CandidateAction { Select, PageNext, PagePrev };

// Identity of the dispatch that produced the currently displayed candidate page.
// A candidate action is admitted only while this fence still matches the live
// backend session, context binding and link incarnation/connection; a move drops
// the projection so a stale page never materializes against a new context.
struct PageFence {
  std::uint64_t backend_session_id = 0;
  Binding binding{};
  std::uint64_t stimulus_id = 0;
  std::uint64_t host_incarnation = 0;
  std::uint64_t connection_generation = 0;
};

// One queued execution opportunity after a refused synchronous session.
struct PendingWork {
  Kind kind = Kind::State;
  Key key{};
  std::string text; // Real frontend: immutable callback; Mock STATE: latest-wins
  // Real frontend SetPreedit only: the displayed preedit's segments/format and
  // engine cursor (fcitx::Text contract). Absent = plain text, caret at end.
  std::optional<FormattedPreedit> preedit;
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

  // Candidate-callback entry point (real frontend only). Runs the same admission
  // and ordered Core settlement as a real key dispatch, minus the physical-key
  // eligibility answers, and only against a currently displayed, unfenced page.
  SemanticDecision begin_candidate_action(CandidateAction action, std::int32_t index);

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

  // Whether Return is engine work right now: raw input, a displayed candidate/panel
  // page, unsettled frontend work or an unresolved pass barrier. When this is false the
  // engine only answers Return with ForwardKey, so the TSF shell must not claim the key
  // in OnTestKeyDown: an application that honours only that callback drops it otherwise
  // (measured in Notepad--, R36).
  bool return_has_engine_work() const;

  // The unresolved effect to drain synchronously before allowing PASS, if any.
  std::optional<PendingWork> drain_work();

  // End of the PASS key path after a successful drain (or Clear probe).
  // Returns the pass verdict: true = genuine PASS (not eaten).
  bool finish_pass();

  // §7.3 recovery disposition: explicitly cancel the current key; never a fake
  // PASS. Returns the cancelled key.
  Key cancel_pass_recovery();

  // Fallback queue after a refused synchronous session (state or effect).
  void enqueue_state(Key key, std::string text,
                     std::optional<FormattedPreedit> preedit = std::nullopt);
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

  // The panel snapshot (candidate page + panel rows) the last admitted dispatch
  // projected, or nullopt when nothing is displayed (nothing projected, or a
  // panel_empty() snapshot acted as the hide signal). The renderer consumes this;
  // it is display text, not semantic state.
  const std::optional<CandidatePage> &displayed_page() const { return displayed_page_; }

  // Discontinuity plumbing (context replacement), delegated to Core.
  Result replace_context(std::uint64_t context_id);
  void destroy();

  // --- Stage 4B link/incarnation readiness (real frontend only) ---
  //
  // "Semantic ready" is not "pipe connected": it requires an established
  // protocol incarnation AND a context binding established for that same
  // incarnation and connection.
  bool semantic_ready() const;
  // Current link identity, for observability and tests.
  LinkStatus link_status() const;

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
  // Identity of the incarnation/connection this controller is bound to. 0 means
  // "not bound to any incarnation".
  std::uint64_t bound_incarnation_ = 0;
  std::uint64_t bound_connection_ = 0;
  std::uint64_t next_stimulus_ = 1;
  SemanticDecision begin_real_key(const KeyAction &action, bool policy_allows);
  // Shared real-frontend tail: dispatch one stimulus, validate the returned
  // batch, own the ordered Core records, and project any candidate snapshot.
  // Used by both the key and the candidate-action paths.
  void dispatch_real(const BackendStimulus &stimulus, SemanticDecision &decision);
  // Drop the displayed candidate projection (hide). The fence is no longer
  // meaningful once there is no page to fence.
  void clear_displayed_page();
  // Bounded reconnect + incarnation/connection fencing + context (re)bind.
  bool ensure_semantic_ready();
  // The previous incarnation's Host-provided semantic projection is not current
  // STATE for the new one. Ownership/uncertainty accounting is NOT touched.
  void fence_previous_incarnation();
  std::string buffer_;
  std::vector<PendingWork> pending_;
  // Currently displayed candidate page projection and the identity of the dispatch
  // that produced it. page_present_ is false both when nothing was ever projected
  // and when the latest projection had nothing to display (the hide signal); only
  // a present, unfenced page with candidate slots can be acted on.
  std::optional<CandidatePage> displayed_page_;
  PageFence displayed_fence_;
  bool page_present_ = false;
};

} // namespace fusion::tsf
