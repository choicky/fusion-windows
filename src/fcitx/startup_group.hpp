#pragma once

// Stage 6 WP3 input-method selection (owner decision U1, 2026-10-18).
//
// Fcitx owns selection: the input-method profile decides the group and its
// default input method, and the pinyin addon's own configuration decides the
// Shuangpin scheme and the other pinyin options. The Host therefore keeps the
// loaded group verbatim, and only builds a first-run default when the profile
// names no input method this runtime defines — through the same group type Fcitx
// itself uses. These two decisions are pure so they can be tested without
// constructing a Fcitx instance (which a test must never do on the user path).

#include <algorithm>
#include <string>
#include <vector>

namespace fusion::selection {

// What the Host may do with the group Fcitx loaded from the profile.
struct GroupPlan {
  // False: keep the loaded group exactly as the profile defines it.
  bool replace = false;
  // Only meaningful when replace is true: the first-run default group.
  std::vector<std::string> methods;
  std::string default_method;
};

// A group is usable when at least one of the input methods the profile names
// exists in this runtime. A profile that names none (or an empty profile) would
// leave contexts with no input method, so it is replaced by the first-run default.
inline bool group_is_usable(const std::string &default_method,
                            const std::vector<std::string> &methods,
                            const std::vector<std::string> &available_methods) {
  const auto available = [&](const std::string &name) {
    return std::find(available_methods.begin(), available_methods.end(), name) !=
           available_methods.end();
  };
  if (!methods.empty()) {
    return std::any_of(methods.begin(), methods.end(), available);
  }
  return !default_method.empty() && available(default_method);
}

// Decides the startup group.
//
// `profile_present` says whether Fcitx found an input-method profile at all.
// `available_methods` are the input methods this runtime actually defines (the
// pinyin addon's entries in the current build).
//
// With no profile this is a first run: Fcitx's own built-in fallback is not the
// product's first-run default (it can name only the keyboard layout, or, once a
// second input method exists, pick it as the default depending on load order), so
// the first-run default is built explicitly with Pinyin as the default — today's
// behaviour. With a profile the loaded group is kept verbatim unless it names no
// input method this runtime defines, which would leave contexts with nothing to
// select.
inline GroupPlan plan_startup_group(bool profile_present, const std::string &default_method,
                                    const std::vector<std::string> &methods,
                                    const std::vector<std::string> &available_methods) {
  if (profile_present && group_is_usable(default_method, methods, available_methods)) {
    return GroupPlan{};
  }
  GroupPlan plan;
  plan.replace = true;
  for (const char *name : {"pinyin", "shuangpin"}) {
    if (std::find(available_methods.begin(), available_methods.end(), name) !=
        available_methods.end()) {
      plan.methods.emplace_back(name);
    }
  }
  plan.default_method = "pinyin";
  return plan;
}

// The input method a bound context starts in — what Fcitx itself resolves for a
// context whose input state is **active**: the group's `DefaultIM`, unless a local
// selection was made for that context (`Instance::inputMethod` reads
// `localIM_` first and falls back to `defaultInputMethod()`). The inactive branch
// returns `inputMethodList()[0]`, which is why Items/0 is the group's *other* slot
// (see group_items_with_off_slot) and not the method in use.
//
// Reading Items/0 here would select the slot Fcitx treats as inactive. That was the
// behaviour while contexts were only ever inactive; a context that can be switched
// (Hotkey/AltTriggerKeys, the settings' "temporarily switch input method") must
// start in the same method the active state resolves to, or the first toggle would
// change the typing scheme.
inline std::string binding_input_method(const std::string &current_method,
                                        const std::string &default_method) {
  if (!default_method.empty()) {
    return default_method;
  }
  return current_method;
}

// The method Items/0 carries: the group's *other* slot, which upstream fills with the
// keyboard layout. A method the user has not enabled is preferred, so the slot is
// never mistaken for one of theirs; otherwise any method other than the one in use.
//
// It must be a **different** method, because that is what makes Fcitx see an input
// method change when the state flips: `CheckInputMethodChanged` compares the resolved
// method and only then calls the engine's deactivate, whose `reset()` clears the
// preedit (PinyinEngine::deactivate). With the slot repeating the method in use the
// toggle would leave a composition on screen while reporting English.
inline std::string group_off_slot_method(const std::vector<std::string> &methods,
                                         const std::string &active,
                                         const std::vector<std::string> &available) {
  for (const auto &name : available) {
    if (name != active && std::find(methods.begin(), methods.end(), name) == methods.end()) {
      return name;
    }
  }
  for (const auto &name : available) {
    if (name != active) {
      return name;
    }
  }
  return active;
}

// The group's Items for an ordered enabled list and the method in use.
//
// Fcitx's on/off state needs a second group entry to exist at all: `canTrigger()` is
// `inputMethodList().size() > 1`, and `activate()`, `toggle()` and
// `setCurrentInputMethod()` all return early without it. It also refuses a `DefaultIM`
// equal to Items/0 (InputMethodGroup::setDefaultInputMethod rewrites that to Items/1),
// so a group whose only entry is the method in use can neither be activated nor
// switched. Items/0 therefore holds the other method (`group_off_slot_method`) and
// Items[1..] are the user's list unchanged.
//
// The disabled half of that state is implemented by the frontend instead (see
// InputContext::inactive_passthrough): an inactive context's keys are the
// application's, so Items/0 is never composed through.
inline std::vector<std::string> group_items_with_off_slot(const std::vector<std::string> &methods,
                                                         const std::string &active,
                                                         const std::vector<std::string> &available) {
  if (active.empty()) {
    return methods;
  }
  std::vector<std::string> items;
  items.reserve(methods.size() + 1);
  items.emplace_back(group_off_slot_method(methods, active, available));
  items.insert(items.end(), methods.begin(), methods.end());
  return items;
}

// Whether a group already has the shape group_items_with_off_slot produces: Items/0 is
// the other slot and Items/1 is the method in use, which is what DefaultIM names.
inline bool group_has_off_slot(const std::vector<std::string> &items,
                               const std::string &default_method) {
  return items.size() > 1 && !default_method.empty() && items[1] == default_method;
}

} // namespace fusion::selection
