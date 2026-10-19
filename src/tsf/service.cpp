#include "tsf/service.hpp"

#include "tsf/build_identity.hpp"
#include "tsf/diag.hpp"
#include "tsf/observation.hpp"

#ifdef FUSION_WITH_FCITX5
#include "ipc/ipc_backend.hpp"
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace fusion::tsf {

namespace {

// InputScope.h declares these GUIDs as link-time symbols, but the desktop SDK
// does not ship a separate msctf import library in all supported toolchains.
// Keep the documented values local to the probe instead of changing the link
// surface of the TIP.
constexpr GUID kIidTfInputScope{
    0xfde1eaee, 0x6924, 0x4cdf, {0x91, 0xe7, 0xda, 0x38, 0xcf, 0xf5, 0x55, 0x9d}};
constexpr GUID kGuidPropInputScope{
    0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

// QI helper: wraps the new reference (QI addref'd) in a ComRef.
template <typename T> ComRef<T> qi(IUnknown *object, const IID &iid) {
  T *raw = nullptr;
  if (object == nullptr || FAILED(object->QueryInterface(iid, reinterpret_cast<void **>(&raw)))) {
    return ComRef<T>();
  }
  return ComRef<T>(raw);
}

bool move_caret(ITfContext *context, ITfRange *range, TfEditCookie ec) {
  ComRef<ITfRange> caret;
  if (FAILED(range->Clone(caret.out())) || FAILED(caret->Collapse(ec, TF_ANCHOR_END))) {
    return false;
  }
  TF_SELECTION selection{};
  selection.range = caret.get();
  selection.style.ase = TF_AE_NONE;
  selection.style.fInterimChar = FALSE;
  return SUCCEEDED(context->SetSelection(ec, 1, &selection));
}

void diagf(const char *name, const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  diag::event(name, buffer);
}

std::wstring widen(const std::string &text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          result.data(), count) != count)
    return {};
  return result;
}
std::string narrow(const std::wstring &text) {
  if (text.empty())
    return {};
  const int count =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    return {};
  std::string result(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          result.data(), count, nullptr, nullptr) != count)
    return {};
  return result;
}
KeyAction physical_key(WPARAM vk, LPARAM scan) {
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool win = (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000);
#ifndef FUSION_WITH_FCITX5
  (void)scan;
  return classify_key(static_cast<std::uint32_t>(vk), ctrl, alt, win);
#else
  BYTE state[256]{};
  wchar_t text[8]{};
  char32_t character = 0;
  if (GetKeyboardState(state)) {
    // Flag 4 leaves the keyboard dead-key state untouched (Windows 10 1607+).
    const int count = ToUnicodeEx(static_cast<UINT>(vk), static_cast<UINT>((scan >> 16) & 0xff),
                                  state, text, 8, 4, GetKeyboardLayout(0));
    if (count == 1 && !(text[0] >= 0xd800 && text[0] <= 0xdfff))
      character = text[0];
    else if (count == 2 && text[0] >= 0xd800 && text[0] <= 0xdbff && text[1] >= 0xdc00 &&
             text[1] <= 0xdfff)
      character = 0x10000u + ((text[0] - 0xd800u) << 10) + text[1] - 0xdc00u;
  }
  return translate_key(static_cast<std::uint32_t>(vk), ctrl, alt, win, character,
                       (state[VK_SHIFT] & 0x80) != 0, (state[VK_CAPITAL] & 1) != 0);
#endif
}
const char *route_name(SemanticDecision::Route route) {
  switch (route) {
  case SemanticDecision::Route::Pass:
    return "Pass";
  case SemanticDecision::Route::State:
    return "State";
  case SemanticDecision::Route::Effect:
    return "Effect";
  case SemanticDecision::Route::Reject:
    return "Reject";
  case SemanticDecision::Route::FailOpen:
    return "FailOpen";
  case SemanticDecision::Route::CancelledRecovery:
    return "CancelledRecovery";
  }
  return "?";
}

// --- development diagnostics for one key event's chain (requirement J) ---
void diag_key_arrival(ITfContext *pic, WPARAM vk, std::uint64_t context_id) {
  diagf("KEY_ARRIVAL", "vk=0x%03llX ctx=%p cid=%" PRIu64, static_cast<unsigned long long>(vk),
        static_cast<void *>(pic), context_id);
}

// ---------------------------------------------------------------------------
// Edit sessions. Each captures the service COM identity (AddRef), the context
// entry pointer plus its generation, and revalidates both at grant time, so a
// session granted against a destroyed/replaced context fences itself instead
// of mutating the wrong document (D072 §8).
// ---------------------------------------------------------------------------

class StateEditSession final : public ITfEditSession {
public:
  StateEditSession(TextService &service, ContextEntry &entry, std::uint64_t generation,
                   fusion::Key key, std::wstring text)
      : service_(&service), entry_(&entry), generation_(generation), key_(key),
        text_(std::move(text)) {
    hold_ = service.hold_entry(&entry);
    service.AddRef();
  }
  ~StateEditSession() { service_->Release(); }
  StateEditSession(const StateEditSession &) = delete;
  StateEditSession &operator=(const StateEditSession &) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ContextEntry *entry = service_->find_entry_alive(entry_, generation_);
    if (entry == nullptr) {
      diagf("FENCE", "kind=state stale-entry key.seq=%" PRIu64, key_.sequence);
      return S_OK;
    }
    if (entry->cleanup.pending()) {
      service_->schedule_cleanup(*entry);
      return S_OK;
    }
    bool mutated = false;
    if (!entry->semantic.state_applicable(key_)) {
      entry->semantic.enqueue_state(key_, narrow(text_));
      service_->schedule_async_pump(*entry);
      return S_OK;
    }
    const fusion::EditResult observed = service_->apply_state_text(*entry, ec, text_, mutated);
    const fusion::Result settled = entry->semantic.settle_state(key_, observed);
    diagf("SETTLE_STATE", "key.seq=%" PRIu64 " observed=%d settled=%d mutated=%d", key_.sequence,
          static_cast<int>(observed), static_cast<int>(settled), mutated ? 1 : 0);
    if (settled == fusion::Result::Blocked) {
      // An earlier record is still unresolved: keep this owned key queued for a
      // later async opportunity (latest-wins projection already applied).
      entry->semantic.enqueue_state(key_, narrow(text_));
      service_->schedule_async_pump(*entry);
    }
    return S_OK;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  std::shared_ptr<ContextEntry> hold_;
  TextService *service_;
  ContextEntry *entry_;
  std::uint64_t generation_;
  fusion::Key key_;
  std::wstring text_;
};

class EffectEditSession final : public ITfEditSession {
public:
  EffectEditSession(TextService &service, ContextEntry &entry, std::uint64_t generation,
                    fusion::Key key, std::wstring text)
      : service_(&service), entry_(&entry), generation_(generation), key_(key),
        text_(std::move(text)) {
    hold_ = service.hold_entry(&entry);
    service.AddRef();
  }
  ~EffectEditSession() { service_->Release(); }
  EffectEditSession(const EffectEditSession &) = delete;
  EffectEditSession &operator=(const EffectEditSession &) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ContextEntry *entry = service_->find_entry_alive(entry_, generation_);
    if (entry == nullptr) {
      diagf("FENCE", "kind=effect stale-entry key.seq=%" PRIu64, key_.sequence);
      return S_OK;
    }
    if (entry->cleanup.pending()) {
      service_->schedule_cleanup(*entry);
      return S_OK;
    }
    // D072 §8/README: the single execution ticket opens only now — after grant
    // and revalidation, immediately before the real document mutation.
    auto ticket = entry->semantic.open_effect(key_);
    if (!ticket) {
      diagf("EFFECT_OPEN_BLOCKED", "key.seq=%" PRIu64, key_.sequence);
      entry->semantic.enqueue_effect(key_, narrow(text_));
      service_->schedule_async_pump(*entry);
      return S_OK;
    }
    bool mutated = false;
    const fusion::EditResult observed = service_->apply_effect_text(*entry, ec, text_, mutated);
    const fusion::Result settled = entry->semantic.settle_effect(*ticket, observed);
    diagf("SETTLE_EFFECT", "key.seq=%" PRIu64 " observed=%d settled=%d mutated=%d", key_.sequence,
          static_cast<int>(observed), static_cast<int>(settled), mutated ? 1 : 0);
    if (observed == fusion::EditResult::RefusedNoMutation && settled == fusion::Result::Accepted) {
      entry->semantic.enqueue_effect(key_, narrow(text_));
      service_->schedule_async_pump(*entry);
    }
    return S_OK;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  std::shared_ptr<ContextEntry> hold_;
  TextService *service_;
  ContextEntry *entry_;
  std::uint64_t generation_;
  fusion::Key key_;
  std::wstring text_;
};

// A thin execution opportunity for the original ordered frontend callbacks.
// Core retains all causal/terminal accounting; this session adds no ledger.
class FrontendEditSession final : public ITfEditSession {
public:
  FrontendEditSession(TextService &service, ContextEntry &entry)
      : service_(service), hold_(service.hold_entry(&entry)), generation_(entry.generation) {
    service_.AddRef();
  }
  ~FrontendEditSession() { service_.Release(); }
  STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession)
      return E_NOINTERFACE;
    *out = static_cast<ITfEditSession *>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const auto refs = --refs_;
    if (!refs)
      delete this;
    return refs;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    auto *entry = service_.find_entry_alive(hold_.get(), generation_);
    if (entry && !entry->cleanup.pending())
      service_.apply_frontend(*entry, ec);
    return S_OK;
  }

