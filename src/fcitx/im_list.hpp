#pragma once

// Stage A input-method list management: the product-side validation the Fcitx
// core does not perform, kept pure so it can be unit-tested without constructing
// a Fcitx instance (which a test must never do on the real user path).
//
// Fcitx's InputMethodManager owns persistence and its own invariants: setGroup
// silently drops names this runtime does not define, save() rewrites only the
// current group's profile, and setDefaultInputMethod keeps DefaultIM distinct
// from Items/0 by itself. This file adds the two guards the core leaves open —
// an empty enabled list and an unavailable selection are refused here instead of
// being silently persisted or dropped — and decides the enabled order. It does
// NOT reimplement the DefaultIM rewrite: the previous fallback is passed straight
// to setDefaultInputMethod so Fcitx remains the single source of truth for it.

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace fusion::selection {

struct ImListPlan {
  // The ordered Items to write (Items/0..N); the active method is methods.front().
  std::vector<std::string> methods;
  // Passed verbatim to InputMethodGroup::setDefaultInputMethod. Fcitx rewrites a
  // default equal to Items/0 (or a no-longer-enabled default) by its own rule.
  std::string requested_fallback;
};

// Normalizes and validates a requested enabled list against the methods the
// packaged runtime actually defines.
//
// `available` is every input method this Host build provides; a request may only
// enable names from it. `requested` is the desired order produced by the UI's
// add / remove / reorder; duplicates collapse to their first occurrence and the
// remaining order is preserved. `previous_fallback` is the group's current
// DefaultIM, carried through so the user's fallback survives an unrelated edit.
//
// Returns nullopt — refuse — when the request would leave the group with no
// input method (the core would persist that and contexts would have nothing to
// select) or names a method this runtime does not define (the core would silently
// drop it, so a partial success would look like a saved list).
inline std::optional<ImListPlan>
plan_im_list_update(const std::vector<std::string> &requested,
                    const std::vector<std::string> &available,
                    const std::string &previous_fallback) {
  const auto defined = [&](const std::string &name) {
    return std::find(available.begin(), available.end(), name) != available.end();
  };
  ImListPlan plan;
  for (const auto &name : requested) {
    if (!defined(name)) {
      return std::nullopt;
    }
    if (std::find(plan.methods.begin(), plan.methods.end(), name) == plan.methods.end()) {
      plan.methods.push_back(name);
    }
  }
  if (plan.methods.empty()) {
    return std::nullopt;
  }
  plan.requested_fallback = previous_fallback;
  return plan;
}

} // namespace fusion::selection
