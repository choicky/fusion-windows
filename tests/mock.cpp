#include "mock.hpp"

#include <algorithm>
#include <stdexcept>

namespace fusion::test {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Key Mock::own(Kind kind) {
  auto key = core.own(kind, true);
  require(key.has_value(), "expected ownership admission");
  owned.push_back(*key);
  return *key;
}

bool Mock::edit(Key key, EditResult outcome) {
  auto ticket = core.begin_effect(key);
  if (!ticket) {
    return false;
  }
  // A successful document observation is deliberately separate from core accounting.
  if (outcome == EditResult::AppliedReadback) {
    document.push_back(key);
  }
  require(core.complete_effect(*ticket, outcome) == Result::Accepted, "completion accepted");
  return true;
}

void Mock::at(std::uint64_t tick, std::function<void()> action) {
  schedule_.push_back({tick, std::move(action)});
}

void Mock::run() {
  std::stable_sort(schedule_.begin(), schedule_.end(),
                   [](const Stimulus &a, const Stimulus &b) { return a.tick < b.tick; });
  // Scheduling during callbacks is not supported: no mutable iterator invalidation.
  auto batch = std::move(schedule_);
  schedule_.clear();
  for (auto &stimulus : batch) {
    stimulus.action();
  }
}

void Mock::audit_terminal() const {
  for (auto key : owned) {
    const auto &records = core.records();
    auto it = std::find_if(records.begin(), records.end(),
                           [key](const Record &r) { return r.key == key; });
    require(it != records.end(), "silent owned-key loss");
    require(it->terminal != Terminal::Pending, "owned key missing terminal disposition");
  }
}

void Mock::audit_unique() const {
  for (std::size_t i = 0; i < document.size(); ++i) {
    require(std::find(document.begin() + static_cast<std::ptrdiff_t>(i + 1), document.end(),
                      document[i]) == document.end(),
            "duplicate irreversible EFFECT");
  }
}

} // namespace fusion::test