private:
  std::atomic<ULONG> refs_{1};
  TextService &service_;
  std::shared_ptr<ContextEntry> hold_;
  std::uint64_t generation_;
};

class CleanupEditSession final : public ITfEditSession {
public:
  CleanupEditSession(TextService &service, ContextEntry &entry, std::uint64_t generation)
      : service_(&service), entry_(&entry), generation_(generation),
        serial_(entry.cleanup.serial()) {
    hold_ = service.hold_entry(&entry);
    identity_ = qi<IUnknown>(entry.composition, IID_IUnknown);
    service.AddRef();
  }
  ~CleanupEditSession() { service_->Release(); }
  CleanupEditSession(const CleanupEditSession &) = delete;
  CleanupEditSession &operator=(const CleanupEditSession &) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ContextEntry *entry = service_->find_entry_alive(entry_, generation_);
    if (entry == nullptr) {
      diagf("FENCE", "kind=cleanup stale-entry");
      return S_OK;
    }
    const auto current = qi<IUnknown>(entry->composition, IID_IUnknown);
    if (!identity_ || current.get() != identity_.get())
      return S_OK; // never clear a replacement composition
    const auto result = entry->cleanup.execute(serial_, generation_, identity_.get(), [&] {
      if (!service_->clear_composition_text(*entry, ec))
        return false;
      const auto after_clear = qi<IUnknown>(entry->composition, IID_IUnknown);
      if (entry->generation != generation_ ||
          (after_clear && after_clear.get() != identity_.get()) ||
          !service_->end_composition(*entry, ec))
        return false;
      entry->semantic.note_composition_terminated();
      return true;
    });
    if (!result)
      return S_OK;
    diagf(*result ? "CLEANUP_DONE" : "CLEANUP_FAILED", "cid=%" PRIu64,
          entry->semantic.context_id());
    return *result ? S_OK : E_FAIL;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  std::shared_ptr<ContextEntry> hold_;
  TextService *service_;
  ContextEntry *entry_;
  std::uint64_t generation_;
  std::uint64_t serial_;
  ComRef<IUnknown> identity_;
};

// Async fallback pump: executes queued work after a refused synchronous
// session. STATE work applies latest-wins text and settles FIFO; EFFECT work
// reopens through the normal propose/open/mutate/settle boundary.
class AsyncPumpSession final : public ITfEditSession {
public:
  AsyncPumpSession(TextService &service, ContextEntry &entry, std::uint64_t generation)
      : service_(&service), entry_(&entry), generation_(generation) {
    hold_ = service.hold_entry(&entry);
    service.AddRef();
  }
  ~AsyncPumpSession() { service_->Release(); }
  AsyncPumpSession(const AsyncPumpSession &) = delete;
  AsyncPumpSession &operator=(const AsyncPumpSession &) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ContextEntry *entry = service_->find_entry_alive(entry_, generation_);
    if (entry == nullptr) {
      diagf("FENCE", "kind=async-pump stale-entry");
      return S_OK;
    }
    if (entry->cleanup.pending()) {
      service_->note_async_done(*entry, false);
      service_->schedule_cleanup(*entry);
      return S_OK;
    }
    if (entry->semantic.real_frontend()) {
      const bool progress = service_->apply_frontend(*entry, ec);
      service_->note_async_done(*entry, progress);
      return S_OK;
    }
    const auto &pending = entry->semantic.pending();
    const std::size_t before = pending.size();
    if (!pending.empty() && pending.front().kind == fusion::Kind::State &&
        entry->semantic.state_applicable(pending.front().key)) {
      std::wstring latest;
      for (const auto &work : pending) {
        if (work.kind != fusion::Kind::State)
          break;
        latest = widen(work.text);
      }
      bool mutated = false;
      const fusion::EditResult observed = service_->apply_state_text(*entry, ec, latest, mutated);
      entry->semantic.settle_queued_states(observed);
      diagf("PUMP_STATE", "observed=%d queue=%zu", static_cast<int>(observed),
            entry->semantic.pending().size());
    } else if (!pending.empty() && pending.front().kind == fusion::Kind::Effect) {
      const PendingWork work = pending.front();
      auto ticket = entry->semantic.open_effect(work.key);
      if (ticket) {
        bool mutated = false;
        const fusion::EditResult observed =
            service_->apply_effect_text(*entry, ec, widen(work.text), mutated);
        const fusion::Result settled = entry->semantic.settle_effect(*ticket, observed);
        diagf("PUMP_EFFECT", "key.seq=%" PRIu64 " observed=%d settled=%d", work.key.sequence,
              static_cast<int>(observed), static_cast<int>(settled));
      } else {
        diagf("PUMP_EFFECT_OPEN_BLOCKED", "key.seq=%" PRIu64, work.key.sequence);
      }
    }
    service_->note_async_done(*entry, entry->semantic.pending().size() < before);
    return S_OK;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  std::shared_ptr<ContextEntry> hold_;
  TextService *service_;
  ContextEntry *entry_;
  std::uint64_t generation_;
};

// Read-only caret probe for the Stage 5 candidate surface. It resolves the live
// composition range (or the default selection when no composition exists yet)
// through the context's active view (ITfContext::GetActiveView) and records the
// screen-space anchor on the entry. The popup itself is positioned outside the
// session, where UI calls belong.
class AnchorProbeSession final : public ITfEditSession {
public:
  AnchorProbeSession(TextService &service, ContextEntry &entry)
      : service_(&service), entry_(&entry), generation_(entry.generation) {
    hold_ = service.hold_entry(&entry);
    service.AddRef();
  }
  ~AnchorProbeSession() { service_->Release(); }
  AnchorProbeSession(const AnchorProbeSession &) = delete;
  AnchorProbeSession &operator=(const AnchorProbeSession &) = delete;

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0)
      delete this;
    return count;
  }

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ContextEntry *entry = service_->find_entry_alive(entry_, generation_);
    if (entry == nullptr)
      return S_OK;
    entry->candidate_anchor_valid = false;
    // The authoritative live view is the context's active view: ITfContext is not
    // queried for ITfContextView, it hands one out through GetActiveView. A
    // failing HRESULT, a null view or no active view is a presentation failure
    // only: the anchor stays invalid, the caller hides the page, and no engine
    // semantic event is invented.
    ComRef<ITfContextView> view;
    if (FAILED(entry->context->GetActiveView(view.out())) || !view)
      return S_OK;
    ComRef<ITfRange> range;
    if (entry->composition != nullptr && SUCCEEDED(entry->composition->GetRange(range.out())) &&
        range) {
      // Composition range is the caret while preedit text is present.
    } else {
      TF_SELECTION selection{};
      ULONG fetched = 0;
      if (FAILED(entry->context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
          fetched == 0 || selection.range == nullptr)
        return S_OK;
      range = ComRef<ITfRange>(selection.range); // takes over the GetSelection reference
    }
    RECT rect{};
    BOOL at_start = FALSE;
    if (FAILED(view->GetTextExt(ec, range.get(), &rect, &at_start)))
      return S_OK;
    entry->candidate_anchor = POINT{rect.right, rect.bottom};
    entry->candidate_anchor_valid = true;
    return S_OK;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  std::shared_ptr<ContextEntry> hold_;
  TextService *service_;
  ContextEntry *entry_;
  std::uint64_t generation_;
};

} // namespace

// ---------------------------------------------------------------------------
// ContextEntry
// ---------------------------------------------------------------------------

ContextEntry::ContextEntry(std::uint64_t context_id, std::size_t capacity, ServiceBackend &backend,
                           ITfDocumentMgr *dim, ITfContext *pic)
    : document_mgr(dim), context(pic), semantic(context_id, capacity + 1, backend, 1) {
  if (document_mgr) {
    document_mgr->AddRef();
  }
  if (context) {
    context->AddRef();
    context->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(identity.out()));
  }
}

// ---------------------------------------------------------------------------
// TextService — IUnknown
// ---------------------------------------------------------------------------

TextService::TextService() {
  wchar_t module_path[1024] = {};
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&diagf), &module);
  GetModuleFileNameW(module, module_path, 1024);
  diagf("SERVICE_CREATED", "this=%p build=%s dll=%ls", static_cast<void *>(this), kBuildIdentity,
        module_path);
}

TextService::~TextService() { destroy_all_entries("destroy"); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void **ppv) {
  if (ppv == nullptr) {
    return E_POINTER;
  }
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor)) {
    *ppv = static_cast<ITfTextInputProcessor *>(this);
  } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
    *ppv = static_cast<ITfKeyEventSink *>(this);
  } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
    *ppv = static_cast<ITfThreadMgrEventSink *>(this);
  } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
    *ppv = static_cast<ITfCompositionSink *>(this);
  } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
    *ppv = static_cast<ITfDisplayAttributeProvider *>(this);
  } else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return ++ref_count_; }

