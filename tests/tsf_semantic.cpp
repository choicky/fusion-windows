#include "fusion/host.hpp"
#include "mock.hpp"
#include "mock_backend.hpp"
#include "tsf/key_map.hpp"
#include "tsf/observation.hpp"
#include "tsf/semantic.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace fusion;
using namespace fusion::test;
using namespace fusion::tsf;

namespace {

const Record &record(const Core &core, Key key) {
  const auto &records = core.records();
  auto it =
      std::find_if(records.begin(), records.end(), [key](const Record &r) { return r.key == key; });
  require(it != records.end(), "missing ledger record");
  return *it;
}

bool all_terminal(const Core &core) {
  return std::all_of(core.records().begin(), core.records().end(),
                     [](const Record &r) { return r.terminal != Terminal::Pending; });
}

KeyAction key_append(char ch) { return {KeyAction::Type::AppendChar, ch}; }
KeyAction key_commit() { return {KeyAction::Type::Commit, '\0'}; }
KeyAction key_backspace() { return {KeyAction::Type::Backspace, '\0'}; }
KeyAction key_reject() { return {KeyAction::Type::Reject, '\0'}; }
KeyAction key_pass() { return {KeyAction::Type::Pass, '\0'}; }

// Simulates the TSF shell's synchronous happy path for one owned decision:
// session grant -> (effect: open) -> mutate -> settle.
void execute_sync(SemanticController &semantic, const SemanticDecision &decision) {
  if (decision.route == SemanticDecision::Route::State) {
    require(semantic.settle_state(*decision.key, EditResult::AppliedReadback) == Result::Accepted,
            "sync state settled");
  } else if (decision.route == SemanticDecision::Route::Effect) {
    require(semantic.propose_effect(*decision.key) == Result::Accepted, "sync effect proposed");
    auto ticket = semantic.open_effect(*decision.key);
    require(ticket.has_value(), "sync effect opened");
    require(semantic.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
            "sync effect settled");
  }
}

// --- classification ---------------------------------------------------------

void classification() {
  require(classify_key('N', false, false, false).type == KeyAction::Type::AppendChar,
          "letter appends");
  require(classify_key('N', false, false, false).ch == 'n', "letter lowercased (dev policy)");
  require(classify_key('5', false, false, false).type == KeyAction::Type::AppendChar,
          "digit appends");
  require(classify_key(kVkNumpad0, false, false, false).ch == '0', "numpad digit appends");
  require(classify_key(kVkSpace, false, false, false).type == KeyAction::Type::Commit,
          "space commits");
  require(classify_key(kVkBack, false, false, false).type == KeyAction::Type::Backspace,
          "backspace edits");
  require(classify_key(kVkEscape, false, false, false).type == KeyAction::Type::Reject,
          "escape rejects");
  require(classify_key(kVkReturn, false, false, false).type == KeyAction::Type::Pass,
          "enter is unmapped");
  require(classify_key('N', true, false, false).type == KeyAction::Type::Pass, "ctrl combo passes");
  require(classify_key('N', false, true, false).type == KeyAction::Type::Pass, "alt combo passes");
  require(classify_key('N', false, false, true).type == KeyAction::Type::Pass, "win combo passes");
  require(classify_key(0x74, false, false, false).type == KeyAction::Type::Pass,
          "function key passes");
}

// --- happy-path typing: preedit letters then a space commit -----------------

void typing_composition_commit() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto n = semantic.begin_key(key_append('n'), true);
  require(n.route == SemanticDecision::Route::State && n.eaten && n.text == "n",
          "first letter is owned STATE");
  auto i = semantic.begin_key(key_append('i'), true);
  require(i.text == "ni", "preedit projects the whole raw buffer");
  auto h = semantic.begin_key(key_append('h'), true);
  auto a = semantic.begin_key(key_append('a'), true);
  auto o = semantic.begin_key(key_append('o'), true);
  execute_sync(semantic, n);
  execute_sync(semantic, i);
  execute_sync(semantic, h);
  execute_sync(semantic, a);
  execute_sync(semantic, o);
  require(semantic.raw_buffer() == "nihao", "raw buffer accumulates");
  require(semantic.pass_probe() == Pass::Clear, "states leave PASS clear");

