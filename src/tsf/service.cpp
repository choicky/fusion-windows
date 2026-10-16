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

TextService::~TextService() { destroy_all_entries(); }

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
    // P0-D OPEN: FusionHost startup is external to the DLL.
    // Attempt one connection with a short timeout; fail open if unreachable.
    try {
      backend_ = fusion::ipc::make_ipc_backend(std::chrono::milliseconds(200));
      if (!backend_) {
        diagf("IPC_HOST_UNREACHABLE", "FusionHost not reachable, fail-open PASS");
        // Fail open: activate without backend, all keys will be PASS.
      }
    } catch (const std::exception &error) {
      diagf("IPC_HOST_CONNECT_FAILED", "%s, fail-open PASS", error.what());
      // Fail open: activate without backend.
    }
  }
#endif
  thread_mgr_ = ComRef<ITfThreadMgr>(ptim);
  thread_mgr_->AddRef();
  client_id_ = tid;
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
  // Initialize the real keyboard compartment. Windows can subsequently restore
  // closed mode; the fixture honors that value. Use the OS mode command (Ctrl+Space
  // on the validated machine) instead of overriding policy on each key.
  if (auto compartments = qi<ITfCompartmentMgr>(thread_mgr_.get(), IID_ITfCompartmentMgr)) {
    ComRef<ITfCompartment> compartment;
    if (SUCCEEDED(
            compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, compartment.out())) &&
        compartment) {
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_I4;
      value.lVal = 1;
      if (SUCCEEDED(compartment->SetValue(client_id_, &value))) {
        diagf("KEYBOARD_OPEN", "declared");
      }
      VariantClear(&value);
    }
  }
  diagf("ACTIVATE", "tid=%lu", static_cast<unsigned long>(tid));
  return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
  diagf("DEACTIVATE", "tid=%lu", static_cast<unsigned long>(client_id_));
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
  destroy_all_entries();
  client_id_ = TF_CLIENTID_NULL;
  // TSF contract: release the thread manager before returning.
  thread_mgr_.reset();
  return S_OK;
}

void TextService::destroy_all_entries() {
  for (auto &entry : entries_) {
    entry->semantic.destroy();
    entry->generation++;
  }
  entries_.clear();
}

// ---------------------------------------------------------------------------
// TextService — ITfKeyEventSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
  (void)fForeground;
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

  if (decision.backend_session_id)
    diagf("FCITX_BATCH", "session=%" PRIu64 " stimulus=%" PRIu64 " ops=%zu",
          decision.backend_session_id, decision.stimulus_id, decision.operation_count);
  if (entry->semantic.real_frontend() && decision.route != SemanticDecision::Route::FailOpen &&
      decision.route != SemanticDecision::Route::CancelledRecovery) {
    for (const auto &work : decision.frontend) {
      if (GetEnvironmentVariableW(L"FUSION_TSF_FRONTEND_TRACE", nullptr, 0))
        diagf("FRONTEND_OP", "stimulus=%" PRIu64 " seq=%" PRIu64 " type=%d text=%s",
              decision.stimulus_id, work.key.sequence, static_cast<int>(work.operation.type),
              work.operation.text.c_str());
      if (work.operation.type == BackendOperation::Type::CommitText)
        entry->semantic.enqueue_effect(work.key, work.operation.text);
      else
        entry->semantic.enqueue_state(work.key, work.operation.text);
    }
    if (!entry->semantic.pending().empty()) {
      auto *session = new FrontendEditSession(*this, *entry);
      HRESULT outer = E_FAIL, inner = E_FAIL;
      request_sync_session(*entry, session, outer, inner);
      session->Release();
      schedule_async_pump(*entry);
    }
    if (decision.forward) {
      // Only the original physical key passes naturally, after its preceding
      // callbacks and prior effects have actually settled. No replay.
      if (entry->semantic.pending().empty() && entry->semantic.finish_pass())
        *pfEaten = FALSE;
      else
        cancel_current_key(*entry, pfEaten);
    } else {
      *pfEaten = TRUE;
    }
    entry->semantic.retire_terminals();
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
  entry->semantic.retire_terminals();
  handling_key_ = false;
  return S_OK;
}

// ---------------------------------------------------------------------------
// TextService — ITfThreadMgrEventSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr *pdim) {
  (void)pdim;
  return S_OK;
}

STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr *pdim) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if ((*it)->document_mgr != nullptr && (*it)->document_mgr == pdim) {
      (*it)->semantic.destroy();
      (*it)->generation++;
      diagf("ENTRY_DESTROY", "cid=%" PRIu64, (*it)->semantic.context_id());
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr *pdimFocus, ITfDocumentMgr *pdimPrevFocus) {
  (void)pdimPrevFocus;
  diagf("FOCUS", "dim=%p", static_cast<void *>(pdimFocus));
  return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext *pic) {
  (void)pic;
  return S_OK;
}

STDMETHODIMP TextService::OnPopContext(ITfContext *pic) {
  ContextEntry *entry = find_entry(pic);
  if (entry) {
    entry->semantic.destroy();
    ++entry->generation;
    diagf("ENTRY_FENCE", "pop cid=%" PRIu64, entry->semantic.context_id());
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const auto &item) { return item.get() == entry; }),
                   entries_.end());
  }
  return S_OK;
}

// ---------------------------------------------------------------------------
// TextService — ITfCompositionSink
// ---------------------------------------------------------------------------

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ecWrite,
                                                  ITfComposition *pComposition) {
  (void)ecWrite;
  for (auto &entry : entries_) {
    const auto callback_identity = qi<IUnknown>(pComposition, IID_IUnknown);
    const auto live_identity = qi<IUnknown>(entry->composition, IID_IUnknown);
    if (callback_identity && callback_identity.get() == live_identity.get()) {
      if (entry->self_end.terminated(entry->generation, callback_identity.get(), [&] {
            entry->semantic.note_composition_terminated();
            entry->cleanup.reconciled();
          })) {
        diagf("COMPOSITION_SELF_END", "cid=%" PRIu64, entry->semantic.context_id());
        return S_OK;
      }
      // External authority event (D072 §9): reconcile the projection; queued
      // and pending work reaches explicit terminals, never silent loss.
      entry->composition->Release();
      entry->composition = nullptr;
      diagf("COMPOSITION_TERMINATED", "cid=%" PRIu64, entry->semantic.context_id());
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
  if (!backend_ || !backend_->healthy())
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
  return raw;
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
                                                 const std::wstring &text, bool &mutated) {
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
  if (!move_caret(entry.context, composition_range.get(), ec)) {
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
      observed = apply_state_text(entry, ec, widen(work.text), mutated);
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