STDMETHODIMP_(ULONG) TextService::Release() {
  const ULONG count = --ref_count_;
  if (count == 0) {
    delete this;
  }
  return count;
}

// ---------------------------------------------------------------------------
// TextService — ITfTextInputProcessor
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::Activate(ITfThreadMgr *ptim, TfClientId tid) {
  if (ptim == nullptr) {
    return E_INVALIDARG;
  }
#ifdef FUSION_WITH_FCITX5
  if (!backend_) {
    // P0-D OPEN: FusionHost startup is external to the DLL. The client object is
    // created unconditionally so a Host that becomes available later is reachable
    // through the bounded reconnect path on the normal key/context path; this
    // activation-time attempt only shortens the first-key latency.
    try {
      backend_ = std::make_unique<fusion::ipc::IpcBackend>();
      if (!backend_->ensure_link(std::chrono::milliseconds(200))) {
        diagf("IPC_HOST_UNREACHABLE", "FusionHost not reachable, fail-open PASS, reconnect armed");
        // Fail open: activate without a link, all keys PASS until a Host answers.
      }
    } catch (const std::exception &error) {
      diagf("IPC_HOST_CONNECT_FAILED", "%s, fail-open PASS", error.what());
      // Fail open: activate without a link.
    }
  }
#endif
  thread_mgr_ = ComRef<ITfThreadMgr>(ptim);
  thread_mgr_->AddRef();
  client_id_ = tid;
  // Composition styling is presentation only: without atoms the preedit is still
  // projected (text + caret), just unstyled.
  display_atoms_ = register_display_attribute_atoms();
  if (!display_atoms_.valid())
    diagf("DISPLAY_ATTRIBUTE", "atoms-unavailable");
  auto keystroke_mgr = thread_mgr_.as<ITfKeystrokeMgr>(IID_ITfKeystrokeMgr);
  // fForeground=TRUE: the sink takes the foreground key-event position, which
  // is how a keyboard TIP actually receives OnKeyDown deliveries.
  if (!keystroke_mgr ||
      FAILED(keystroke_mgr->AdviseKeyEventSink(tid, static_cast<ITfKeyEventSink *>(this), TRUE))) {
    diagf("ACTIVATE", "advise-key-failed");
    Deactivate();
    return E_FAIL;
  }
  auto source = thread_mgr_.as<ITfSource>(IID_ITfSource);
  if (!source || FAILED(source->AdviseSink(IID_ITfThreadMgrEventSink,
                                           static_cast<ITfThreadMgrEventSink *>(this),
                                           &thread_event_cookie_))) {
    diagf("ACTIVATE", "advise-thread-events-failed");
    Deactivate();
    return E_FAIL;
  }
  // U6: declare the thread's keyboard open/close compartment open at activation.
  // Windows can reset it to closed when a new application context is created later;
  // resolve_or_create_entry declares it again for exactly that case (see
  // declare_keyboard_open).
  declare_keyboard_open("activation");
  diagf("ACTIVATE", "tid=%lu", static_cast<unsigned long>(tid));
  return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
  diagf("DEACTIVATE", "tid=%lu entries=%zu", static_cast<unsigned long>(client_id_),
        entries_.size());
  if (thread_mgr_) {
    if (auto keystroke_mgr = thread_mgr_.as<ITfKeystrokeMgr>(IID_ITfKeystrokeMgr)) {
      keystroke_mgr->UnadviseKeyEventSink(client_id_);
    }
    if (thread_event_cookie_ != 0) {
      if (auto source = thread_mgr_.as<ITfSource>(IID_ITfSource)) {
        source->UnadviseSink(thread_event_cookie_);
      }
      thread_event_cookie_ = 0;
    }
  }
  if (lifecycle_obligation(LifecycleEvent::Deactivate).end_context)
    destroy_all_entries("deactivate");
  client_id_ = TF_CLIENTID_NULL;
  // TSF contract: release the thread manager before returning.
  thread_mgr_.reset();
  return S_OK;
}

void TextService::destroy_all_entries(const char *reason) {
  for (auto &entry : entries_) {
    reconcile_context_teardown(*entry, reason);
  }
  entries_.clear();
}

void TextService::reconcile_context_teardown(ContextEntry &entry, const char *reason) {
  // Accounting happens before the ledger and the queue are dropped, so a teardown
  // records what it could not settle instead of silently erasing responsibility
  // (D072 §25.2). The user-visible disposition of that responsibility stays
  // P0-A/P0-B, so nothing is fabricated here.
  const TeardownSummary summary =
      summarize_teardown(entry.semantic.core().records(), entry.semantic.pending().size());
  const std::uint64_t context_id = entry.semantic.context_id();
  if (entry.candidates)
    entry.candidates->hide(); // no candidate page survives its context teardown
  entry.semantic.destroy();   // unbind the Host context; Core::fence() -> explicit terminals
  entry.generation++;         // late edit sessions / pumps can no longer match this entry
  if (summary.needs_audit()) {
    ++unresolved_teardowns_;
    std::size_t fenced_uncertain = 0;
    std::size_t fenced_no_effect = 0;
    for (const fusion::Record &record : entry.semantic.core().records()) {
      if (record.uncertain)
        ++fenced_uncertain;
      if (record.terminal == fusion::Terminal::FailedNoEffect)
        ++fenced_no_effect;
    }
    diagf("CONTEXT_TEARDOWN",
          "reason=%s cid=%" PRIu64 " owned=%zu pending_effects=%zu uncertain=%zu queued=%zu "
          "fenced_uncertain=%zu fenced_no_effect=%zu unresolved_teardowns=%" PRIu64,
          reason, context_id, summary.records, summary.pending_effects, summary.uncertain,
          summary.queued, fenced_uncertain, fenced_no_effect, unresolved_teardowns_);
  } else {
    diagf("CONTEXT_TEARDOWN", "reason=%s cid=%" PRIu64 " owned=%zu clean=1", reason, context_id,
          summary.records);
  }
}

bool TextService::context_reports_document(ContextEntry &entry, ITfDocumentMgr *pdim) {
  // An entry created while its context was not the focused document's top context
  // has no recorded document manager; ask the live context instead so document
  // destruction cannot leave a semantically live Fusion context (and its Host
  // binding) behind. A failed or foreign lookup keeps the previous behaviour.
  if (pdim == nullptr || entry.context == nullptr)
    return false;
  ComRef<ITfDocumentMgr> document;
  if (FAILED(entry.context->GetDocumentMgr(document.out())) || !document)
    return false;
  return document.get() == pdim;
}

