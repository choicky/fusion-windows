#include "tsf/semantic.hpp"

#include <algorithm>

#include <utility>

namespace fusion::tsf {

SemanticController::SemanticController(std::uint64_t context_id, std::size_t capacity,
                                       fusion::test::MockBackend &backend,
                                       std::size_t recovery_reserve)
    : recovery_reserve_(recovery_reserve), id_(context_id), lifetime_(1),
      core_(Binding{context_id, 1, 1}, capacity), host_(core_), backend_(&backend) {}

SemanticDecision SemanticController::begin_key(const KeyAction &action, bool policy_allows) {
  if (real_backend_)
    return begin_real_key(action, policy_allows);
  SemanticDecision decision;
  // Raw-buffer projection first; the Mock Backend consumes the symbolic input.
  // Buffer edits are reverted if ownership admission fails, so a fail-open key
  // leaves the projection untouched.
  std::string symbolic;
  Kind kind = Kind::State;
  const std::string original_buffer = buffer_;
  bool buffer_changed = false;
  switch (action.type) {
  case KeyAction::Type::AppendChar:
    if (buffer_.size() >= 256) { // fixture payload bound, not production policy
      decision.route = SemanticDecision::Route::FailOpen;
      decision.symbolic = "payload-bound";
      return decision;
    }
    buffer_ += action.ch;
    buffer_changed = true;
    symbolic = "preedit:" + buffer_;
    kind = Kind::State;
    break;
  case KeyAction::Type::Backspace:
    if (buffer_.empty()) {
      symbolic = "backspace"; // not in the Stage 2 vocabulary: genuine PASS
    } else {
      buffer_.pop_back();
      buffer_changed = true;
      symbolic = "preedit:" + buffer_;
    }
    break;
  case KeyAction::Type::Commit:
    if (buffer_.empty()) {
      symbolic = "space"; // not in the Stage 2 vocabulary: genuine PASS
    } else {
      symbolic = "commit:" + buffer_;
      kind = Kind::Effect;
    }
    break;
  case KeyAction::Type::Reject:
    if (buffer_.empty()) {
      symbolic = "escape"; // not in the Stage 2 vocabulary: genuine PASS
    } else {
      symbolic = "reject:" + buffer_;
      kind = Kind::Effect; // rejection settles an owned effect key
    }
    break;
  case KeyAction::Type::Pass:
    symbolic = "unmapped";
    break;
  }

  decision.symbolic = symbolic;
  const bool unowned =
      action.type == KeyAction::Type::Pass ||
      ((action.type == KeyAction::Type::Backspace || action.type == KeyAction::Type::Commit ||
        action.type == KeyAction::Type::Reject) &&
       original_buffer.empty());
  if (unowned) {
    decision.intent = backend_->respond(symbolic).intent;
    decision.route = SemanticDecision::Route::Pass;
    decision.eaten = false;
    return decision;
  }

  // Windows fixture classification selects ownership before backend intent.
  // Before this point the key can still
  // fail open (D072 §4: fail-open is allowed before ownership).
  const bool capacity_allows = core_.records().size() + recovery_reserve_ < core_.capacity();
  std::optional<Key> owned = core_.own(kind, policy_allows && capacity_allows);
  if (!owned) {
    if (buffer_changed) {
      // Revert the projection so it matches the un-owned reality: the symbolic
      // input still carries the pre-edit buffer from before this key.
      buffer_ = original_buffer;
    }
    decision.route = SemanticDecision::Route::FailOpen;
    decision.eaten = false;
    return decision;
  }
  decision.key = owned;
  decision.eaten = true;
  const fusion::test::Response response = backend_->respond(symbolic);
  decision.intent = response.intent;
  decision.text = response.text;

  switch (response.intent) {
  case Intent::Preedit:
    state_dependency_ = owned->sequence;
    decision.route = SemanticDecision::Route::State;
    break;
  case Intent::Commit:
    decision.route = SemanticDecision::Route::Effect;
    break;
  case Intent::Reject:
    // Failure after ownership: explicit no-effect terminal, never PASS.
    decision.route = SemanticDecision::Route::Reject;
    (void)host_.reject(*owned);
    buffer_.clear();
    break;
  case Intent::Pass:
    decision.route = SemanticDecision::Route::Reject;
    (void)host_.reject(*owned);
    buffer_ = original_buffer;
    break; // unexpected PASS after ownership is a failure terminal
  }
  return decision;
}

SemanticController::SemanticController(std::uint64_t context_id, std::size_t capacity,
                                       Backend &backend, std::size_t recovery_reserve)
    : recovery_reserve_(recovery_reserve), id_(context_id), lifetime_(1),
      core_(Binding{context_id, 1, 1}, capacity), host_(core_), real_backend_(&backend) {
  backend_ready_ = backend.bind(core_.binding());
}

bool SemanticController::routes_key(const KeyAction &action) const {
  if (real_backend_)
    return backend_ready_ && real_backend_->healthy() && !action.modified &&
           (action.keysym != 0 || action.type != KeyAction::Type::Pass ||
            action.virtual_key == kVkReturn);
  return action.type == KeyAction::Type::AppendChar ||
         (action.type != KeyAction::Type::Pass && !buffer_.empty());
}

SemanticDecision SemanticController::begin_real_key(const KeyAction &action, bool policy_allows) {
  if (backend_reset_pending_) {
    backend_ready_ = real_backend_->reset(core_.binding());
    backend_reset_pending_ = false;
  }
  SemanticDecision decision;
  if (!policy_allows || !backend_ready_ || !real_backend_->healthy()) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  retire_terminals();
  if (!routes_key(action)) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  BackendStimulus stimulus{core_.binding(), next_stimulus_++};
  switch (action.type) {
  case KeyAction::Type::AppendChar:
    stimulus.type = BackendStimulus::Type::Character;
    stimulus.character = action.ch;
    break;
  case KeyAction::Type::Commit:
    stimulus.type = BackendStimulus::Type::Space;
    break;
  case KeyAction::Type::Backspace:
    stimulus.type = BackendStimulus::Type::Backspace;
    break;
  case KeyAction::Type::Reject:
    stimulus.type = BackendStimulus::Type::Escape;
    break;
  case KeyAction::Type::Pass:
    if (action.modified || (!action.keysym && action.virtual_key != kVkReturn))
      return decision;
    stimulus.type = BackendStimulus::Type::Return;
    break;
  }
  stimulus.keysym = action.keysym;
  stimulus.modifiers = action.modifiers;
  // Reserve room for the bounded frontend callback list before invoking Fcitx.
  // This is resource admission, not a prediction of semantic consumption.
  constexpr std::size_t callback_bound = frontend_operation_bound;
  if (core_.records().size() + callback_bound + recovery_reserve_ > core_.capacity() ||
      core_.probe_pass() == Pass::RecoveryRequired) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  const auto batch = real_backend_->send(stimulus);
  decision.symbolic = "fcitx:" + std::to_string(stimulus.keysym);
  decision.backend_session_id = batch.backend_session_id;
  decision.stimulus_id = batch.stimulus_id;
  decision.operation_count = batch.operations.size();
  bool valid = batch.backend_session_id == real_backend_->session_id() &&
               batch.binding == stimulus.binding && batch.stimulus_id == stimulus.stimulus_id &&
               batch.disposition != BackendBatch::Disposition::Reject &&
               batch.operations.size() <= callback_bound;
  bool original_forward = false;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CandidateSnapshot)
      continue;
    if (original_forward)
      valid = false; // no callback may follow the natural key
    if (operation.type == BackendOperation::Type::ForwardKey) {
      valid = valid && operation.original_key;
      original_forward = true;
    }
  }
  if (!valid) {
    decision.key = core_.record_cancelled_recovery();
    decision.route = SemanticDecision::Route::CancelledRecovery;
    decision.eaten = true;
    decision.intent = Intent::Reject;
    backend_ready_ = real_backend_->reset(core_.binding());
    buffer_.clear();
    state_dependency_ = 0;
    return decision;
  }
  decision.forward = original_forward || batch.disposition == BackendBatch::Disposition::Forward;
  decision.eaten = !decision.forward;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CandidateSnapshot ||
        operation.type == BackendOperation::Type::ForwardKey)
      continue; // retained by BackendBatch; Stage 3B has no candidate renderer
    const bool effect = operation.type == BackendOperation::Type::CommitText;
    auto key = core_.own(effect ? Kind::Effect : Kind::State, true);
    if (!key) { // reserved capacity and readiness were checked before dispatch
      decision.route = SemanticDecision::Route::CancelledRecovery;
      decision.eaten = true;
      for (const auto &work : decision.frontend)
        (void)host_.cancel(work.key);
      decision.frontend.clear();
      backend_ready_ = real_backend_->reset(core_.binding());
      return decision;
    }
    decision.frontend.push_back({operation, *key});
    if (effect) {
      // Capture the actual preceding STATE dependency, including callbacks in
      // this dispatch. The existing Core gate delays execution until consumed.
      (void)core_.propose_effect(*key, state_dependency_);
    } else {
      state_dependency_ = key->sequence;
      buffer_ = operation.text;
    }
  }
  if (decision.frontend.empty()) {
    if (!decision.forward) {
      // Consumed without a document callback still receives an explicit terminal.
      auto key = core_.own(Kind::State, true);
      if (key)
        (void)host_.reject(*key); // explicit no-document-effect terminal
    }
    decision.route =
        decision.forward ? SemanticDecision::Route::Pass : SemanticDecision::Route::State;
    return decision;
  }
  const auto &first = decision.frontend.front();
  decision.key = first.key;
  decision.text = first.operation.text;
  decision.route = first.operation.type == BackendOperation::Type::CommitText
                       ? SemanticDecision::Route::Effect
                       : SemanticDecision::Route::State;
  decision.intent =
      decision.route == SemanticDecision::Route::Effect ? Intent::Commit : Intent::Preedit;
  return decision;
}

