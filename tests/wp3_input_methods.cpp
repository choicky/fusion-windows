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
  // that a pre-feature profile — which can look identical in names and DefaultIM — is never
  // mistaken for one. Everything below runs the product's own decision (`normalize_group`)
  // twice, so "idempotent" is measured, not asserted by comparing an expression to itself.
  using fusion::selection::GroupItem;
  using fusion::selection::group_enabled_items;
  using fusion::selection::group_has_off_slot;
  using fusion::selection::group_items_with_off_slot;
  using fusion::selection::items_from_names;
  using fusion::selection::kOffSlotLayout;
  using fusion::selection::legacy_in_use_method;
  using fusion::selection::normalize_group;

  const std::vector<std::string> both{"pinyin", "shuangpin"};
  const auto marked =
      group_items_with_off_slot(items_from_names({"shuangpin"}), "shuangpin", both);
  require(same(names(marked), {"pinyin", "shuangpin"}), "a single method gains the other slot");
  require(marked.front().layout == kOffSlotLayout, "the slot carries the format marker");
  require(marked.back().layout.empty(), "the user's entry carries no marker");
  require(group_has_off_slot(marked), "the marked group is recognized");
  require(same(names(group_enabled_items(marked)), {"shuangpin"}),
          "the enabled list is what follows the slot");

  // A user entry keeps the fields it had — the layout is theirs, and the previous rebuild
  // from names dropped it. `[shuangpin, pinyin]` with a layout on the second entry is the
  // shape a real profile can have.
  const std::vector<GroupItem> legacy_pair{{"shuangpin", ""}, {"pinyin", "us"}};
  const auto first = normalize_group(legacy_pair, "pinyin", false, {}, {}, both);
  require(first.rewrite, "a pre-feature group is normalized");
  require(first.default_method == "shuangpin", "the migration keeps the method in use");
  require(same(names(first.items), {"pinyin", "shuangpin", "pinyin"}),
          "the migration keeps the user's members and their order");
  require(first.items.front().layout == kOffSlotLayout, "the migration writes one marked slot");
  require(first.items[2].layout == "us", "the user's own layout survives the migration");

  // Running the same decision on its own result changes nothing: the marker is the whole
  // test, so a second start cannot add a slot, drop a method or move the method in use.
  const auto second =
      normalize_group(first.items, first.default_method, false, {}, {}, both);
  require(!second.rewrite, "a normalized group is not rewritten again");
  require(same(names(group_enabled_items(first.items)), {"shuangpin", "pinyin"}),
          "the user's list reads back in its order");

  // The same, with the other order and the other method in use.
  const std::vector<GroupItem> legacy_reversed{{"pinyin", ""}, {"shuangpin", ""}};
  const auto reversed = normalize_group(legacy_reversed, "shuangpin", false, {}, {}, both);
  require(reversed.default_method == "pinyin", "the other order keeps its own method");
  require(same(names(reversed.items), {"shuangpin", "pinyin", "shuangpin"}),
          "the other order keeps its members");
  require(!normalize_group(reversed.items, reversed.default_method, false, {}, {}, both).rewrite,
          "the other order is idempotent too");

  // A single pre-feature method: the slot is added, the method and its layout are not lost.
  const std::vector<GroupItem> legacy_single{{"shuangpin", "us"}};
  const auto single = normalize_group(legacy_single, "shuangpin", false, {}, {}, both);
  require(same(names(single.items), {"pinyin", "shuangpin"}), "a single method gains one slot");
  require(single.items.back().layout == "us", "the single method keeps its layout");
  require(!normalize_group(single.items, single.default_method, false, {}, {}, both).rewrite,
          "the single method is idempotent");

  // The collision the shape-based recovery could not survive: the slot and a user method
  // share a name, so `layoutFor("shuangpin")` returns the slot's marker. Reading the user's
  // entries directly — and stripping the marker — keeps it off their entry.
  const auto collision = normalize_group({{"pinyin", ""}, {"shuangpin", ""}}, "pinyin", false,
                                         {}, {}, both);
  require(same(names(collision.items), {"shuangpin", "pinyin", "shuangpin"}),
          "the slot may share a name with a user method");
  const auto enabled = group_enabled_items(collision.items);
  require(same(names(enabled), {"pinyin", "shuangpin"}), "the collision keeps both methods");
  require(enabled[0].layout.empty() && enabled[1].layout.empty(),
          "the marker never reaches a user entry");

  // A profile written by the leaking build: the marker sits on a user entry as well. The
  // normalization clears it, so the slot stays the only one.
  const std::vector<GroupItem> leaked{{"shuangpin", kOffSlotLayout},
                                      {"pinyin", ""},
                                      {"shuangpin", kOffSlotLayout}};
  require(group_has_off_slot(leaked), "the leaked group is still recognized by its slot");
  const auto cleaned = group_enabled_items(leaked);
  require(cleaned[0].layout.empty() && cleaned[1].layout.empty(),
          "a leaked marker is cleared from the user's entries");
  require(names(cleaned) == std::vector<std::string>({"pinyin", "shuangpin"}),
          "clearing the marker does not drop a method");

  // Editing the enabled list keeps the retained entries' fields: the new list is built from
  // the user's entries, never from a name lookup that could hit the slot.
  const auto edited = group_items_with_off_slot(enabled, "shuangpin", both);
  require(same(names(edited), {"pinyin", "pinyin", "shuangpin"}),
          "the edited list keeps its order");
  require(edited.front().layout == kOffSlotLayout, "the edited list writes one slot");
  require(edited[1].layout.empty() && edited[2].layout.empty(),
          "editing does not copy the marker onto a retained entry");
  require(!normalize_group(edited, "shuangpin", false, {}, {}, both).rewrite,
          "the edited list is already normalized");

  // Reading, not just writing: a marked group's enabled list never includes the slot.
  require(names(group_enabled_items(marked)) == std::vector<std::string>({"shuangpin"}),
          "the slot is not reported as one of the user's methods");

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