// ---------------------------------------------------------------------------
// TextService — ITfKeyEventSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
  // Thread-level foreground notification. D072 §9: focus is not a semantic
  // incarnation, so this only records the transition; no context is rebound,
  // reset or ended. Losing the foreground is a presentation event: hide any
  // candidate page we show, but keep the projected page and every semantic
  // record intact so a later dispatch can re-present it.
  const LifecycleObligation obligation = lifecycle_obligation(LifecycleEvent::ThreadForeground);
  if (!fForeground) {
    for (auto &entry : entries_)
      if (entry->candidates)
        entry->candidates->hide();
  }
  diagf("FOCUS_FOREGROUND", "fg=%d entries=%zu end_context=%d", fForeground ? 1 : 0,
        entries_.size(), obligation.end_context ? 1 : 0);
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext *pic, WPARAM wparam, LPARAM lparam,
                                        BOOL *pfEaten) {
  // Eligibility only: no engine consumption prediction or dispatch in T.
  // Applications which consult T must receive TRUE to subsequently route K.
  (void)lparam;
  if (pfEaten == nullptr)
    return E_INVALIDARG;
  *pfEaten = FALSE;
  if (!pic)
    return S_OK;
  if (handling_key_) {
    // Preserve routing to K for explicit recovery; T itself takes no ownership.
    if (auto *entry = find_entry(pic))
      *pfEaten = entry->cleanup.pending() || entry->semantic.pass_probe() != fusion::Pass::Clear;
    return S_OK;
  }
  ContextEntry *entry = resolve_or_create_entry(pic);
  if (!entry)
    return S_OK;
  const auto action = physical_key(wparam, lparam);
  const bool mapped = entry->semantic.routes_key(action);
  *pfEaten = entry->cleanup.pending() || entry->semantic.pass_probe() != fusion::Pass::Clear ||
             (mapped && hard_policy_allows(*entry));
  diagf("TEST_KEY", "vk=0x%03llX eligible=%d", static_cast<unsigned long long>(wparam), *pfEaten);
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext *pic, WPARAM wparam, LPARAM lparam,
                                      BOOL *pfEaten) {
  (void)pic;
  (void)wparam;
  (void)lparam;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  *pfEaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext *pic, WPARAM wparam, LPARAM lparam, BOOL *pfEaten) {
  (void)pic;
  (void)wparam;
  (void)lparam;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  *pfEaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext *pic, REFGUID rguid, BOOL *pfEaten) {
  (void)pic;
  (void)rguid;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  *pfEaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext *pic, WPARAM wparam, LPARAM lparam, BOOL *pfEaten) {
  (void)lparam;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  *pfEaten = FALSE;
  if (pic == nullptr) {
    return S_OK;
  }
  if (handling_key_) {
    // D072 §7.5: key-callback reentrancy is not assumed impossible; refuse to
    // nest ownership handling.
    diagf("REENTRANT_KEY", "vk=0x%03llX", static_cast<unsigned long long>(wparam));
    if (auto *current = find_entry(pic);
        current &&
        (current->cleanup.pending() || current->semantic.pass_probe() != fusion::Pass::Clear)) {
      const auto cancelled = current->semantic.cancel_pass_recovery();
      diagf("CANCELLED_RECOVERY", "reentrant key.seq=%" PRIu64, cancelled.sequence);
      *pfEaten = TRUE;
      current->semantic.retire_terminals();
    }
    return S_OK;
  }
  ContextEntry *entry = resolve_or_create_entry(pic);
  if (entry == nullptr) {
    diagf("UNKNOWN_CONTEXT", "vk=0x%03llX ctx=%p", static_cast<unsigned long long>(wparam),
          static_cast<void *>(pic));
    return S_OK; // fail-open: the key stays application-owned
  }
  auto lifetime_guard = hold_entry(entry);
  handling_key_ = true;
  const bool policy_allows = hard_policy_allows(*entry);
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
  const KeyAction action = physical_key(wparam, lparam);
  diag_key_arrival(pic, wparam, entry->semantic.context_id());
  diagf("CLASSIFY", "type=%d policy=%d ctrl=%d alt=%d win=%d", static_cast<int>(action.type),
        policy_allows ? 1 : 0, ctrl ? 1 : 0, alt ? 1 : 0, win ? 1 : 0);

  entry->semantic.retire_terminals();
  if (entry->cleanup.pending()) {
    schedule_cleanup(*entry);
    cancel_current_key(*entry, pfEaten);
    entry->semantic.retire_terminals();
    handling_key_ = false;
    return S_OK;
  }
  if (!continue_current_key([&] { return entry->semantic.pass_probe(); },
                            [&] { drain_prior_effect(*entry); },
                            [&] { cancel_current_key(*entry, pfEaten); })) {
    entry->semantic.retire_terminals();
    handling_key_ = false;
    return S_OK;
  }
  const SemanticDecision decision = entry->semantic.begin_key(action, policy_allows);
  diagf("BACKEND", "symbolic=%s intent=%d route=%s eaten=%d", decision.symbolic.c_str(),
        static_cast<int>(decision.intent), route_name(decision.route), decision.eaten ? 1 : 0);

  if (entry->semantic.real_frontend()) {
    // Observability (D072 §21): disconnect / reconnect / incarnation / connection
    // moves on the real product path, with the semantic-readiness answer.
    const fusion::LinkStatus link = entry->semantic.link_status();
    const bool moved = !entry->link_observed || link.state != entry->observed_link.state ||
                       link.host_incarnation != entry->observed_link.host_incarnation ||
                       link.connection_generation != entry->observed_link.connection_generation;
    if (moved) {
      diagf("LINK", "state=%d incarnation=%llu generation=%llu semantic_ready=%d route=%s",
            static_cast<int>(link.state), static_cast<unsigned long long>(link.host_incarnation),
            static_cast<unsigned long long>(link.connection_generation),
            entry->semantic.semantic_ready() ? 1 : 0, route_name(decision.route));
      entry->observed_link = link;
      entry->link_observed = true;
    }
  }

  if (decision.backend_session_id)
    diagf("FCITX_BATCH", "session=%" PRIu64 " stimulus=%" PRIu64 " ops=%zu",
          decision.backend_session_id, decision.stimulus_id, decision.operation_count);
  if (entry->semantic.real_frontend() && decision.route != SemanticDecision::Route::FailOpen &&
      decision.route != SemanticDecision::Route::CancelledRecovery) {
    settle_frontend_work(*entry, decision, pfEaten);
    sync_candidate_view(*entry);
    handling_key_ = false;
    return S_OK;
  }
  switch (decision.route) {
  case SemanticDecision::Route::Pass:
    handle_pass_key(*entry, decision, pfEaten);
    break;
  case SemanticDecision::Route::State:
    handle_owned_state(*entry, decision);
    *pfEaten = TRUE;
    break;
  case SemanticDecision::Route::Effect:
    handle_owned_effect(*entry, decision);
    *pfEaten = TRUE;
    break;
  case SemanticDecision::Route::Reject:
    // Already settled inside begin_key: explicit FailedNoEffect terminal.
    handle_reject_cleanup(*entry);
    *pfEaten = TRUE;
    break;
  case SemanticDecision::Route::FailOpen:
    // D072 §4: fail-open is allowed before ownership only.
    diagf("OWN_FAIL_OPEN", "vk=0x%03llX policy=%d", static_cast<unsigned long long>(wparam),
          policy_allows ? 1 : 0);
    handle_pass_key(*entry, decision, pfEaten);
    break;
  case SemanticDecision::Route::CancelledRecovery:
    *pfEaten = TRUE;
    break;
  }
  if (entry->semantic.real_frontend())
    sync_candidate_view(*entry); // a fenced/failed real dispatch hides a stale page
  entry->semantic.retire_terminals();
  handling_key_ = false;
  return S_OK;
}

// ---------------------------------------------------------------------------
// TextService — ITfThreadMgrEventSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr *pdim) {
  const LifecycleObligation obligation = lifecycle_obligation(LifecycleEvent::DocumentCreated);
  diagf("DOCMGR_INIT", "dim=%p entries=%zu end_context=%d", static_cast<void *>(pdim),
        entries_.size(), obligation.end_context ? 1 : 0);
  return S_OK;
}

STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr *pdim) {
  const LifecycleObligation obligation = lifecycle_obligation(LifecycleEvent::DocumentDestroyed);
  std::size_t reclaimed = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    ContextEntry &entry = **it;
    const bool recorded = entry.document_mgr != nullptr && entry.document_mgr == pdim;
    const bool reported = !recorded && context_reports_document(entry, pdim);
    if (obligation.end_context && (recorded || reported)) {
      diagf("ENTRY_DESTROY", "cid=%" PRIu64 " via=%s", entry.semantic.context_id(),
            reported ? "context-document" : "recorded-document");
      reconcile_context_teardown(entry, reported ? "docmgr-reported" : "docmgr");
      it = entries_.erase(it);
      ++reclaimed;
    } else {
      ++it;
    }
  }
  diagf("DOCMGR_UNINIT", "dim=%p reclaimed=%zu entries=%zu", static_cast<void *>(pdim), reclaimed,
        entries_.size());
  return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr *pdimFocus, ITfDocumentMgr *pdimPrevFocus) {
  // D072 §9: focus loss is not by itself an epoch advance or a composition
  // termination, so the previous context keeps its Fusion identity, Host binding
  // and projection. This records the real transition and what was attached to
  // each document manager at that moment, which is the evidence the OPEN
  // continuity sub-boundary needs.
  const LifecycleObligation obligation = lifecycle_obligation(LifecycleEvent::FocusChanged);
  std::size_t in_focus = 0;
  std::size_t in_previous = 0;
  for (const auto &entry : entries_) {
    if (pdimFocus != nullptr && entry->document_mgr == pdimFocus)
      ++in_focus;
    if (pdimPrevFocus != nullptr && entry->document_mgr == pdimPrevFocus)
      ++in_previous;
  }
  diagf("FOCUS_CHANGED",
        "dim=%p prev=%p entries=%zu in_focus=%zu in_previous=%zu end_context=%d reset_epoch=0",
        static_cast<void *>(pdimFocus), static_cast<void *>(pdimPrevFocus), entries_.size(),
        in_focus, in_previous, obligation.end_context ? 1 : 0);
  // Presentation only (D072 §9): a candidate page attached to the document that
  // just lost focus must not stay visible or actionable in the wrong focused
  // context. Hiding is the complete obligation here: no Host unbind, no
  // incarnation advance, no projection or semantic-state discard. When focus
  // returns, the next admitted dispatch re-presents through sync_candidate_view.
  if (pdimPrevFocus != nullptr && pdimPrevFocus != pdimFocus) {
    for (auto &entry : entries_) {
      const bool was_in_previous =
          entry->document_mgr == pdimPrevFocus || context_reports_document(*entry, pdimPrevFocus);
      if (was_in_previous && entry->candidates)
        entry->candidates->hide();
    }
  }
  return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext *pic) {
  // D072 §9/§18: a pushed context is not by itself a semantic discontinuity of the
  // previous one, and push/pop behaviour is UNFROZEN until real execution
  // provides evidence. A pushed context is represented by its own Fusion context,
  // created lazily on its first use; nothing is ended or rebound here.
  const LifecycleObligation obligation = lifecycle_obligation(LifecycleEvent::ContextPushed);
  ContextEntry *entry = pic != nullptr ? find_entry(pic) : nullptr;
  diagf("CONTEXT_PUSH", "ctx=%p cid=%" PRIu64 " known=%d entries=%zu end_context=%d",
        static_cast<void *>(pic), entry != nullptr ? entry->semantic.context_id() : 0,
        entry != nullptr ? 1 : 0, entries_.size(), obligation.end_context ? 1 : 0);
  if (obligation.end_context && entry != nullptr) {
    reconcile_context_teardown(*entry, "push");
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const auto &item) { return item.get() == entry; }),
                   entries_.end());
  }
  return S_OK;
}