Result SemanticController::propose_effect(Key key) {
  if (real_backend_) {
    for (const auto &record : core_.records())
      if (record.key == key && record.kind == Kind::Effect && record.proposed &&
          record.terminal == Terminal::Pending)
        return Result::Accepted; // already proposed with its preceding STATE dependency
    return Result::Invalid;
  }
  if (core_.consumed_state() < state_dependency_)
    return Result::Blocked;
  return host_.commit(key);
}

std::optional<Ticket> SemanticController::open_effect(Key key) { return host_.open(key); }

Result SemanticController::settle_effect(Ticket ticket, EditResult observed) {
  Result result = host_.settle(ticket, observed);
  if (result != Result::Accepted) {
    return result;
  }
  // Drop the fallback-queue entry once the effect reaches a real terminal
  // (Applied / Indeterminate). A known no-mutation refusal keeps it queued:
  // Core released the ticket and the effect stays unresolved.
  const Record *record = nullptr;
  for (const auto &r : core_.records()) {
    if (r.key == ticket.key) {
      record = &r;
      break;
    }
  }
  if (record && record->terminal != Terminal::Pending) {
    pending_.erase(
        std::remove_if(pending_.begin(), pending_.end(),
                       [key = ticket.key](const PendingWork &work) { return work.key == key; }),
        pending_.end());
  }
  if (!real_backend_ && record && record->terminal == Terminal::Applied) {
    note_composition_reset();
  }
  return result;
}

