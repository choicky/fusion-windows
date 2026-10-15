#include "fusion/core.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fusion {

Core::Core(Binding binding, std::size_t capacity) : binding_(binding), capacity_(capacity) {
  if (capacity == 0 || binding.context == 0 || binding.lifetime == 0 || binding.epoch == 0)
    throw std::invalid_argument("nonzero identity and capacity required");
  records_.reserve(capacity);
}
bool Core::applicable(Key key) const { return alive_ && key.binding == binding_; }
Record *Core::find(Key key) {
  auto it = std::find_if(records_.begin(), records_.end(), [key](const Record &r) { return r.key == key; });
  return it == records_.end() ? nullptr : &*it;
}
Record *Core::find(Ticket ticket) {
  auto *r = find(ticket.key);
  if (!r || ticket.operation_index >= r->effects.size()) return nullptr;
  return r->effects[ticket.operation_index].ticket == ticket ? r : nullptr;
}
bool Core::prior_pending(Key key) const {
  return std::any_of(records_.begin(), records_.end(), [key](const Record &r) {
    return r.key.binding == key.binding && r.key.sequence < key.sequence &&
           (r.terminal == Terminal::Pending || r.uncertain || (r.has_state && !r.state_settled));
  });
}

bool Core::fully_settled(const Record &record) const {
  if (record.has_state && !record.state_settled)
    return false;
  for (const auto &effect : record.effects) {
    if (effect.present && effect.terminal != Terminal::Applied)
      return false;
  }
  return true;
}
bool Core::terminally_settled(const Record &record) const {
  if (record.has_state && !record.state_settled)
    return false;
  for (const auto &effect : record.effects)
    if (effect.present && (effect.terminal == Terminal::Pending || effect.terminal == Terminal::Indeterminate))
      return false;
  return true;
}