STDMETHODIMP TextService::OnPopContext(ITfContext *pic) {
  const LifecycleObligation obligation = lifecycle_obligation(LifecycleEvent::ContextPopped);
  ContextEntry *entry = pic != nullptr ? find_entry(pic) : nullptr;
  diagf("CONTEXT_POP", "ctx=%p cid=%" PRIu64 " known=%d entries=%zu end_context=%d",
        static_cast<void *>(pic), entry != nullptr ? entry->semantic.context_id() : 0,
        entry != nullptr ? 1 : 0, entries_.size(), obligation.end_context ? 1 : 0);
  if (obligation.end_context && entry != nullptr) {
    diagf("ENTRY_FENCE", "pop cid=%" PRIu64, entry->semantic.context_id());
    reconcile_context_teardown(*entry, "pop");
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const auto &item) { return item.get() == entry; }),
                   entries_.end());
  }
  return S_OK;
}

// ---------------------------------------------------------------------------
// TextService — ITfDisplayAttributeProvider
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo **ppEnum) {
  return create_display_attribute_enum(ppEnum);
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo **ppInfo) {
  return create_display_attribute_info(guid, ppInfo);
}

// ---------------------------------------------------------------------------
// TextService — ITfCompositionSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ecWrite,
                                                  ITfComposition *pComposition) {
  for (auto &entry : entries_) {
    const auto callback_identity = qi<IUnknown>(pComposition, IID_IUnknown);
    const auto live_identity = qi<IUnknown>(entry->composition, IID_IUnknown);
    if (callback_identity && callback_identity.get() == live_identity.get()) {
      // Accounting for the external authority event (D072 §9): report what the
      // reconciliation had to settle, so an externally destroyed composition is
      // auditable rather than silently absorbed.
      const TeardownSummary before =
          summarize_teardown(entry->semantic.core().records(), entry->semantic.pending().size());
      if (entry->self_end.terminated(entry->generation, callback_identity.get(), [&] {
            entry->semantic.note_composition_terminated();
            entry->cleanup.reconciled();
          })) {
        diagf("COMPOSITION_SELF_END", "cid=%" PRIu64, entry->semantic.context_id());
        return S_OK;
      }
      // External authority event (D072 §9): reconcile the projection; queued
      // and pending work reaches explicit terminals, never silent loss. The text
      // the app keeps loses its composition styling (presentation only).
      ITfRange *terminated_range = nullptr;
      if (SUCCEEDED(entry->composition->GetRange(&terminated_range)) && terminated_range) {
        ComRef<ITfRange> held(terminated_range);
        if (!clear_display_attributes(entry->context, held.get(), ecWrite))
          diagf("DISPLAY_ATTRIBUTE", "clear-failed terminated");
      }
      entry->composition->Release();
      entry->composition = nullptr;
      if (entry->candidates)
        entry->candidates->hide(); // the composition the candidates belonged to is gone
      diagf("COMPOSITION_TERMINATED",
            "cid=%" PRIu64 " owned=%zu pending_effects=%zu uncertain=%zu queued=%zu remaining=%zu",
            entry->semantic.context_id(), before.records, before.pending_effects, before.uncertain,
            before.queued, entry->semantic.pending().size());
      break;
    }
  }
  return S_OK;
}

// ---------------------------------------------------------------------------
// Entry management and internal API
// ---------------------------------------------------------------------------

std::shared_ptr<ContextEntry> TextService::hold_entry(ContextEntry *entry) {
  for (auto &item : entries_)
    if (item.get() == entry)
      return item;
  return {};
}

ContextEntry *TextService::find_entry(ITfContext *pic) {
  auto identity = qi<IUnknown>(pic, IID_IUnknown);
  if (!identity)
    return nullptr;
  for (auto &entry : entries_) {
    if (entry->identity.get() == identity.get()) {
      return entry.get();
    }
  }
  return nullptr;
}

ContextEntry *TextService::find_entry_alive(void *entry, std::uint64_t generation) {
  for (auto &candidate : entries_) {
    if (candidate.get() == entry && same_entry_generation(generation, candidate->generation)) {
      return candidate.get();
    }
  }
  return nullptr;
}

ContextEntry *TextService::resolve_or_create_entry(ITfContext *pic) {
  if (ContextEntry *found = find_entry(pic)) {
    return found;
  }
  if (!qi<IUnknown>(pic, IID_IUnknown) || entries_.size() >= kLedgerCapacity)
    return nullptr;
  ITfDocumentMgr *dim = nullptr;
  if (thread_mgr_) {
    if (SUCCEEDED(thread_mgr_->GetFocus(&dim)) && dim != nullptr) {
      ITfContext *top = nullptr;
      const bool matches = SUCCEEDED(dim->GetTop(&top)) && top != nullptr && top == pic;
      if (top != nullptr) {
        top->Release();
      }
      if (!matches) {
        dim->Release();
        dim = nullptr;
      }
    } else {
      dim = nullptr;
    }
  }
#ifdef FUSION_WITH_FCITX5
  // Bounded reconnect on the context path: a Host that became available after
  // the context was first needed is reachable without an IME switch. Failure is
  // fail-open (the key stays application-owned), never a semantic verdict.
  if (!backend_ || !backend_->ensure_link(std::chrono::milliseconds(200)))
    return nullptr;
#endif
  auto entry =
      std::make_shared<ContextEntry>(next_context_id_++, kLedgerCapacity, backend(), dim, pic);
  if (dim)
    dim->Release(); // ContextEntry retains its own reference; release GetFocus's.
  entry->generation = next_generation_++;
  ContextEntry *raw = entry.get();
  entries_.push_back(std::move(entry));
  diagf("ENTRY_CREATE", "cid=%" PRIu64 " ctx=%p dim=%p entries=%zu", raw->semantic.context_id(),
        static_cast<void *>(pic), static_cast<void *>(dim), entries_.size());
  // U6: a new application context starts with Fusion open (owner decision
  // 2026-10-19). This runs before the same key's hard_policy_allows, so the first
  // key of a new window is admitted without Ctrl+Space.
  declare_keyboard_open("new_context");
  return raw;
}

class InputScopeProbe final : public ITfEditSession {
public:
  InputScopeProbe(std::shared_ptr<ContextEntry> entry, std::uint64_t generation)
      : entry_(std::move(entry)), generation_(generation) {}

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --refs_;
    if (count == 0)
      delete this;
    return count;
  }

  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    if (!entry_ || entry_->generation != generation_ || entry_->context == nullptr) {
      diagf("INPUT_SCOPE", "class=unknown reason=stale-entry");
      return S_OK;
    }
    entry_->input_scope = InputScopeClass::Unknown;
    ComRef<ITfProperty> property;
    ComRef<ITfRange> range;
    VARIANT value;
    VariantInit(&value);
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(entry_->context->GetProperty(kGuidPropInputScope, property.out())) || !property ||
        FAILED(entry_->context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
        fetched == 0 || selection.range == nullptr ||
        FAILED(selection.range->QueryInterface(IID_ITfRange,
                                               reinterpret_cast<void **>(range.out()))) ||
        FAILED(property->GetValue(ec, range.get(), &value))) {
      if (selection.range != nullptr)
        selection.range->Release();
      VariantClear(&value);
      return S_OK;
    }
    selection.range->Release();
    ComRef<ITfInputScope> scope;
    if (value.vt == VT_UNKNOWN && value.punkVal != nullptr)
      scope = qi<ITfInputScope>(value.punkVal, kIidTfInputScope);
    if (scope) {
      InputScope *values = nullptr;
      UINT count = 0;
      if (SUCCEEDED(scope->GetInputScopes(&values, &count))) {
        entry_->input_scope = InputScopeClass::Ordinary;
        for (UINT i = 0; i < count; ++i) {
          if (values[i] == IS_PASSWORD || values[i] == IS_PRIVATE ||
              values[i] == IS_NUMERIC_PASSWORD || values[i] == IS_NUMERIC_PIN ||
              values[i] == IS_ALPHANUMERIC_PIN || values[i] == IS_ALPHANUMERIC_PIN_SET) {
            entry_->input_scope = InputScopeClass::Sensitive;
            break;
          }
        }
      }
      if (values != nullptr)
        CoTaskMemFree(values);
    }
    VariantClear(&value);
    return S_OK;
  }

private:
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<ContextEntry> entry_;
  std::uint64_t generation_;
};

void refresh_input_scope(TextService &service, ContextEntry &entry) {
  if (entry.context == nullptr || service.client_id() == TF_CLIENTID_NULL)
    return;
  auto hold = service.hold_entry(&entry);
  if (!hold) {
    entry.input_scope = InputScopeClass::Unknown;
    diagf("INPUT_SCOPE", "class=unknown reason=entry-not-live");
    return;
  }
  auto *probe = new InputScopeProbe(std::move(hold), entry.generation);
  HRESULT inner = E_FAIL;
  // Admission consumes this result immediately. TF_ES_READ alone may queue
  // the edit session and leave the previous classification in place.
  const HRESULT outer = entry.context->RequestEditSession(service.client_id(), probe,
                                                          TF_ES_SYNC | TF_ES_READ, &inner);
  probe->Release();
  if (outer != S_OK || inner != S_OK) {
    entry.input_scope = InputScopeClass::Unknown;
    diagf("INPUT_SCOPE", "class=unknown request=non-sync outer=0x%08lX inner=0x%08lX",
          static_cast<unsigned long>(outer), static_cast<unsigned long>(inner));
    return;
  }
  const char *classification = entry.input_scope == InputScopeClass::Sensitive  ? "sensitive"
                               : entry.input_scope == InputScopeClass::Ordinary ? "ordinary"
                                                                                : "unknown";
  diagf("INPUT_SCOPE", "class=%s", classification);
}

