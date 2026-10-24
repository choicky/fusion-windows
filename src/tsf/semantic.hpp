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

  // Key-up entry point for a modifier-only key whose down this shell relayed. It owns no
  // composition and materializes nothing: it asks the engine whether the press completed
  // one of its own modifier-only bindings, and reports what the engine decided. Only the
  // Shift keys are relayed (see routes_release); every other release is not ours.
  SemanticDecision release_key(const KeyAction &action);

  // How many complete candidate rows the popup can draw with the current work area, display
  // DPI and font. Sent with every stimulus: it is the only part of "where the scroll page
  // ends" that this side knows, and the Host needs it to group rows the same way the popup
  // draws them. 0 keeps the Host maximum.
  void set_window_rows(int rows) { window_rows_ = rows < 0 ? 0 : (rows > 5 ? 5 : rows); }
  int window_rows() const { return window_rows_; }

  // Whether a key-up of this key may be relayed at all: a Shift key whose press this
  // shell relayed and that stayed lone, nothing else.
  bool routes_release(const KeyAction &action) const;

  // Whether a release may be relayed *and* has the resources to be settled: `routes_release`
  // plus the ledger admission the release's own document work needs. This is the one answer
  // both the T and the K callback use, so T can never claim a release that K would refuse
  // (a claimed key-up the application never sees is a stuck modifier on its side).
  bool release_admissible(const KeyAction &action) const;

  // Records whether a key-down starts the press a modifier-only release may settle.
  // Called from both T and K (the application may deliver either), and idempotent.
  void observe_key_down(const KeyAction &action);

  // Candidate-callback entry point (real frontend only). Runs the same admission
  // and ordered Core settlement as a real key dispatch, minus the physical-key
  // eligibility answers, and only against a currently displayed, unfenced page.
  SemanticDecision begin_candidate_action(CandidateAction action, std::int32_t index);

  /// Moves the engine's candidate cursor left (-1) or right (+1) in the automatic scrolling
  /// window. Returns false when this key must keep its ordinary meaning — no window is
  /// displayed, it is not a scrolling one, or the move would leave the list — in which case
  /// the caller dispatches the key as it always did, which is also how pinyin-string editing
  /// stays reachable.
  bool begin_candidate_cursor(int step, SemanticDecision &decision);

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

  // Whether the TSF shell may claim this key in OnTestKeyDown right now.
  //
  // Only the editing/navigation keys whose engine handling needs a live composition are
  // gated (Backspace, Delete, the four arrows, Home/End, and Return — see
  // editing_key_needs_composition); every other key keeps exactly the admission this
  // shell had before R37. That narrowness matters in both directions: with nothing
  // composing the engine can only forward a gated key, so a claim made in T cannot be
  // retracted in K and an application that honours only T loses it (Notepad-- 3.4 lost
  // Return, R36; WeChat lost the editing keys, R37); but a configured Fcitx global
  // hotkey (for example its own toggle-input-method action on F6) is handled with no
  // composition at all, so filtering every character-less key would swallow it.
  bool engine_has_work_for(const KeyAction &action) const;

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
  // The popup's current row capacity, updated whenever it lays a page out.
  std::int32_t window_rows_ = 0;
  // The Shift key whose physical press was last observed (T or K); 0 when none. Only a
  // physical fact: it never claims the engine received anything.
  std::uint32_t shift_pressed_keysym_ = 0;
  // The press the engine actually received, with the binding and link it belongs to. A
  // key-up may settle only this, and only while that same link is current, so a release
  // can never cross a policy refusal, a failed send, a reconnect or a Host change.
  struct ShiftPress {
    std::uint32_t keysym = 0;
    std::uint64_t context = 0;
    std::uint64_t incarnation = 0;
    std::uint64_t generation = 0;
    void reset() { *this = ShiftPress{}; }
  };
  ShiftPress shift_forwarded_;
  SemanticDecision begin_real_key(const KeyAction &action, bool policy_allows);
  // The composition-state work that makes a gated key engine work: raw input, a projected
  // page, queued frontend work, an unresolved pass barrier. The Mock Backend answers true
  // so the development fixture keeps its previous routing.
  bool engine_has_composition_work() const;
  // Whether this key is engine work only while a composition has something for it: the
  // editing/navigation keys Fcitx handles inside its non-empty-input branch, plus Return
  // (R36). Measured against the pinned engine in stage3c, not inferred from the key class.
  static bool editing_key_needs_composition(const KeyAction &action);
  // A number key selects a candidate only while a page is on screen — top row and keypad
  // alike, measured in stage3c. Kept apart from the composition gate above because the page,
  // not the composition, is the oracle.
  static bool is_candidate_digit(const KeyAction &action);
  bool digit_selects_a_candidate(const KeyAction &action) const;
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
