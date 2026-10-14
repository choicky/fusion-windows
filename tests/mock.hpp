#pragma once

#include "fusion/core.hpp"

#include <functional>
#include <vector>

namespace fusion::test {

// Test-only logical scheduling and document observations; no correctness decisions.
class Mock {
public:
  explicit Mock(std::size_t capacity = 32) : core({1, 1, 1}, capacity) {}
  Key own(Kind kind);
  bool edit(Key key, EditResult outcome = EditResult::AppliedReadback);
  void at(std::uint64_t tick, std::function<void()> action);
  void run();
  void audit_terminal() const;
  void audit_unique() const;
  Core core;
  std::vector<Key> owned;
  std::vector<Key> document;

private:
  struct Stimulus {
    std::uint64_t tick;
    std::function<void()> action;
  };
  std::vector<Stimulus> schedule_;
};

void require(bool condition, const char *message);

} // namespace fusion::test