  auto commit = semantic.begin_key(key_commit(), true);
  require(commit.route == SemanticDecision::Route::Effect && commit.text == "nihao",
          "space commits the raw buffer");
  require(semantic.pass_probe() == Pass::DrainRequired,
          "owned effect blocks PASS before settlement");
  execute_sync(semantic, commit);
  require(record(semantic.core(), *commit.key).terminal == Terminal::Applied,
          "commit terminal Applied");
  require(semantic.raw_buffer().empty(), "commit resets the projection");
  require(semantic.pass_probe() == Pass::Clear, "PASS clear after applied effect");
  require(all_terminal(semantic.core()), "all keys terminal");
}

// --- genuine PASS / empty-buffer edge cases ---------------------------------

void pass_paths() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto space = semantic.begin_key(key_commit(), true);
  require(space.route == SemanticDecision::Route::Pass && !space.eaten,
          "space with empty buffer is a genuine PASS");
  auto enter = semantic.begin_key(key_pass(), true);
  require(enter.route == SemanticDecision::Route::Pass && !enter.eaten,
          "unmapped key is a genuine PASS");
  auto escape = semantic.begin_key(key_reject(), true);
  require(escape.route == SemanticDecision::Route::Pass, "escape with empty buffer passes");
  auto back = semantic.begin_key(key_backspace(), true);
  require(back.route == SemanticDecision::Route::Pass, "backspace with empty buffer passes");
  require(semantic.core().records().empty(), "PASS owns nothing");
  require(semantic.finish_pass(), "barrier clear permits PASS");
}

// --- fail-open before ownership ----------------------------------------------

void fail_open() {
  MockBackend backend;
  SemanticController semantic(1, 2, backend);
  auto first = semantic.begin_key(key_append('x'), true);
  require(first.route == SemanticDecision::Route::State, "first key owned");
  execute_sync(semantic, first);
  // Terminal records still occupy ledger capacity until acknowledged, so the
  // bound bites without any acknowledgement here.
  auto second = semantic.begin_key(key_append('y'), true);
  require(second.route == SemanticDecision::Route::State, "second key owned");
  execute_sync(semantic, second);
  auto overflow = semantic.begin_key(key_append('z'), true);
  require(overflow.route == SemanticDecision::Route::FailOpen && !overflow.eaten,
          "capacity exhausted fails open before ownership");
  require(semantic.raw_buffer() == "xy", "fail-open does not advance the projection");
  require(all_terminal(semantic.core()), "no un-owned records");
}

// --- reject after ownership is never PASS ------------------------------------

void reject_after_ownership() {
  MockBackend backend;
  SemanticController semantic(2, 16, backend);
  auto state = semantic.begin_key(key_append('a'), true);
  require(state.route == SemanticDecision::Route::State, "letter owned");
  require(semantic.settle_state(*state.key, EditResult::AppliedReadback) == Result::Accepted,
          "state consumed");
  auto esc = semantic.begin_key(key_reject(), true);
  require(esc.route == SemanticDecision::Route::Reject && esc.eaten,
          "escape during composition is owned rejection");
  require(esc.intent == Intent::Reject, "backend verdict is reject");
  require(record(semantic.core(), *esc.key).terminal == Terminal::FailedNoEffect,
          "rejection reaches FailedNoEffect");
  require(record(semantic.core(), *esc.key).terminal != Terminal::Cancelled,
          "rejection is not cancellation");
  require(semantic.raw_buffer().empty(), "rejection clears the projection");
  require(semantic.pass_probe() == Pass::Clear, "failure terminal keeps PASS lawfully clear");
  require(all_terminal(semantic.core()), "rejection is terminal");
}