std::optional<Key> Core::own(bool policy_allows) {
  if (!policy_allows || !alive_ || !ready_ || records_.size() >= capacity_ ||
      sequence_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  Key key{binding_, ++sequence_};
  Record record{};
  record.key = key;
  record.kind = Kind::State;
  record.kind_known = false;
  records_.push_back(std::move(record));
  return key;
}
std::optional<Key> Core::own(Kind kind, bool policy_allows) {
  auto key = own(policy_allows);
  if (!key) return std::nullopt;
  auto *r = find(*key);
  r->kind = kind; r->kind_known = true; r->has_state = kind == Kind::State; r->has_effect = kind == Kind::Effect;
  if (r->has_effect) {
    r->effects.resize(1);
    r->effects[0].present = true;
  }
  return key;
}
Result Core::classify(Key key, std::optional<std::size_t> state_index,
                      const std::vector<std::size_t> &effect_indices) {
  if (!applicable(key)) return Result::Stale;
  auto *r = find(key);
  if (!r) return Result::Invalid;
  if (r->terminal != Terminal::Pending || r->kind_known) return Result::Duplicate;
  r->state_index = state_index;
  r->has_state = state_index.has_value();
  r->has_effect = !effect_indices.empty();
  const auto max_index = effect_indices.empty()
                             ? 0
                             : *std::max_element(effect_indices.begin(), effect_indices.end());
  r->effects.assign(effect_indices.empty() ? 0 : max_index + 1, EffectOperation{});
  for (const auto index : effect_indices)
    r->effects[index].present = true;
  r->kind_known = state_index.has_value() != r->has_effect;
  if (r->kind_known) r->kind = r->has_effect ? Kind::Effect : Kind::State;
  return Result::Accepted;
}

Result Core::consume_state(Key key, EditResult result) {
  if (!applicable(key)) return Result::Stale;
  auto *r = find(key);
  if (!r || (!r->has_state && (!r->kind_known || r->kind != Kind::State))) return Result::Invalid;
  if (r->state_settled) return Result::Duplicate;
  if (r->terminal != Terminal::Pending && !(r->has_effect && r->terminal == Terminal::Applied)) return Result::Duplicate;
  if (prior_pending(key) || (!r->has_effect && key.sequence <= effect_fence_) || key.sequence <= consumed_state_)
    return Result::Blocked;
  switch (result) {
  case EditResult::AppliedReadback:
    consumed_state_ = key.sequence;
    r->state_settled = true;
    r->terminal = r->has_effect ? (fully_settled(*r) ? Terminal::Applied : Terminal::Pending)
                                : Terminal::Consumed;
    break;
  case EditResult::RefusedNoMutation:
    if (!r->has_effect)
      r->terminal = Terminal::FailedNoEffect;
    break;
  case EditResult::Indeterminate:
    if (r->has_effect) return Result::Invalid;
    r->terminal = Terminal::Indeterminate; r->uncertain = true; ready_ = false; break;
  }
  return Result::Accepted;
}

Result Core::propose_effect(Key key, std::uint64_t required_state, std::size_t operation_index) {
  if (!applicable(key)) return Result::Stale;
  auto *r = find(key);
  if (!r || (!r->has_effect && (!r->kind_known || r->kind != Kind::Effect)) ||
      operation_index >= r->effects.size() || !r->effects[operation_index].present)
    return Result::Invalid;
  auto &effect = r->effects[operation_index];
  if (effect.terminal != Terminal::Pending || effect.proposed) return Result::Duplicate;
  if (required_state >= key.sequence) return Result::Invalid;
  for (std::size_t i = 0; i < operation_index; ++i)
    if (r->effects[i].present && r->effects[i].terminal != Terminal::Applied)
      return Result::Blocked;
  effect.proposed = true; effect.required_state = required_state; r->proposed = true; r->required_state = required_state;
  return Result::Accepted;
}
std::optional<Ticket> Core::begin_effect(Key key, std::size_t operation_index) {
  if (!applicable(key) || !ready_) return std::nullopt;
  auto *r = find(key);
  if (!r || (!r->has_effect && (!r->kind_known || r->kind != Kind::Effect)) || prior_pending(key) ||
      ticket_serial_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  if (operation_index == SIZE_MAX) {
    operation_index = 0;
    while (operation_index < r->effects.size() &&
           (!r->effects[operation_index].present ||
            r->effects[operation_index].terminal != Terminal::Pending))
      ++operation_index;
  }
  if (operation_index >= r->effects.size() || !r->effects[operation_index].present)
    return std::nullopt;
  auto &effect = r->effects[operation_index];
  if (effect.terminal != Terminal::Pending || !effect.proposed || effect.issued || effect.uncertain || effect.required_state > consumed_state_) return std::nullopt;
  if (r->state_index && operation_index > *r->state_index && !r->state_settled)
    return std::nullopt;
  for (std::size_t i = 0; i < operation_index; ++i)
    if (r->effects[i].present && r->effects[i].terminal != Terminal::Applied)
      return std::nullopt;
  effect.issued = true; effect.ticket = Ticket{key, ++ticket_serial_, operation_index}; r->issued = true; r->ticket = effect.ticket;
  return effect.ticket;
}
Result Core::complete_effect(Ticket ticket, EditResult result) {
  if (!applicable(ticket.key)) return Result::Stale;
  auto *r = find(ticket); if (!r) return Result::Invalid;
  auto &effect = r->effects[ticket.operation_index];
  if (effect.terminal != Terminal::Pending || !effect.issued) return Result::Duplicate;
  switch (result) {
  case EditResult::AppliedReadback:
    effect.terminal = Terminal::Applied;
    effect.issued = false;
    r->issued = false;
    r->ticket = effect.ticket;
    r->terminal = fully_settled(*r) ? Terminal::Applied : Terminal::Pending;
    effect_fence_ = ticket.key.sequence;
    break;
  case EditResult::RefusedNoMutation:
    effect.issued = false; effect.ticket.reset(); r->issued = false; r->ticket.reset(); break;
  case EditResult::Indeterminate:
    effect.terminal = Terminal::Indeterminate; effect.uncertain = true; r->terminal = Terminal::Indeterminate; r->uncertain = true; ready_ = false; break;
  }
  return Result::Accepted;
}
Result Core::reject(Key key) { auto *r = find(key); if (!applicable(key)) return Result::Stale; if (!r) return Result::Invalid; if (r->terminal != Terminal::Pending) return Result::Duplicate; if (r->issued) return Result::Blocked; for (auto &effect : r->effects) if (effect.present && !effect.issued) effect.terminal = Terminal::FailedNoEffect; r->terminal = Terminal::FailedNoEffect; return Result::Accepted; }
Result Core::cancel(Key key) { auto *r = find(key); if (!applicable(key)) return Result::Stale; if (!r) return Result::Invalid; if (r->terminal != Terminal::Pending) return Result::Duplicate; if (r->issued) return Result::Blocked; for (auto &effect : r->effects) if (effect.present && !effect.issued) effect.terminal = Terminal::Cancelled; r->terminal = Terminal::Cancelled; return Result::Accepted; }
Result Core::reconcile(Ticket ticket, Readback result) { auto *r = find(ticket); if (!r) return Result::Invalid; auto &effect = r->effects[ticket.operation_index]; if (!effect.uncertain) return Result::Invalid; effect.uncertain = false; effect.terminal = result == Readback::Applied ? Terminal::Applied : Terminal::FailedNoEffect; r->uncertain = false; r->terminal = fully_settled(*r) ? (r->has_effect ? Terminal::Applied : Terminal::Consumed) : (terminally_settled(*r) ? Terminal::FailedNoEffect : Terminal::Pending); return Result::Accepted; }
Result Core::reconcile_state(Key key, Readback result) { auto *r = find(key); if (!r || (!r->has_state && (!r->kind_known || r->kind != Kind::State)) || !r->uncertain) return Result::Invalid; r->uncertain = false; if (result == Readback::Applied && applicable(key)) { r->state_settled = true; consumed_state_ = std::max(consumed_state_, key.sequence); r->terminal = r->has_effect ? (fully_settled(*r) ? Terminal::Applied : Terminal::Pending) : Terminal::Consumed; } else { r->terminal = r->has_effect ? Terminal::Pending : Terminal::FailedNoEffect; } return Result::Accepted; }

void Core::fence() { for (auto &r : records_) { if (r.terminal != Terminal::Pending) continue; for (auto &effect : r.effects) if (effect.terminal == Terminal::Pending && effect.issued) { effect.terminal = Terminal::Indeterminate; effect.uncertain = true; } r.terminal = r.issued ? Terminal::Indeterminate : Terminal::FailedNoEffect; r.uncertain = r.issued; } ready_ = false; }
void Core::disconnect() { fence(); }
Result Core::restart(std::uint64_t epoch) { if (epoch <= binding_.epoch) return Result::Invalid; fence(); binding_.epoch = epoch; consumed_state_ = 0; effect_fence_ = 0; return Result::Accepted; }
Result Core::rebind() { if (!alive_ || std::any_of(records_.begin(), records_.end(), [](const Record &r) { return r.uncertain; })) return Result::Blocked; ready_ = true; return Result::Accepted; }
void Core::destroy_context() { fence(); alive_ = false; }
Result Core::replace_context(Binding binding) { if (binding.context == 0 || binding.lifetime <= binding_.lifetime || binding.epoch < binding_.epoch) return Result::Invalid; fence(); binding_ = binding; consumed_state_ = 0; effect_fence_ = 0; alive_ = true; return Result::Accepted; }
Pass Core::probe_pass() const { for (const auto &r : records_) if (r.key.binding.context == binding_.context && r.key.binding.lifetime == binding_.lifetime && r.uncertain) return Pass::RecoveryRequired; for (const auto &r : records_) if (r.key.binding == binding_ && (r.terminal == Terminal::Pending || (r.has_state && !r.state_settled))) return Pass::DrainRequired; return Pass::Clear; }
std::optional<Key> Core::record_cancelled_recovery() {
  if (!alive_ || records_.size() >= capacity_ || sequence_ == std::numeric_limits<std::uint64_t>::max())
    return std::nullopt;
  Key key{binding_, ++sequence_};
  Record record{};
  record.key = key;
  record.kind = Kind::Effect;
  record.has_effect = true;
  record.effects.resize(1);
  record.effects[0].present = true;
  record.terminal = Terminal::Cancelled;
  records_.push_back(std::move(record));
  return key;
}
Result Core::acknowledge(Key key) { auto *r = find(key); if (!r) return Result::Invalid; if (r->terminal == Terminal::Pending || r->uncertain || (r->has_state && !r->state_settled)) return Result::Blocked; records_.erase(std::remove_if(records_.begin(), records_.end(), [key](const Record &record) { return record.key == key; }), records_.end()); return Result::Accepted; }

} // namespace fusion
