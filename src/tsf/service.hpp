#pragma once

// Stage 2.5 real TSF text service (development proof slice).
//
// Thin native TSF/COM shell per D072 §1-§3: Windows hard policy, key ownership,
// Windows context identity, causal barriers via the existing FusionCore, real
// edit-session projection/materialization, bounded causal ledger linkage, link
// health, plus development diagnostics. All semantic decisions go through the
// existing fusion::Host / FusionCore and the existing Mock Backend — this shell
// adds no second engine, ledger, decoder or materializer.
//
// Stage 4C: the sink bodies apply `lifecycle_obligation` (tsf/lifecycle.hpp) so
// the real TSF lifecycle drives explicit Fusion context transitions, and every
// context teardown fences what can be fenced and emits one audit record for
// whatever remained unresolved.

#include "mock_backend.hpp"
#include "tsf/candidate_view.hpp"
#include "tsf/com_ptr.hpp"
#include "tsf/display_attribute.hpp"
#include "tsf/lifecycle.hpp"
#include "tsf/open_declaration.hpp"
#include "tsf/orchestration.hpp"
#include "tsf/semantic.hpp"

#include <InputScope.h>
#include <msctf.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fusion::tsf {
#ifdef FUSION_WITH_FCITX5
using ServiceBackend = fusion::Backend;
#else
using ServiceBackend = fusion::test::MockBackend;
#endif

enum class InputScopeClass { Unknown, Ordinary, Sensitive };

struct ContextEntry {
  ~ContextEntry() {
    release_layout_sink(); // before the context reference goes: it is what to unadvise
    if (composition) {
      composition->Release();
    }
    if (context) {
      context->Release();
    }
    if (document_mgr) {
      document_mgr->Release();
    }
  }
  ITfDocumentMgr *document_mgr = nullptr; // ref-held; null for detached contexts
  ComRef<IUnknown> identity;
  ITfContext *context = nullptr; // ref-held identity key
  std::uint64_t generation = 1;  // bumped when the entry is destroyed
  ITfComposition *composition = nullptr;
  SemanticController semantic;
  CleanupIntent cleanup;
  ComRef<IUnknown> cleanup_identity;
  SelfEnd self_end;
  bool async_in_flight = false;
  // The current selection's TSF InputScope classification. Sensitive scopes
  // are never sent to Fcitx; unknown metadata preserves ordinary-field
  // behavior but is recorded as unknown.
  InputScopeClass input_scope = InputScopeClass::Unknown;
  // Last link identity observed for this context, purely for diagnostics: a
  // change means a disconnect, reconnect or incarnation/connection move.
  LinkStatus observed_link{};
  bool link_observed = false;

  ContextEntry(std::uint64_t context_id, std::size_t capacity, ServiceBackend &backend,
               ITfDocumentMgr *dim, ITfContext *pic);

  // R46: drop this context's ITfTextLayoutSink registration. TSF holds the sink's only
  // reference, so unadvising is also what releases it; a cookie of 0 means there is nothing
  // to unadvise and the call does nothing.
  void release_layout_sink();

  // Stage 5 candidate presentation. Real frontend only: the view is created
  // lazily when a non-empty page must be shown and hidden on an empty page or a
  // fenced projection. candidate_anchor remembers the last caret position the
  // read-only probe resolved, so paging keeps the popup glued to the text.
  std::unique_ptr<CandidateView> candidates;
  POINT candidate_anchor{};
  bool candidate_anchor_valid = false;
  // R46: the anchor of the last probe that resolved one in this context. A text store that
  // answers TS_E_NOLAYOUT for the range it was asked about has not laid the text out yet;
  // the page then stays where this context's text last had a place instead of disappearing
  // for the rest of the word. Kept across compositions on purpose: R46 also fires on the
  // first key, when this context has no composition range to ask.
  POINT candidate_anchor_last{};
  bool candidate_anchor_last_valid = false;
  // R49: the application window the candidate popup is owned by, decided per entry — the
  // context's view window (ITfContextView::GetWnd) or, when no view reports one, the thread
  // focus. `candidate_owner_from_view` records which, for the one diagnostic line per show.
  HWND candidate_owner = nullptr;
  bool candidate_owner_from_view = false;
  // R46: the layout-change sink advised on this context, so a layout that arrives after a
  // TS_E_NOLAYOUT probe re-places the page that is already on screen. Cookie only.
  DWORD layout_sink_cookie = 0;
  // R46: one re-probe is enough for one layout. Set when a notification asks for the async
  // session, cleared when that session runs, so a burst of notifications cannot stack a
  // session per notification behind the same popup.
  bool layout_reprobe_pending = false;
  // R46: a page exists but had no anchor to be placed at, so it is off screen waiting for a
  // layout change to supply one. Without the flag that page could never be re-placed: the
  // notification path only looked at a *visible* popup, and this one has nothing on screen.
  // Cleared by any show, any hide (every product-path hide goes through
  // `TextService::hide_candidate_page`), an empty page, and the teardown of the layout
  // relationship — so a page hidden because focus moved is never waiting for a layout.
  bool candidate_page_deferred = false;
};