// --- backspace edits the projection ------------------------------------------

void backspace_projection() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto a = semantic.begin_key(key_append('a'), true);
  auto b = semantic.begin_key(key_append('b'), true);
  execute_sync(semantic, a);
  execute_sync(semantic, b);
  auto undo = semantic.begin_key(key_backspace(), true);
  require(undo.route == SemanticDecision::Route::State && undo.text == "a",
          "backspace owns a STATE edit with the shortened buffer");
  execute_sync(semantic, undo);
  auto commit = semantic.begin_key(key_commit(), true);
  require(commit.text == "a", "commit carries the edited buffer");
  execute_sync(semantic, commit);
  require(record(semantic.core(), *commit.key).terminal == Terminal::Applied, "edited commit");
  require(all_terminal(semantic.core()), "all terminal after backspace flow");
}

// --- sync refusal: async fallback queue, FIFO state settlement ---------------

void async_fallback_fifo() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  // n, i both owned; both sync sessions refused -> queued FIFO.
  auto n = semantic.begin_key(key_append('n'), true);
  semantic.enqueue_state(*n.key, n.text);
  auto i = semantic.begin_key(key_append('i'), true);
  semantic.enqueue_state(*i.key, i.text);
  require(semantic.pending().size() == 2, "two queued states");
  // Later async session applies latest-wins text and settles FIFO.
  semantic.settle_queued_states(EditResult::AppliedReadback);
  require(semantic.pending().empty(), "queue drained");
  require(record(semantic.core(), *n.key).terminal == Terminal::Consumed, "n consumed");
  require(record(semantic.core(), *i.key).terminal == Terminal::Consumed, "i consumed");
  require(semantic.raw_buffer() == "ni", "projection intact");
  require(all_terminal(semantic.core()), "async states terminal");
}

// --- drain-before-PASS: pending effect then a genuine pass key ----------------

void drain_before_pass() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto a = semantic.begin_key(key_append('a'), true);
  execute_sync(semantic, a);
  auto commit = semantic.begin_key(key_commit(), true);
  require(semantic.propose_effect(*commit.key) == Result::Accepted, "proposed");
  // Sync session refused: the effect is queued, still unresolved.
  semantic.enqueue_effect(*commit.key, commit.text);
  auto later = semantic.begin_key(key_pass(), true);
  require(later.route == SemanticDecision::Route::Pass, "later key is a genuine pass");
  require(semantic.pass_probe() == Pass::DrainRequired, "PASS cannot overtake the effect");
  require(!semantic.finish_pass(), "no genuine pass while unresolved");
  // Drain: the K-callback executes the pending effect synchronously.
  auto work = semantic.drain_work();
  require(work.has_value() && work->key == *commit.key, "drain targets the pending effect");
  auto ticket = semantic.open_effect(*commit.key);
  require(ticket.has_value(), "drain opens the ticket");
  require(semantic.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "drain settles applied");
  require(semantic.finish_pass(), "drained barrier now permits genuine PASS");
  require(all_terminal(semantic.core()), "drained effect terminal");
}

// --- refused drain: explicit CANCELLED recovery, never fake PASS -------------

void cancelled_recovery() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto a = semantic.begin_key(key_append('a'), true);
  execute_sync(semantic, a);
  auto commit = semantic.begin_key(key_commit(), true);
  semantic.propose_effect(*commit.key);
  semantic.enqueue_effect(*commit.key, commit.text);
  auto later = semantic.begin_key(key_pass(), true);
  require(later.route == SemanticDecision::Route::Pass, "pass key");
  require(semantic.pass_probe() == Pass::DrainRequired, "blocked");
  // Drain attempt refused (session denied): explicit §7.3 disposition.
  Key cancelled = semantic.cancel_pass_recovery();
  require(cancelled.sequence != 0, "cancellation owned a key");
  require(record(semantic.core(), cancelled).terminal == Terminal::Cancelled, "cancelled terminal");
  require(!semantic.finish_pass(), "still no fake pass (prior effect unresolved)");
  require(record(semantic.core(), *commit.key).terminal != Terminal::Pending ||
              !semantic.pending().empty(),
          "prior effect honestly unresolved");
}

