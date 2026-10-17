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
// context that is not switched locally: the group's **current** input method, i.e.
// `inputMethodList()[0]` (Instance::inputMethod returns exactly this while the input
// state is inactive), falling back to `DefaultIM` when the list is empty.
//
// This distinction matters: in Fcitx's model `Items/0` is the input method in use
// and `DefaultIM` is the fallback it switches back to — InputMethodGroup::
// setDefaultInputMethod even rewrites a default equal to the first item to the
// second one. Reading `DefaultIM` here would select the *fallback*, not the input
// method the profile's order asks for.
inline std::string binding_input_method(const std::string &current_method,
                                        const std::string &default_method) {
  if (!current_method.empty()) {
    return current_method;
  }
  return default_method;
}

} // namespace fusion::selection
