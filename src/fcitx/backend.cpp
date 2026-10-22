#include "fusion/backend.hpp"
#include "fusion/settings_protocol.hpp"
#include "candidate_projection.hpp"
#include "presentation_config.hpp"
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include "startup_group.hpp"
#include "im_list.hpp"
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
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/globalconfig.h>
#include <fcitx/instance.h>
#include <future>
#include <iterator>
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
constexpr char input_method_config_prefix[] = "fcitx://config/inputmethod/";
// The official Fcitx global-configuration URI (fcitx5 modules/dbus/dbusmodule.cpp
// globalConfigPath). It is served here with the same GetConfig/SetConfig messages
// and the same dumpDescription/save/load+safeSave+reloadConfig semantics as the
// D-Bus controller uses; no Fusion-specific schema is introduced.
constexpr char global_config_uri[] = "fcitx://config/global";
// The Windows candidate-presentation configuration. Owner authorization 2026-10-22: a
// Windows-specific presentation setting may have its own configuration, while official
// input semantics must not acquire duplicate stores. It is served with the same
// GetConfig/SetConfig messages and the same dumpDescription/safeSave/reload semantics as
// the three official URIs, and it is a normal Fcitx configuration stored in Fcitx's own
// per-user directory — not an additional configuration system. It holds presentation
// only: no Pinyin/Shuangpin value and no global value is repeated here.
constexpr char candidate_presentation_uri[] = "fcitx://config/candidate-presentation";

bool parse_supported_input_method_uri(const std::string &uri, std::string &name) {
  const std::string prefix = input_method_config_prefix;
  if (!uri.starts_with(prefix)) {
    return false;
  }
  name = uri.substr(prefix.size());
  return name == "pinyin" || name == "shuangpin";
}

bool is_global_config_uri(const std::string &uri) { return uri == global_config_uri; }

bool is_candidate_presentation_uri(const std::string &uri) {
  return uri == candidate_presentation_uri;
}

// The stored preference and the transport enum are two vocabularies for one decision: the
// configuration persists Horizontal/Vertical, while CandidatePage::layout mirrors
// fcitx::CandidateLayoutHint. Converting in one place keeps the stored value from ever
// becoming a second layout model.
CandidateLayout candidate_layout_for(CandidateLayoutPreference preference) {
  return preference == CandidateLayoutPreference::Horizontal ? CandidateLayout::Horizontal
                                                             : CandidateLayout::Vertical;
}

// The font the popup draws with, taken from the same stored configuration. Kept as one
// immutable value published whole, so a page can never carry a family from one edit and a
// size from another.
std::shared_ptr<const CandidateFont> candidate_font_for(const CandidatePresentationConfig &config) {
  auto font = std::make_shared<CandidateFont>();
  font->family = config.font_family.value();
  font->size_pt = config.font_size.value();
  return font;
}

// Convert the public RawConfig tree to the same string/map shape used by Fcitx's
// Controller1.GetConfig. An empty map key carries a node's own scalar value.
SettingsValue raw_config_to_settings_value(const fcitx::RawConfig &config) {
  if (!config.hasSubItems()) {
    return SettingsValue::string(config.value());
  }
  SettingsValue result = SettingsValue::object();
  if (!config.value().empty()) {
    result.entries.emplace("", SettingsValue::string(config.value()));
  }
  for (const auto &name : config.subItems()) {
    if (auto child = config.get(name)) {
      result.entries.emplace(name, raw_config_to_settings_value(*child));
    }
  }
  return result;
}

bool settings_value_fill_raw_config(const SettingsValue &value, fcitx::RawConfig &config) {
  if (!value.is_map) {
    config.setValue(value.scalar);
    return true;
  }
  for (const auto &[name, childValue] : value.entries) {
    if (name.empty()) {
      if (childValue.is_map) {
        return false;
      }
      config.setValue(childValue.scalar);
      continue;
    }
    auto child = config.get(name, true);
    if (!child || !settings_value_fill_raw_config(childValue, *child)) {
      return false;
    }
  }
  return true;
}

SettingsValue merge_opaque_map(const SettingsValue &current, const SettingsValue &submitted) {
  if (!current.is_map || !submitted.is_map) {
    return submitted;
  }
  SettingsValue merged = current;
  for (const auto &[key, child] : submitted.entries) {
    const auto found = merged.entries.find(key);
    if (found != merged.entries.end() && found->second.is_map && child.is_map) {
      found->second = merge_opaque_map(found->second, child);
    } else {
      merged.entries[key] = child;
    }
  }
  return merged;
}

const SettingsConfigType *find_config_type(const std::vector<SettingsConfigType> &types,
                                           const std::string &name) {
  const auto found = std::find_if(types.begin(), types.end(), [&name](const auto &type) {
    return type.name == name;
  });
  return found == types.end() ? nullptr : &*found;
}

