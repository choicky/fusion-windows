// The enhanced Chinese runtime inside the product Host.
//
// Windows now runs the same fcitx5-chinese-addons implementation as the Android build
// (choicky/fcitx5-chinese-addons, pinned in tools/fcitx/build-native.ps1), so the English
// lexicon and the mixed Chinese/English pipeline are native parts of the engine the Host
// already loads. This check measures that on the real pinned engine and the packed data:
// a Latin word produces an English candidate from the lexicon and commits it, a Chinese
// syllable still produces the Chinese candidate and commits it, and the default auxiliary
// filter is still Stroke (MoQi is opt-in through the configuration, unchanged default).
//
// Printed lines are the evidence record. It runs as an isolated process: no TIP is
// registered, no product installation is used and no user configuration is read or
// written (the fixture skips Fcitx's per-user paths).

#include "fusion/backend.hpp"

#include <crtdbg.h>

#include <cstdio>
#include <stdexcept>
#include <string>
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

  Driver() : backend(make_fcitx_backend({})) {
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

} // namespace

int main() {
  try {
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    section_english_candidate();
    section_chinese_candidate();
    section_default_auxiliary_filter();
    std::printf("PASS the Host runs the enhanced Chinese runtime: English candidates, the "
                "Chinese candidate, and the Stroke default\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("FAIL %s\n", error.what());
    return 1;
  }
}