Result SemanticController::settle_state(Key key, EditResult observed) {
  return host_.preedit(key, observed);
}

Pass SemanticController::pass_probe() const { return host_.pass_barrier(); }

std::optional<PendingWork> SemanticController::drain_work() {
  for (const auto &work : pending_) {
    if (real_backend_ || work.kind == Kind::Effect) {
      return work;
    }
  }
  return std::nullopt;
}

bool SemanticController::finish_pass() {
  // The caller drained (or the probe was Clear): verify honestly before pass.
  return host_.pass_barrier() == Pass::Clear;
}

Key SemanticController::cancel_pass_recovery() {
  // §7.3: the current key gets an explicit CANCELLED terminal; it is eaten and
  // never reported as genuine PASS. The shell reserves capacity for it, even
  // while uncertainty correctly disables normal semantic admission.
  auto owned = core_.record_cancelled_recovery();
  if (!owned) {
    return Key{};
  }
  Key key = *owned;
  return key;
}

void SemanticController::enqueue_state(Key key, std::string text) {
  const bool live =
      std::any_of(core_.records().begin(), core_.records().end(), [&](const Record &record) {
        return record.key == key && record.key.binding == core_.binding() &&
               record.terminal == Terminal::Pending && !record.uncertain;
      });
  if (!live)
    return;
  if (std::any_of(pending_.begin(), pending_.end(),
                  [&](const PendingWork &w) { return w.key == key; }))
    return;
  pending_.push_back(PendingWork{Kind::State, key, std::move(text)});
}