// ConfigWidget serializes only options it knows. Merge at schema-defined paths
// onto the live RawConfig so unexposed/future fields survive. List/Optional nodes
// replace their whole value tree, which is necessary for list deletions and an
// unchecked Optional value; ordinary nested config types merge recursively.
bool merge_schema_values(const std::vector<SettingsConfigType> &types,
                         const std::string &typeName, const SettingsValue &submitted,
                         SettingsValue &merged, std::string &error, std::size_t depth = 0) {
  if (depth > fusion::ipc::max_settings_depth || !submitted.is_map || !merged.is_map) {
    error = "The submitted configuration does not match the Fcitx schema.";
    return false;
  }
  const auto *type = find_config_type(types, typeName);
  if (!type) {
    error = "Fcitx returned a nested configuration type the settings client could not resolve.";
    return false;
  }
  for (const auto &[name, submittedValue] : submitted.entries) {
    const auto option = std::find_if(type->options.begin(), type->options.end(),
                                     [&name](const auto &candidate) {
                                       return candidate.name == name;
                                     });
    if (option == type->options.end() || option->type == "External") {
      error = "The settings client submitted an option not represented by the current Fcitx schema.";
      return false;
    }
    auto currentValue = merged.entries.find(name);
    if (option->type.starts_with("Optional|")) {
      if (!submittedValue.is_map) {
        error = "An optional Fcitx option was submitted as a scalar.";
        return false;
      }
      if (submittedValue.entries.empty()) {
        merged.entries[name] = submittedValue; // the UI disabled this Optional option
        continue;
      }
      const std::string nestedType = option->type.substr(std::string("Optional|").size());
      if (find_config_type(types, nestedType)) {
        if (currentValue == merged.entries.end()) {
          currentValue = merged.entries.emplace(name, SettingsValue::object()).first;
        }
        SettingsValue nestedSubmitted = submittedValue;
        auto submittedChild = nestedSubmitted.entries.find("Value");
        SettingsValue nestedCurrent = SettingsValue::object();
        if (currentValue->second.is_map) {
          const auto currentChild = currentValue->second.entries.find("Value");
          if (currentChild != currentValue->second.entries.end()) {
            nestedCurrent = currentChild->second;
          }
        }
        if (submittedChild == nestedSubmitted.entries.end() || !nestedSubmitted.is_map ||
            !merge_schema_values(types, nestedType, submittedChild->second, nestedCurrent,
                                 error, depth + 1)) {
          return false;
        }
        nestedSubmitted.entries["Value"] = std::move(nestedCurrent);
        merged.entries[name] = std::move(nestedSubmitted);
      } else {
        merged.entries[name] = merge_opaque_map(
            currentValue == merged.entries.end() ? SettingsValue::object() : currentValue->second,
            submittedValue);
      }
      continue;
    }
    if (option->type.starts_with("List|")) {
      if (!submittedValue.is_map || submittedValue.entries.size() > fusion::ipc::max_settings_map_entries) {
        error = "A list-valued Fcitx option has an invalid value.";
        return false;
      }
      const std::string elementType = option->type.substr(std::string("List|").size());
      SettingsValue mergedList = SettingsValue::object();
      const auto &currentList = currentValue != merged.entries.end() && currentValue->second.is_map
                                    ? currentValue->second
                                    : SettingsValue::object();
      for (std::size_t index = 0; index < submittedValue.entries.size(); ++index) {
        const std::string indexKey = std::to_string(index);
        const auto submittedItem = submittedValue.entries.find(indexKey);
        if (submittedItem == submittedValue.entries.end()) {
          error = "A list-valued Fcitx option has a non-contiguous index.";
          return false;
        }
        const auto currentItem = currentList.entries.find(indexKey);
        SettingsValue item = currentItem == currentList.entries.end()
                                 ? (submittedItem->second.is_map ? SettingsValue::object()
                                                                 : submittedItem->second)
                                 : currentItem->second;
        if (find_config_type(types, elementType) && submittedItem->second.is_map) {
          if (!merge_schema_values(types, elementType, submittedItem->second, item, error,
                                   depth + 1)) {
            return false;
          }
        } else if (submittedItem->second.is_map) {
          item = merge_opaque_map(item, submittedItem->second);
        } else {
          item = submittedItem->second;
        }
        mergedList.entries.emplace(indexKey, std::move(item));
      }
      merged.entries[name] = std::move(mergedList); // preserve edits/deletions/order from UI
      continue;
    }
    if (find_config_type(types, option->type)) {
      if (!submittedValue.is_map) {
        error = "A nested Fcitx configuration option had a scalar value.";
        return false;
      }
      if (currentValue == merged.entries.end()) {
        currentValue = merged.entries.emplace(name, SettingsValue::object()).first;
      }
      if (!merge_schema_values(types, option->type, submittedValue, currentValue->second,
                               error, depth + 1)) {
        return false;
      }
      continue;
    }
    if (submittedValue.is_map) {
      error = "A scalar Fcitx option was submitted as a map.";
      return false;
    }
    merged.entries[name] = submittedValue;
  }
  return true;
}

std::vector<SettingsConfigType> dump_settings_description(const fcitx::Configuration &config) {
  fcitx::RawConfig rawDescription;
  config.dumpDescription(rawDescription);
  std::vector<SettingsConfigType> result;
  for (const auto &typeName : rawDescription.subItems()) {
    auto typeConfig = rawDescription.get(typeName);
    if (!typeConfig) {
      continue;
    }
    SettingsConfigType type;
    type.name = typeName;
    for (const auto &optionName : typeConfig->subItems()) {
      auto optionConfig = typeConfig->get(optionName);
      if (!optionConfig) {
        continue;
      }
      auto typeField = optionConfig->get("Type");
      auto descriptionField = optionConfig->get("Description");
      if (!typeField || !descriptionField) {
        continue;
      }
      SettingsOptionDescription option;
      option.name = optionName;
      option.type = typeField->value();
      option.description = descriptionField->value();
      if (auto defaultField = optionConfig->get("DefaultValue")) {
        option.default_value = raw_config_to_settings_value(*defaultField);
      }
      option.properties = SettingsValue::object();
      optionConfig->visitSubItems([&option](const fcitx::RawConfig &value,
                                            const std::string &path) {
        if (path == "Type" || path == "Description" || path == "DefaultValue") {
          return true;
        }
        option.properties.entries.emplace(path, raw_config_to_settings_value(value));
        return true;
      });
      type.options.push_back(std::move(option));
    }
    result.push_back(std::move(type));
  }
  return result;
}

