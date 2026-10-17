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

  // The profile's order wins: a bound context follows the group's current input
  // method (Items/0), not Fcitx's fallback DefaultIM.
  require(binding_input_method("shuangpin", "pinyin") == "shuangpin",
          "the group's current input method is selected");
  require(binding_input_method("pinyin", "shuangpin") == "pinyin",
          "DefaultIM is a fallback, not the selection");
  require(binding_input_method("", "pinyin") == "pinyin", "an empty list falls back to DefaultIM");
  require(binding_input_method("", "").empty(), "no group means no input method to select");

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
