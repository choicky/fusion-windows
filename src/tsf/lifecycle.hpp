#pragma once

// Stage 4C: what one real TSF lifecycle event obliges the Windows shell to do
// with a Fusion context, plus the teardown accounting that keeps unresolved
// responsibility auditable.
//
// D072 §9 fixes only part of this: focus is not a semantic incarnation, focus
// loss alone is neither an epoch advance nor a composition termination, and an
// external composition termination is an authority event that must reconcile
// projection/pending/semantic binding. §9/§18 leave push/pop and transitory
// context semantics UNFROZEN until real execution provides evidence, so this
// table states nothing beyond the accepted boundary and is deliberately the
// single place where the lifecycle obligation lives — shared by the real sinks
// and their regression test, like `continue_current_key` on the key path.

#include "fusion/core.hpp"

#include <cstddef>
#include <vector>

namespace fusion::tsf {

// The real TSF lifecycle events this shell observes.
enum class LifecycleEvent {
  Activate,              // ITfTextInputProcessor::Activate
  Deactivate,            // ITfTextInputProcessor::Deactivate
  FocusChanged,          // ITfThreadMgrEventSink::OnSetFocus(dimFocus, dimPrevFocus)
  ThreadForeground,      // ITfKeyEventSink::OnSetFocus(fForeground)
  ContextPushed,         // ITfThreadMgrEventSink::OnPushContext
  ContextPopped,         // ITfThreadMgrEventSink::OnPopContext
  DocumentCreated,       // ITfThreadMgrEventSink::OnInitDocumentMgr
  DocumentDestroyed,     // ITfThreadMgrEventSink::OnUninitDocumentMgr
  CompositionTerminated, // ITfCompositionSink::OnCompositionTerminated (external)
};

// What an event obliges the shell to do with a Fusion context.
//   end_context          destroy the Fusion context: unbind the Host binding,
//                        fence the ledger to explicit terminals, audit, drop identity.
//   reconcile_projection discard the document projection and settle queued/pending
//                        work explicitly, but keep the context identity and binding.
struct LifecycleObligation {
  bool end_context = false;
  bool reconcile_projection = false;
  bool operator==(const LifecycleObligation &) const = default;
};

inline LifecycleObligation lifecycle_obligation(LifecycleEvent event) {
  switch (event) {
  case LifecycleEvent::Deactivate:
  case LifecycleEvent::ContextPopped:
  case LifecycleEvent::DocumentDestroyed:
    return {true, false};
  case LifecycleEvent::CompositionTerminated:
    // External authority event (D072 §9): the document's composition is gone, so
    // the projection and any queued/pending work reach explicit terminals, while
    // the Fusion context identity and its Host binding stay current.
    return {false, true};
  case LifecycleEvent::Activate:
  case LifecycleEvent::FocusChanged:
  case LifecycleEvent::ThreadForeground:
  case LifecycleEvent::ContextPushed:
  case LifecycleEvent::DocumentCreated:
    // Focus, thread foreground, a newly pushed context and a new document manager
    // are not by themselves a semantic discontinuity: the context keeps its Fusion
    // identity and Host binding and is created lazily on first use.
    return {false, false};
  }
  return {false, false};
}

// What was still unresolved when a Fusion context was torn down. Accounting only:
// it names the responsibility so teardown is auditable and chooses no
// user-visible recovery, which P0-A/P0-B still own.
struct TeardownSummary {
  std::size_t records = 0;         // owned keys still in the ledger
  std::size_t pending_effects = 0; // Effect records without a terminal yet
  std::size_t uncertain = 0;       // execution certainty unknown
  std::size_t queued = 0;          // owned callbacks received but never issued
  bool needs_audit() const { return pending_effects != 0 || uncertain != 0 || queued != 0; }
};

inline TeardownSummary summarize_teardown(const std::vector<Record> &records, std::size_t queued) {
  TeardownSummary summary;
  summary.records = records.size();
  summary.queued = queued;
  for (const Record &record : records) {
    if (record.uncertain)
      ++summary.uncertain;
    if (record.kind == Kind::Effect && record.terminal == Terminal::Pending)
      ++summary.pending_effects;
  }
  return summary;
}

} // namespace fusion::tsf
