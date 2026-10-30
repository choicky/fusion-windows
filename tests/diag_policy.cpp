#include "tsf/diag.hpp"
#include "tsf/key_map.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

std::string path() {
  char temp[MAX_PATH]{};
  require(GetTempPathA(MAX_PATH, temp) != 0, "GetTempPath failed");
  return std::string(temp) + "fusion-tsf-diag-policy.log";
}

// R52: the whole modifier diagnostic rests on one rule — a key *position* may be named, a key
// that could carry content may not. So the vocabulary is fixed here, and anything the rule
// would let through that is not one of these eight words is a failure.
constexpr const char *kModifierNames[] = {"lshift", "rshift", "lctrl", "rctrl",
                                          "lalt",   "ralt",   "lwin",  "rwin"};

bool is_modifier_name(const char *name) {
  const std::string found = name != nullptr ? name : "";
  for (const char *allowed : kModifierNames)
    if (found == allowed)
      return true;
  return false;
}

void check_modifier_vocabulary() {
  using fusion::tsf::modifier_identity;
  std::set<std::string> names;
  int named_codes = 0;
  for (std::uint32_t vk = 0; vk < 256; ++vk) {
    // No scan code and no extended flag: the left/right virtual keys answer on their own, and
    // every other key must answer with nothing at all.
    const char *name = modifier_identity(vk, 0, false);
    if (name == nullptr)
      continue;
    ++named_codes;
    require(is_modifier_name(name), "a key that is not a modifier was given a name");
    names.insert(name);
  }
  require(names.size() == 8, "the eight modifier positions are not all named");
  // The three generic codes Windows reports for either side, plus the eight left/right keys.
  require(named_codes == 11, "the named key codes are not the modifier set");

  // Letters, digits and punctuation are never named, whatever the rest of the event says —
  // including when they arrive with the scan code and extended flag of a real press.
  const std::uint32_t ordinary_keys[] = {'A',           'F',       'Z',     'a',       'f',
                                         '0',           '5',       '9',     VK_SPACE,  VK_OEM_1,
                                         VK_OEM_PERIOD, VK_RETURN, VK_BACK, VK_ESCAPE, VK_TAB};
  for (const std::uint32_t vk : ordinary_keys) {
    require(modifier_identity(vk, 0x1E, true) == nullptr,
            "a letter, digit or punctuation key was given a name");
  }

  // Which side of a pair, when Windows only reports the generic code: the two Shift keys have
  // their own scan codes, and Ctrl and Alt are separated by the extended flag alone.
  require(std::string(modifier_identity(VK_SHIFT, fusion::tsf::kScanShiftLeft, false)) == "lshift",
          "the left Shift scan code was not named left");
  require(std::string(modifier_identity(VK_SHIFT, fusion::tsf::kScanShiftRight, false)) == "rshift",
          "the right Shift scan code was not named right");
  require(std::string(modifier_identity(VK_SHIFT, 0, true)) == "rshift",
          "an extended generic Shift was not named right");
  require(std::string(modifier_identity(VK_SHIFT, 0, false)) == "lshift",
          "a plain generic Shift was not named left");
  require(std::string(modifier_identity(VK_CONTROL, 0x1D, true)) == "rctrl",
          "an extended generic Control was not named right");
  require(std::string(modifier_identity(VK_CONTROL, 0x1D, false)) == "lctrl",
          "a plain generic Control was not named left");
  require(std::string(modifier_identity(VK_MENU, 0x1D, true)) == "ralt",
          "an extended generic Alt was not named right");
  require(std::string(modifier_identity(VK_MENU, 0x1D, false)) == "lalt",
          "a plain generic Alt was not named left");
}
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "mode required");
    check_modifier_vocabulary();
    const std::string file = path();
    DeleteFileA(file.c_str());
    SetEnvironmentVariableA("FUSION_TSF_LOG", file.c_str());
    SetEnvironmentVariableA("FUSION_TSF_DIAGNOSTICS", argc == 2 && std::string(argv[1]) == "optin"
                                                            ? "1"
                                                            : nullptr);
    fusion::tsf::diag::event("TEST_KEY", "vk=0x41 text=secret");
    if (std::string(argv[1]) == "default") {
      require(GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES,
              "diagnostics are enabled by default");
      return 0;
    }
    fusion::tsf::diag::event("TECHNICAL", "hr=0x0 count=2");
    // R52: the shape a key diagnostic takes once the key is a modifier. It is written verbatim
    // because it names a position, not a character — the line the shell writes for Shift.
    fusion::tsf::diag::event("TEST_KEY", "mod=lshift down eligible=1 sent=0 result=none");
    fusion::tsf::diag::event("KEY_ARRIVAL", "mod=rctrl down ctx=0000000000000000 cid=7");
    fusion::tsf::diag::event("TEST_KEY_UP", "mod=rctrl up eligible=1 sent=0 result=none");
    // The open/close line the compartment sink writes. It carries no cid: the compartment
    // belongs to the thread, and every line here already names the thread.
    fusion::tsf::diag::event("OPENCLOSE", "old=closed new=open source=self");
    std::ifstream input(file);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    require(contents.find("TEST_KEY privacy=redacted") != std::string::npos,
            "prohibited fields were not redacted");
    require(contents.find("TECHNICAL hr=0x0 count=2") != std::string::npos,
            "technical opt-in event missing");
    require(contents.find("TEST_KEY mod=lshift down eligible=1 sent=0 result=none") !=
                std::string::npos,
            "a modifier press was not recorded");
    require(contents.find("KEY_ARRIVAL mod=rctrl down") != std::string::npos,
            "a modifier arrival was not recorded");
    require(contents.find("TEST_KEY_UP mod=rctrl up") != std::string::npos,
            "a modifier release was not recorded");
    require(contents.find("OPENCLOSE old=closed new=open source=self") != std::string::npos,
            "the open/close compartment change was not recorded");
    require(contents.find("vk=") == std::string::npos && contents.find("text=") == std::string::npos,
            "diagnostic content policy leaked key/text data");
    DeleteFileA(file.c_str());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
