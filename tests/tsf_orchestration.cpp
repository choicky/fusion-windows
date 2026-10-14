#include "mock.hpp"
#include "tsf/orchestration.hpp"
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
void routing() {
  for (const bool mapped : {true, false}) {
    MockBackend backend;
    SemanticController semantic(1, 8, backend, 1);
    const auto a = semantic.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
    semantic.settle_state(*a.key, EditResult::AppliedReadback);
    const auto effect = semantic.begin_key({KeyAction::Type::Commit, 0}, true);
    require(semantic.propose_effect(*effect.key) == Result::Accepted, "prior proposed");
    semantic.enqueue_effect(*effect.key, effect.text);
    std::vector<std::string> order;
    int drains = 0, cancellations = 0, current_calls = 0;
    const bool proceed = continue_current_key(
        [&] { return semantic.pass_probe(); },
        [&] {
          ++drains;
          auto ticket = semantic.open_effect(*effect.key);
          require(ticket.has_value(), "prior opens");
          require(semantic.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
                  "prior settles");
          order.push_back("prior");
        },
        [&] { ++cancellations; });
    require(proceed && drains == 1 && cancellations == 0, "drain preserves current key");
    if (proceed) {
      ++current_calls;
      const auto current = semantic.begin_key(
          {mapped ? KeyAction::Type::AppendChar : KeyAction::Type::Pass, 'b'}, true);
      order.push_back("current");
      require(current.eaten == mapped, "mapped stays owned, genuine PASS stays unowned");
      if (mapped) {
        require(current.route == SemanticDecision::Route::State && current.text == "b",
                "same current key becomes new preedit, not PASS");
        require(semantic.settle_state(*current.key, EditResult::AppliedReadback) ==
                    Result::Accepted,
                "current settles after prior");
      } else {
        require(current.route == SemanticDecision::Route::Pass && semantic.finish_pass(),
                "genuine PASS remains possible");
      }
    }
    require(current_calls == 1 && order == std::vector<std::string>{"prior", "current"},
            "current neither lost nor duplicated");
  }
  MockBackend backend;
  SemanticController blocked(2, 8, backend, 1);
  const auto effect = blocked.begin_key({KeyAction::Type::AppendChar, 'x'}, true);
  blocked.settle_state(*effect.key, EditResult::AppliedReadback);
  const auto commit = blocked.begin_key({KeyAction::Type::Commit, 0}, true);
  blocked.propose_effect(*commit.key);
  int current_calls = 0, cancellations = 0;
  const bool proceed = continue_current_key([&] { return blocked.pass_probe(); }, [] {},
                                            [&] {
                                              ++cancellations;
                                              const auto cancelled = blocked.cancel_pass_recovery();
                                              require(record(blocked.core(), cancelled).terminal ==
                                                          Terminal::Cancelled,
                                                      "explicit recovery");
                                            });
  if (proceed)
    ++current_calls;
  require(!proceed && current_calls == 0 && cancellations == 1 && !blocked.finish_pass(),
          "refused drain cannot fake PASS or process current key");
}
void cleanup() {
  MockBackend backend;
  SemanticController semantic(1, 8, backend);
  const auto a = semantic.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
  semantic.settle_state(*a.key, EditResult::AppliedReadback);
  const auto rejection = semantic.begin_key({KeyAction::Type::Reject, 0}, true);
  require(rejection.eaten && semantic.raw_buffer().empty(), "owned rejection already settled");
  CleanupIntent work;
  int composition = 0, replacement = 0, edits = 0;
  work.retain(7, &composition);
  const auto serial = work.serial();
  require(work.request(), "sync cleanup requested");
  work.refused(serial); // no DoEditSession on sync admission refusal
  require(work.pending() && work.request(), "intent retained for real async request");
  require(!work.request(), "at most one admitted opportunity");
  auto edit = [&] {
    ++edits;
    return true;
  };
  require(!work.execute(serial, 8, &composition, edit), "generation fence");
  require(!work.execute(serial, 7, &replacement, edit), "composition identity fence");
  const auto result = work.execute(serial, 7, &composition, [&] {
    ++edits;
    require(!work.execute(serial, 7, &composition, edit), "reentrant duplicate fenced");
    semantic.note_composition_terminated();
    return true;
  });
  require(result && *result && edits == 1 && !work.pending(), "one later cleanup executes");
  require(!work.execute(serial, 7, &composition, edit), "late duplicate cannot mutate");
  require(record(semantic.core(), *rejection.key).terminal == Terminal::FailedNoEffect &&
              semantic.raw_buffer().empty(),
          "no semantic resurrection or fake PASS");
  work.retain(7, &replacement);
  require(!work.execute(serial, 7, &replacement, edit), "old serial cannot clear newer intent");
  const auto failed = work.execute(work.serial(), 7, &replacement, [] { return false; });
  require(failed && !*failed && work.pending() && !work.request(),
          "attempted uncertain cleanup is not blindly retried");
  work.reconciled();
  require(!work.pending(), "external termination reconciles failed cleanup");
  work.retain(7, &composition);
  const auto reconciled_failure = work.execute(work.serial(), 7, &composition, [&] {
    work.reconciled(); // real external termination can reenter the attempted cleanup
    return false;
  });
  require(reconciled_failure && !*reconciled_failure && !work.pending(),
          "failed return cannot resurrect externally reconciled cleanup");
}
void termination() {
  MockBackend backend;
  SemanticController semantic(1, 8, backend);
  const auto a = semantic.begin_key({KeyAction::Type::AppendChar, 'a'}, true);
  semantic.settle_state(*a.key, EditResult::AppliedReadback);
  const auto effect = semantic.begin_key({KeyAction::Type::Commit, 0}, true);
  semantic.propose_effect(*effect.key);
  const auto ticket = semantic.open_effect(*effect.key);
  require(ticket.has_value(), "normal effect opened");
  int composition = 0, other = 0, external_calls = 0;
  SelfEnd self{7, &composition}; // exactly the scope surrounding real EndComposition
  auto external = [&] {
    ++external_calls;
    semantic.note_composition_terminated();
  };
  require(self.terminated(7, &composition, external), "self callback recognized");
  require(external_calls == 0 && record(semantic.core(), *effect.key).terminal == Terminal::Pending,
          "self callback cannot cancel active settlement");
  require(semantic.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "normal effect settlement remains valid");
  require(record(semantic.core(), *effect.key).terminal == Terminal::Applied, "effect applied");
  const auto pending = semantic.begin_key({KeyAction::Type::AppendChar, 'b'}, true);
  semantic.enqueue_state(*pending.key, pending.text);
  self = {}; // service's EndComposition has returned
  require(!self.terminated(7, &composition, external), "genuine external callback remains active");
  require(external_calls == 1 && semantic.raw_buffer().empty() && semantic.pending().empty() &&
              record(semantic.core(), *pending.key).terminal == Terminal::Cancelled,
          "external termination reconciles projection and cancels queued work");
  self = {7, &composition};
  require(!self.terminated(8, &composition, [&] { ++external_calls; }),
          "generation not suppressed");
  require(!self.terminated(7, &other, [&] { ++external_calls; }),
          "different composition not suppressed");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const std::string name = argv[1];
    if (name == "routing")
      routing();
    else if (name == "cleanup")
      cleanup();
    else if (name == "termination")
      termination();
    else
      return 2;
    std::cout << "PASS orchestration " << name << '\n';
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
