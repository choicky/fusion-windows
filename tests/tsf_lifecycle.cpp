// Stage 4C lifecycle tests: the TSF lifecycle obligation for a Fusion context and
// the teardown accounting that keeps unresolved responsibility auditable.
//
// Usage: tsf_lifecycle <obligation|teardown_clean|teardown_unresolved|
//                       composition_terminated|identity_across_focus|context_isolation>
//
// These are the platform-free decisions the real TSF sinks apply. They assert
// *transitions* (what ended, what was reconciled, what stayed current), never
// merely that a callback ran.

#include "mock.hpp"
#include "tsf/lifecycle.hpp"
#include "tsf/semantic.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fusion;
using namespace fusion::test;
using namespace fusion::tsf;

namespace {

const Record &record(const Core &core, Key key) {
  for (const auto &r : core.records())
    if (r.key == key)
      return r;
  throw std::runtime_error("missing record");
}

// Applies the obligation table exactly as the real sinks do, so the tests assert
// the consequence of the shared decision rather than restating the table.
void apply_obligation(SemanticController &controller, LifecycleEvent event) {
  const LifecycleObligation obligation = lifecycle_obligation(event);
  if (obligation.end_context) {
    controller.destroy();
  } else if (obligation.reconcile_projection) {
    controller.note_composition_terminated();
  }
}

// --- obligation ------------------------------------------------------------

void obligation() {
  struct Case {
    LifecycleEvent event;
    bool end_context;
    bool reconcile;
    const char *name;
  };
  const Case cases[] = {
      {LifecycleEvent::Activate, false, false, "activate"},
      {LifecycleEvent::Deactivate, true, false, "deactivate"},
      {LifecycleEvent::FocusChanged, false, false, "focus"},
      {LifecycleEvent::ThreadForeground, false, false, "thread-foreground"},
      {LifecycleEvent::ContextPushed, false, false, "push"},
      {LifecycleEvent::ContextPopped, true, false, "pop"},
      {LifecycleEvent::DocumentCreated, false, false, "docmgr-init"},
      {LifecycleEvent::DocumentDestroyed, true, false, "docmgr-uninit"},
      {LifecycleEvent::CompositionTerminated, false, true, "composition-terminated"},
  };
  for (const Case &c : cases) {
    const LifecycleObligation o = lifecycle_obligation(c.event);
    require(o.end_context == c.end_context, c.name);
    require(o.reconcile_projection == c.reconcile, c.name);
    // A context is either ended or kept: never both, never ambiguous.
    require(!(o.end_context && o.reconcile_projection), "no event both ends and reconciles");
  }
  // D072 §9: focus is not a semantic incarnation, so neither focus event ends a
  // Fusion context nor reconciles its projection.
  require(!lifecycle_obligation(LifecycleEvent::FocusChanged).end_context, "focus keeps context");
  require(!lifecycle_obligation(LifecycleEvent::ThreadForeground).end_context,
          "foreground keeps ctx");
  // An external composition termination keeps the identity but settles projection.
  require(lifecycle_obligation(LifecycleEvent::CompositionTerminated).reconcile_projection,
          "termination reconciles");
  std::cout << "PASS obligation: only teardown events end a context; focus and push do not\n";
}

// --- teardown_clean --------------------------------------------------------

void teardown_clean() {
  MockBackend backend;
  SemanticController controller(1, 8, backend, 1);
  const auto state = controller.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
  require(state.key.has_value(), "state owned");
  require(controller.settle_state(*state.key, EditResult::AppliedReadback) == Result::Accepted,
          "state consumed");
  const auto effect = controller.begin_key({KeyAction::Type::Commit, 0}, true);
  require(effect.key.has_value(), "effect owned");
  require(controller.propose_effect(*effect.key) == Result::Accepted, "effect proposed");
  auto ticket = controller.open_effect(*effect.key);
  require(ticket.has_value(), "effect ticket");
  require(controller.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "effect applied");

  const TeardownSummary summary =
      summarize_teardown(controller.core().records(), controller.pending().size());
  require(!summary.needs_audit(), "settled work needs no teardown audit");
  require(summary.records == 2 && summary.pending_effects == 0 && summary.uncertain == 0 &&
              summary.queued == 0,
          "clean summary");
  apply_obligation(controller, LifecycleEvent::DocumentDestroyed);
  for (const Record &r : controller.core().records())
    require(r.terminal != Terminal::Pending, "every owned key reached a terminal");
  require(record(controller.core(), *effect.key).terminal == Terminal::Applied,
          "an applied effect is not undone by teardown");
  std::cout << "PASS teardown_clean\n";
}

// --- teardown_unresolved ---------------------------------------------------

void teardown_unresolved() {
  // (a) An effect whose execution certainty became unknown. Core refuses new
  // ownership after that (drain/recovery required), so this is its own context.
  {
    MockBackend backend;
    SemanticController controller(1, 8, backend, 1);
    const auto state = controller.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
    require(controller.settle_state(*state.key, EditResult::AppliedReadback) == Result::Accepted,
            "state consumed");
    const auto uncertain_effect = controller.begin_key({KeyAction::Type::Commit, 0}, true);
    require(controller.propose_effect(*uncertain_effect.key) == Result::Accepted, "proposed");
    auto ticket = controller.open_effect(*uncertain_effect.key);
    require(ticket.has_value(), "ticket");
    require(controller.settle_effect(*ticket, EditResult::Indeterminate) == Result::Accepted,
            "uncertain settled");

    const TeardownSummary summary =
        summarize_teardown(controller.core().records(), controller.pending().size());
    require(summary.needs_audit(), "unknown execution certainty is detected at teardown");
    require(summary.uncertain == 1 && summary.pending_effects == 0, "uncertain accounting");

    apply_obligation(controller, LifecycleEvent::ContextPopped);
    require(record(controller.core(), *uncertain_effect.key).uncertain,
            "unknown execution certainty survives as an explicit terminal");
    require(record(controller.core(), *uncertain_effect.key).terminal == Terminal::Indeterminate,
            "indeterminate terminal is retained");
    require(record(controller.core(), *state.key).terminal == Terminal::Consumed,
            "consumed STATE is not rewritten by teardown");
    require(controller.settle_state(*state.key, EditResult::AppliedReadback) == Result::Stale,
            "late state result for a destroyed context is fenced");
  }

  // (b) An owned callback received from the engine but never issued to the document.
  {
    MockBackend backend;
    SemanticController controller(1, 8, backend, 1);
    const auto state = controller.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
    require(controller.settle_state(*state.key, EditResult::AppliedReadback) == Result::Accepted,
            "state consumed");
    const auto queued_effect = controller.begin_key({KeyAction::Type::Commit, 0}, true);
    require(queued_effect.key.has_value(), "queued effect owned");
    require(controller.propose_effect(*queued_effect.key) == Result::Accepted, "queued proposed");
    controller.enqueue_effect(*queued_effect.key, queued_effect.text);
    require(controller.pending().size() == 1, "queued work retained");

    const TeardownSummary summary =
        summarize_teardown(controller.core().records(), controller.pending().size());
    require(summary.needs_audit(), "never-issued work is detected at teardown");
    require(summary.pending_effects == 1 && summary.queued == 1, "queued accounting");

    apply_obligation(controller, LifecycleEvent::DocumentDestroyed);
    require(controller.pending().empty(), "teardown drops no queued work silently");
    require(record(controller.core(), *queued_effect.key).terminal == Terminal::FailedNoEffect,
            "never-issued work reaches an explicit no-effect terminal");
    for (const Record &r : controller.core().records())
      require(r.terminal != Terminal::Pending, "no record is left pending after teardown");
    require(!controller.open_effect(*queued_effect.key).has_value(),
            "late effect execution for a destroyed context is fenced");
  }
  std::cout << "PASS teardown_unresolved\n";
}

// --- composition_terminated ------------------------------------------------

void composition_terminated() {
  MockBackend backend;
  SemanticController controller(7, 8, backend, 1);
  const Binding binding = controller.core().binding();

  // An applied effect is document history: a later lifecycle event must not undo it.
  const auto settled_state = controller.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
  require(controller.settle_state(*settled_state.key, EditResult::AppliedReadback) ==
              Result::Accepted,
          "state consumed");
  const auto applied_effect = controller.begin_key({KeyAction::Type::Commit, 0}, true);
  require(controller.propose_effect(*applied_effect.key) == Result::Accepted, "proposed");
  auto ticket = controller.open_effect(*applied_effect.key);
  require(ticket.has_value(), "ticket");
  require(controller.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "applied");

  // A projection and a never-issued callback are current work at termination.
  const auto live_state = controller.begin_key({KeyAction::Type::AppendChar, 'b'}, true);
  require(live_state.key.has_value(), "live state owned");
  controller.enqueue_state(*live_state.key, live_state.text);
  require(!controller.raw_buffer().empty(), "projection present");
  require(controller.pending().size() == 1, "queued work present");

  apply_obligation(controller, LifecycleEvent::CompositionTerminated);

  // Identity and binding are preserved: external termination is not a teardown.
  require(controller.core().binding() == binding, "termination keeps the context identity");
  require(controller.core().binding().context == 7, "termination keeps the context id");
  require(record(controller.core(), *applied_effect.key).terminal == Terminal::Applied,
          "an applied effect is never undone by termination");
  // Projection is reconciled and queued work reaches an explicit terminal.
  require(controller.raw_buffer().empty(), "termination clears the projection");
  require(controller.pending().empty(), "termination drains the queue explicitly");
  require(record(controller.core(), *live_state.key).terminal == Terminal::Cancelled,
          "unissued work reaches an explicit cancelled terminal");
  // The context stays usable: a later key still reaches a terminal.
  const auto next = controller.begin_key({KeyAction::Type::AppendChar, 'c'}, true);
  require(next.key.has_value(), "context remains usable after termination");
  require(controller.settle_state(*next.key, EditResult::AppliedReadback) == Result::Accepted,
          "later state settles");
  std::cout << "PASS composition_terminated\n";
}

// --- identity_across_focus -------------------------------------------------

void identity_across_focus() {
  MockBackend backend;
  SemanticController controller(3, 8, backend, 1);
  const Binding binding = controller.core().binding();
  const auto state = controller.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
  require(state.key.has_value(), "state owned");
  const std::string projection = controller.raw_buffer();
  const std::size_t records = controller.core().records().size();

  // D072 §9: focus / thread foreground / a pushed context / a new document do not
  // reset, rebind or end the context.
  for (const LifecycleEvent event :
       {LifecycleEvent::FocusChanged, LifecycleEvent::ThreadForeground,
        LifecycleEvent::ContextPushed, LifecycleEvent::DocumentCreated}) {
    apply_obligation(controller, event);
    require(controller.core().binding() == binding, "identity preserved across the event");
    require(controller.core().records().size() == records, "no record dropped by the event");
    require(controller.raw_buffer() == projection, "projection preserved across the event");
    require(record(controller.core(), *state.key).terminal == Terminal::Pending,
            "owned work stays pending across the event");
  }
  // The context continues where it was: the same owned key settles.
  require(controller.settle_state(*state.key, EditResult::AppliedReadback) == Result::Accepted,
          "the pre-event key still settles");
  std::cout << "PASS identity_across_focus\n";
}

// --- context_isolation -----------------------------------------------------

void context_isolation() {
  MockBackend backend_a;
  MockBackend backend_b;
  SemanticController first(1, 8, backend_a, 1);
  SemanticController second(2, 8, backend_b, 1);
  require(!(first.core().binding() == second.core().binding()), "distinct context identities");

  const auto other_state = second.begin_key({KeyAction::Type::AppendChar, 'z'}, true);
  require(second.settle_state(*other_state.key, EditResult::AppliedReadback) == Result::Accepted,
          "second context state consumed");
  const auto other_effect = second.begin_key({KeyAction::Type::Commit, 0}, true);
  require(second.propose_effect(*other_effect.key) == Result::Accepted, "second effect proposed");
  auto ticket = second.open_effect(*other_effect.key);
  require(ticket.has_value(), "second ticket");
  require(second.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "second effect applied");
  const std::size_t second_records = second.core().records().size();
  const Binding second_binding = second.core().binding();

  // First context: an owned effect proposed but never issued, then teardown.
  const auto first_state = first.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
  require(first_state.key.has_value(), "first state owned");
  require(first.settle_state(*first_state.key, EditResult::AppliedReadback) == Result::Accepted,
          "first state consumed");
  const auto first_effect = first.begin_key({KeyAction::Type::Commit, 0}, true);
  require(first.propose_effect(*first_effect.key) == Result::Accepted, "first effect proposed");
  const TeardownSummary summary =
      summarize_teardown(first.core().records(), first.pending().size());
  require(summary.pending_effects == 1, "first context has unresolved work");
  apply_obligation(first, LifecycleEvent::DocumentDestroyed);

  // The other context is untouched and stays functional.
  require(second.core().binding() == second_binding,
          "teardown of one context cannot rebind another");
  require(second.core().records().size() == second_records, "no cross-context record change");
  require(record(second.core(), *other_effect.key).terminal == Terminal::Applied,
          "no cross-context effect change");
  const auto continued = second.begin_key({KeyAction::Type::AppendChar, 'y'}, true);
  require(continued.key.has_value(), "second context still admits work");
  require(second.settle_state(*continued.key, EditResult::AppliedReadback) == Result::Accepted,
          "second context still settles its own work");

  // The destroyed context accepts nothing, including its own late results.
  require(first.settle_state(*first_state.key, EditResult::AppliedReadback) == Result::Stale,
          "destroyed context fences its own late state");
  require(second.core().records().size() == second_records + 1,
          "the second context accounted only its own new key");
  std::cout << "PASS context_isolation\n";
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: tsf_lifecycle <obligation|teardown_clean|teardown_unresolved|"
                 "composition_terminated|identity_across_focus|context_isolation>\n";
    return 1;
  }
  const std::string mode = argv[1];
  try {
    if (mode == "obligation")
      obligation();
    else if (mode == "teardown_clean")
      teardown_clean();
    else if (mode == "teardown_unresolved")
      teardown_unresolved();
    else if (mode == "composition_terminated")
      composition_terminated();
    else if (mode == "identity_across_focus")
      identity_across_focus();
    else if (mode == "context_isolation")
      context_isolation();
    else {
      std::cerr << "Unknown mode: " << mode << "\n";
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << mode << ": " << e.what() << "\n";
    return 1;
  }
}