inline bool is_candidate_action(BackendStimulus::Type type) {
  return type == BackendStimulus::Type::SelectCandidate ||
         type == BackendStimulus::Type::PageNext || type == BackendStimulus::Type::PagePrev;
}
// Whether a stimulus carries a character, i.e. is something an input method would compose
// from. Everything else — function keys, bare modifiers, navigation and editing keys — is a
// candidate for the engine's own global hotkeys and stays the engine's business even while
// the context is inactive.
inline bool stimulus_carries_character(const BackendStimulus &stimulus) {
  if (stimulus.type == BackendStimulus::Type::Character ||
      stimulus.type == BackendStimulus::Type::Space)
    return true;
  return (stimulus.keysym >= 0x20 && stimulus.keysym < 0x7f) || stimulus.keysym >= 0x01000000u;
}

class InputContext final : public fcitx::InputContext {
public:
  InputContext(fcitx::Instance &instance, Binding value,
               const std::atomic<CandidateLayout> &layout_preference,
               const std::atomic<std::shared_ptr<const CandidateFont>> &font)
      : fcitx::InputContext(instance.inputContextManager(), "fusion-stage3b"), binding(value),
        layout_preference_(layout_preference), font_(font) {
    instance_ = &instance;
    setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    created();
  }
  ~InputContext() override { destroy(); }
  const char *frontend() const override { return "fusion-stage3b"; }
  // Fcitx hands "an inactive context forwards keys to the application" to the inactive
  // keyboard input method, and this runtime compiles none. The frontend half of that same
  // rule lives here: once the engine has been active for this context and reports it
  // inactive again, an ordinary key is the application's, and only the modifier-only keys
  // stay in the engine because a release of one is what can toggle the context back
  // (`Hotkey/AltTriggerKeys`). No state is invented: the answer is Fcitx's own, read per
  // dispatch, and `ever_active_` only keeps a brand-new context's first key going to the
  // engine, which is what activates it.
  // Fcitx hands "an inactive context forwards keys to the application" to the keyboard
  // input method, and this runtime compiles none. The frontend half of that rule lives
  // here: once the engine has been active for this context and reports it inactive again,
  // a key that carries a character is the application's and must not reach an input method
  // (which would start composing again).
  //
  // "English" is not "Fcitx stops seeing keys". The engine's own global keys —
  // `Hotkey/TriggerKeys`, `ActivateKeys`, the layout hots — are handled before any input
  // method and deliberately do not require an active context; that is exactly how a
  // configured key turns the input method back on. So character-less keys are still
  // dispatched and the engine decides: a bound key consumes them, anything else comes back
  // as Forward with nothing composed. No second matcher, no hard-coded key.
  bool inactive_passthrough(const BackendStimulus &stimulus) {
    if (!instance_ || !ever_active_)
      return false;
    // Instance::state() describes the most recent input context. Read it only when that
    // is this context, so a background context never inherits another one's state.
    if (instance_->inputContextManager().mostRecentInputContext() != this)
      return false;
    if (instance_->state() != 1) // 2 = active, 1 = inactive, 0 = no context at all
      return false;
    return stimulus_carries_character(stimulus);
  }
  // Record that the engine has been active here. Called after every dispatch, so the
  // transition is observed rather than predicted.
  void note_engine_state() {
    if (!instance_ || instance_->inputContextManager().mostRecentInputContext() != this)
      return;
    if (instance_->state() == 2)
      ever_active_ = true;
  }
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
      auto projected = project_candidate_page(
          *instance_, this, list, layout_preference_.load(std::memory_order_relaxed),
          candidate_page_entries, candidate_page_bytes, payload_bound);
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
    // The font is presentation, so it travels with the page it is drawn on. A page built
    // without one (Mock, tests) leaves the fields empty and the popup uses its default.
    if (auto font = font_.load())
      page.font = *font;
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
  // The Host's one stored presentation preference, read at projection time. It lives in
  // the backend and outlives every context, so a save is visible to the next projection
  // of every context without any per-context copy that could drift.
  const std::atomic<CandidateLayout> &layout_preference_;
  // The font the same configuration carries, for the popup's drawing. Read once per
  // projected page; the backend publishes a new immutable value on a save.
  const std::atomic<std::shared_ptr<const CandidateFont>> &font_;
  std::uint64_t last_stimulus = 0;
  BackendBatch *batch = nullptr;
  bool lifecycle = true;
  bool invalid = false;
  // Set once the engine has been seen active for this context (see
  // inactive_passthrough): a brand-new context's first key must reach the engine, because
  // reaching it is what activates the context.
  bool ever_active_ = false;
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
      auto context =
          std::make_unique<InputContext>(*instance_, binding, candidate_layout_, candidate_font_);
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
      // Fusion's "Chinese is on" is Fcitx's own active state, and a context that was never
      // activated has none of the modifier-only bindings available (Hotkey/AltTriggerKeys
      // toggles from active to inactive and back; it does not start from nothing). Declare
      // it through the frontend entry point — the same one fcitx5-remote uses — instead of
      // keeping an active flag of our own. The context was just focused above, so the
      // no-argument form addresses this one.
      instance_->activate();
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
              // A stimulus that carries a keysym is identified by it; the symbolic name is
              // only the fallback for the semantic key classes.
              const auto key =
                  stimulus.keysym
                      ? fcitx::Key(static_cast<fcitx::KeySym>(stimulus.keysym),
                                   fcitx::KeyStates(stimulus.modifiers))
                      : fcitx::Key(key_name(stimulus));
              if (!context->inactive_passthrough(stimulus)) {
                context->dispatched_key = key;
                fcitx::KeyEvent event(context, key,
                                      stimulus.type == BackendStimulus::Type::KeyRelease);
                const bool consumed = context->keyEvent(event);
                output->disposition = consumed ? BackendBatch::Disposition::Consumed
                                               : BackendBatch::Disposition::Forward;
              } else {
                // The engine is inactive for this context, so the key is the
                // application's: nothing was composed, nothing is forwarded twice.
                output->disposition = BackendBatch::Disposition::Forward;
              }
            }
            context->observe_candidate_page();
            context->note_engine_state();
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

  SettingsStatus settings_get_config(const std::string &uri, SettingsSnapshot &snapshot,
                                     std::string &error) override {
    if (is_global_config_uri(uri)) {
      auto result = std::make_shared<SettingsSnapshot>();
      const auto status =
          invoke_settings([this, result](std::string &taskError) {
                          return read_global_settings_on_loop(*result, taskError);
                        },
                        error, SettingsStatus::Timeout);
      if (status == SettingsStatus::Ok) {
        snapshot = std::move(*result);
      }
      return status;
    }
    if (is_candidate_presentation_uri(uri)) {
      auto result = std::make_shared<SettingsSnapshot>();
      const auto status =
          invoke_settings([this, result](std::string &taskError) {
                          return read_presentation_settings_on_loop(*result, taskError);
                        },
                        error, SettingsStatus::Timeout);
      if (status == SettingsStatus::Ok) {
        snapshot = std::move(*result);
      }
      return status;
    }
    std::string method;
    if (!parse_supported_input_method_uri(uri, method)) {
      error = "Only the Global, candidate-presentation and Pinyin/Shuangpin input-method "
              "settings are available.";
      return SettingsStatus::Invalid;
    }
    auto result = std::make_shared<SettingsSnapshot>();
    const auto status = invoke_settings(
        [this, method, result](std::string &taskError) {
          return read_settings_on_loop(method, *result, taskError);
        },
        error, SettingsStatus::Timeout);
    if (status == SettingsStatus::Ok) {
      snapshot = std::move(*result);
    }
    return status;
  }

  SettingsStatus settings_set_config(const std::string &uri, const SettingsValue &submitted,
                                     SettingsSnapshot &snapshot, std::string &error) override {
    if (!submitted.is_map) {
      error = "The settings request is invalid.";
      return SettingsStatus::Invalid;
    }
    if (is_global_config_uri(uri)) {
      auto result = std::make_shared<SettingsSnapshot>();
      const auto status =
          invoke_settings([this, submitted, result](std::string &taskError) {
                            return apply_global_settings_on_loop(submitted, *result, taskError);
                          },
                          error, SettingsStatus::Indeterminate);
      if (status == SettingsStatus::Ok) {
        snapshot = std::move(*result);
      }
      return status;
    }
    if (is_candidate_presentation_uri(uri)) {
      auto result = std::make_shared<SettingsSnapshot>();
      const auto status =
          invoke_settings([this, submitted, result](std::string &taskError) {
                            return apply_presentation_settings_on_loop(submitted, *result,
                                                                      taskError);
                          },
                          error, SettingsStatus::Indeterminate);
      if (status == SettingsStatus::Ok) {
        snapshot = std::move(*result);
      }
      return status;
    }
    std::string method;
    if (!parse_supported_input_method_uri(uri, method)) {
      error = "The settings request is invalid.";
      return SettingsStatus::Invalid;
    }
    auto result = std::make_shared<SettingsSnapshot>();
    const auto status = invoke_settings(
        [this, method, submitted, result](std::string &taskError) {
          return apply_settings_on_loop(method, submitted, *result, taskError);
        },
        error, SettingsStatus::Indeterminate);
    if (status == SettingsStatus::Ok) {
      snapshot = std::move(*result);
    }
    return status;
  }

  SettingsStatus settings_get_selection(SettingsSelection &selection,
                                        std::string &error) override {
    auto result = std::make_shared<SettingsSelection>();
    const auto status = invoke_settings(
        [this, result](std::string &taskError) { return selection_on_loop(*result, taskError); },
        error, SettingsStatus::Timeout);
    if (status == SettingsStatus::Ok) {
      selection = std::move(*result);
    }
    return status;
  }

  SettingsStatus settings_select_input_method(const std::string &method,
                                              SettingsSelection &selection,
                                              std::string &error) override {
    if (method != "pinyin" && method != "shuangpin") {
      error = "Only Pinyin and Shuangpin can be selected.";
      return SettingsStatus::Invalid;
    }
    auto result = std::make_shared<SettingsSelection>();
    const auto status = invoke_settings(
        [this, method, result](std::string &taskError) {
          auto &manager = instance_->inputMethodManager();
          if (!manager.entry(method)) {
            taskError = "The requested input method is not available in this Host.";
            return SettingsStatus::Unsupported;
          }
          const auto &liveGroup = manager.currentGroup();
          const std::string liveCurrent = liveGroup.inputMethodList().empty()
                                              ? std::string()
                                              : liveGroup.inputMethodList().front().name();
          if (liveCurrent == method) {
            return selection_on_loop(*result, taskError);
          }
          if (has_active_composition_on_loop()) {
            taskError = "Finish the current composition, then retry the input-method switch.";
            return SettingsStatus::Busy;
          }
          auto group = liveGroup;
          auto &items = group.inputMethodList();
          const auto oldCurrent = items.empty() ? std::string() : items.front().name();
          auto target = std::find_if(items.begin(), items.end(), [&method](const auto &item) {
            return item.name() == method;
          });
          if (target == items.end()) {
            items.emplace_back(method);
            target = std::prev(items.end());
          }
          if (target != items.begin()) {
            std::rotate(items.begin(), target, std::next(target));
          }
          // Preserve every existing group entry, its layout, and the old current
          // input method as the fallback. Items/0 remains the selected method.
          const std::string fallback = oldCurrent.empty() || oldCurrent == method
                                           ? (items.size() > 1 ? items[1].name() : method)
                                           : oldCurrent;
          group.setDefaultInputMethod(fallback);
          manager.setGroup(std::move(group));
          manager.save();
          return selection_on_loop(*result, taskError);
        },
        error, SettingsStatus::Indeterminate);
    if (status == SettingsStatus::Ok) {
      selection = std::move(*result);
    }
    return status;
  }

  SettingsStatus settings_get_im_list(ImList &list, std::string &error) override {
    auto result = std::make_shared<ImList>();
    const auto status = invoke_settings(
        [this, result](std::string &taskError) { return im_list_on_loop(*result, taskError); },
        error, SettingsStatus::Timeout);
    if (status == SettingsStatus::Ok) {
      list = std::move(*result);
    }
    return status;
  }

  SettingsStatus settings_set_im_list(const std::vector<std::string> &enabled, ImList &list,
                                      std::string &error) override {
    auto result = std::make_shared<ImList>();
    const auto status = invoke_settings(
        [this, enabled, result](std::string &taskError) {
          return apply_im_list_on_loop(enabled, *result, taskError);
        },
        error, SettingsStatus::Indeterminate);
    if (status == SettingsStatus::Ok) {
      list = std::move(*result);
    }
    return status;
  }

