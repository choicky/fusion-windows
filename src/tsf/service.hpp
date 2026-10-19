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
#include "tsf/orchestration.hpp"
#include "tsf/semantic.hpp"

#include <InputScope.h>
#include <msctf.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
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

  // Stage 5 candidate presentation. Real frontend only: the view is created
  // lazily when a non-empty page must be shown and hidden on an empty page or a
  // fenced projection. candidate_anchor remembers the last caret position the
  // read-only probe resolved, so paging keeps the popup glued to the text.
  std::unique_ptr<CandidateView> candidates;
  POINT candidate_anchor{};
  bool candidate_anchor_valid = false;
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

  // --- internal API used by the edit-session objects ---
  ContextEntry *find_entry(ITfContext *pic);
  std::shared_ptr<ContextEntry> hold_entry(ContextEntry *entry);
  ContextEntry *find_entry_alive(void *entry, std::uint64_t generation);
  bool hard_policy_allows(ContextEntry &entry);
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
  DisplayAttributeAtoms display_atoms_; // registered on Activate
#ifdef FUSION_WITH_FCITX5
  std::unique_ptr<Backend> backend_;
#else
  fusion::test::MockBackend backend_;
#endif
  std::vector<std::shared_ptr<ContextEntry>> entries_;
  // U6: document managers whose window has already been given the open default. A
  // pointer that is reused after release can only suppress one re-declaration, never
  // force a window that the user closed; the set is cleared on Deactivate and capped.
  std::set<ITfDocumentMgr *> open_declared_document_managers_;
  static constexpr std::size_t kMaxOpenDeclaredDocumentManagers = 256;
  std::uint64_t next_context_id_ = 1;
  std::uint64_t next_generation_ = 1;
  bool handling_key_ = false; // reentrancy guard (D072 §7.5)
  // Diagnostics only: how many context teardowns had to record unresolved
  // responsibility. Not a ledger and not an admission policy.
  std::uint64_t unresolved_teardowns_ = 0;
};

} // namespace fusion::tsf
