#include "fusion/backend.hpp"
#include "candidate_projection.hpp"
#include "startup_group.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcitx-utils/environ.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/addonloader.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <windows.h>

namespace fcitx {
extern FCITXUTILS_EXPORT HINSTANCE mainInstanceHandle;
}
namespace fusion {
namespace {
FCITX_DEFINE_STATIC_ADDON_REGISTRY(registry);
FCITX_IMPORT_ADDON_FACTORY(registry, pinyin);
FCITX_IMPORT_ADDON_FACTORY(registry, punctuation);
FCITX_IMPORT_ADDON_FACTORY(registry, pinyinhelper);
FCITX_IMPORT_ADDON_FACTORY(registry, quickphrase);
FCITX_IMPORT_ADDON_FACTORY(registry, spell);
FCITX_IMPORT_ADDON_FACTORY(registry, clipboard);
std::atomic<std::uint64_t> next_session{1};
std::once_flag paths_initialized;
constexpr std::size_t operation_bound = frontend_operation_bound;
constexpr std::size_t payload_bound = 4096;
constexpr std::size_t context_bound = 64;
// Transport bounds for one candidate page, not a semantic page size: the visible
// page width belongs to the engine (pinyin configures 7, constrained 3-10).
// D072 keeps numerical thresholds OPEN, so these are engineering constants and
// exceeding them is recorded by CandidatePage::truncated rather than hidden.
constexpr std::size_t candidate_page_entries = 10;
constexpr std::size_t candidate_page_bytes = 4096;
// Segment bound for one displayed preedit (after Text::normalize()); an
// engineering transport bound like the others, not a semantic limit.
constexpr std::size_t preedit_segment_bound = 16;
// Engineering bounds for this PoC only; D072 numerical policy remains OPEN.
constexpr auto request_wait = std::chrono::seconds(2);

inline bool is_candidate_action(BackendStimulus::Type type) {
  return type == BackendStimulus::Type::SelectCandidate ||
         type == BackendStimulus::Type::PageNext || type == BackendStimulus::Type::PagePrev;
}

class InputContext final : public fcitx::InputContext {
public:
  InputContext(fcitx::Instance &instance, Binding value)
      : fcitx::InputContext(instance.inputContextManager(), "fusion-stage3b"), binding(value) {
    instance_ = &instance;
    setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    created();
  }
  ~InputContext() override { destroy(); }
  const char *frontend() const override { return "fusion-stage3b"; }
  void commitStringImpl(const std::string &text) override {
    observe({BackendOperation::Type::CommitText, text, {}});
  }
  // The displayed client preedit: engine outputFilter, segments/format flags and
  // the engine byte cursor (project_client_preedit), never a bare toString().
  void updatePreeditImpl() override {
    if (lifecycle)
      return; // fenced bind/reset/close: observe() would drop it anyway
    auto projected = project_client_preedit(*instance_, this, payload_bound, preedit_segment_bound);
    if (!projected) {
      invalid = true;
      return;
    }
    BackendOperation operation{BackendOperation::Type::SetPreedit, std::move(projected->text)};
    operation.preedit = std::move(projected->format);
    observe(std::move(operation));
  }
  void forwardKeyImpl(const fcitx::ForwardKeyEvent &event) override {
    BackendOperation operation{BackendOperation::Type::ForwardKey, event.rawKey().toString(), {}};
    operation.original_key = event.rawKey() == dispatched_key && !event.isRelease();
    observe(std::move(operation));
  }
  void deleteSurroundingTextImpl(int, unsigned) override {
    // Unsupported materialization cannot be silently lost or executed by Fcitx.
    invalid = true;
  }
  void observe(BackendOperation operation) {
    if (lifecycle)
      return; // fenced bind/reset/close: no admitted key or document operation
    if (!batch || (!operation.text.empty() && !fcitx::utf8::validate(operation.text)) ||
        operation.text.size() > payload_bound || batch->operations.size() >= operation_bound) {
      invalid = true;
      return;
    }
    batch->operations.push_back(std::move(operation));
  }
  // Pull the engine's current candidate page and panel rows after a dispatch or a
  // candidate action. The snapshot is always emitted — including an explicitly
  // empty one — because Fcitx has no hide signal: an updated panel with nothing
  // to display IS the signal to close the presentation (InputPanel::empty()).
  void observe_candidate_page() {
    if (invalid)
      return;
    CandidatePage page;
    if (auto list = inputPanel().candidateList()) {
      auto projected = project_candidate_page(*instance_, this, list, candidate_page_entries,
                                              candidate_page_bytes, payload_bound);
      if (!projected) {
        invalid = true;
        return;
      }
      page = std::move(*projected);
    }
    // The panel rows of the same InputPanel update (auxUp / preedit / auxDown):
    // with them the snapshot can tell "no candidates" from "nothing to show".
    if (!project_panel_text(*instance_, this, page, payload_bound)) {
      invalid = true;
      return;
    }
    BackendOperation operation{BackendOperation::Type::CandidateSnapshot};
    operation.page = std::move(page);
    observe(std::move(operation));
  }
  // Candidate actions are the engine's own operations, executed on the Fcitx
  // thread against the *current* list. Index bounds are re-checked here because a
  // page can be rebuilt by select() (partial selection replaces the list).
  bool apply_candidate_action(const BackendStimulus &stimulus) {
    auto list = inputPanel().candidateList();
    if (!list)
      return false;
    switch (stimulus.type) {
    case BackendStimulus::Type::SelectCandidate:
      if (stimulus.candidate_index < 0 || stimulus.candidate_index >= list->size())
        return false;
      list->candidate(stimulus.candidate_index).select(this);
      return true;
    case BackendStimulus::Type::PageNext: {
      auto *pager = list->toPageable();
      if (!pager || !pager->hasNext())
        return false;
      pager->next();
      // Fcitx paging emits no signal of its own; the UI re-read is the contract.
      updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
      return true;
    }
    case BackendStimulus::Type::PagePrev: {
      auto *pager = list->toPageable();
      if (!pager || !pager->hasPrev())
        return false;
      pager->prev();
      updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
      return true;
    }
    default:
      return false;
    }
  }
  fcitx::Key dispatched_key;
  Binding binding;
  fcitx::Instance *instance_ = nullptr; // owned by the Host worker thread
  std::uint64_t last_stimulus = 0;
  BackendBatch *batch = nullptr;
  bool lifecycle = true;
  bool invalid = false;
};

class FcitxBackend final : public Backend {
public:
  explicit FcitxBackend(FcitxBackendOptions options)
      : session_(next_session.fetch_add(1)), options_(options) {
    auto ready = startup_.get_future();
    thread_ = std::thread([this] { run(); });
    if (ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready || !ready.get()) {
      shutdown();
      throw std::runtime_error("Fcitx Host initialization failed; inspect runtime resources");
    }
  }
  ~FcitxBackend() override { shutdown(); }
  bool healthy() const override { return healthy_.load(); }
  std::uint64_t session_id() const override { return session_; }
  bool bind(Binding binding) override {
    return invoke([this, binding] {
      if (contexts_.contains(binding.context) || contexts_.size() >= context_bound)
        return false;
      auto context = std::make_unique<InputContext>(*instance_, binding);
      context->focusIn();
      // The profile decides the input method (U1): the bound context starts in the
      // group's current input method (Items/0) as Fcitx resolves it, never in a
      // hard-coded name. DefaultIM is Fcitx's fallback, not the selection.
      const auto &group = instance_->inputMethodManager().currentGroup();
      const auto names = method_names(group);
      const auto method = selection::binding_input_method(
          names.empty() ? std::string() : names.front(), group.defaultInputMethod());
      if (!method.empty())
        instance_->setCurrentInputMethod(context.get(), method, true);
      context->lifecycle = false;
      contexts_.emplace(binding.context, std::move(context));
      return true;
    });
  }
  bool reset(Binding binding) override {
    return invoke([this, binding] {
      auto *context = find(binding);
      if (!context)
        return false;
      context->lifecycle = true;
      context->reset();
      context->invalid = false;
      context->lifecycle = false;
      return true;
    });
  }
  void unbind(Binding binding) override {
    (void)invoke([this, binding] {
      auto *context = find(binding);
      if (!context)
        return false;
      context->lifecycle = true;
      contexts_.erase(binding.context);
      return true;
    });
  }
  BackendBatch send(const BackendStimulus &stimulus) override {
    BackendBatch result{session_, stimulus.binding, stimulus.stimulus_id};
    auto output = std::make_shared<BackendBatch>(result);
    if (!invoke([this, stimulus, output] {
          auto *context = find(stimulus.binding);
          if (!context || context->invalid || stimulus.stimulus_id <= context->last_stimulus)
            return false;
          context->last_stimulus = stimulus.stimulus_id;
          context->batch = output.get();
          try {
            if (is_candidate_action(stimulus.type)) {
              // A non-key stimulus carries no key identity, so it is never a
              // Forward: either the engine performed the operation, or nothing was
              // admitted and the caller receives a Reject with an empty batch.
              if (!context->apply_candidate_action(stimulus)) {
                context->batch = nullptr;
                return false;
              }
              output->disposition = BackendBatch::Disposition::Consumed;
            } else {
              const auto name = key_name(stimulus);
              const auto key = stimulus.keysym
                                   ? fcitx::Key(static_cast<fcitx::KeySym>(stimulus.keysym),
                                                fcitx::KeyStates(stimulus.modifiers))
                                   : fcitx::Key(name);
              context->dispatched_key = key;
              fcitx::KeyEvent event(context, key, false);
              const bool consumed = context->keyEvent(event);
              output->disposition = consumed ? BackendBatch::Disposition::Consumed
                                             : BackendBatch::Disposition::Forward;
            }
            context->observe_candidate_page();
          } catch (...) {
            context->batch = nullptr;
            context->invalid = true;
            throw;
          }
          context->batch = nullptr;
          return !context->invalid;
        }))
      return result;
    return *output;
  }

private:
  static std::vector<std::string> method_names(const fcitx::InputMethodGroup &group) {
    std::vector<std::string> names;
    names.reserve(group.inputMethodList().size());
    for (const auto &item : group.inputMethodList()) {
      names.push_back(item.name());
    }
    return names;
  }
  // Fcitx owns the profile: with a profile the loaded group is kept verbatim, and
  // only a group that names no input method this runtime defines is replaced. With
  // no profile at all this is a first run, and the first-run default is built
  // explicitly (Pinyin default) instead of accepting Fcitx's built-in fallback,
  // which depends on load order and layout availability.
  static void ensure_usable_group(fcitx::Instance &instance) {
    auto &manager = instance.inputMethodManager();
    std::vector<std::string> available;
    for (const char *name : {"pinyin", "shuangpin"}) {
      if (manager.entry(name)) {
        available.emplace_back(name);
      }
    }
    const bool profile_present = !fcitx::StandardPaths::global()
                                      .locate(fcitx::StandardPathsType::PkgConfig, "profile")
                                      .empty();
    const auto &current = manager.currentGroup();
    const auto plan = selection::plan_startup_group(profile_present, current.defaultInputMethod(),
                                                    method_names(current), available);
    if (!plan.replace) {
      log_group(instance, profile_present, false);
      return;
    }
    auto group = current;
    group.inputMethodList().clear();
    for (const auto &name : plan.methods) {
      group.inputMethodList().emplace_back(name);
    }
    group.setDefaultInputMethod(plan.default_method);
    manager.setGroup(std::move(group));
    log_group(instance, profile_present, true);
  }
  // Development observable for the WP3 verification: which input method a bound
  // context will start in, and where the group came from. Selection itself stays
  // Fcitx's (the profile); this line never decides anything.
  static void log_group(const fcitx::Instance &instance, bool profile_present, bool replaced) {
    const auto &group = instance.inputMethodManager().currentGroup();
    std::string methods;
    for (const auto &item : group.inputMethodList()) {
      methods += (methods.empty() ? "" : ",") + item.name();
    }
    const auto current =
        group.inputMethodList().empty() ? std::string() : group.inputMethodList().front().name();
    std::fprintf(
        stderr,
        "[FusionHost] input method group: [%s] current=%s default=%s profile=%s source=%s\n",
        methods.c_str(), current.c_str(), group.defaultInputMethod().c_str(),
        profile_present ? "present" : "absent", replaced ? "first-run-default" : "loaded");
  }
  static std::string key_name(const BackendStimulus &stimulus) {
    switch (stimulus.type) {
    case BackendStimulus::Type::Character:
      return std::string(1, stimulus.character);
    case BackendStimulus::Type::Space:
      return "space";
    case BackendStimulus::Type::Backspace:
      return "BackSpace";
    case BackendStimulus::Type::Escape:
      return "Escape";
    case BackendStimulus::Type::Return:
      return "Return";
    case BackendStimulus::Type::SelectCandidate:
    case BackendStimulus::Type::PageNext:
    case BackendStimulus::Type::PagePrev:
      break; // not a key: apply_candidate_action handled it
    }
    throw std::runtime_error("unsupported backend stimulus");
  }
  InputContext *find(Binding binding) {
    auto found = contexts_.find(binding.context);
    return found != contexts_.end() && found->second->binding == binding ? found->second.get()
                                                                         : nullptr;
  }
  template <class Function> bool invoke(Function function) {
    // At most one admitted request, including timed-out work. Contending callers
    // reject immediately: no unbounded application queue or second causal ledger.
    std::unique_lock admission(admission_, std::try_to_lock);
    if (!admission || !healthy())
      return false;
    auto task = std::make_shared<std::packaged_task<bool()>>(std::move(function));
    auto result = task->get_future();
    {
      std::lock_guard dispatch(dispatch_);
      if (!instance_ || !healthy())
        return false;
      instance_->eventDispatcher().schedule([task] { (*task)(); });
    }
    if (result.wait_for(request_wait) != std::future_status::ready) {
      healthy_ = false; // timeout is a permanent fence for this PoC backend session
      return false;
    }
    try {
      return result.get();
    } catch (...) {
      healthy_ = false;
      return false;
    }
  }
  void run() noexcept {
    bool startup_sent = false;
    std::unique_ptr<fcitx::Instance> runtime;
    try {
      // Normal Windows built-in resource resolution must use the backend runtime
      // DLL location, never Notepad.exe's installation directory.
      std::call_once(paths_initialized, [user_path = options_.user_path] {
        fcitx::mainInstanceHandle = GetModuleHandleW(L"Fcitx5Utils.dll");
        // U1: the product Host persists through Fcitx's own per-user paths. A test
        // or fixture keeps the skipped path so it never reads or writes them.
        if (!user_path)
          fcitx::setEnvironment("SKIP_FCITX_USER_PATH", "1");
        fcitx::setEnvironment("SKIP_FCITX_SYSTEM_PATH", "1");
      });
      char program[] = "fusion-host", disable[] = "--disable=all";
      char enable[] = "--enable=pinyin,punctuation,pinyinhelper,quickphrase,spell,clipboard";
      char *argv[] = {program, disable, enable};
      runtime = std::make_unique<fcitx::Instance>(3, argv);
      auto &instance = *runtime;
      instance.addonManager().registerDefaultLoader(&registry());
      {
        std::lock_guard dispatch(dispatch_);
        instance_ = &instance;
      }
      instance.eventDispatcher().schedule([this, &instance, &startup_sent] {
        if (stopping_.load()) {
          if (!startup_sent) {
            try {
              startup_.set_value(false);
            } catch (...) {
            }
          }
          instance.exit();
          return;
        }
        if (stopping_.load()) {
          if (!startup_sent) {
            try {
              startup_.set_value(false);
            } catch (...) {
            }
          }
          instance.exit();
          return;
        }
        const bool available = instance.addonManager().addon("pinyin", true) != nullptr;
        if (available) {
          ensure_usable_group(instance);
          // Development observable for the WP3 verification: where Fcitx resolves its
          // per-user directories (U1 storage). The group line is printed by
          // ensure_usable_group together with where the group came from.
          const auto &paths = fcitx::StandardPaths::global();
          std::fprintf(stderr, "[FusionHost] fcitx user config: %s\n",
                       paths.userDirectory(fcitx::StandardPathsType::Config).string().c_str());
          std::fprintf(stderr, "[FusionHost] fcitx user data: %s\n",
                       paths.userDirectory(fcitx::StandardPathsType::Data).string().c_str());
        }
        healthy_ = available;
        startup_.set_value(available);
        startup_sent = true;
        if (!available)
          instance.exit(1);
      });
      instance.exec();
      healthy_ = false;
      for (auto &[id, context] : contexts_) {
        (void)id;
        context->lifecycle = true;
      }
      contexts_.clear();
      std::lock_guard dispatch(dispatch_);
      instance_ = nullptr;
      if (!startup_sent) {
        try {
          startup_.set_value(false);
        } catch (...) {
        }
      }
      if (!startup_sent) {
        try {
          startup_.set_value(false);
        } catch (...) {
        }
      }
    } catch (...) {
      healthy_ = false;
      std::lock_guard dispatch(dispatch_);
      instance_ = nullptr;
      for (auto &[id, context] : contexts_) {
        (void)id;
        context->lifecycle = true;
      }
      contexts_.clear();
      if (!startup_sent) {
        try {
          startup_.set_value(false);
        } catch (...) {
        }
      }
    }
  }
  void shutdown() {
    stopping_ = true;
    std::lock_guard admission(admission_);
    {
      std::lock_guard dispatch(dispatch_);
      if (instance_)
        instance_->eventDispatcher().schedule([this] {
          for (auto &[id, context] : contexts_) {
            (void)id;
            context->lifecycle = true;
          }
          contexts_.clear();
          instance_->exit();
        });
    }
    if (thread_.joinable())
      thread_.join();
  }
  std::uint64_t session_;
  const FcitxBackendOptions options_;
  std::atomic<bool> healthy_{false};
  std::atomic<bool> stopping_{false};
  std::mutex admission_, dispatch_;
  std::promise<bool> startup_;
  std::thread thread_;
  fcitx::Instance *instance_ = nullptr; // dispatch_ protects publication; worker owns it
  std::map<std::uint64_t, std::unique_ptr<InputContext>> contexts_;
};
} // namespace
std::unique_ptr<Backend> make_fcitx_backend(FcitxBackendOptions options) {
  return std::make_unique<FcitxBackend>(options);
}
} // namespace fusion
