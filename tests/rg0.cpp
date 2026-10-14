#include "mock.hpp"

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

void proposal(Core &core, Key key, std::uint64_t state = 0) {
  require(core.propose_effect(key, state) == Result::Accepted, "proposal accepted");
}

void loss() {
  Mock m;
  auto key = m.own(Kind::Effect);
  require(m.core.reject(key) == Result::Accepted, "backend reject accounted");
  require(record(m.core, key).terminal == Terminal::FailedNoEffect, "reject is not PASS");
  require(m.core.propose_effect(key, 0) == Result::Duplicate, "reject cannot revive key");
  m.audit_terminal();
  Mock state_fault;
  auto uncertain_state = state_fault.own(Kind::State);
  require(state_fault.core.consume_state(uncertain_state, EditResult::Indeterminate) ==
              Result::Accepted,
          "uncertain STATE accounted");
  require(state_fault.core.rebind() == Result::Blocked, "uncertain STATE prevents readiness");
  require(state_fault.core.acknowledge(uncertain_state) == Result::Blocked,
          "uncertain STATE cannot disappear");
  require(state_fault.core.reconcile_state(uncertain_state, Readback::NoEffect) == Result::Accepted,
          "STATE readback reconciliation");
  require(state_fault.core.consumed_state() == 0, "failed STATE does not advance watermark");
  require(state_fault.core.rebind() == Result::Accepted, "STATE reconciled readiness");
  state_fault.audit_terminal();
  auto pending = m.own(Kind::Effect);
  require(m.core.acknowledge(pending) == Result::Blocked, "pending cannot disappear");
  m.core.disconnect();
  m.audit_terminal();
}

void duplicate() {
  Mock m;
  auto key = m.own(Kind::Effect);
  proposal(m.core, key);
  auto ticket = m.core.begin_effect(key);
  require(ticket.has_value(), "first execution admitted");
  require(!m.core.begin_effect(key), "reentrant execution forbidden");
  m.document.push_back(key);
  require(m.core.complete_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "first completion");
  require(m.core.complete_effect(*ticket, EditResult::AppliedReadback) == Result::Duplicate,
          "late duplicate completion rejected");
  m.edit(key);
  m.edit(key);
  m.audit_unique();
  require(m.document.size() == 1, "exactly one document effect");
  require(m.core.acknowledge(key) == Result::Accepted, "terminal acknowledgement");
  require(m.core.propose_effect(key, 0) == Result::Invalid, "retired response cannot resurrect");
}

void context() {
  Mock m;
  auto old = m.own(Kind::Effect);
  proposal(m.core, old);
  auto ticket = m.core.begin_effect(old);
  require(ticket.has_value(), "old ticket captured");
  m.core.destroy_context();
  require(m.core.complete_effect(*ticket, EditResult::AppliedReadback) == Result::Stale,
          "destroyed context completion fenced");
  require(m.core.replace_context({2, 2, 1}) == Result::Accepted, "replacement");
  require(m.core.propose_effect(old, 0) == Result::Stale, "cross-context proposal fenced");
  require(!m.core.begin_effect(old), "cross-context effect fenced");
  require(m.core.reconcile(*ticket, Readback::NoEffect) == Result::Accepted, "old accounting");
  require(m.core.rebind() == Result::Accepted, "explicit replacement readiness");
  auto fresh = m.own(Kind::Effect);
  proposal(m.core, fresh);
  require(m.edit(fresh), "new context effect");
  require(m.document.size() == 1 && m.document.front().binding.context == 2,
          "no cross-context mutation");
  m.audit_terminal();
}

