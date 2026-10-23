// The enhanced Chinese runtime inside the product Host.
//
// Windows now runs the same fcitx5-chinese-addons implementation as the Android build
// (choicky/fcitx5-chinese-addons, pinned in tools/fcitx/build-native.ps1), so the English
// lexicon and the mixed Chinese/English pipeline are native parts of the engine the Host
// already loads. This check measures that on the real pinned engine and the packed data:
// a Latin word produces an English candidate from the lexicon and commits it, a Chinese
// syllable still produces the Chinese candidate and commits it, the default auxiliary
// filter is still Stroke (MoQi is opt-in through the configuration, unchanged default), and
// a raw string that mixes both produces a candidate carrying both readings which commits as
// the whole mixed text — on Pinyin, on Shuangpin, and on the Ziranma raw the owner reported
// (`woxdmliphone`), whose fused candidate has to be on the first screen and not merely
// somewhere in the list.
//
// Printed lines are the evidence record. It runs as an isolated process: no TIP is
// registered, no product installation is used and no user configuration is read or
// written (the fixture skips Fcitx's per-user paths).

#include "fusion/backend.hpp"

#include <crtdbg.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace fusion;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

struct Driver {
  std::unique_ptr<Backend> backend;
  Binding binding{8101, 1, 1};
  std::uint64_t next = 1;
  CandidatePage page;

  explicit Driver(FcitxBackendOptions options = {})
      : backend(make_fcitx_backend(std::move(options))) {
    if (!backend->healthy() || !backend->bind(binding))
      throw std::runtime_error("the real backend did not bind");
  }

  BackendBatch send(BackendStimulus stimulus) {
    stimulus.binding = binding;
    stimulus.stimulus_id = next++;
    auto batch = backend->send(stimulus);
    for (const auto &operation : batch.operations)
      if (operation.type == BackendOperation::Type::CandidateSnapshot && operation.page)
        page = *operation.page;
    return batch;
  }

  BackendBatch type(const std::string &text) {
    BackendBatch last;
    for (char ch : text) {
      BackendStimulus stimulus{binding, 0, BackendStimulus::Type::Character, ch};
      stimulus.keysym = static_cast<std::uint8_t>(ch);
      last = send(stimulus);
    }
    return last;
  }

  // A key identified by keysym, the way the TSF sends a non-character key.
  BackendBatch keysym(std::uint32_t symbol) {
    BackendStimulus stimulus{binding, 0, BackendStimulus::Type::Character, 0};
    stimulus.keysym = symbol;
    return send(stimulus);
  }

  BackendBatch select(std::int32_t index) {
    BackendStimulus stimulus{binding, 0, BackendStimulus::Type::SelectCandidate};
    stimulus.candidate_index = index;
    return send(stimulus);
  }

  std::string committed(const BackendBatch &batch) const {
    std::string text;
    for (const auto &operation : batch.operations)
      if (operation.type == BackendOperation::Type::CommitText)
        text += operation.text;
    return text;
  }

  std::vector<std::string> texts() const {
    std::vector<std::string> out;
    for (const auto &entry : page.entries)
      out.push_back(entry.text);
    return out;
  }

  std::int32_t index_of(const std::string &text) const {
    for (std::size_t index = 0; index < page.entries.size(); ++index)
      if (page.entries[index].text == text)
        return static_cast<std::int32_t>(index);
    return -1;
  }
};

void print_candidates(const char *label, const Driver &driver) {
  std::printf("[%s] aux=[%s] candidates:", label, driver.page.aux_up.c_str());
  for (const auto &text : driver.texts())
    std::printf(" [%s]", text.c_str());
  std::printf("\n");
}

// Finds `expected` on the candidate screen the engine shows first, and commits it by
// selecting it there. The two Ziranma cases below are members of the bounded insertion class,
// whose whole point is that the fused reading takes the front-row slot behind the Chinese
// head — so "on the first screen" is the promise being checked, not a lucky rank. A candidate
// that needs paging would be a regression, and the paging tests cover paging.
std::string commit_first_screen(Driver &driver, const std::string &expected) {
  const auto index = driver.index_of(expected);
  if (index < 0)
    return std::string();
  std::printf("[first screen] found [%s] at %d\n", expected.c_str(),
              static_cast<int>(index));
  return driver.committed(driver.select(index));
}

// 1. A Latin word is an English candidate of the mixed pipeline and commits as itself.
void section_english_candidate() {
  Driver driver;
  driver.type("hello");
  print_candidates("hello", driver);
  const auto index = driver.index_of("hello");
  require(index >= 0, "the English lexicon did not supply a candidate for \"hello\"");
  const auto committed = driver.committed(driver.select(index));
  std::printf("[hello] committed [%s]\n", committed.c_str());
  require(committed == "hello", "the English candidate did not commit its own text");
}

// 2. A Chinese syllable is unchanged: the Chinese candidate still comes first and commits.
void section_chinese_candidate() {
  Driver driver;
  driver.type("nihao");
  print_candidates("nihao", driver);
  require(!driver.page.entries.empty(), "the engine produced no candidates for \"nihao\"");
  require(driver.page.entries.front().text == "\u4f60\u597d",
          "the first candidate for \"nihao\" is not the Chinese word");
  const auto committed = driver.committed(driver.select(0));
  std::printf("[nihao] committed [%s]\n", committed.c_str());
  require(committed == "\u4f60\u597d", "the Chinese candidate did not commit");
}

