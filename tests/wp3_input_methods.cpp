// Stage 6 WP3 input-method selection decisions (owner decision U1).
//
// Usage: wp3_input_methods
//
// These are the two pure decisions the Host applies: which input method a bound
// context starts in, and whether the group Fcitx loaded from the profile is kept
// or replaced by the first-run default. They are tested here, without a Fcitx
// instance, because a test must never construct the engine on the user path.

#include "fcitx/startup_group.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

bool same(const std::vector<std::string> &value, const std::vector<std::string> &expected) {
  return value == expected;
}

std::vector<std::string> names(const std::vector<fusion::selection::GroupItem> &items) {
  std::vector<std::string> result;
  for (const auto &item : items)
    result.push_back(item.name);
  return result;
}

void test_selection() {
  using fusion::selection::binding_input_method;
  using fusion::selection::plan_startup_group;

  // A bound context is activated (its switch needs an active state), and Fcitx then
  // resolves DefaultIM: `Instance::inputMethod` reads localIM, then
  // `defaultInputMethod()`, and only returns Items/0 while the state is inactive. The
  // group therefore carries the method in use as Items[0] *and* as DefaultIM (see
  // group_items_with_off_slot), so the selection is DefaultIM's.
  require(binding_input_method("shuangpin", "pinyin") == "pinyin",
          "DefaultIM is the input method a bound context resolves to");
  require(binding_input_method("pinyin", "shuangpin") == "shuangpin",
          "Items/0 alone never overrides DefaultIM");
  require(binding_input_method("pinyin", "") == "pinyin", "an empty DefaultIM keeps Items/0");
  require(binding_input_method("", "pinyin") == "pinyin", "an empty list falls back to DefaultIM");
  require(binding_input_method("", "").empty(), "no group means no input method to select");

  // The marked slot: Items/0 is an entry this product owns, marked in its layout field so
  // that a pre-feature profile — which can look identical in names and DefaultIM — is
  // never mistaken for one. `group_items_with_off_slot` writes it, `group_enabled_items`
  // reads the user's list back out, and the marker alone decides which is which.
  using fusion::selection::GroupItem;
  using fusion::selection::group_enabled_items;
  using fusion::selection::group_has_off_slot;
  using fusion::selection::group_items_with_off_slot;
  using fusion::selection::kOffSlotLayout;
  using fusion::selection::legacy_in_use_method;

  const auto marked = group_items_with_off_slot({"shuangpin"}, "shuangpin",
                                                {"pinyin", "shuangpin"});
  require(same(names(marked), {"pinyin", "shuangpin"}), "a single method gains the other slot");
  require(marked.front().layout == kOffSlotLayout, "the slot carries the format marker");
  require(marked.back().layout.empty(), "the user's entry carries no marker");
  require(group_has_off_slot(marked), "the marked group is recognized");
  require(same(names(group_enabled_items(marked)), {"shuangpin"}),
          "the enabled list is what follows the slot");

  // Both orders, both methods in use: the slot is never the method in use, so Fcitx sees
  // an input-method change when the state flips.
  const auto pair_pinyin = group_items_with_off_slot({"pinyin", "shuangpin"}, "pinyin",
                                                     {"pinyin", "shuangpin"});
  require(same(names(pair_pinyin), {"shuangpin", "pinyin", "shuangpin"}),
          "a two-method list keeps its order after the slot");
  require(names(group_enabled_items(pair_pinyin)) == std::vector<std::string>({"pinyin", "shuangpin"}),
          "a two-method list reads back unchanged");
  const auto pair_shuangpin = group_items_with_off_slot({"shuangpin", "pinyin"}, "shuangpin",
                                                        {"pinyin", "shuangpin"});
  require(same(names(pair_shuangpin), {"pinyin", "shuangpin", "pinyin"}),
          "the other order gets the other slot");
  require(names(group_enabled_items(pair_shuangpin)) ==
              std::vector<std::string>({"shuangpin", "pinyin"}),
          "the other order reads back unchanged");

  // A pre-feature group has no marker, whatever its shape. Reading it by shape would take
  // `[shuangpin, pinyin]` with DefaultIM=pinyin — a user typing Shuangpin — for a marked
  // group and move them to Pinyin while hiding Shuangpin.
  const std::vector<GroupItem> legacy_single{{"shuangpin", ""}};
  const std::vector<GroupItem> legacy_pair{{"shuangpin", ""}, {"pinyin", ""}};
  require(!group_has_off_slot(legacy_single), "a single-entry legacy group is unmarked");
  require(!group_has_off_slot(legacy_pair), "a two-entry legacy group is unmarked");
  require(legacy_in_use_method(legacy_pair, "pinyin") == "shuangpin",
          "a legacy group's method in use is its first item, not DefaultIM");
  require(legacy_in_use_method(legacy_single, "shuangpin") == "shuangpin",
          "a single-entry legacy group keeps its method");
  require(same(names(group_enabled_items(legacy_pair)), {"shuangpin", "pinyin"}),
          "a legacy group's members are all of its items");

  // Migrating either shape preserves the method in use and the members in order.
  const auto migrated_pair = group_items_with_off_slot(
      names(legacy_pair), legacy_in_use_method(legacy_pair, "pinyin"), {"pinyin", "shuangpin"});
  require(same(names(migrated_pair), {"pinyin", "shuangpin", "pinyin"}),
          "the migrated pair keeps its order and its scheme");
  require(same(names(group_enabled_items(migrated_pair)), {"shuangpin", "pinyin"}),
          "the migrated pair reads back as the user's list");
  const auto migrated_single = group_items_with_off_slot(
      names(legacy_single), legacy_in_use_method(legacy_single, "shuangpin"),
      {"pinyin", "shuangpin"});
  require(same(names(migrated_single), {"pinyin", "shuangpin"}),
          "the migrated single keeps its scheme");

  // Idempotence: migrating a marked group changes nothing, so a restart or a save cannot
  // grow the slot or drop a method.
  require(group_has_off_slot(migrated_pair), "the migrated group is marked");
  require(same(names(group_enabled_items(migrated_pair)),
               names(group_enabled_items(migrated_pair))),
          "reading the migrated group twice yields the same list");

  // A marked group with one user method still has the second entry Fcitx's switch needs.
  require(names(marked).size() > 1, "a single-method user still gets two group entries");

  const auto kept =
      plan_startup_group(true, "shuangpin", {"pinyin", "shuangpin"}, {"pinyin", "shuangpin"});
  require(!kept.replace, "a profile naming available input methods is kept verbatim");

  // A profile that names only input methods this runtime does not define would
  // leave contexts without an input method, so the first-run default replaces it.
  const auto replaced = plan_startup_group(true, "wbpy", {"wbpy"}, {"pinyin", "shuangpin"});
  require(replaced.replace, "an unusable profile is replaced");
  require(replaced.default_method == "pinyin", "Pinyin is the first-run default");
  require(same(replaced.methods, {"pinyin", "shuangpin"}),
          "the first-run group lists the pinyin addon's input methods");

  // The first-run group only lists what the runtime actually defines: Shuangpin is
  // included when (and only when) its input-method definition is installed.
  const auto pinyin_only = plan_startup_group(false, "", {}, {"pinyin"});
  require(pinyin_only.replace, "an empty group is replaced");
  require(same(pinyin_only.methods, {"pinyin"}), "unavailable input methods are not listed");

  // No profile at all is a first run even when Fcitx's own fallback already named an
  // available input method: the product's first-run default is Pinyin, not whatever
  // Fcitx's load order produced.
  const auto first_run =
      plan_startup_group(false, "shuangpin", {"shuangpin"}, {"pinyin", "shuangpin"});
  require(first_run.replace, "no profile is a first run even with a usable fallback");
  require(first_run.default_method == "pinyin", "Pinyin is the first-run default");

  require(plan_startup_group(true, "", {}, {"pinyin", "shuangpin"}).replace,
          "a profile with an empty group is replaced");
  require(!plan_startup_group(true, "pinyin", {}, {"pinyin"}).replace,
          "a profile with only a default is usable");
}

} // namespace

int main() {
  try {
    test_selection();
    std::cout << "PASS wp3_input_methods: profile-driven selection and first-run default\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL wp3_input_methods: " << e.what() << "\n";
    return 1;
  }
}