// U6: the owner decided that a newly opened application window starts with Fusion
// open. The compartment write at Activate alone does not achieve that: the diagnostic
// log shows the thread compartment reading closed again on the first key of a new
// context, with no service-side write and no Deactivate in between (2026-10-19,
// fusion-tsf-2028.log: KEYBOARD_OPEN declared at activation, then HARD_POLICY
// openclose=closed on the new context's first key). Windows/the application therefore
// re-initialises the compartment when the context appears, after our activation write.
//
// The minimal correction is to declare it open once per *new context* as well. It is
// deliberately not called on focus changes or per key, so the user's own Ctrl+Space
// close still holds inside a context; only a new context starts open.
bool TextService::declare_keyboard_open(const char *reason) {
  if (!thread_mgr_)
    return false;
  auto compartments = qi<ITfCompartmentMgr>(thread_mgr_.get(), IID_ITfCompartmentMgr);
  if (!compartments)
    return false;
  ComRef<ITfCompartment> compartment;
  if (FAILED(
          compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, compartment.out())) ||
      !compartment) {
    diagf("KEYBOARD_OPEN", "declared reason=%s unavailable=1", reason);
    return false;
  }
  VARIANT current;
  VariantInit(&current);
  bool open = false;
  if (SUCCEEDED(compartment->GetValue(&current))) {
    open = (current.vt == VT_I4 && current.lVal != 0) ||
           (current.vt == VT_UI4 && current.uintVal != 0) ||
           (current.vt == VT_BOOL && current.boolVal != VARIANT_FALSE);
  }
  VariantClear(&current);
  const bool was_closed = !open;
  if (was_closed) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = 1;
    const HRESULT hr = compartment->SetValue(client_id_, &value);
    VariantClear(&value);
    if (FAILED(hr)) {
      diagf("KEYBOARD_OPEN", "declared reason=%s set_failed=0x%08lX", reason,
            static_cast<unsigned long>(hr));
      return false;
    }
    open = true;
  }
  diagf("KEYBOARD_OPEN", "declared reason=%s was_closed=%d", reason, was_closed ? 1 : 0);
  return open;
}

bool TextService::hard_policy_allows(ContextEntry &entry) {
  // Minimal Windows hard policy (D072 §10): keyboard open/close on the thread
  // manager, keyboard-disabled and empty-context on the context. A
  // failed read does not fabricate a policy verdict (treated as allowed and
  // logged); the backend can never override this gate.
  if (thread_mgr_) {
    auto compartments = qi<ITfCompartmentMgr>(thread_mgr_.get(), IID_ITfCompartmentMgr);
    if (compartments) {
      ComRef<ITfCompartment> compartment;
      if (SUCCEEDED(compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                                 compartment.out())) &&
          compartment) {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(compartment->GetValue(&value))) {
          const bool closed = (value.vt == VT_I4 && value.lVal == 0) ||
                              (value.vt == VT_UI4 && value.uintVal == 0) ||
                              (value.vt == VT_BOOL && value.boolVal == VARIANT_FALSE);
          VariantClear(&value);
          if (closed) {
            diagf("HARD_POLICY", "openclose=closed");
            return false;
          }
        }
      }
    }
  }
  refresh_input_scope(*this, entry);
  if (entry.input_scope == InputScopeClass::Sensitive) {
    diagf("SENSITIVE_SCOPE", "admission=blocked");
    return false;
  }
  if (entry.context != nullptr) {
    auto compartments = qi<ITfCompartmentMgr>(entry.context, IID_ITfCompartmentMgr);
    if (compartments) {
      for (const GUID &guid : {GUID_COMPARTMENT_KEYBOARD_DISABLED, GUID_COMPARTMENT_EMPTYCONTEXT}) {
        ComRef<ITfCompartment> compartment;
        if (SUCCEEDED(compartments->GetCompartment(guid, compartment.out())) && compartment) {
          VARIANT value;
          VariantInit(&value);
          if (SUCCEEDED(compartment->GetValue(&value))) {
            const bool blocked = (value.vt == VT_I4 && value.lVal != 0) ||
                                 (value.vt == VT_UI4 && value.uintVal != 0) ||
                                 (value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE);
            VariantClear(&value);
            if (blocked) {
              diagf("HARD_POLICY", "guid-blocked");
              return false;
            }
          }
        }
      }
    }
  }
  return true;
}

fusion::EditResult TextService::apply_state_text(ContextEntry &entry, TfEditCookie ec,
                                                 const std::wstring &text, bool &mutated,
                                                 const PreeditPresentation *presentation) {
  mutated = false;
  if (entry.composition == nullptr) {
    if (text.empty()) {
      // Trivially satisfied: an empty preedit with no composition mutates
      // nothing and reads back as applied.
      return fusion::EditResult::AppliedReadback;
    }
    auto composition_mgr = qi<ITfContextComposition>(entry.context, IID_ITfContextComposition);
    if (!composition_mgr) {
      diagf("STATE_REFUSED", "reason=no-context-composition");
      return fusion::EditResult::RefusedNoMutation;
    }
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(entry.context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
        fetched == 0 || selection.range == nullptr) {
      diagf("STATE_REFUSED", "reason=no-selection");
      return fusion::EditResult::RefusedNoMutation;
    }
    // GetSelection addrefs the range; the ComRef takes over that reference.
    ComRef<ITfRange> anchor(selection.range);
    if (FAILED(anchor->Collapse(ec, TF_ANCHOR_START))) {
      diagf("STATE_REFUSED", "reason=collapse");
      return fusion::EditResult::RefusedNoMutation;
    }
    ITfComposition *composition = nullptr;
    if (FAILED(composition_mgr->StartComposition(
            ec, anchor.get(), static_cast<ITfCompositionSink *>(this), &composition)) ||
        composition == nullptr) {
      diagf("STATE_REFUSED", "reason=start-composition");
      return fusion::EditResult::RefusedNoMutation;
    }
    entry.composition = composition;
    diagf("COMPOSITION_START", "cid=%" PRIu64, entry.semantic.context_id());
  }
  ITfRange *range = nullptr;
  if (FAILED(entry.composition->GetRange(&range)) || range == nullptr) {
    diagf("STATE_REFUSED", "reason=no-range");
    return fusion::EditResult::RefusedNoMutation;
  }
  ComRef<ITfRange> composition_range(range);
  if (FAILED(composition_range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size())))) {
    diagf("STATE_INDETERMINATE", "reason=settext");
    return fusion::EditResult::Indeterminate;
  }
  mutated = true;
  // Required readback: mismatch is uncertain, never consumed STATE success.
  std::wstring readback(text.size() + 1, L'\0');
  ULONG length = 0;
  if (FAILED(composition_range->GetText(ec, 0, readback.data(), static_cast<ULONG>(text.size()),
                                        &length)) ||
      length != text.size() || readback.compare(0, length, text) != 0) {
    diagf("STATE_READBACK", "mismatch len=%lu want=%zu", length, text.size());
    return observed_edit(true, false, false);
  }
  if (presentation != nullptr && !text.empty() &&
      !apply_display_attributes(entry.context, composition_range.get(), ec, *presentation,
                                display_atoms_)) {
    // Presentation only: the composition text itself is applied and read back.
    diagf("DISPLAY_ATTRIBUTE", "apply-failed ranges=%zu", presentation->ranges.size());
  }
  // The caret is part of the Fcitx preedit contract (engine byte cursor, mapped
  // to UTF-16 by layout_preedit); a hidden or absent cursor keeps it at the end.
  const bool caret_placed = presentation != nullptr && presentation->caret
                                ? place_caret_in_composition(entry.context, composition_range.get(),
                                                             ec, *presentation->caret)
                                : move_caret(entry.context, composition_range.get(), ec);
  if (!caret_placed) {
    return observed_edit(true, true, false);
  }
  if (text.empty() && !end_composition(entry, ec)) {
    return observed_edit(true, false, false);
  }
  return fusion::EditResult::AppliedReadback;
}

