#include "tsf/semantic.hpp"

#include <algorithm>
#include <chrono>

#include <utility>

namespace fusion::tsf {

namespace {

// UNFROZEN engineering constant: upper bound of one bounded reconnect attempt
// (transport connect + one Hello round trip) on the key/context path. A missing
// Host fails the transport open immediately, so this bound is only reached when
// the pipe exists but cannot be served.
constexpr auto kLinkTimeout = std::chrono::milliseconds(200);

// A backend that reports no incarnation has no transport to fence: the Stage 4A
// single-bind lifecycle applies unchanged.
bool incarnation_tracked(const LinkStatus &link) {
  return link.host_incarnation != 0 || link.connection_generation != 0;
}

} // namespace

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
  if (backend_ready_) {
    const LinkStatus link = backend.link_status();
    bound_incarnation_ = link.host_incarnation;
    bound_connection_ = link.connection_generation;
  }
}

LinkStatus SemanticController::link_status() const {
  if (!real_backend_)
    return LinkStatus{LinkState::Established, 0, 0};
  return real_backend_->link_status();
}

bool SemanticController::semantic_ready() const {
  if (!real_backend_)
    return false;
  const LinkStatus link = real_backend_->link_status();
  if (link.state != LinkState::Established)
    return false;
  if (!incarnation_tracked(link))
    return backend_ready_; // in-process backend: Stage 4A single-bind readiness
  return backend_ready_ && bound_incarnation_ == link.host_incarnation &&
         bound_connection_ == link.connection_generation;
}

void SemanticController::fence_previous_incarnation() {
  // Host-provided semantic projection from the previous incarnation must never
  // become current STATE for the new one (D072 §23 "zero stale resurrection").
  buffer_.clear();
  state_dependency_ = 0;
  // A deferred reset intent belonged to the connection that is gone.
  backend_reset_pending_ = false;
  // The previous incarnation's candidate page is not the new one's projection;
  // it must not be displayed or acted on after the link moved.
  clear_displayed_page();
  // Deliberately NOT cleared: Core records, uncertainty and the pending
  // materialization queue. An unresolved/indeterminate EFFECT stays owned and
  // auditable (P0-A/P0-B), and a callback that was received but never issued to
  // the document is still owed to the document authority. Neither is replayed
  // against the new incarnation.
}

void SemanticController::clear_displayed_page() {
  displayed_page_.reset();
  displayed_fence_ = PageFence{};
  page_present_ = false;
}

bool SemanticController::ensure_semantic_ready() {
  // Bounded reconnect. Failure is fail-open before ownership, never a semantic
  // verdict and never a replay of earlier work.
  if (!real_backend_->ensure_link(kLinkTimeout))
    return false;

  const LinkStatus link = real_backend_->link_status();
  if (link.state != LinkState::Established)
    return false;
  if (!incarnation_tracked(link))
    return backend_ready_; // in-process backend: no incarnation to fence

  const bool moved = bound_incarnation_ != link.host_incarnation ||
                     bound_connection_ != link.connection_generation;
  if (moved) {
    if (bound_incarnation_ != 0 || bound_connection_ != 0)
      fence_previous_incarnation();
    // A binding belongs to one incarnation and connection; it is never carried
    // across either.
    backend_ready_ = false;
  }
  if (!backend_ready_) {
    // CONTEXT REBIND (D072 §13): readiness for this context requires an
    // established binding on the current incarnation/connection.
    backend_ready_ = real_backend_->bind(core_.binding());
    if (backend_ready_) {
      bound_incarnation_ = link.host_incarnation;
      bound_connection_ = link.connection_generation;
    } else {
      bound_incarnation_ = 0;
      bound_connection_ = 0;
    }
  }
  return backend_ready_;
}

bool SemanticController::routes_key(const KeyAction &action) const {
  if (real_backend_)
    return semantic_ready() && real_backend_->healthy() && !action.modified &&
           (action.keysym != 0 || action.type != KeyAction::Type::Pass ||
            action.virtual_key == kVkReturn);
  return action.type == KeyAction::Type::AppendChar ||
         (action.type != KeyAction::Type::Pass && !buffer_.empty());
}

