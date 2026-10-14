#include "fusion/core.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fusion {

Core::Core(Binding binding, std::size_t capacity) : binding_(binding), capacity_(capacity) {
  if (capacity == 0 || binding.context == 0 || binding.lifetime == 0 || binding.epoch == 0) {
    throw std::invalid_argument("nonzero identity and capacity required");
  }
  records_.reserve(capacity);
}

bool Core::applicable(Key key) const { return alive_ && key.binding == binding_; }

Record *Core::find(Key key) {
  auto it = std::find_if(records_.begin(), records_.end(),
                         [key](const Record &r) { return r.key == key; });
  return it == records_.end() ? nullptr : &*it;
}

Record *Core::find(Ticket ticket) {
  auto *r = find(ticket.key);
  return r && r->ticket == ticket ? r : nullptr;
}

bool Core::prior_pending(Key key) const {
  return std::any_of(records_.begin(), records_.end(), [key](const Record &r) {
    return r.key.binding == key.binding && r.key.sequence < key.sequence &&
           (r.terminal == Terminal::Pending || r.uncertain);
  });
}

std::optional<Key> Core::own(Kind kind, bool policy_allows) {
  if (!policy_allows || !alive_ || !ready_ || records_.size() >= capacity_ ||
      sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  Key key{binding_, ++sequence_};
  records_.push_back(Record{key, kind, Terminal::Pending, false, false, false, 0, std::nullopt});
  return key;
}

Result Core::consume_state(Key key, EditResult result) {
  if (!applicable(key)) {
    return Result::Stale;
  }
  auto *r = find(key);
  if (!r || r->kind != Kind::State) {
    return Result::Invalid;
  }
  if (r->terminal != Terminal::Pending) {
    return Result::Duplicate;
  }
  if (prior_pending(key) || key.sequence <= effect_fence_ || key.sequence <= consumed_state_) {
    return Result::Blocked;
  }
  switch (result) {
  case EditResult::AppliedReadback:
    consumed_state_ = key.sequence;
    r->terminal = Terminal::Consumed;
    break;
  case EditResult::RefusedNoMutation:
    r->terminal = Terminal::FailedNoEffect;
    break;
  case EditResult::Indeterminate:
    r->terminal = Terminal::Indeterminate;
    r->uncertain = true;
    ready_ = false;
    break;
  }
  return Result::Accepted;
}

Result Core::propose_effect(Key key, std::uint64_t required_state) {
  if (!applicable(key)) {
    return Result::Stale;
  }
  auto *r = find(key);
  if (!r || r->kind != Kind::Effect) {
    return Result::Invalid;
  }
  if (r->terminal != Terminal::Pending || r->proposed) {
    return Result::Duplicate;
  }
  if (required_state >= key.sequence) {
    return Result::Invalid;
  }
  r->proposed = true;
  r->required_state = required_state;
  return Result::Accepted;
}

std::optional<Ticket> Core::begin_effect(Key key) {
  if (!applicable(key) || !ready_) {
    return std::nullopt;
  }
  auto *r = find(key);
  if (!r || r->kind != Kind::Effect || r->terminal != Terminal::Pending || !r->proposed ||
      r->issued || r->uncertain || prior_pending(key) || r->required_state > consumed_state_ ||
      ticket_serial_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  r->issued = true;
  r->ticket = Ticket{key, ++ticket_serial_};
  return r->ticket;
}

Result Core::complete_effect(Ticket ticket, EditResult result) {
  if (!applicable(ticket.key)) {
    return Result::Stale;
  }
  auto *r = find(ticket);
  if (!r || !r->issued) {
    return Result::Invalid;
  }
  if (r->terminal != Terminal::Pending || r->uncertain) {
    return Result::Duplicate;
  }
  switch (result) {
  case EditResult::AppliedReadback:
    r->terminal = Terminal::Applied;
    effect_fence_ = ticket.key.sequence;
    break;
  case EditResult::RefusedNoMutation:
    // Known refusal permits another execution opportunity, never completion.
    r->issued = false;
    r->ticket.reset();
    break;
  case EditResult::Indeterminate:
    r->terminal = Terminal::Indeterminate;
    r->uncertain = true;
    ready_ = false;
    break;
  }
  return Result::Accepted;
}

Result Core::reject(Key key) {
  auto *r = find(key);
  if (!applicable(key)) {
    return Result::Stale;
  }
  if (!r) {
    return Result::Invalid;
  }
  if (r->terminal != Terminal::Pending) {
    return Result::Duplicate;
  }
  if (r->issued) {
    return Result::Blocked;
  }
  r->terminal = Terminal::FailedNoEffect;
  return Result::Accepted;
}

Result Core::cancel(Key key) {
  auto *r = find(key);
  if (!applicable(key)) {
    return Result::Stale;
  }
  if (!r) {
    return Result::Invalid;
  }
  if (r->terminal != Terminal::Pending) {
    return Result::Duplicate;
  }
  if (r->issued) {
    return Result::Blocked;
  }
  r->terminal = Terminal::Cancelled;
  return Result::Accepted;
}

Result Core::reconcile(Ticket ticket, Readback result) {
  auto *r = find(ticket);
  if (!r || !r->uncertain) {
    return Result::Invalid;
  }
  r->uncertain = false;
  r->terminal = result == Readback::Applied ? Terminal::Applied : Terminal::FailedNoEffect;
  // This is readback accounting, never another materialization permission.
  return Result::Accepted;
}

Result Core::reconcile_state(Key key, Readback result) {
  auto *r = find(key);
  if (!r || r->kind != Kind::State || !r->uncertain) {
    return Result::Invalid;
  }
  r->uncertain = false;
  r->terminal = result == Readback::Applied ? Terminal::Consumed : Terminal::FailedNoEffect;
  if (result == Readback::Applied && applicable(key)) {
    consumed_state_ = std::max(consumed_state_, key.sequence);
  }
  return Result::Accepted;
}

void Core::fence() {
  for (auto &r : records_) {
    if (r.terminal != Terminal::Pending) {
      continue;
    }
    r.terminal = r.issued ? Terminal::Indeterminate : Terminal::FailedNoEffect;
    r.uncertain = r.issued;
  }
  ready_ = false;
}

void Core::disconnect() { fence(); }

Result Core::restart(std::uint64_t epoch) {
  if (epoch <= binding_.epoch) {
    return Result::Invalid;
  }
  fence();
  binding_.epoch = epoch;
  consumed_state_ = 0;
  effect_fence_ = 0;
  return Result::Accepted;
}

Result Core::rebind() {
  if (!alive_ ||
      std::any_of(records_.begin(), records_.end(), [](const Record &r) { return r.uncertain; })) {
    return Result::Blocked;
  }
  ready_ = true;
  return Result::Accepted;
}

void Core::destroy_context() {
  fence();
  alive_ = false;
}

Result Core::replace_context(Binding binding) {
  if (binding.context == 0 || binding.lifetime <= binding_.lifetime ||
      binding.epoch < binding_.epoch) {
    return Result::Invalid;
  }
  fence();
  binding_ = binding;
  consumed_state_ = 0;
  effect_fence_ = 0;
  alive_ = true;
  // Readiness is explicit; a connection alone does not imply semantic readiness.
  return Result::Accepted;
}

Pass Core::probe_pass() const {
  for (const auto &r : records_) {
    if (r.key.binding.context == binding_.context && r.key.binding.lifetime == binding_.lifetime &&
        r.uncertain) {
      return Pass::RecoveryRequired;
    }
  }
  for (const auto &r : records_) {
    if (r.key.binding == binding_ && r.kind == Kind::Effect && r.terminal == Terminal::Pending) {
      return Pass::DrainRequired;
    }
  }
  return Pass::Clear;
}

std::optional<Key> Core::record_cancelled_recovery() {
  if (!alive_ || records_.size() >= capacity_ ||
      sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  Key key{binding_, ++sequence_};
  records_.push_back(
      Record{key, Kind::Effect, Terminal::Cancelled, false, false, false, 0, std::nullopt});
  return key;
}

Result Core::acknowledge(Key key) {
  auto *r = find(key);
  if (!r) {
    return Result::Invalid;
  }
  if (r->terminal == Terminal::Pending || r->uncertain) {
    return Result::Blocked;
  }
  records_.erase(std::remove_if(records_.begin(), records_.end(),
                                [key](const Record &record) { return record.key == key; }),
                 records_.end());
  return Result::Accepted;
}

} // namespace fusion
