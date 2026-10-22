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

// One entry of the group, as Fcitx stores it: the input method and its optional layout.
// The layout field is the only free-form, persisted per-entry value the official model
// has (`Items/N/Layout` in the profile), so it is what this product uses to mark its own
// otherwise-invisible slot.
struct GroupItem {
  std::string name;
  std::string layout;
};

// The marker written into the other slot's layout. It identifies **the format of the
// group**, not a behaviour: nothing reads it to decide what to type or which method is in
// use, and removing it only makes the product migrate the group again exactly as it
// migrates any profile written before this feature existed.
//
// The alternative — guessing from the shape (`Items/1 == DefaultIM`) — cannot be used: a
// profile that enabled two methods before this feature has exactly that shape, and reading
// it as "the first item is ours" would switch the user to a different scheme and hide one
// of their methods.
inline constexpr char kOffSlotLayout[] = "fusion-off";

// The user's methods, when all that is known is their names (a first-run plan).
inline std::vector<GroupItem> items_from_names(const std::vector<std::string> &names) {
  std::vector<GroupItem> items;
  items.reserve(names.size());
  for (const auto &name : names)
    items.push_back({name, {}});
  return items;
}

// A user entry never carries the marker: it belongs to the slot this product owns, and a
// copy on a real entry would both leak the format into the user's profile and make the
// "one slot" invariant untrue. Entries that carry it (a profile written by a build that
// had the leak) are cleared here rather than at every read.
inline std::vector<GroupItem> without_marker(const std::vector<GroupItem> &items) {
  std::vector<GroupItem> cleaned;
  cleaned.reserve(items.size());
  for (const auto &item : items)
    cleaned.push_back({item.name, item.layout == kOffSlotLayout ? std::string() : item.layout});
  return cleaned;
}

// The method Items/0 carries: the group's *other* slot, which upstream fills with the
// keyboard layout. A method the user has not enabled is preferred, so the slot is never
// mistaken for one of theirs; otherwise any method other than the one in use.
//
// It must be a **different** method, because that is what makes Fcitx see an input method
// change when the state flips: `CheckInputMethodChanged` compares the resolved method and
// only then calls the engine's deactivate, whose `reset()` clears the preedit
// (PinyinEngine::deactivate). With the slot repeating the method in use the toggle would
// leave a composition on screen while reporting English.
inline std::string group_off_slot_method(const std::vector<GroupItem> &methods,
                                         const std::string &active,
                                         const std::vector<std::string> &available) {
  const auto enabled = [&](const std::string &name) {
    return std::any_of(methods.begin(), methods.end(),
                       [&](const GroupItem &item) { return item.name == name; });
  };
  for (const auto &name : available) {
    if (name != active && !enabled(name)) {
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
// equal to Items/0 (InputMethodGroup::setDefaultInputMethod rewrites that to Items/1), so
// a group whose only entry is the method in use can neither be activated nor switched.
// Items/0 is therefore the marked other slot and Items[1..] are the user's list, unchanged
// and in the user's order.
//
// **The user's entries are carried, not rebuilt.** Only the name and the layout exist on an
// `InputMethodGroupItem`, and the layout is the user's own persisted value — a profile can
// carry one per entry (`Items/N/Layout`), and `layoutFor(name)` cannot be used to recover it
// because it returns the *first* item with that name, which may be the slot. So the items
// pass through with both fields, minus the marker.
//
// The disabled half of that state is implemented by the frontend instead (see
// InputContext::inactive_passthrough): while the context is inactive its keys are the
// application's, so Items/0 is never composed through.
inline std::vector<GroupItem> group_items_with_off_slot(const std::vector<GroupItem> &methods,
                                                        const std::string &active,
                                                        const std::vector<std::string> &available) {
  if (active.empty()) {
    return without_marker(methods);
  }
  std::vector<GroupItem> items = without_marker(methods);
  items.reserve(methods.size() + 1);
  items.insert(items.begin(), GroupItem{group_off_slot_method(methods, active, available),
                                        kOffSlotLayout});
  return items;
}

// Whether a group already carries the marker, i.e. was written by this feature.
// Deliberately independent of the names and of `DefaultIM`: a pre-feature profile can look
// like a marked one in every other respect.
inline bool group_has_off_slot(const std::vector<GroupItem> &items) {
  return !items.empty() && items.front().layout == kOffSlotLayout;
}

// The user's enabled methods: the items after the marked slot, or all of them when the
// group predates the marker.
inline std::vector<GroupItem> group_enabled_items(const std::vector<GroupItem> &items) {
  if (!group_has_off_slot(items) || items.size() < 2) {
    return without_marker(items);
  }
  return without_marker(std::vector<GroupItem>(items.begin() + 1, items.end()));
}

// The input method a pre-feature group has been using: `Instance::inputMethod` returns
// `Items/0` while the input state is inactive, which is what those builds always were, so
// that entry is the one a migration must keep. Only for a group without the marker.
inline std::string legacy_in_use_method(const std::vector<GroupItem> &items,
                                        const std::string &default_method) {
  for (const auto &item : items) {
    if (!item.name.empty())
      return item.name;
  }
  return default_method;
}

// What the startup path does with the group it loaded, as one decision so that it can be
// applied twice in a test without a second copy of the rule.
struct GroupNormalization {
  bool rewrite = false;
  std::vector<GroupItem> items;
  std::string default_method;
};

// `replace_with_plan` is the first-run case (no usable profile): the plan names the user's
// methods and the method to use. Otherwise the loaded group is the user's: its entries in
// their order, and — only when it has no marker — its first entry as the method in use.
//
// Idempotence is the point: a normalized group is detected by its marker and left alone, so
// starting twice cannot add a second slot or drop a method, and the caller can compare the
// result of a second call with the first.
inline GroupNormalization normalize_group(const std::vector<GroupItem> &stored,
                                          const std::string &stored_default,
                                          bool replace_with_plan,
                                          const std::vector<std::string> &plan_methods,
                                          const std::string &plan_default,
                                          const std::vector<std::string> &available) {
  if (!replace_with_plan && group_has_off_slot(stored)) {
    return GroupNormalization{}; // already this feature's shape
  }
  std::vector<GroupItem> methods =
      replace_with_plan ? items_from_names(plan_methods) : stored;
  std::string active;
  if (replace_with_plan) {
    active = plan_methods.empty() ? plan_default : plan_methods.front();
  } else {
    active = legacy_in_use_method(stored, stored_default);
  }
  if (active.empty())
    return GroupNormalization{};
  GroupNormalization result;
  result.rewrite = true;
  result.items = group_items_with_off_slot(methods, active, available);
  result.default_method = active;
  return result;
}

} // namespace fusion::selection