class TextService final : public ITfTextInputProcessor,
                          public ITfKeyEventSink,
                          public ITfThreadMgrEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider {
public:
  static constexpr std::size_t kLedgerCapacity = 64; // development bound

  TextService();
  ~TextService();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfTextInputProcessor
  STDMETHODIMP Activate(ITfThreadMgr *ptim, TfClientId tid) override;
  STDMETHODIMP Deactivate() override;

  // ITfKeyEventSink
  STDMETHODIMP OnSetFocus(BOOL fForeground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext *pic, WPARAM wparam, LPARAM lparam, BOOL *pfEaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext *pic, WPARAM wparam, LPARAM lparam, BOOL *pfEaten) override;
  STDMETHODIMP OnKeyDown(ITfContext *pic, WPARAM wparam, LPARAM lparam, BOOL *pfEaten) override;
  STDMETHODIMP OnKeyUp(ITfContext *pic, WPARAM wparam, LPARAM lparam, BOOL *pfEaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext *pic, REFGUID rguid, BOOL *pfEaten) override;

  // ITfThreadMgrEventSink
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr *pdim) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr *pdim) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr *pdimFocus, ITfDocumentMgr *pdimPrevFocus) override;
  STDMETHODIMP OnPushContext(ITfContext *pic) override;
  STDMETHODIMP OnPopContext(ITfContext *pic) override;

  // ITfCompositionSink
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition *pComposition) override;

  // ITfDisplayAttributeProvider (Stage 5: composition styling of the Fcitx
  // preedit format; see tsf/display_attribute.hpp)
  STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo **ppEnum) override;
  STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo **ppInfo) override;

  // Stage 5 candidate click entry point (called by CandidateView's window
  // procedure). Validates the owning entry through the same stale-entry fence the
  // edit sessions use, then dispatches the non-key action on the product path.
  void on_candidate_hit(void *entry, std::uint64_t generation, CandidateAction action,
                        std::int32_t index);

  // R46 (called by the per-context ITfTextLayoutSink): TSF says this context's text layout
  // changed. For a context whose page is on screen but whose last probe had no layout to
  // read, that is the signal to ask for the anchor again — through one read-only async
  // session, never a text mutation. Anything else (no such entry, no displayed page, no
  // popup) does nothing.
  void note_layout_change(void *entry, std::uint64_t generation);

  // R52 (called by the compartment sink this service advises on activation): one notification
  // that this thread's keyboard open/close compartment was written. The sink reads the new
  // value itself, because `OnChange` carries nothing but the compartment's GUID.
  void note_openclose_change();
  // R52: answers the sink with "this notification is the one my own write caused", once. The
  // marker is set around `SetValue` and taken here, so a write this process did not make is
  // reported as another's; a notification TSF delivered later than the write it follows loses
  // the attribution and is reported as `other`.
  bool take_openclose_self_write();

  // R46 (called by the popup's own reposition message, so it runs on the message loop and
  // not inside the session that probed): place the page this entry displays at the anchor
  // the probe just resolved. Fenced by entry identity and generation like every other
  // callback that arrives late.
  void place_candidate_from_anchor(void *entry, std::uint64_t generation);

  // R51 (called from inside the callback or session that ended a composition): arm the Host reset
  // that termination needs and hand this thread's message loop the wake-up, because
  // `Backend::reset` is a blocking pipe call and may not run here. The intent, not the message,
  // is the truth: `flush_backend_reset` answers it and clears it, so a duplicate arm, a late
  // wake-up and a boundary that got there first all reset at most once. Called by the edit
  // sessions, like the seam above.
  void request_backend_reset(ContextEntry &entry, ResetSource source);

  // --- internal API used by the edit-session objects ---
  ContextEntry *find_entry(ITfContext *pic);
  std::shared_ptr<ContextEntry> hold_entry(ContextEntry *entry);
  ContextEntry *find_entry_alive(void *entry, std::uint64_t generation);
  bool hard_policy_allows(ContextEntry &entry);

  // The one answer both key-up callbacks use: may this release be relayed at all, and does
  // this context have the resources to settle it. T claims a key-up only when this is true,
  // so a release the application never sees cannot be caused by T and K disagreeing.
  bool release_admissible(ContextEntry &entry, const KeyAction &action);
  // U6: declares this thread's keyboard open/close compartment open, once per
  // activation and once per document manager that has not been seen yet (a new
  // application window). It is never called from a key path, so a Ctrl+Space close
  // made before or after the first key of that window is preserved. Returns true
  // when the compartment reads open afterwards.
  bool declare_keyboard_open(const char *reason);
  // `presentation` (real frontend only) places the caret at the engine cursor
  // and styles the composition; without it the caret goes to the end.
  fusion::EditResult apply_state_text(ContextEntry &entry, TfEditCookie ec,
                                      const std::wstring &text, bool &mutated,
                                      const PreeditPresentation *presentation = nullptr);
  // Returns the fusion EditResult observed by the real mutation, and whether an
  // effect mutation happened (verified by readback) for reconcile purposes.
  fusion::EditResult apply_effect_text(ContextEntry &entry, TfEditCookie ec,
                                       const std::wstring &text, bool &mutated);
  bool end_composition(ContextEntry &entry, TfEditCookie ec);
  bool clear_composition_text(ContextEntry &entry, TfEditCookie ec);
  bool request_sync_session(ContextEntry &entry, ITfEditSession *session, HRESULT &outer,
                            HRESULT &inner);
  void schedule_async_pump(ContextEntry &entry);
  void schedule_cleanup(ContextEntry &entry);
  ITfThreadMgr *thread_mgr() { return thread_mgr_.get(); }
  TfClientId client_id() const { return client_id_; }
  void note_async_done(ContextEntry &entry, bool progress);
  bool apply_frontend(ContextEntry &entry, TfEditCookie ec);
  ServiceBackend &backend() {
#ifdef FUSION_WITH_FCITX5
    return *backend_;
#else
    return backend_;
#endif
  }