fusion::EditResult TextService::apply_effect_text(ContextEntry &entry, TfEditCookie ec,
                                                  const std::wstring &text, bool &mutated) {
  mutated = false;
  if (entry.composition != nullptr) {
    ITfRange *range = nullptr;
    if (FAILED(entry.composition->GetRange(&range)) || range == nullptr) {
      diagf("EFFECT_REFUSED", "reason=no-range");
      return fusion::EditResult::RefusedNoMutation;
    }
    ComRef<ITfRange> composition_range(range);
    if (FAILED(composition_range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size())))) {
      diagf("EFFECT_INDETERMINATE", "reason=settext");
      return fusion::EditResult::Indeterminate;
    }
    mutated = true;
    std::wstring readback(text.size() + 1, L'\0');
    ULONG length = 0;
    if (FAILED(composition_range->GetText(ec, 0, readback.data(), static_cast<ULONG>(text.size()),
                                          &length)) ||
        length != text.size() || readback.compare(0, length, text) != 0) {
      diagf("EFFECT_INDETERMINATE", "readback len=%lu want=%zu", length, text.size());
      return observed_edit(true, false, false);
    }
    // Committed text keeps no composition styling (presentation only).
    if (!clear_display_attributes(entry.context, composition_range.get(), ec))
      diagf("DISPLAY_ATTRIBUTE", "clear-failed");
    if (!move_caret(entry.context, composition_range.get(), ec)) {
      return observed_edit(true, true, false);
    }
    if (!end_composition(entry, ec)) {
      return observed_edit(true, false, false);
    }
    diagf("EFFECT_APPLIED", "via=composition len=%zu", text.size());
    return fusion::EditResult::AppliedReadback;
  }
  auto insertion = qi<ITfInsertAtSelection>(entry.context, IID_ITfInsertAtSelection);
  if (!insertion) {
    diagf("EFFECT_REFUSED", "reason=no-insert-at-selection");
    return fusion::EditResult::RefusedNoMutation;
  }
  ITfRange *inserted = nullptr;
  if (FAILED(insertion->InsertTextAtSelection(ec, 0, text.c_str(), static_cast<LONG>(text.size()),
                                              &inserted))) {
    diagf("EFFECT_INDETERMINATE", "reason=insert");
    return fusion::EditResult::Indeterminate;
  }
  mutated = true;
  ComRef<ITfRange> inserted_range(inserted);
  if (!inserted_range) {
    diagf("EFFECT_INDETERMINATE", "reason=no-inserted-range");
    return fusion::EditResult::Indeterminate;
  }
  std::wstring readback(text.size() + 1, L'\0');
  ULONG length = 0;
  if (FAILED(inserted_range->GetText(ec, 0, readback.data(), static_cast<ULONG>(text.size()),
                                     &length)) ||
      length != text.size() || readback.compare(0, length, text) != 0) {
    diagf("EFFECT_INDETERMINATE", "readback len=%lu want=%zu", length, text.size());
    return observed_edit(true, false, false);
  }
  if (!move_caret(entry.context, inserted_range.get(), ec)) {
    return observed_edit(true, true, false);
  }
  diagf("EFFECT_APPLIED", "via=insert len=%zu", text.size());
  return fusion::EditResult::AppliedReadback;
}

bool TextService::end_composition(ContextEntry &entry, TfEditCookie ec) {
  if (entry.composition == nullptr) {
    return true;
  }
  ComRef<ITfComposition> ending(entry.composition);
  ending->AddRef();
  const auto identity = qi<IUnknown>(ending.get(), IID_IUnknown);
  if (!identity || entry.self_end.identity)
    return false;
  entry.self_end = SelfEnd{entry.generation, identity.get()};
  const HRESULT hr = ending->EndComposition(ec);
  entry.self_end = {};
  if (FAILED(hr)) {
    diagf("COMPOSITION_END_FAILED", "hr=0x%08lX", static_cast<unsigned long>(hr));
    return false;
  }
  if (entry.composition == ending.get()) {
    entry.composition->Release();
    entry.composition = nullptr;
  }
  diagf("COMPOSITION_END", "cid=%" PRIu64, entry.semantic.context_id());
  return true;
}

bool TextService::clear_composition_text(ContextEntry &entry, TfEditCookie ec) {
  if (entry.composition == nullptr)
    return true;
  ComRef<ITfRange> range;
  if (FAILED(entry.composition->GetRange(range.out())) || !range ||
      FAILED(range->SetText(ec, 0, L"", 0)))
    return false;
  wchar_t readback = 0;
  ULONG length = 0;
  return SUCCEEDED(range->GetText(ec, 0, &readback, 1, &length)) && length == 0;
}

bool TextService::request_sync_session(ContextEntry &entry, ITfEditSession *session, HRESULT &outer,
                                       HRESULT &inner) {
  inner = E_FAIL;
  outer =
      entry.context->RequestEditSession(client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &inner);
  diagf("SYNC_REQUEST", "outer=0x%08lX inner=0x%08lX", static_cast<unsigned long>(outer),
        static_cast<unsigned long>(inner));
  return SUCCEEDED(outer) && SUCCEEDED(inner);
}

void TextService::schedule_async_pump(ContextEntry &entry) {
  if (entry.async_in_flight || entry.semantic.pending().empty() || entry.context == nullptr) {
    return;
  }
  // Ownership: the session starts with refcount 1; TSF adds its own reference
  // for the async queue, our Release drops the creation reference.
  auto *session = new AsyncPumpSession(*this, entry, entry.generation);
  HRESULT inner = E_FAIL;
  const HRESULT outer =
      entry.context->RequestEditSession(client_id_, session, TF_ES_ASYNC | TF_ES_READWRITE, &inner);
  session->Release();
  if (async_admitted(outer, inner)) {
    entry.async_in_flight = true;
    diagf("ASYNC_ADMITTED", "outer=0x%08lX inner=0x%08lX queue=%zu",
          static_cast<unsigned long>(outer), static_cast<unsigned long>(inner),
          entry.semantic.pending().size());
  } else {
    diagf("ASYNC_REFUSED", "outer=0x%08lX inner=0x%08lX", static_cast<unsigned long>(outer),
          static_cast<unsigned long>(inner));
  }
}

void TextService::note_async_done(ContextEntry &entry, bool progress) {
  entry.async_in_flight = false;
  if (progress && !entry.semantic.pending().empty()) {
    schedule_async_pump(entry);
  }
}

// ---------------------------------------------------------------------------
// Key routing
// ---------------------------------------------------------------------------

bool TextService::apply_frontend(ContextEntry &entry, TfEditCookie ec) {
  bool progress = false;
  // The callback bound also bounds one execution opportunity. Stop at the first
  // refusal/uncertainty: later callbacks must never overtake it.
  for (unsigned count = 0; count < frontend_operation_bound && !entry.semantic.pending().empty();
       ++count) {
    const PendingWork work = entry.semantic.pending().front();
    bool mutated = false;
    EditResult observed;
    if (work.kind == Kind::Effect) {
      auto ticket = entry.semantic.open_effect(work.key);
      if (!ticket)
        break;
      observed = apply_effect_text(entry, ec, widen(work.text), mutated);
      entry.semantic.settle_effect(*ticket, observed);
    } else {
      if (!entry.semantic.state_applicable(work.key))
        break;
      std::optional<PreeditPresentation> presentation;
      if (work.preedit)
        presentation = layout_preedit(work.text, *work.preedit);
      observed = apply_state_text(entry, ec, widen(work.text), mutated,
                                  presentation ? &*presentation : nullptr);
      entry.semantic.settle_front_state(work.key, observed);
    }
    if (GetEnvironmentVariableW(L"FUSION_TSF_FRONTEND_TRACE", nullptr, 0))
      diagf("FRONTEND_SETTLE", "seq=%" PRIu64 " observed=%d mutated=%d", work.key.sequence,
            static_cast<int>(observed), mutated ? 1 : 0);
    if (observed != EditResult::AppliedReadback)
      break;
    progress = true;
  }
  return progress;
}

void TextService::settle_frontend_work(ContextEntry &entry, const SemanticDecision &decision,
                                       BOOL *pfEaten) {
  for (const auto &work : decision.frontend) {
    if (GetEnvironmentVariableW(L"FUSION_TSF_FRONTEND_TRACE", nullptr, 0))
      diagf("FRONTEND_OP", "stimulus=%" PRIu64 " seq=%" PRIu64 " type=%d text=%s",
            decision.stimulus_id, work.key.sequence, static_cast<int>(work.operation.type),
            work.operation.text.c_str());
    if (work.operation.type == BackendOperation::Type::CommitText)
      entry.semantic.enqueue_effect(work.key, work.operation.text);
    else
      entry.semantic.enqueue_state(work.key, work.operation.text, work.operation.preedit);
  }
  if (!entry.semantic.pending().empty()) {
    auto *session = new FrontendEditSession(*this, entry);
    HRESULT outer = E_FAIL, inner = E_FAIL;
    request_sync_session(entry, session, outer, inner);
    session->Release();
    schedule_async_pump(entry);
  }
  if (decision.forward) {
    // Only the original physical key passes naturally, after its preceding
    // callbacks and prior effects have actually settled. A candidate action never
    // forwards (the engine consumed it), so the click path has no pfEaten to set.
    if (pfEaten == nullptr)
      return;
    if (entry.semantic.pending().empty() && entry.semantic.finish_pass())
      *pfEaten = FALSE;
    else
      cancel_current_key(entry, pfEaten);
  } else if (pfEaten != nullptr) {
    *pfEaten = TRUE;
  }
  entry.semantic.retire_terminals();
}

bool TextService::request_readonly_sync(ContextEntry &entry, ITfEditSession *session,
                                        HRESULT &outer, HRESULT &inner) {
  inner = E_FAIL;
  outer =
      entry.context->RequestEditSession(client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &inner);
  diagf("SYNC_READ", "outer=0x%08lX inner=0x%08lX", static_cast<unsigned long>(outer),
        static_cast<unsigned long>(inner));
  return SUCCEEDED(outer) && SUCCEEDED(inner);
}