void SemanticController::enqueue_effect(Key key, std::string text) {
  const bool live =
      std::any_of(core_.records().begin(), core_.records().end(), [&](const Record &record) {
        return record.key == key && record.key.binding == core_.binding() &&
               record.terminal == Terminal::Pending && !record.uncertain;
      });
  if (!live)
    return;
  if (std::any_of(pending_.begin(), pending_.end(),
                  [&](const PendingWork &w) { return w.key == key; }))
    return;
  pending_.push_back(PendingWork{Kind::Effect, key, std::move(text)});
}

void SemanticController::settle_queued_states(EditResult observed) {
  // FIFO, stopping at the first state that cannot settle yet: a blocked state
  // means an earlier record is still pending/uncertain, and order must hold.
  while (!pending_.empty() && pending_.front().kind == Kind::State) {
    const Key key = pending_.front().key;
    const Result result = settle_state(key, observed);
    if (result == Result::Accepted) {
      pending_.erase(pending_.begin());
    } else if (result == Result::Blocked) {
      return;
    } else {
      // Stale (context fenced — record already settled by Core) or Duplicate
      // (already settled): the queue entry no longer represents live work.
      pending_.erase(pending_.begin());
    }
  }
}

void SemanticController::settle_front_state(Key key, EditResult observed) {
  if (observed == EditResult::RefusedNoMutation)
    return;
  if (settle_state(key, observed) == Result::Accepted)
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const PendingWork &work) { return work.key == key; }),
                   pending_.end());
}

void SemanticController::clear_pending() { pending_.clear(); }

bool SemanticController::state_applicable(Key key) const {
  if (key.binding != core_.binding() || key.sequence <= core_.consumed_state())
    return false;
  bool found = false;
  for (const auto &record : core_.records()) {
    if (record.key == key) {
      found =
          record.kind == Kind::State && record.terminal == Terminal::Pending && !record.uncertain;
    }
    if (record.key.binding == key.binding && record.key.sequence < key.sequence &&
        (record.terminal == Terminal::Pending || record.uncertain))
      return false;
  }
  return found;
}

void SemanticController::retire_terminals() {
  std::vector<Key> retired;
  for (const auto &record : core_.records()) {
    const bool queued = std::any_of(pending_.begin(), pending_.end(), [&](const PendingWork &work) {
      return work.key == record.key;
    });
    if (record.terminal != Terminal::Pending && !record.uncertain && !queued) {
      retired.push_back(record.key);
    }
  }
  for (const Key &key : retired) {
    (void)core_.acknowledge(key);
  }
}

void SemanticController::note_composition_reset() {
  buffer_.clear();
  state_dependency_ = 0;
}

void SemanticController::note_composition_terminated() {
  if (real_backend_)
    backend_reset_pending_ = true; // defer Host work until outside EditSession
  buffer_.clear();
  state_dependency_ = 0;
  // External termination is an authority event: queued work reaches explicit
  // terminals (cancel), never a silent drop.
  std::vector<PendingWork> queued = std::move(pending_);
  pending_.clear();
  for (const auto &work : queued) {
    (void)host_.cancel(work.key);
  }
  for (const auto &record : core_.records()) {
    if (record.terminal == Terminal::Pending) {
      (void)host_.cancel(record.key);
    }
  }
}

Result SemanticController::replace_context(std::uint64_t context_id) {
  const Binding previous = core_.binding();
  const std::uint64_t next_lifetime = lifetime_ + 1;
  Result result = core_.replace_context(Binding{context_id, next_lifetime, 1});
  if (result == Result::Accepted) {
    // Replacement fences readiness; a fresh binding must rebind explicitly.
    (void)core_.rebind();
    if (real_backend_) {
      real_backend_->unbind(previous);
      backend_ready_ = real_backend_->bind(core_.binding());
    }
    lifetime_ = next_lifetime;
    id_ = context_id;
    state_dependency_ = 0;
    buffer_.clear();
    clear_pending();
  }
  return result;
}

void SemanticController::destroy() {
  if (real_backend_) {
    real_backend_->unbind(core_.binding());
    backend_ready_ = false;
  }
  core_.destroy_context();
  buffer_.clear();
  clear_pending();
}

} // namespace fusion::tsf
