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

  // The off slot: Items/0 repeats the method in use, which is what gives the group the
  // second entry Fcitx's switch requires and keeps DefaultIM from being rewritten.
  require(same(fusion::selection::group_items_with_off_slot(
                   {"shuangpin"}, "shuangpin", {"pinyin", "shuangpin"}),
               {"pinyin", "shuangpin"}),
          "a single-method list gains the other method as its off slot");
  require(same(fusion::selection::group_items_with_off_slot(
                   {"pinyin", "shuangpin"}, "pinyin", {"pinyin", "shuangpin"}),
               {"shuangpin", "pinyin", "shuangpin"}),
          "an all-methods list still gets a different off slot");
  require(same(fusion::selection::group_items_with_off_slot(
                   {"pinyin"}, "pinyin", {"pinyin", "shuangpin"}),
               {"shuangpin", "pinyin"}),
          "the enabled order is untouched after the slot");
  require(fusion::selection::group_has_off_slot({"shuangpin", "pinyin"}, "pinyin"),
          "the normalized shape is recognized");
  require(!fusion::selection::group_has_off_slot({"shuangpin"}, "shuangpin"),
          "a single-entry group is not normalized yet");
  // A two-entry group whose DefaultIM already is Items/0 resolves to the same method in
  // both states, so it needs nothing; Fcitx cannot produce it (setDefaultInputMethod
  // rewrites that pair), which is exactly why the slot is written explicitly.
  require(fusion::selection::group_has_off_slot({"shuangpin", "pinyin", "shuangpin"}, "pinyin"),
          "a repeated method is still an off slot");

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