void stale() {
  Mock m;
  auto state = m.own(Kind::State);
  require(m.core.consume_state(state, EditResult::AppliedReadback) == Result::Accepted,
          "state consumed");
  auto effect = m.own(Kind::Effect);
  proposal(m.core, effect, state.sequence);
  require(m.edit(effect), "dependent effect");
  require(m.core.consume_state(state, EditResult::AppliedReadback) == Result::Duplicate,
          "old state cannot revive composition after effect");
  auto late = m.own(Kind::State);
  require(m.core.restart(2) == Result::Accepted, "host epoch advance");
  require(m.core.consume_state(late, EditResult::AppliedReadback) == Result::Stale,
          "old epoch state fenced");
  require(m.core.consumed_state() == 0, "new epoch has no inherited watermark");
  require(m.core.restart(2) == Result::Invalid, "epoch cannot be reused");
  m.audit_terminal();
}

void order() {
  Mock m;
  auto state = m.own(Kind::State);
  auto first = m.own(Kind::Effect);
  auto second = m.own(Kind::Effect);
  proposal(m.core, second, state.sequence);
  require(!m.core.begin_effect(second), "later effect cannot overtake earlier event");
  proposal(m.core, first, state.sequence);
  require(!m.core.begin_effect(first), "unconsumed state prevents admission");
  require(m.core.consume_state(state, EditResult::AppliedReadback) == Result::Accepted,
          "delayed STATE consumption");
  require(!m.core.begin_effect(second), "still no effect reorder");
  require(m.edit(first), "first effect");
  require(m.edit(second), "second effect");
  require(m.document == std::vector<Key>{first, second}, "causal document order");
  m.audit_terminal();
  Mock refused;
  auto s = refused.own(Kind::State);
  require(refused.core.consume_state(s, EditResult::RefusedNoMutation) == Result::Accepted,
          "STATE refusal explicit");
  auto e = refused.own(Kind::Effect);
  proposal(refused.core, e, s.sequence);
  require(!refused.core.begin_effect(e), "refused STATE is not consumed watermark");
  require(refused.core.reject(e) == Result::Accepted, "blocked dependent effect settled");
  refused.audit_terminal();
}

void bounds() {
  Mock m(2);
  auto a = m.own(Kind::Effect);
  auto b = m.own(Kind::Effect);
  require(!m.core.own(Kind::Effect, true), "bounded admission rejects before ownership");
  require(m.core.records().size() == 2, "bound retained");
  require(m.core.reject(a) == Result::Accepted, "explicit settlement");
  require(!m.core.own(Kind::Effect, true), "terminal records counted until acknowledged");
  require(m.core.acknowledge(a) == Result::Accepted, "caller receives terminal accounting");
  require(m.core.own(Kind::State, true).has_value(), "capacity released without ID reuse");
  require(!m.core.own(Kind::Effect, false), "hard policy cannot be overridden");
  require(m.core.cancel(b) == Result::Accepted, "cancel second key");
  m.core.disconnect();
  for (const auto &r : m.core.records()) {
    require(r.terminal != Terminal::Pending, "pressure causes no outstanding ownership");
  }
}

void pass() {
  Mock m;
  auto effect = m.own(Kind::Effect);
  require(m.core.probe_pass() == Pass::DrainRequired, "delayed response still blocks PASS");
  proposal(m.core, effect);
  require(m.edit(effect, EditResult::RefusedNoMutation), "sync edit refused, not completed");
  require(m.core.probe_pass() == Pass::DrainRequired, "async admission is not completion");
  require(record(m.core, effect).terminal == Terminal::Pending, "refusal remains unresolved");
  // T unavailable / K available is a caller condition: core has no callback dependency.
  require(m.edit(effect), "later lawful K drain");
  require(m.core.probe_pass() == Pass::Clear, "only APPLIED drain releases PASS");
  m.audit_terminal();
}

void terminal() {
  Mock m;
  auto key = m.own(Kind::Effect);
  m.core.disconnect();
  m.audit_terminal();
  require(record(m.core, key).terminal == Terminal::FailedNoEffect, "disconnect settles unissued");
  require(!m.core.own(Kind::Effect, true), "connected is not ready");
  require(m.core.rebind() == Result::Accepted, "explicit readiness");
  auto another = m.own(Kind::State);
  m.core.destroy_context();
  require(record(m.core, another).terminal != Terminal::Pending, "destroy settles ownership");
  m.audit_terminal();
}

