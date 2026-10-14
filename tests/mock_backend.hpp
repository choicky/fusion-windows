#pragma once

#include "fusion/host.hpp"

#include <cstdint>
#include <string>

namespace fusion::test {

// TEST ABSTRACTION ONLY. A deterministic symbolic vocabulary stands in for an
// input-method backend; it is not a language engine and carries no correctness
// policy. FusionCore owns ownership/ordering/fencing/ledger/terminal disposition;
// this class only says what a symbolic input *means* semantically.
//
// Recognized inputs (test-only prefixes; anything unrecognized genuinely passes):
//   "preedit:<text>" -> Intent::Preedit
//   "commit:<text>"  -> Intent::Commit   (irreversible commit intent)
//   "reject:<text>"  -> Intent::Reject   (failure after ownership, never PASS)
//   "pass" / unknown -> Intent::Pass
struct Response {
  Intent intent;
  std::string text;
  std::uint64_t session; // backend incarnation label; fencing is Core's job, not ours
};

class MockBackend {
public:
  // Same input + same session always yields the same response: fully deterministic.
  Response respond(const std::string &input) const;

  // Backend restart/reset: bumps the incarnation label only. FusionCore epoch and
  // fencing are driven separately by the caller.
  void reset() { ++session_; }
  std::uint64_t session() const { return session_; }

private:
  std::uint64_t session_ = 1;
};

} // namespace fusion::test