private:
  ContextEntry *resolve_or_create_entry(ITfContext *pic);
  void handle_pass_key(ContextEntry &entry, const SemanticDecision &decision, BOOL *pfEaten);
  // Shared real-frontend settlement for a key dispatch and a candidate click:
  // enqueue the ordered callbacks, run one synchronous frontend session, apply the
  // natural-forward/PASS answer when a physical key is present (pfEaten nullable).
  void settle_frontend_work(ContextEntry &entry, const SemanticDecision &decision, BOOL *pfEaten);
  // Show, reposition or hide the candidate surface from the controller projection.
  void sync_candidate_view(ContextEntry &entry);
  // The one door every product-path hide goes through: the page leaves the screen and this
  // context stops waiting for a layout to place it. Clearing the flag is what makes a hide
  // final — `popup_may_be_placed` then refuses both a reposition that was queued while the
  // page was on screen and a later layout notification — because R49 makes the popup owned and
  // topmost, so a page that came back on its own would come back above whatever the user has
  // focused now, and would be clickable. `layout_reprobe_pending` is not this door's business:
  // it records an async session TSF still owes the context, and only that session's own run or
  // `release_layout_sink` settles it.
  void hide_candidate_page(ContextEntry &entry);
  // R51: run a reset the key path has not reached, from a boundary that is outside every edit
  // session (document focus moving away, a context popped, a context torn down, deactivation),
  // and log the one `BACKEND_RESET reason=` line that says which. Nothing else may clear the
  // intent, so these boundaries and the deferred drain share one door and one answer.
  void flush_backend_reset(ContextEntry &entry, ResetSource source);
  // The window procedure of the wake-up message and its window, created on the TSF thread at the
  // first deferred arm and destroyed with the activation. `drain_deferred_resets` re-queues
  // instead of resetting while a key is being handled, so a wake-up never runs inside key
  // handling either.
  bool ensure_deferred_reset_window();
  // Hand one wake-up to this thread's message loop. False means no wake-up is outstanding: the
  // queued intents stay, and the in-flight mark is released so the next arm asks again.
  bool post_deferred_reset_wakeup();
  void close_deferred_reset_window();
  void drain_deferred_resets();
  static constexpr UINT deferred_reset_message = WM_APP + 2;
  static LRESULT CALLBACK deferred_reset_proc(HWND, UINT, WPARAM, LPARAM);
  static bool register_deferred_reset_class();
  // R46: advise this context's ITfTextLayoutSink (through ITfSource on ITfContext) while
  // the entry is live, and stop when TSF pops the context. Idempotent in both directions:
  // an entry that already has the registration is not advised twice, and a context that
  // never had one is not unadvised.
  void advise_layout_sink(ContextEntry &entry);
  // R52: the open/close notification is installed on the thread's own compartment object, so
  // it is one registration for the whole activation — advised once in `Activate`, unadvised
  // once in `Deactivate`, and idempotent in both directions like the layout sink.
  void advise_openclose_sink();
  void unadvise_openclose_sink();
  bool request_readonly_sync(ContextEntry &entry, ITfEditSession *session, HRESULT &outer,
                             HRESULT &inner);
  void drain_prior_effect(ContextEntry &entry);
  void cancel_current_key(ContextEntry &entry, BOOL *pfEaten);
  void handle_owned_state(ContextEntry &entry, const SemanticDecision &decision);
  void handle_owned_effect(ContextEntry &entry, const SemanticDecision &decision);
  void handle_reject_cleanup(ContextEntry &entry);
  void destroy_all_entries(const char *reason);
  // Stage 4C teardown boundary: audit what is unresolved, fence the ledger to
  // explicit terminals, unbind the Host context and fence late results by
  // bumping the entry generation. Never resolves P0-A/P0-B responsibility.
  void reconcile_context_teardown(ContextEntry &entry, const char *reason);
  // Authoritative document-manager lookup for an entry whose document manager was
  // not recorded at creation (the context was not the focused top context then).
  bool context_reports_document(ContextEntry &entry, ITfDocumentMgr *pdim);

  std::atomic<ULONG> ref_count_{1};
  ComRef<ITfThreadMgr> thread_mgr_;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  DWORD thread_event_cookie_ = 0;
  // R52: the thread's keyboard open/close compartment, held for the length of the activation
  // because it is both the object the sink is registered on and the object the new value is
  // read back from. `openclose_state_` is the last value this service saw (-1 before the first
  // read) and `openclose_self_write_` marks the notification its own `SetValue` causes. All
  // three are read and written from whatever thread TSF delivers on, so they carry no lock
  // and take no dependency on which thread asked.
  ComRef<ITfCompartment> openclose_compartment_;
  DWORD openclose_sink_cookie_ = 0;
  std::atomic<int> openclose_state_{-1};
  std::atomic<bool> openclose_self_write_{false};
  DisplayAttributeAtoms display_atoms_; // registered on Activate
