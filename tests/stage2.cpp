#include "fusion/host.hpp"
#include "mock.hpp"
#include "mock_backend.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace fusion;
using namespace fusion::test;

namespace {

const Record &record(const Core &core, Key key) {
  const auto &records = core.records();
  auto it =
      std::find_if(records.begin(), records.end(), [key](const Record &r) { return r.key == key; });
  require(it != records.end(), "missing ledger record");
  return *it;
}

// A Stage 2 rig wires stimulus -> Mock Backend -> Host boundary -> FusionCore, and
// records document materialization through the shared FusionMock oracles.
struct Rig {
  explicit Rig(std::size_t capacity = 32) : m(capacity), host(m.core) {}
  Mock m;
  Host host;
  MockBackend backend;
};

// Apply a backend Preedit for an already-owned State key (semantic STATE path).
void apply_preedit(Rig &r, Key key, EditResult observed = EditResult::AppliedReadback) {
  require(r.backend.respond("preedit:").intent == Intent::Preedit, "preedit is a preedit");
  require(r.host.preedit(key, observed) == Result::Accepted, "preedit consumed");
}

// Materialize a backend Commit exactly once: propose at watermark, open the single
// opportunity, the document authority mutates, then settle. Returns settle Result.
Result materialize_commit(Rig &r, Key key, EditResult observed = EditResult::AppliedReadback) {
  require(r.backend.respond("commit:你好").intent == Intent::Commit, "commit is a commit");
  require(r.host.commit(key) == Result::Accepted, "commit proposed at watermark");
  auto ticket = r.host.open(key);
  if (!ticket) {
    return Result::Blocked;
  }
  if (observed == EditResult::AppliedReadback) {
    r.m.document.push_back(key);
  }
  return r.host.settle(*ticket, observed);
}

// A. STATE path: owned input -> backend preedit -> STATE consumed & observed.
void state_path() {
  Rig r;
  auto key = r.m.own(Kind::State);
  apply_preedit(r, key);
  require(record(r.m.core, key).terminal == Terminal::Consumed, "STATE consumed terminal");
  require(r.host.watermark() == key.sequence, "STATE advances watermark");
  require(r.m.document.empty(), "STATE materializes no document effect");
  r.m.audit_terminal();
}

// B. EFFECT path: owned input -> backend commit -> ticket -> document readback ->
// exactly one materialized result -> terminal Applied.
void effect_path() {
  Rig r;
  auto state = r.m.own(Kind::State);
  apply_preedit(r, state);
  auto effect = r.m.own(Kind::Effect);
  require(materialize_commit(r, effect) == Result::Accepted, "commit applied");
  require(record(r.m.core, effect).terminal == Terminal::Applied, "EFFECT terminal Applied");
  r.m.audit_unique();
  require(r.m.document.size() == 1, "exactly one materialized result");
  r.m.audit_terminal();
}

// C. Genuine PASS: an unowned passable input reaches PASS with no fake ownership.
void pass() {
  Rig r;
  std::size_t before = r.m.core.records().size();
  require(r.backend.respond("f5").intent == Intent::Pass, "unhandled input is a genuine pass");
  require(r.host.pass_barrier() == Pass::Clear, "clear barrier permits PASS");
  require(r.m.core.records().size() == before, "PASS owns nothing");
  require(r.m.document.empty(), "PASS materializes nothing");
  r.m.audit_terminal();
}

// D. PASS barrier: an unresolved prior owned EFFECT blocks a later genuine PASS.
void pass_barrier() {
  Rig r;
  auto effect = r.m.own(Kind::Effect);
  require(r.host.pass_barrier() == Pass::DrainRequired, "prior EFFECT blocks PASS");
  require(r.backend.respond("pass").intent == Intent::Pass, "later input is a genuine pass");
  require(r.host.pass_barrier() == Pass::DrainRequired, "PASS cannot overtake unresolved EFFECT");
  require(materialize_commit(r, effect) == Result::Accepted, "prior EFFECT drains");
  require(r.host.pass_barrier() == Pass::Clear, "only completion releases PASS");
  r.m.audit_terminal();
  r.m.audit_unique();
}

// E. Delayed response: the semantic response arrives later while ownership and
// accounting stay correct throughout.
void delay() {
  Rig r;
  auto state = r.m.own(Kind::State);
  auto effect = r.m.own(Kind::Effect);
  r.m.at(0, [&] {
    require(r.host.pass_barrier() == Pass::DrainRequired, "delay keeps EFFECT pending");
  });
  r.m.at(5, [&] {
    apply_preedit(r, state);
    require(materialize_commit(r, effect) == Result::Accepted, "delayed commit lands once");
  });
  r.m.run();
  require(r.host.pass_barrier() == Pass::Clear, "resolved after delay");
  require(r.m.document.size() == 1, "single materialization despite delay");
  r.m.audit_terminal();
  r.m.audit_unique();
}

// Illegal causal reorder: a later EFFECT cannot overtake an earlier event or an
// unconsumed STATE through the backend/adapter path.
void reorder() {
  Rig r;
  auto state = r.m.own(Kind::State);
  auto first = r.m.own(Kind::Effect);
  auto second = r.m.own(Kind::Effect);
  require(r.host.commit(second) == Result::Accepted, "second proposed");
  require(!r.host.open(second), "second cannot overtake earlier event");
  require(r.host.commit(first) == Result::Accepted, "first proposed");
  require(!r.host.open(first), "unconsumed STATE prevents admission");
  apply_preedit(r, state);
  auto t1 = r.host.open(first);
  require(t1.has_value(), "first admitted after state consumed");
  r.m.document.push_back(first);
  require(r.host.settle(*t1, EditResult::AppliedReadback) == Result::Accepted, "first applied");
  auto t2 = r.host.open(second);
  require(t2.has_value(), "second admitted after first");
  r.m.document.push_back(second);
  require(r.host.settle(*t2, EditResult::AppliedReadback) == Result::Accepted, "second applied");
  require(r.m.document == std::vector<Key>{first, second}, "causal document order preserved");
  r.m.audit_terminal();
  r.m.audit_unique();
}

// F. Duplicate response: a duplicated semantic/effect response cannot produce a
// second irreversible materialization.
void duplicate() {
  Rig r;
  auto effect = r.m.own(Kind::Effect);
  require(materialize_commit(r, effect) == Result::Accepted, "first commit lands");
  require(r.host.commit(effect) == Result::Duplicate, "duplicate proposal rejected");
  require(!r.host.open(effect), "duplicate cannot reopen execution");
  r.m.audit_unique();
  require(r.m.document.size() == 1, "duplicate response materializes nothing extra");
  r.m.audit_terminal();
}

// G. Stale context/lifetime response: an old response after context replacement is
// fenced; no cross-context mutation.
void stale() {
  Rig r;
  auto old = r.m.own(Kind::Effect);
  require(r.host.commit(old) == Result::Accepted, "old proposed");
  auto ticket = r.host.open(old);
  require(ticket.has_value(), "old ticket captured");
  require(r.m.core.replace_context({2, 2, 1}) == Result::Accepted, "context replaced");
  require(r.host.preedit(old, EditResult::AppliedReadback) == Result::Stale, "old state fenced");
  require(r.host.commit(old) == Result::Stale, "old proposal fenced");
  require(!r.host.open(old), "old effect cannot execute");
  require(r.host.settle(*ticket, EditResult::AppliedReadback) == Result::Stale,
          "old completion stale");
  require(r.host.reconcile(*ticket, Readback::NoEffect) == Result::Accepted, "old accounting");
  require(r.m.core.rebind() == Result::Accepted, "replacement readiness explicit");
  require(r.m.document.empty(), "no cross-context mutation");
  auto fresh = r.m.own(Kind::Effect);
  require(materialize_commit(r, fresh) == Result::Accepted, "fresh context effect");
  require(r.m.document.size() == 1 && r.m.document.front().binding.context == 2,
          "materialized in new context only");
  r.m.audit_terminal();
}

// H. Backend restart/reset: an old-epoch response cannot resurrect stale state or
// replay an irreversible effect.
void restart() {
  Rig r;
  auto state = r.m.own(Kind::State);
  apply_preedit(r, state);
  require(r.host.watermark() == state.sequence, "watermark before restart");
  auto effect = r.m.own(Kind::Effect);
  require(r.host.commit(effect) == Result::Accepted, "old-epoch commit proposed");
  r.backend.reset(); // backend incarnation advances
  require(r.m.core.restart(2) == Result::Accepted, "Windows epoch advance");
  require(r.host.watermark() == 0, "no inherited watermark into new epoch");
  require(record(r.m.core, effect).terminal == Terminal::FailedNoEffect, "old effect fenced");
  require(!r.host.open(effect), "old effect cannot replay");
  require(r.host.commit(effect) == Result::Stale, "old-epoch proposal fenced");
  require(r.m.core.rebind() == Result::Accepted, "new epoch readiness explicit");
  auto fresh = r.m.own(Kind::Effect);
  require(materialize_commit(r, fresh) == Result::Accepted, "new epoch effect lands");
  r.m.audit_unique();
  require(r.m.document.size() == 1, "single materialization after restart");
  r.m.audit_terminal();
}

// I. Backend rejection after ownership: an owned key that the backend rejects is a
// failure input reaching an explicit terminal, never silently converted to PASS.
void reject() {
  Rig r;
  auto effect = r.m.own(Kind::Effect); // Windows ate the key (owned)
  require(r.backend.respond("reject:x").intent == Intent::Reject,
          "backend rejects after ownership");
  require(r.host.reject(effect) == Result::Accepted, "rejection settled");
  require(record(r.m.core, effect).terminal == Terminal::FailedNoEffect,
          "reject is a failure terminal");
  require(record(r.m.core, effect).terminal != Terminal::Cancelled, "reject differs from cancel");
  auto cancelled = r.m.own(Kind::Effect);
  require(r.host.commit(cancelled) == Result::Accepted, "second proposed");
  require(r.host.cancel(cancelled) == Result::Accepted, "explicit cancellation");
  require(record(r.m.core, cancelled).terminal == Terminal::Cancelled,
          "cancel has its own terminal");
  require(r.m.document.empty(), "neither reject nor cancel materializes");
  r.m.audit_terminal();
}

// J. Indeterminate irreversible mutation: reconnect/reset must not blindly retry an
// effect whose materialization may already have occurred.
void indeterminate() {
  Rig r;
  auto effect = r.m.own(Kind::Effect);
  require(r.host.commit(effect) == Result::Accepted, "proposed");
  auto ticket = r.host.open(effect);
  require(ticket.has_value(), "first execution");
  r.m.document.push_back(effect); // document may already have changed
  require(r.host.settle(*ticket, EditResult::Indeterminate) == Result::Accepted,
          "uncertain completion");
  require(!r.host.open(effect), "indeterminate cannot retry");
  require(r.host.pass_barrier() == Pass::RecoveryRequired, "uncertainty fences PASS");
  require(r.m.core.acknowledge(effect) == Result::Blocked, "uncertainty accounting retained");
  require(r.m.core.restart(2) == Result::Accepted, "reconnect/reset");
  require(r.m.core.rebind() == Result::Blocked, "restart cannot discard uncertainty");
  require(!r.host.open(effect), "old epoch cannot blind-retry");
  require(r.host.reconcile(*ticket, Readback::Applied) == Result::Accepted,
          "readback accounting only");
  require(r.m.core.rebind() == Result::Accepted, "reconciled readiness");
  require(!r.host.open(effect), "readback grants no new materialization");
  r.m.audit_unique();
  r.m.audit_terminal();
  require(r.m.document.size() == 1, "indeterminate mutation happened exactly once");
}

// Queue bound: the semantic path cannot bypass bounded admission.
void bound() {
  Rig r(2);
  auto a = r.m.own(Kind::Effect);
  auto b = r.m.own(Kind::Effect);
  require(!r.m.core.own(Kind::Effect, true), "bounded admission rejects before ownership");
  require(r.m.core.records().size() == 2, "bound retained");
  require(!r.m.core.own(Kind::Effect, false), "hard policy cannot be overridden");
  require(r.host.reject(a) == Result::Accepted, "one owned key settled");
  require(record(r.m.core, a).terminal == Terminal::FailedNoEffect, "settlement is terminal");
  require(r.m.core.acknowledge(a) == Result::Accepted, "caller receives terminal accounting");
  require(r.m.core.records().size() == 1, "acknowledge frees capacity");
  require(r.m.core.own(Kind::State, true).has_value(), "capacity released without ID reuse");
  require(r.host.reject(b) == Result::Accepted, "second settled");
  require(r.m.document.empty(), "no materialization through bounded admission");
  r.m.audit_unique();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  const std::string name = argv[1];
  try {
    if (name == "state_path") {
      state_path();
    } else if (name == "effect_path") {
      effect_path();
    } else if (name == "pass") {
      pass();
    } else if (name == "pass_barrier") {
      pass_barrier();
    } else if (name == "delay") {
      delay();
    } else if (name == "reorder") {
      reorder();
    } else if (name == "duplicate") {
      duplicate();
    } else if (name == "stale") {
      stale();
    } else if (name == "restart") {
      restart();
    } else if (name == "reject") {
      reject();
    } else if (name == "indeterminate") {
      indeterminate();
    } else if (name == "bound") {
      bound();
    } else {
      return 2;
    }
    std::cout << "PASS stage2 " << name << '\n';
  } catch (const std::exception &error) {
    std::cerr << "FAIL stage2 " << name << ": " << error.what() << '\n';
    return 1;
  }
  return 0;
}