// --- composition termination: queued work reaches explicit terminals ----------

void composition_terminated() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto n = semantic.begin_key(key_append('n'), true);
  semantic.enqueue_state(*n.key, n.text);
  require(semantic.pending().size() == 1, "queued");
  semantic.note_composition_terminated();
  require(record(semantic.core(), *n.key).terminal == Terminal::Cancelled,
          "queued state cancelled by termination");
  require(semantic.raw_buffer().empty(), "projection reset");
  require(all_terminal(semantic.core()), "termination leaves no pending keys");
}

// --- context replacement fences stale work ------------------------------------

void context_replacement_fence() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  auto n = semantic.begin_key(key_append('n'), true);
  require(semantic.settle_state(*n.key, EditResult::AppliedReadback) == Result::Accepted,
          "pre-replacement state consumed");
  require(semantic.replace_context(2) == Result::Accepted, "context replaced");
  // Old-lifetime settlement attempts are fenced; no cross-context effect.
  require(semantic.settle_state(*n.key, EditResult::AppliedReadback) == Result::Stale,
          "old-lifetime state fenced");
  require(semantic.pass_probe() == Pass::Clear, "fresh context barrier clear");
  auto fresh = semantic.begin_key(key_append('m'), true);
  require(fresh.route == SemanticDecision::Route::State, "fresh context accepts input");
  require(semantic.settle_state(*fresh.key, EditResult::AppliedReadback) == Result::Accepted,
          "current binding settles");
  require(all_terminal(semantic.core()), "replacement leaves explicit terminals");
}

// --- sequential owned events keep document order --------------------------------

void ordering() {
  MockBackend backend;
  SemanticController semantic(1, 16, backend);
  // n-i-h-a-o then space, then h-a then space: two commits in order.
  std::vector<std::string> committed;
  for (char ch : std::string("nihao")) {
    execute_sync(semantic, semantic.begin_key(key_append(ch), true));
  }
  auto c1 = semantic.begin_key(key_commit(), true);
  execute_sync(semantic, c1);
  committed.push_back(c1.text);
  for (char ch : std::string("ha")) {
    execute_sync(semantic, semantic.begin_key(key_append(ch), true));
  }
  auto c2 = semantic.begin_key(key_commit(), true);
  execute_sync(semantic, c2);
  committed.push_back(c2.text);
  require(committed == std::vector<std::string>{"nihao", "ha"}, "commit order preserved");
  require(all_terminal(semantic.core()), "sequence terminal");
  require(semantic.pass_probe() == Pass::Clear, "clear after sequence");
}