// 3. The auxiliary filter still defaults to Stroke: the trigger key enters that band and
//    the engine says so. MoQi is a value of the same option, never the default.
void section_default_auxiliary_filter() {
  Driver driver;
  driver.type("ni");
  require(!driver.page.entries.empty(), "the engine produced no candidates for \"ni\"");
  driver.keysym(0x60); // grave, the default auxiliary-filter trigger
  print_candidates("ni+grave", driver);
  require(driver.page.aux_up.find("[Stroke Filtering]") != std::string::npos,
          "the default auxiliary filter is not Stroke");
}

// 4-5. The mixed pipeline proper: one raw string that is part Chinese syllables and part
//    English word, one candidate that carries both, and a commit that is that whole mixed
//    string. Sections 1 and 2 above only prove the two halves separately, which a Chinese
//    candidate plus an English candidate in the same list would also satisfy; these read the
//    fused candidate and commit it.
//
//    Both cases are the pinned addon's own, from `test/testmixedengine.cpp` section 2
//    ("woxiangmaiiphone" -> 我想买 iPhone) and `test/testmixedshuangpin.cpp` Part C
//    (Ziranma "woiphone" -> 我 + iPhone), so the expectation is the upstream one and not
//    something written to fit this build. The target is located by its text, on the first
//    screen but not necessarily first: the order inside the screen is the engine's, and a
//    test must not push it around.
void section_mixed_full_pinyin() {
  const std::string expected = "\u6211\u60f3\u4e70iPhone"; // 我想买iPhone
  Driver driver;
  driver.type("woxiangmaiiphone");
  print_candidates("woxiangmaiiphone", driver);
  const auto committed = commit_first_screen(driver, expected);
  std::printf("[woxiangmaiiphone] committed [%s]\n", committed.c_str());
  require(!committed.empty(),
          "no candidate fused the Chinese reading with the English word for "
          "\"woxiangmaiiphone\"");
  require(committed == expected,
          "the fused candidate did not commit the whole mixed text");
}

// 5. The same on the second input method. Ziranma is the engine's default Shuangpin
//    profile, so "wo" is 我 and the rest of the raw is the English word. An even-length
//    English tail is also a run of valid two-letter Ziranma codes, so the classical decoder
//    also has a whole-Han reading of the raw; before the placement fix that sent the fused
//    candidate to page 16, and it now takes the front-row slot behind the Chinese head.
void section_mixed_shuangpin() {
  const std::string expected = "\u6211iPhone"; // 我iPhone
  FcitxBackendOptions options;
  // A user whose group holds Shuangpin: applied in memory through the same normalization
  // the product uses at startup, so nothing is written and the owner's profile is untouched.
  options.fixture_input_methods = {"shuangpin"};
  options.fixture_default_method = "shuangpin";
  Driver driver(std::move(options));
  driver.type("woiphone");
  print_candidates("shuangpin woiphone", driver);
  const auto committed = commit_first_screen(driver, expected);
  std::printf("[shuangpin woiphone] committed [%s]\n", committed.c_str());
  require(!committed.empty(),
          "no candidate fused the Ziranma reading with the English word for \"woiphone\"");
  require(committed == expected,
          "the fused Shuangpin candidate did not commit the whole mixed text");
}

// 6. The owner's case (2026-10-23): Ziranma "woxdmliphone" (preedit wo xd ml ip ho ne) must
//    offer 我想买iPhone on the first screen and commit it whole. Not a ranking preference: the
//    candidate was generated all along and landed at index 127, behind the whole classical
//    list, because the bounded insertion class only accepted a canonical English word at the
//    FRONT of the raw ("openaimoxing" -> [OpenAI][模型]). Ziranma produces the trailing shape
//    often — an even-length English tail is also a run of two-letter codes, so "ip ho ne"
//    force-decodes to 蠢货呢 — and the class is position-independent now.
void section_mixed_ziranma_trailing() {
  const std::string expected = "\u6211\u60f3\u4e70iPhone"; // 我想买iPhone
  FcitxBackendOptions options;
  options.fixture_input_methods = {"shuangpin"};
  options.fixture_default_method = "shuangpin";
  Driver driver(std::move(options));
  driver.type("woxdmliphone");
  print_candidates("shuangpin woxdmliphone", driver);
  const auto committed = commit_first_screen(driver, expected);
  std::printf("[shuangpin woxdmliphone] committed [%s]\n", committed.c_str());
  require(!committed.empty(),
          "the fused Ziranma reading is not on the first screen for \"woxdmliphone\"");
  require(committed == expected,
          "the fused Ziranma candidate did not commit the whole mixed text");
}

} // namespace

int main() {
  try {
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    section_english_candidate();
    section_chinese_candidate();
    section_default_auxiliary_filter();
    section_mixed_full_pinyin();
    section_mixed_shuangpin();
    section_mixed_ziranma_trailing();
    std::printf("PASS the Host runs the enhanced Chinese runtime: English candidates, the "
                "Chinese candidate, the Stroke default, and a mixed Chinese/English "
                "candidate that commits whole on Pinyin and Shuangpin\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("FAIL %s\n", error.what());
    return 1;
  }
}