#ifdef FUSION_WITH_FCITX5
  std::unique_ptr<Backend> backend_;
#else
  fusion::test::MockBackend backend_;
#endif
  std::vector<std::shared_ptr<ContextEntry>> entries_;
  // U6: document managers whose window has already been given the open default. An
  // entry is released when TSF reports that document manager destroyed, so a reused
  // identity counts as new; the set is cleared on Deactivate and bounded without
  // forgetting a known window (see tsf/open_declaration.hpp).
  OpenDeclarationTracker open_declarations_;
  // R51: reset intents armed while an edit session owned the thread, waiting for this thread's
  // message loop. Coalesced per entry+generation, so at most one wake-up is in flight and the
  // batch it carries is whatever had armed by the time it runs.
  DeferredResetQueue<std::shared_ptr<ContextEntry>> deferred_resets_;
  // Message-only window carrying the wake-up: created on the TSF thread at the first deferred
  // arm, destroyed before the activation ends. `GWLP_USERDATA` is this service, so the window is
  // never left alive with a pointer to a released one (DestroyWindow also drops its posted
  // messages). 0 while no deferred work has ever been armed on this activation.
  HWND deferred_reset_window_ = nullptr;
  std::uint64_t next_context_id_ = 1;
  std::uint64_t next_generation_ = 1;
  bool handling_key_ = false; // reentrancy guard (D072 §7.5)
  // Diagnostics only: how many context teardowns had to record unresolved
  // responsibility. Not a ledger and not an admission policy.
  std::uint64_t unresolved_teardowns_ = 0;
};

} // namespace fusion::tsf