void cancelled() {
  Mock m;
  auto key = m.own(Kind::Effect);
  proposal(m.core, key);
  require(m.core.cancel(key) == Result::Accepted, "explicit cancellation");
  require(record(m.core, key).terminal == Terminal::Cancelled, "cancel is terminal, not PASS");
  require(!m.edit(key), "cancelled key cannot mutate document");
  require(m.document.empty(), "cancel has no document effect");
  m.audit_terminal();
}

void retry() {
  Mock m;
  auto key = m.own(Kind::Effect);
  proposal(m.core, key);
  auto ticket = m.core.begin_effect(key);
  require(ticket.has_value(), "first execution");
  // Document may already have changed even though completion was indeterminate.
  m.document.push_back(key);
  require(m.core.complete_effect(*ticket, EditResult::Indeterminate) == Result::Accepted,
          "uncertain completion");
  require(!m.core.begin_effect(key), "indeterminate cannot retry");
  require(m.core.probe_pass() == Pass::RecoveryRequired, "uncertainty fences PASS");
  require(m.core.acknowledge(key) == Result::Blocked, "uncertainty accounting retained");
  require(m.core.restart(2) == Result::Accepted, "restart");
  require(m.core.rebind() == Result::Blocked, "restart cannot discard uncertainty");
  require(!m.core.begin_effect(key), "old epoch cannot retry");
  require(m.core.reconcile(*ticket, Readback::Applied) == Result::Accepted, "readback only");
  require(m.core.rebind() == Result::Accepted, "reconciled readiness");
  require(!m.core.begin_effect(key), "readback grants no new materialization");
  m.audit_unique();
  m.audit_terminal();
  require(m.document.size() == 1, "indeterminate mutation happened once");
}

void reference() {
  // Independent synchronous reference for a fixed semantic workload: state is
  // consumed, then two distinct effects append tokens in input order.
  // It never calls Core to compute expected output.
  const std::vector<std::uint64_t> expected{2, 3};
  for (unsigned permutation = 0; permutation < 6; ++permutation) {
    Mock m;
    auto s = m.own(Kind::State);
    auto a = m.own(Kind::Effect);
    auto b = m.own(Kind::Effect);
    std::vector<unsigned> order{0, 1, 2};
    for (unsigned i = 0; i < permutation; ++i) {
      std::next_permutation(order.begin(), order.end());
    }
    for (unsigned i = 0; i < order.size(); ++i) {
      m.at(i, [&, action = order[i]] {
        if (action == 0) {
          require(m.core.consume_state(s, EditResult::AppliedReadback) == Result::Accepted,
                  "scheduled STATE");
        } else {
          proposal(m.core, action == 1 ? a : b, s.sequence);
          m.edit(action == 1 ? a : b);
        }
      });
    }
    m.run();
    // A refused admission is not a dropped EVENT: immutable intents stay pending.
    m.edit(a);
    m.edit(b);
    std::vector<std::uint64_t> actual;
    for (auto key : m.document) {
      actual.push_back(key.sequence);
    }
    require(actual == expected, "async model differs from synchronous reference");
    m.audit_unique();
    m.audit_terminal();
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  const std::string name = argv[1];
  try {
    if (name == "loss") {
      loss();
    } else if (name == "duplicate") {
      duplicate();
    } else if (name == "context") {
      context();
    } else if (name == "stale") {
      stale();
    } else if (name == "order") {
      order();
    } else if (name == "bounds") {
      bounds();
    } else if (name == "pass") {
      pass();
    } else if (name == "terminal") {
      terminal();
    } else if (name == "cancelled") {
      cancelled();
    } else if (name == "retry") {
      retry();
    } else if (name == "reference") {
      reference();
    } else {
      return 2;
    }
    std::cout << "PASS " << name << '\n';
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    return 1;
  }
}