private:
  enum class SettingsTaskPhase : int { Queued, Running, Cancelled, Done };
  struct SettingsTaskState {
    std::atomic<SettingsTaskPhase> phase{SettingsTaskPhase::Queued};
    std::promise<void> completed;
    SettingsStatus status = SettingsStatus::Error;
    std::string error;
  };

  SettingsStatus invoke_settings(const std::function<SettingsStatus(std::string &)> &function,
                                 std::string &error, SettingsStatus running_timeout_status) {
    bool expected = false;
    if (!settings_pending_.compare_exchange_strong(expected, true)) {
      error = "Another settings request is already in progress.";
      return SettingsStatus::Busy;
    }
    if (!healthy()) {
      settings_pending_ = false;
      error = "The Fusion Host backend is unavailable.";
      return SettingsStatus::Error;
    }

    auto task = std::make_shared<SettingsTaskState>();
    auto ready = task->completed.get_future();
    {
      std::lock_guard dispatch(dispatch_);
      if (!instance_ || !healthy()) {
        settings_pending_ = false;
        error = "The Fusion Host backend is shutting down.";
        return SettingsStatus::Error;
      }
      instance_->eventDispatcher().schedule([this, task, function] {
        auto phase = SettingsTaskPhase::Queued;
        if (!task->phase.compare_exchange_strong(phase, SettingsTaskPhase::Running)) {
          // The requester cancelled before this callback reached the loop; no
          // Fcitx touch or late settings mutation is permitted.
          settings_pending_ = false;
          return;
        }
        try {
          task->status = function(task->error);
        } catch (const std::exception &exception) {
          task->status = SettingsStatus::Error;
          task->error = exception.what();
        } catch (...) {
          task->status = SettingsStatus::Error;
          task->error = "The Fcitx settings operation failed.";
        }
        task->phase = SettingsTaskPhase::Done;
        try {
          task->completed.set_value();
        } catch (...) {
        }
        settings_pending_ = false;
      });
    }

    if (ready.wait_for(request_wait) != std::future_status::ready) {
      auto phase = SettingsTaskPhase::Queued;
      if (task->phase.compare_exchange_strong(phase, SettingsTaskPhase::Cancelled)) {
        error = "The settings request timed out before it reached the Fcitx event loop.";
        return SettingsStatus::Timeout;
      }
      if (task->phase == SettingsTaskPhase::Running) {
        error = "The Host did not confirm whether the settings request completed. Reload before retrying.";
        // Keep settings_pending_ set until the event-loop task completes, fencing
        // retries while a prior mutation may still be running.
        return running_timeout_status;
      }
      if (task->phase == SettingsTaskPhase::Done) {
        ready.wait(); // the loop published Done immediately before fulfilling the promise
      }
    }
    error = task->error;
    return task->status;
  }

  bool has_active_composition_on_loop() const {
    for (const auto &[id, context] : contexts_) {
      (void)id;
      if (!context->inputPanel().clientPreedit().empty() || !context->inputPanel().empty() ||
          context->hasPendingEventsStrictOrder()) {
        return true;
      }
    }
    return false;
  }

  SettingsStatus read_global_settings_on_loop(SettingsSnapshot &snapshot, std::string &error) {
    const auto &global = instance_->globalConfig();
    fcitx::RawConfig values;
    global.save(values);
    snapshot.values = raw_config_to_settings_value(values);
    snapshot.types = dump_settings_description(global.config());
    (void)error;
    return SettingsStatus::Ok;
  }

  // The official D-Bus controller apply sequence for fcitx://config/global
  // (dbusmodule.cpp:425-432): partial load onto the live GlobalConfig, safeSave, and
  // reloadConfig only if the save succeeded. The submitted tree is merged onto the
  // current values first (same delta/no-change path as an input-method page), so a
  // partial load never resets an unexposed option to its default, and a no-change
  // Apply calls neither load nor safeSave nor reloadConfig and touches no file.
  SettingsStatus apply_global_settings_on_loop(const SettingsValue &submitted,
                                               SettingsSnapshot &snapshot, std::string &error) {
    SettingsSnapshot current;
    if (const auto status = read_global_settings_on_loop(current, error);
        status != SettingsStatus::Ok) {
      return status;
    }
    if (current.types.empty()) {
      error = "Fcitx returned no schema for the global configuration.";
      return SettingsStatus::Error;
    }
    SettingsValue merged = current.values;
    if (!merge_schema_values(current.types, current.types.front().name, submitted, merged,
                             error)) {
      return SettingsStatus::Invalid;
    }
    if (merged == current.values) {
      snapshot = std::move(current);
      return SettingsStatus::Ok;
    }
    if (has_active_composition_on_loop()) {
      error = "Finish the current composition, then retry applying the global settings.";
      return SettingsStatus::Busy;
    }
    fcitx::RawConfig raw;
    if (!settings_value_fill_raw_config(merged, raw)) {
      error = "The global settings value tree is malformed.";
      return SettingsStatus::Invalid;
    }
    instance_->globalConfig().load(raw, /*partial=*/true);
    if (!instance_->globalConfig().safeSave()) {
      error = "Fcitx could not save the global configuration; reload before retrying.";
      return SettingsStatus::Error;
    }
    instance_->reloadConfig();
    return read_global_settings_on_loop(snapshot, error);
  }

  // The candidate-presentation configuration follows the same sequence as the global one,
  // minus reloadConfig: no addon caches this value, so the save plus the in-memory update
  // below is the whole apply. Nothing here touches the engine, and the merge/no-change
  // path is shared, so an Apply with nothing changed writes no file.
  SettingsStatus read_presentation_settings_on_loop(SettingsSnapshot &snapshot,
                                                    std::string &error) {
    fcitx::RawConfig values;
    presentation_config_.save(values);
    snapshot.values = raw_config_to_settings_value(values);
    snapshot.types = dump_settings_description(presentation_config_);
    (void)error;
    return SettingsStatus::Ok;
  }

  SettingsStatus apply_presentation_settings_on_loop(const SettingsValue &submitted,
                                                     SettingsSnapshot &snapshot,
                                                     std::string &error) {
    SettingsSnapshot current;
    if (const auto status = read_presentation_settings_on_loop(current, error);
        status != SettingsStatus::Ok) {
      return status;
    }
    if (current.types.empty()) {
      error = "Fcitx returned no schema for the candidate-presentation configuration.";
      return SettingsStatus::Error;
    }
    SettingsValue merged = current.values;
    if (!merge_schema_values(current.types, current.types.front().name, submitted, merged,
                             error)) {
      return SettingsStatus::Invalid;
    }
    if (merged == current.values) {
      snapshot = std::move(current);
      return SettingsStatus::Ok;
    }
    // Refused while a composition is live for the same reason the other pages are: the
    // candidate window is already on screen, and moving it mid-composition would relocate
    // the panel under the user's pointer.
    if (has_active_composition_on_loop()) {
      error = "Finish the current composition, then retry applying the candidate layout.";
      return SettingsStatus::Busy;
    }
    fcitx::RawConfig raw;
    if (!settings_value_fill_raw_config(merged, raw)) {
      error = "The candidate-presentation settings value tree is malformed.";
      return SettingsStatus::Invalid;
    }
    // The write happens against a throwaway configuration, and the value is published to the
    // live object only after the file is safely on disk. Loading the live object first and
    // saving afterwards would let a failed write leave the Host reporting a value it never
    // persisted: GetConfig would return the new layout, the projection would still use the
    // old one, and — worse — resubmitting the same change would then match the "nothing
    // changed" branch and report success without writing anything. Ordering the publish
    // after the write removes that state instead of trying to roll it back.
    //
    // A fresh object is used rather than copying the live one: the generated copy semantics
    // exist, but copying an object whose options carry an owner pointer is a question this
    // change has no reason to ask. `load` into a default-constructed instance is the same
    // path startup already uses, and it validates the raw tree exactly once, before any I/O.
    CandidatePresentationConfig candidate;
    candidate.load(raw, /*partial=*/true);
    if (!fcitx::safeSaveAsIni(candidate, candidate_presentation_config_path)) {
      error = "Fcitx could not save the candidate-presentation configuration; nothing was "
              "changed, so the same change can be retried.";
      return SettingsStatus::Error;
    }
    // Persisted. Publish the same raw tree to the object the Host reads from, then derive the
    // effective layout from it, so the stored value, what GetConfig reports and what the next
    // projected page uses cannot disagree — including when the write failed, where none of
    // the three moves.
    presentation_config_.load(raw, /*partial=*/true);
    candidate_layout_.store(candidate_layout_for(presentation_config_.layout.value()));
    candidate_font_.store(candidate_font_for(presentation_config_));
    return read_presentation_settings_on_loop(snapshot, error);
  }

  SettingsStatus read_settings_on_loop(const std::string &method, SettingsSnapshot &snapshot,
                                       std::string &error) {
    const auto *entry = instance_->inputMethodManager().entry(method);
    auto *engine = instance_->inputMethodEngine(method);
    if (!entry || !entry->isConfigurable() || !engine) {
      error = "The requested input-method configuration is unavailable.";
      return SettingsStatus::Unsupported;
    }
    const auto *configuration = engine->getConfigForInputMethod(*entry);
    if (!configuration) {
      error = "Fcitx did not provide this input method's configuration.";
      return SettingsStatus::Error;
    }
    fcitx::RawConfig values;
    configuration->save(values);
    snapshot.values = raw_config_to_settings_value(values);
    snapshot.types = dump_settings_description(*configuration);
    return SettingsStatus::Ok;
  }

  SettingsStatus apply_settings_on_loop(const std::string &method,
                                        const SettingsValue &submitted,
                                        SettingsSnapshot &snapshot, std::string &error) {
    SettingsSnapshot current;
    auto status = read_settings_on_loop(method, current, error);
    if (status != SettingsStatus::Ok) {
      return status;
    }
    if (current.types.empty()) {
      error = "Fcitx returned no schema for this input method.";
      return SettingsStatus::Error;
    }
    SettingsValue merged = current.values;
    if (!merge_schema_values(current.types, current.types.front().name, submitted, merged, error)) {
      return SettingsStatus::Invalid;
    }
    if (merged == current.values) {
      // A no-change Apply must not call an engine setter or touch learning files.
      snapshot = std::move(current);
      return SettingsStatus::Ok;
    }
    if (has_active_composition_on_loop()) {
      error = "Finish the current composition, then retry applying settings.";
      return SettingsStatus::Busy;
    }
    const auto *entry = instance_->inputMethodManager().entry(method);
    auto *engine = instance_->inputMethodEngine(method);
    if (!entry || !engine) {
      error = "The input method stopped being available before the settings were applied.";
      return SettingsStatus::Unsupported;
    }
    fcitx::RawConfig raw;
    if (!settings_value_fill_raw_config(merged, raw)) {
      error = "The settings value tree is malformed.";
      return SettingsStatus::Invalid;
    }
    engine->setConfigForInputMethod(*entry, raw);
    return read_settings_on_loop(method, snapshot, error);
  }

  SettingsStatus selection_on_loop(SettingsSelection &selection, std::string &error) {
    const auto &group = instance_->inputMethodManager().currentGroup();
    selection = {};
    for (const char *name : {"pinyin", "shuangpin"}) {
      if (instance_->inputMethodManager().entry(name)) {
        selection.available.emplace_back(name);
      }
    }
    if (!group.defaultInputMethod().empty()) {
      const auto &selected = group.defaultInputMethod();
      if (selected == "pinyin" || selected == "shuangpin") {
        // DefaultIM is the method a bound context resolves to; Items/0 is the
        // group's other slot (see group_items_with_off_slot).
        selection.selected = selected;
      }
    }
    if (selection.available.empty()) {
      error = "This Host has no Pinyin or Shuangpin input method available.";
      return SettingsStatus::Unsupported;
    }
    return SettingsStatus::Ok;
  }

  // The input methods this packaged runtime defines. The Host registers only the
  // Pinyin and Shuangpin engines, so the manageable set is those names where an
  // entry actually exists; a name outside it is not exposed and is refused on
  // write (see im_list.hpp), rather than silently dropped by setGroup.
  std::vector<std::string> available_method_names_on_loop() const {
    auto &manager = instance_->inputMethodManager();
    std::vector<std::string> names;
    for (const char *name : {"pinyin", "shuangpin"}) {
      if (manager.entry(name)) {
        names.emplace_back(name);
      }
    }
    return names;
  }

  // Reads the current group's ordered membership using official semantics:
  // enabled = the group's Items after the off slot at Items/0, active = DefaultIM
  // (the method a bound context resolves to), fallback = DefaultIM,
  // available = the methods this runtime defines.
  SettingsStatus im_list_on_loop(ImList &list, std::string &error) {
    const auto &group = instance_->inputMethodManager().currentGroup();
    list = {};
    list.available = available_method_names_on_loop();
    if (list.available.empty()) {
      error = "This Host has no Pinyin or Shuangpin input method available.";
      return SettingsStatus::Unsupported;
    }
    for (const auto &item : selection::group_enabled_items(group_items(group))) {
      if (std::find(list.available.begin(), list.available.end(), item.name) !=
          list.available.end()) {
        list.enabled.push_back(item.name);
      }
    }
    if (!list.enabled.empty()) {
      list.active = list.enabled.front();
    }
    list.fallback = group.defaultInputMethod();
    return SettingsStatus::Ok;
  }

  // Writes an ordered enabled list back through the official setGroup/save path.
  // The pure validator refuses an empty list and any unavailable name; a no-change
  // Apply neither touches the composition guard nor calls save (no file writes);
  // each retained entry keeps its keyboard layout and the group's existing
  // DefaultIM is carried to setDefaultInputMethod so Fcitx preserves the fallback.
  SettingsStatus apply_im_list_on_loop(const std::vector<std::string> &requested, ImList &out,
                                       std::string &error) {
    auto &manager = instance_->inputMethodManager();
    const auto available = available_method_names_on_loop();
    if (available.empty()) {
      error = "This Host has no Pinyin or Shuangpin input method available.";
      return SettingsStatus::Unsupported;
    }
    const auto &current_group = manager.currentGroup();
    const auto plan =
        selection::plan_im_list_update(requested, available, current_group.defaultInputMethod());
    if (!plan) {
      error = "The input-method list must contain at least one method this Host defines, and every "
              "entry must be available.";
      return SettingsStatus::Invalid;
    }
    // No-change short-circuit: the same enabled list, in the same order, still
    // leaving Items/0 as the group's other slot, persists nothing.
    const auto current_enabled = [&] {
      std::vector<std::string> names;
      for (const auto &item : selection::group_enabled_items(group_items(current_group))) {
        if (std::find(available.begin(), available.end(), item.name) != available.end()) {
          names.push_back(item.name);
        }
      }
      return names;
    }();
    if (current_enabled == plan->methods &&
        current_group.defaultInputMethod() == plan->methods.front()) {
      return im_list_on_loop(out, error);
    }
    if (has_active_composition_on_loop()) {
      error = "Finish the current composition, then retry the input-method list change.";
      return SettingsStatus::Busy;
    }
    // The first enabled method is the one in use, so it is also Items/0's content and
    // DefaultIM; the rest follow in the user's order (see group_items_with_off_slot).
    auto group = current_group;
    // A retained entry keeps the fields it had, taken from the user's own entries. The
    // official `layoutFor(name)` cannot be used here: it returns the *first* item with that
    // name, which is the slot when a user method and the slot share one, and the marker
    // would be written onto a real entry.
    const auto current_items = selection::group_enabled_items(group_items(current_group));
    std::vector<selection::GroupItem> methods;
    methods.reserve(plan->methods.size());
    for (const auto &name : plan->methods) {
      const auto kept = std::find_if(current_items.begin(), current_items.end(),
                                     [&](const selection::GroupItem &item) {
                                       return item.name == name;
                                     });
      methods.push_back(kept == current_items.end() ? selection::GroupItem{name, {}}
                                                    : *kept);
    }
    auto items = selection::group_items_with_off_slot(methods, plan->methods.front(), available);
    write_group(group, items, plan->methods.front());
    manager.setGroup(std::move(group));
    manager.save();
    return im_list_on_loop(out, error);
  }

  static std::vector<selection::GroupItem> group_items(const fcitx::InputMethodGroup &group) {
    std::vector<selection::GroupItem> items;
    items.reserve(group.inputMethodList().size());
    for (const auto &item : group.inputMethodList()) {
      items.push_back({item.name(), item.layout()});
    }
    return items;
  }
  static void write_group(fcitx::InputMethodGroup &group,
                          const std::vector<selection::GroupItem> &items,
                          const std::string &default_method) {
    group.inputMethodList().clear();
    for (const auto &item : items) {
      fcitx::InputMethodGroupItem entry(item.name);
      if (!item.layout.empty())
        entry.setLayout(item.layout);
      group.inputMethodList().push_back(std::move(entry));
    }
    group.setDefaultInputMethod(default_method);
  }
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
  //
  // Whatever the source, the group is then given the shape Fcitx's switch needs:
  // Items/0 carries the method in use as well (see group_items_with_off_slot), so
  // the group has the second entry `canTrigger()` requires and `DefaultIM` names the
  // method in use instead of being rewritten to Items/1. The method in use is the
  // one the group resolved to before this change — Items/0 — so a saved list and
  // its scheme are preserved exactly, and Items/1.. are the user's list unchanged.
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
    auto group = manager.currentGroup();
    const auto plan = selection::plan_startup_group(
        profile_present, group.defaultInputMethod(), method_names(group), available);
    const auto normalization =
        selection::normalize_group(group_items(group), group.defaultInputMethod(), plan.replace,
                                   plan.methods, plan.default_method, available);
    if (!normalization.rewrite) {
      // Already this feature's shape (or nothing to normalize): the user's methods and the
      // method in use are what was written last time, so nothing is touched and the profile
      // is not rewritten on every start.
      log_group(instance, profile_present, false);
      return;
    }
    write_group(group, normalization.items, normalization.default_method);
    manager.setGroup(std::move(group));
    log_group(instance, profile_present, plan.replace);
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
    case BackendStimulus::Type::KeyRelease:
      // A release always carries its keysym, so the name is never used for identity.
      break;
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
          // The Host's one stored presentation value, read from Fcitx's per-user
          // configuration directory before the first page can be projected. A missing file
          // reads as the schema default (vertical), which is exactly the presentation this
          // product shipped before the option existed, so an upgrade changes nothing for a
          // user who never opens the setting.
          fcitx::readAsIni(presentation_config_, candidate_presentation_config_path);
          candidate_layout_.store(candidate_layout_for(presentation_config_.layout.value()));
          candidate_font_.store(candidate_font_for(presentation_config_));
          ensure_usable_group(instance);
          if (!options_.fixture_input_methods.empty()) {
            // Fixture-only: apply a saved profile's list through the same
            // normalization the product uses at startup, so a test can measure e.g. a
            // single-Shuangpin list without touching the user's profile.
            auto group = instance.inputMethodManager().currentGroup();
            std::vector<std::string> names;
            std::vector<std::string> fixture_available;
            for (const auto *name : {"pinyin", "shuangpin"}) {
              if (instance.inputMethodManager().entry(name))
                fixture_available.emplace_back(name);
            }
            for (const auto &name : options_.fixture_input_methods) {
              if (instance.inputMethodManager().entry(name))
                names.emplace_back(name);
            }
            const auto active = options_.fixture_default_method.empty() || names.empty()
                                    ? (names.empty() ? std::string() : names.front())
                                    : options_.fixture_default_method;
            write_group(group,
                        selection::group_items_with_off_slot(
                            selection::items_from_names(names), active, fixture_available),
                        active);
            instance.inputMethodManager().setGroup(std::move(group));
            log_group(instance, true, false);
          }
          if (!options_.fixture_global_hotkey.empty() || !options_.fixture_alt_trigger_key.empty()) {
            // Fixture-only global keys. Fcitx exposes no setter for its global hotkeys, and
            // their configuration layer is only reachable through this instance, so the
            // values are loaded the same way the product loads its presentation option. The
            // partial load leaves every other hotkey at its own value.
            // A Fcitx key list is stored as indexed children of the option node, and the
            // list unmarshaller clears the value first: writing a scalar here would leave
            // the list empty instead of replacing it.
            fcitx::RawConfig hotkeys;
            if (!options_.fixture_global_hotkey.empty()) {
              hotkeys["Hotkey"]["TriggerKeys"]["0"] = options_.fixture_global_hotkey;
            }
            // The modifier-only "temporarily switch input method" key: the product reads
            // this from the user's own configuration and never overrides it, so a test
            // that wants a different key rebinds it through the same partial load.
            if (!options_.fixture_alt_trigger_key.empty()) {
              hotkeys["Hotkey"]["AltTriggerKeys"]["0"] = options_.fixture_alt_trigger_key;
            }
            instance.globalConfig().load(hotkeys, true);
          }
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
  std::atomic<bool> settings_pending_{false};
  std::mutex admission_, dispatch_;
  std::promise<bool> startup_;
  std::thread thread_;
  fcitx::Instance *instance_ = nullptr; // dispatch_ protects publication; worker owns it
  std::map<std::uint64_t, std::unique_ptr<InputContext>> contexts_;
  // The Windows candidate-presentation configuration is the Host's single authoritative
  // store for presentation, read from Fcitx's per-user directory at startup and written
  // only through the settings path below. candidate_layout_ mirrors its layout value for
  // the projection, so no code reads the file or the schema on a keystroke.
  CandidatePresentationConfig presentation_config_;
  std::atomic<CandidateLayout> candidate_layout_{CandidateLayout::Vertical};
  std::atomic<std::shared_ptr<const CandidateFont>> candidate_font_{
      std::make_shared<const CandidateFont>(CandidateFont{"Microsoft YaHei UI", 11})};
};
} // namespace
std::unique_ptr<Backend> make_fcitx_backend(FcitxBackendOptions options) {
  return std::make_unique<FcitxBackend>(options);
}
} // namespace fusion