void repaired_boundaries() {
  require(observed_edit(true, false, true) == EditResult::Indeterminate, "STATE readback mismatch");
  require(observed_edit(true, true, false) == EditResult::Indeterminate, "composition end failure");
  require(!async_admitted(0, static_cast<std::int32_t>(0x80040208)),
          "inner refusal is not admission");
  require(!async_admitted(-1, 0x00040300), "outer failure is not admission");
  require(async_admitted(0, 0x00040300), "queued admission");
  require(!async_admitted(0, 0), "no invented async completion");
  require(!same_entry_generation(1, 2), "reused address different incarnation");
  MockBackend backend;
  SemanticController semantic(1, 3, backend, 1);
  auto first = semantic.begin_key(key_append('a'), true);
  execute_sync(semantic, first);
  auto effect = semantic.begin_key(key_commit(), true);
  require(semantic.propose_effect(*effect.key) == Result::Accepted, "effect proposed");
  semantic.enqueue_effect(*effect.key, effect.text);
  auto overflow = semantic.begin_key(key_append('b'), true);
  require(overflow.route == SemanticDecision::Route::FailOpen, "recovery capacity reserved");
  require(!semantic.finish_pass(), "failed admission does not bypass unresolved effect");
  auto cancelled = semantic.cancel_pass_recovery();
  require(cancelled.sequence != 0 &&
              record(semantic.core(), cancelled).terminal == Terminal::Cancelled,
          "reserved explicit cancellation");
  semantic.retire_terminals();
  require(semantic.core().records().size() == 1, "only pending effect retained");
  require(!semantic.finish_pass(), "reclamation preserves barrier");
  auto ticket = semantic.open_effect(*effect.key);
  require(ticket.has_value(), "pending effect remains executable");
  semantic.settle_effect(*ticket, EditResult::Indeterminate);
  semantic.retire_terminals();
  require(semantic.core().records().size() == 1, "uncertainty never reclaimed");
  auto uncertain_cancel = semantic.cancel_pass_recovery();
  require(uncertain_cancel.sequence != 0,
          "uncertainty still admits explicit recovery cancellation");
  require(record(semantic.core(), uncertain_cancel).terminal == Terminal::Cancelled,
          "recovery terminal");
  require(!semantic.finish_pass(), "cancellation never clears uncertainty");
  semantic.retire_terminals();
  require(semantic.core().records().size() == 1, "uncertainty survives cancellation retirement");
  SemanticController watermark(3, 8, backend);
  auto failed_state = watermark.begin_key(key_append('q'), true);
  watermark.settle_state(*failed_state.key, EditResult::RefusedNoMutation);
  auto invalid_effect = watermark.begin_key(key_commit(), true);
  require(watermark.propose_effect(*invalid_effect.key) == Result::Blocked,
          "unconsumed required state cannot authorize effect");
  SemanticController ordered(4, 8, backend);
  auto s1 = ordered.begin_key(key_append('q'), true);
  auto s2 = ordered.begin_key(key_append('r'), true);
  require(ordered.state_applicable(*s1.key) && !ordered.state_applicable(*s2.key),
          "no document mutation before prior state consumed");
  ordered.enqueue_state(*s1.key, s1.text);
  ordered.enqueue_state(*s1.key, s1.text);
  require(ordered.pending().size() == 1, "duplicate retry does not grow queue");
  SemanticController rollback(2, 4, backend);
  execute_sync(rollback, rollback.begin_key(key_append('a'), true));
  execute_sync(rollback, rollback.begin_key(key_append('b'), true));
  require(rollback.begin_key(key_backspace(), false).route == SemanticDecision::Route::FailOpen,
          "Backspace refused");
  require(rollback.raw_buffer() == "ab", "Backspace transaction rolled back");
  for (int i = 0; i < 200; ++i) {
    rollback.retire_terminals();
    auto state = rollback.begin_key(key_append('x'), true);
    require(state.eaten, "bounded long sequence admission");
    execute_sync(rollback, state);
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  const std::string name = argv[1];
  try {
    if (name == "repaired_boundaries") {
      repaired_boundaries();
    } else if (name == "classification") {
      classification();
    } else if (name == "typing") {
      typing_composition_commit();
    } else if (name == "pass_paths") {
      pass_paths();
    } else if (name == "fail_open") {
      fail_open();
    } else if (name == "reject") {
      reject_after_ownership();
    } else if (name == "backspace") {
      backspace_projection();
    } else if (name == "async_fallback") {
      async_fallback_fifo();
    } else if (name == "drain_before_pass") {
      drain_before_pass();
    } else if (name == "cancelled_recovery") {
      cancelled_recovery();
    } else if (name == "composition_terminated") {
      composition_terminated();
    } else if (name == "context_fence") {
      context_replacement_fence();
    } else if (name == "ordering") {
      ordering();
    } else {
      return 2;
    }
    std::cout << "PASS tsf " << name << '\n';
  } catch (const std::exception &error) {
    std::cerr << "FAIL tsf " << name << ": " << error.what() << '\n';
    return 1;
  }
  return 0;
}