SemanticDecision SemanticController::begin_real_key(const KeyAction &action, bool policy_allows) {
  SemanticDecision decision;
  if (!policy_allows) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  // Reconnect + incarnation fencing + rebind happen before any ownership
  // admission, so a failed reconnect can only fail open.
  if (!ensure_semantic_ready()) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  if (backend_reset_pending_) {
    backend_ready_ = real_backend_->reset(core_.binding());
    backend_reset_pending_ = false;
  }
  if (!backend_ready_ || !real_backend_->healthy()) {
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
  decision.symbolic = "fcitx:" + std::to_string(stimulus.keysym);
  dispatch_real(stimulus, decision);
  return decision;
}

SemanticDecision SemanticController::begin_candidate_action(CandidateAction action,
                                                            std::int32_t index) {
  SemanticDecision decision;
  // Candidate actions exist only on the real frontend: the Mock Backend has no
  // candidate list, so there is nothing to project and nothing to select.
  if (!real_backend_) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  if (!ensure_semantic_ready()) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  if (backend_reset_pending_) {
    backend_ready_ = real_backend_->reset(core_.binding());
    backend_reset_pending_ = false;
  }
  if (!backend_ready_ || !real_backend_->healthy()) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  // A candidate action is meaningful only against a currently displayed page. The
  // fence guard rejects a page whose dispatch identity no longer matches the live
  // session/binding/incarnation/connection, so a stale page is never acted on.
  if (!page_present_) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  const LinkStatus link = real_backend_->link_status();
  if (displayed_fence_.backend_session_id != real_backend_->session_id() ||
      displayed_fence_.binding != core_.binding() ||
      displayed_fence_.host_incarnation != link.host_incarnation ||
      displayed_fence_.connection_generation != link.connection_generation) {
    clear_displayed_page();
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  // The engine re-checks bounds against its current list; this rejects a hit
  // outside the page the user was actually shown before the stimulus is sent.
  if (action == CandidateAction::Select &&
      (index < 0 || static_cast<std::size_t>(index) >= displayed_page_->entries.size())) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  retire_terminals();
  constexpr std::size_t callback_bound = frontend_operation_bound;
  if (core_.records().size() + callback_bound + recovery_reserve_ > core_.capacity() ||
      core_.probe_pass() == Pass::RecoveryRequired) {
    decision.route = SemanticDecision::Route::FailOpen;
    return decision;
  }
  BackendStimulus stimulus{core_.binding(), next_stimulus_++};
  switch (action) {
  case CandidateAction::Select:
    stimulus.type = BackendStimulus::Type::SelectCandidate;
    stimulus.candidate_index = index;
    break;
  case CandidateAction::PageNext:
    stimulus.type = BackendStimulus::Type::PageNext;
    break;
  case CandidateAction::PagePrev:
    stimulus.type = BackendStimulus::Type::PagePrev;
    break;
  }
  // A non-key stimulus carries no key identity: keysym and modifiers stay zero.
  decision.symbolic = "candidate-action";
  dispatch_real(stimulus, decision);
  return decision;
}

void SemanticController::dispatch_real(const BackendStimulus &stimulus,
                                       SemanticDecision &decision) {
  const auto batch = real_backend_->send(stimulus);
  decision.backend_session_id = batch.backend_session_id;
  decision.stimulus_id = batch.stimulus_id;
  decision.operation_count = batch.operations.size();
  bool valid = batch.backend_session_id == real_backend_->session_id() &&
               batch.binding == stimulus.binding && batch.stimulus_id == stimulus.stimulus_id &&
               batch.disposition != BackendBatch::Disposition::Reject &&
               batch.operations.size() <= frontend_operation_bound;
  bool original_forward = false;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CandidateSnapshot)
      continue; // presentation projection, not a document callback
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
    clear_displayed_page();
    return;
  }
  decision.forward = original_forward || batch.disposition == BackendBatch::Disposition::Forward;
  decision.eaten = !decision.forward;
  std::optional<CandidatePage> captured;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CandidateSnapshot) {
      captured = operation.page; // the page as of this dispatch; last snapshot wins
      continue;
    }
    if (operation.type == BackendOperation::Type::ForwardKey)
      continue; // the natural key is answered by the route, not a record
    const bool effect = operation.type == BackendOperation::Type::CommitText;
    auto key = core_.own(effect ? Kind::Effect : Kind::State, true);
    if (!key) { // reserved capacity and readiness were checked before dispatch
      decision.route = SemanticDecision::Route::CancelledRecovery;
      decision.eaten = true;
      for (const auto &work : decision.frontend)
        (void)host_.cancel(work.key);
      decision.frontend.clear();
      backend_ready_ = real_backend_->reset(core_.binding());
      clear_displayed_page();
      return;
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
  // Update the candidate projection only after the batch passed validation and
  // its records were admitted: a rejected dispatch must not resurrect a page. An
  // empty page is the hide signal — present in the decision, absent in the fence.
  if (captured) {
    decision.page = captured;
    const LinkStatus link = real_backend_->link_status();
    page_present_ = !captured->empty();
    displayed_page_ = page_present_ ? captured : std::nullopt;
    displayed_fence_ = PageFence{batch.backend_session_id, stimulus.binding, batch.stimulus_id,
                                 link.host_incarnation, link.connection_generation};
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
    return;
  }
  const auto &first = decision.frontend.front();
  decision.key = first.key;
  decision.text = first.operation.text;
  decision.route = first.operation.type == BackendOperation::Type::CommitText
                       ? SemanticDecision::Route::Effect
                       : SemanticDecision::Route::State;
  decision.intent =
      decision.route == SemanticDecision::Route::Effect ? Intent::Commit : Intent::Preedit;
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
  // External termination removed the composition the candidates belonged to; the
  // projected page is no longer current and must not be shown or acted on.
  clear_displayed_page();
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
      // The new binding belongs to the current link identity.
      const LinkStatus link = real_backend_->link_status();
      bound_incarnation_ = backend_ready_ ? link.host_incarnation : 0;
      bound_connection_ = backend_ready_ ? link.connection_generation : 0;
    }
    lifetime_ = next_lifetime;
    id_ = context_id;
    state_dependency_ = 0;
    buffer_.clear();
    clear_pending();
    clear_displayed_page();
  }
  return result;
}

void SemanticController::destroy() {
  if (real_backend_) {
    real_backend_->unbind(core_.binding());
    backend_ready_ = false;
    bound_incarnation_ = 0;
    bound_connection_ = 0;
  }
  core_.destroy_context();
  buffer_.clear();
  clear_pending();
  clear_displayed_page();
}

} // namespace fusion::tsf