void TextService::sync_candidate_view(ContextEntry &entry) {
  if (!entry.semantic.real_frontend())
    return;
  const auto page = entry.semantic.displayed_page();
  if (!page) {
    // No displayed projection: an empty page (hide signal) or a fenced dispatch.
    if (entry.candidates)
      entry.candidates->hide();
    return;
  }
  if (!entry.candidates)
    entry.candidates = std::make_unique<CandidateView>();
  if (!entry.candidates->ensure_created(this, &entry, entry.generation)) {
    diagf("CANDIDATE_VIEW", "create-failed cid=%" PRIu64, entry.semantic.context_id());
    return;
  }
  // Resolve the caret through a read-only session; position the popup after the
  // session returns (UI calls belong outside the edit session).
  auto *session = new AnchorProbeSession(*this, entry);
  HRESULT outer = E_FAIL, inner = E_FAIL;
  request_readonly_sync(entry, session, outer, inner);
  session->Release();
  if (entry.candidate_anchor_valid)
    entry.candidates->show(*page, entry.candidate_anchor);
  else
    entry.candidates->hide(); // never show a page we cannot place near its text
}

void TextService::on_candidate_hit(void *entry, std::uint64_t generation, CandidateAction action,
                                   std::int32_t index) {
  ContextEntry *target = find_entry_alive(entry, generation);
  if (target == nullptr) {
    diagf("FENCE", "kind=candidate stale-entry gen=%" PRIu64, generation);
    return;
  }
  if (handling_key_) {
    // Serialized on the UI thread, but a candidate action is never nested in a
    // key dispatch; refuse rather than re-enter ownership handling.
    diagf("CANDIDATE_REENTRANT", "index=%d", static_cast<int>(index));
    return;
  }
  auto lifetime_guard = hold_entry(target);
  handling_key_ = true;
  const SemanticDecision decision = target->semantic.begin_candidate_action(action, index);
  diagf("CANDIDATE_ACTION", "action=%d index=%d route=%s eaten=%d", static_cast<int>(action),
        static_cast<int>(index), route_name(decision.route), decision.eaten ? 1 : 0);
  if (decision.route != SemanticDecision::Route::FailOpen)
    settle_frontend_work(*target, decision, nullptr);
  sync_candidate_view(*target);
  handling_key_ = false;
}

void TextService::drain_prior_effect(ContextEntry &entry) {
  if (entry.semantic.real_frontend() && !entry.semantic.pending().empty()) {
    auto *session = new FrontendEditSession(*this, entry);
    HRESULT outer = E_FAIL, inner = E_FAIL;
    request_sync_session(entry, session, outer, inner);
    session->Release();
    schedule_async_pump(entry);
    return;
  }
  const fusion::Pass probe = entry.semantic.pass_probe();
  if (probe != fusion::Pass::Clear) {
    if (probe == fusion::Pass::DrainRequired) {
      // D072 §7.2 drain-before-PASS: attempt one synchronous drain of the
      // prior unresolved effect before letting this key pass.
      auto work = entry.semantic.drain_work();
      if (work && work->kind == fusion::Kind::Effect) {
        diagf("DRAIN", "key.seq=%" PRIu64, work->key.sequence);
        auto *session =
            new EffectEditSession(*this, entry, entry.generation, work->key, widen(work->text));
        HRESULT outer = E_FAIL;
        HRESULT inner = E_FAIL;
        const bool executed = request_sync_session(entry, session, outer, inner);
        session->Release();
        if (!executed) {
          diagf("DRAIN_REFUSED", "outer=0x%08lX inner=0x%08lX", static_cast<unsigned long>(outer),
                static_cast<unsigned long>(inner));
          schedule_async_pump(entry);
        }
      }
    } else {
      diagf("PASS_FENCE", "probe=%d", static_cast<int>(probe));
    }
  }
}

void TextService::handle_pass_key(ContextEntry &entry, const SemanticDecision &decision,
                                  BOOL *pfEaten) {
  drain_prior_effect(entry);
  if (entry.semantic.finish_pass()) {
    // Genuine PASS: projection cleanup only (never a semantic materializer).
    if (entry.composition != nullptr) {
      handle_reject_cleanup(entry); // projection-only cleanup, same bounded fallback
      if (entry.cleanup.pending()) {
        cancel_current_key(entry, pfEaten);
        return;
      }
    }
    diagf("PASS", "genuine symbolic=%s", decision.symbolic.c_str());
    *pfEaten = FALSE;
    return;
  }
  cancel_current_key(entry, pfEaten);
}

void TextService::cancel_current_key(ContextEntry &entry, BOOL *pfEaten) {
  // D072 §7.3: the key cannot lawfully pass; explicit CANCELLED recovery.
  const fusion::Key cancelled = entry.semantic.cancel_pass_recovery();
  if (cancelled.sequence != 0) {
    diagf("CANCELLED_RECOVERY", "key.seq=%" PRIu64, cancelled.sequence);
    *pfEaten = TRUE;
  } else {
    // The fixture reserves a recovery record and retires settled cancellations.
    // This branch is unreachable with its admission invariant; never label it PASS.
    diag::warn("PASS_FENCE_CAPACITY", "recovery-reserve-invariant-broken");
    *pfEaten = TRUE;
  }
}

void TextService::handle_owned_state(ContextEntry &entry, const SemanticDecision &decision) {
  if (!entry.semantic.pending().empty()) {
    entry.semantic.enqueue_state(*decision.key, decision.text);
    schedule_async_pump(entry);
    return;
  }
  auto *session =
      new StateEditSession(*this, entry, entry.generation, *decision.key, widen(decision.text));
  HRESULT outer = E_FAIL;
  HRESULT inner = E_FAIL;
  const bool executed = request_sync_session(entry, session, outer, inner);
  session->Release();
  if (!executed) {
    // Never fake synchronous success: queue the owned key for async execution.
    entry.semantic.enqueue_state(*decision.key, decision.text);
    schedule_async_pump(entry);
    diagf("STATE_ASYNC_FALLBACK", "key.seq=%" PRIu64, decision.key->sequence);
  }
}

void TextService::handle_owned_effect(ContextEntry &entry, const SemanticDecision &decision) {
  const fusion::Result proposed = entry.semantic.propose_effect(*decision.key);
  diagf("HOST_COMMIT", "key.seq=%" PRIu64 " settled=%d", decision.key->sequence,
        static_cast<int>(proposed));
  if (proposed != fusion::Result::Accepted) {
    // Ownership already taken: settle explicitly rather than pretend.
    const fusion::Result cancelled = entry.semantic.host().cancel(*decision.key);
    diagf("EFFECT_CANCELLED", "key.seq=%" PRIu64 " settled=%d", decision.key->sequence,
          static_cast<int>(cancelled));
    return;
  }
  auto *session =
      new EffectEditSession(*this, entry, entry.generation, *decision.key, widen(decision.text));
  HRESULT outer = E_FAIL;
  HRESULT inner = E_FAIL;
  const bool executed = request_sync_session(entry, session, outer, inner);
  session->Release();
  if (!executed) {
    entry.semantic.enqueue_effect(*decision.key, decision.text);
    schedule_async_pump(entry);
    diagf("EFFECT_ASYNC_FALLBACK", "key.seq=%" PRIu64, decision.key->sequence);
  }
}

void TextService::handle_reject_cleanup(ContextEntry &entry) {
  if (entry.composition == nullptr)
    return;
  if (!entry.cleanup.pending()) {
    entry.cleanup_identity = qi<IUnknown>(entry.composition, IID_IUnknown);
    if (!entry.cleanup_identity)
      return;
    entry.cleanup.retain(entry.generation, entry.cleanup_identity.get());
  }
  if (!entry.cleanup.request())
    return;
  const auto serial = entry.cleanup.serial();
  auto *session = new CleanupEditSession(*this, entry, entry.generation);
  HRESULT outer = E_FAIL, inner = E_FAIL;
  const bool executed = request_sync_session(entry, session, outer, inner);
  session->Release();
  if (!executed) {
    entry.cleanup.refused(serial);
    diagf("CLEANUP_DEFERRED", "outer=0x%08lX inner=0x%08lX", static_cast<unsigned long>(outer),
          static_cast<unsigned long>(inner));
    schedule_cleanup(entry);
  }
}

void TextService::schedule_cleanup(ContextEntry &entry) {
  if (!entry.cleanup.request())
    return;
  const auto serial = entry.cleanup.serial();
  auto *session = new CleanupEditSession(*this, entry, entry.generation);
  HRESULT inner = E_FAIL;
  const HRESULT outer =
      entry.context->RequestEditSession(client_id_, session, TF_ES_ASYNC | TF_ES_READWRITE, &inner);
  session->Release();
  if (!async_admitted(outer, inner))
    entry.cleanup.refused(serial); // intent remains; next real key provides another opportunity
  diagf("CLEANUP_ASYNC_REQUEST", "outer=0x%08lX inner=0x%08lX", static_cast<unsigned long>(outer),
        static_cast<unsigned long>(inner));
}

} // namespace fusion::tsf
