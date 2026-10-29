#pragma once

// R51: the wake-up half of "no context leaves with a reset owed".
//
// `SemanticController::note_composition_terminated()` arms a Host-context reset that only a
// later key used to flush, and the arming happens inside a TSF callback that carries an edit
// cookie (ITfCompositionSink::OnCompositionTerminated, and the cleanup session's own end of a
// composition). The reset is a blocking named-pipe call, so it cannot run there; it has to run
// on the same thread after the session has returned. This header holds only the *bookkeeping*
// for that deferral — who is owed a wake-up, and whether a wake-up message is already in
// flight. It knows nothing about windows, COM or the backend, which is what makes it testable.
//
// The queue carries no authority. The truth is the controller's own single
// `backend_reset_pending_` flag: a wake-up that arrives late, twice, or for a context whose
// reset another path already ran costs nothing, because `flush_backend_reset` answers the flag
// and clears it. That is what makes every path here idempotent.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace fusion::tsf {

// Which boundary owed the Host a reset. One name per `BACKEND_RESET reason=` line, so a log can
// say which door ran it; `Key` is the ordinary path, which already has its own lines.
enum class ResetSource {
  Key,        // a key or a candidate action: flush inside the dispatch, no extra line
  Terminated, // the composition ended while an edit session owned the thread
  FocusOut,   // this context's document manager lost focus
  Pop,        // TSF popped the context, or its entry is being torn down
  Deactivate, // this thread's TIP activation ended
};

constexpr const char *reset_source_name(ResetSource source) noexcept {
  switch (source) {
  case ResetSource::Key:
    return "key";
  case ResetSource::Terminated:
    return "terminated";
  case ResetSource::FocusOut:
    return "focus-out";
  case ResetSource::Pop:
    return "pop";
  case ResetSource::Deactivate:
    return "deactivate";
  }
  return "unknown";
}

// A reset armed by the key path is not deferred work: the key path is the safe place to run it.
constexpr bool reset_is_deferred(ResetSource source) noexcept { return source != ResetSource::Key; }

// Intents waiting for one wake-up. `Item` is the held identity of one context — in the product a
// `std::shared_ptr<ContextEntry>`, which keeps the entry's memory alive until the drain runs and
// compares by the entry it points at.
template <class Item> class DeferredResetQueue {
public:
  // One context, at the generation its entry had when the intent was armed. A teardown bumps the
  // generation, so an intent outlives neither the entry it names nor the incarnation of it.
  struct Intent {
    Item item;
    std::uint64_t generation = 0;
    ResetSource source = ResetSource::Terminated;
  };

  // Bound on one batch. Coalescing by item+generation already keeps this at one intent per live
  // context; the bound exists so a thread that never pumps cannot grow it without limit, and a
  // refusal is reported rather than quiet (`saturated`).
  static constexpr std::size_t kCapacity = 32;

  // Record one intent. Returns true only when the caller must post the wake-up message: the
  // batch already in flight covers every intent added before it runs, and a repeat for the same
  // item+generation is coalesced into the intent already queued (which keeps the boundary that
  // armed it first). A queue that holds intents but has no wake-up outstanding always asks for
  // one, whether the intent is new or already there — otherwise a batch left by a failed post or
  // by a busy drain would never be delivered again.
  bool request(Item item, std::uint64_t generation, ResetSource source) {
    for (const Intent &intent : intents_)
      if (intent.item == item && intent.generation == generation)
        return claim_wakeup();
    if (intents_.size() >= kCapacity) {
      ++saturated_;
      return false;
    }
    intents_.push_back(Intent{std::move(item), generation, source});
    return claim_wakeup();
  }

  // The whole batch, with the in-flight mark released: from here the drain owns the intents. An
  // empty batch is a legal no-op, so a duplicated or spurious message changes nothing.
  std::vector<Intent> take_all() {
    in_flight_ = false;
    return std::exchange(intents_, {});
  }

  // A wake-up that could not be posted, and a drain that found the thread busy, both put the
  // intents back so the next request re-posts them; the boundaries that flush directly do not
  // need the queue at all. Returns true when the caller must post — which is the case for a
  // batch put back here, because taking it is what released the in-flight mark.
  bool requeue(std::vector<Intent> intents) {
    bool post = false;
    for (Intent &intent : intents)
      post = request(std::move(intent.item), intent.generation, intent.source) || post;
    return post;
  }

  // The in-flight mark without the batch: the message failed to post, and the intents stay.
  void cancel_in_flight() { in_flight_ = false; }

  std::size_t size() const { return intents_.size(); }
  bool in_flight() const { return in_flight_; }
  std::size_t saturated() const { return saturated_; }

private:
  bool claim_wakeup() {
    if (in_flight_)
      return false; // the batch already on its way covers these intents
    in_flight_ = true;
    return true;
  }

  std::vector<Intent> intents_;
  bool in_flight_ = false;
  std::size_t saturated_ = 0;
};

} // namespace fusion::tsf
