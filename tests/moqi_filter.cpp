// The MoQi auxiliary filter, end to end, on the product's own engine.
//
// MoQi is a value of the pinyin engine's existing `AuxiliaryFilter` option (Disabled /
// Stroke / MoQi); its default is Stroke, unchanged, and `mixed_runtime` covers that default.
// Reaching MoQi therefore needs a configuration, and this check gives itself a throwaway
// one: a temporary per-user Fcitx directory, written before the engine is created, so the
// owner's real profile is never read or written. It then walks the feature the way a user
// does — type a syllable, press the auxiliary-filter trigger, type the MoQi code, select —
// and asserts the auxiliary band, the filtered candidates and the committed text.
//
// The table is the one the package ships: `moqima_gb18030.txt` in the runtime's
// `pinyinhelper` data directory, pinned by hash in the addon source.

#include "fusion/backend.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fusion;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

// The throwaway Fcitx profile: everything this process writes stays under it.
std::filesystem::path prepare_profile(const char *pinyin_conf) {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "fusion-moqi-filter-check";
  std::error_code ec;
  fs::remove_all(root, ec);
  // Fcitx resolves its per-user configuration as <Roaming>\Fcitx5\config\fcitx5, and the
  // pinyin engine reads conf/pinyin.conf under it before any input context exists.
  const fs::path conf_dir =
      root / "User" / "AppData" / "Roaming" / "Fcitx5" / "config" / "fcitx5" / "conf";
  fs::create_directories(conf_dir);
  fs::create_directories(root / "AppData" / "Local");
  {
    std::ofstream conf(conf_dir / "pinyin.conf", std::ios::binary);
    conf << pinyin_conf;
  }
  SetEnvironmentVariableA("APPDATA",
                          (root / "User" / "AppData" / "Roaming").string().c_str());
  SetEnvironmentVariableA("LOCALAPPDATA", (root / "AppData" / "Local").string().c_str());
  SetEnvironmentVariableA("USERPROFILE", (root / "User").string().c_str());
  return root;
}

struct Driver {
  std::unique_ptr<Backend> backend;
  Binding binding{8101, 1, 1};
  std::uint64_t next = 1;
  CandidatePage page;

  explicit Driver(bool user_path) {
    FcitxBackendOptions options;
    options.user_path = user_path;
    backend = make_fcitx_backend(options);
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
  BackendBatch key(std::uint32_t symbol) {
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
};

void print(const char *label, const Driver &driver) {
  std::printf("[%s] aux=[%s] candidates:", label, driver.page.aux_up.c_str());
  for (const auto &text : driver.texts())
    std::printf(" [%s]", text.c_str());
  std::printf("\n");
}

} // namespace

// 1-3: MoQi selected in the configuration.
void section_moqi(const std::filesystem::path &profile) {
  {
  Driver driver(true); // the throwaway profile above is this process's user path

    // 1. Before any trigger the list is the ordinary pinyin list and the first candidate
    //    commits exactly itself.
    driver.type("hao");
    print("hao", driver);
    require(driver.page.aux_up.find("Filtering") == std::string::npos,
            "the auxiliary band appeared before the trigger was pressed");
    require(!driver.page.entries.empty(), "no candidates for \"hao\"");
    const std::string plain_first = driver.page.entries.front().text;
    const std::string plain_commit = driver.committed(driver.select(0));
    std::printf("[hao] first=[%s] committed=[%s]\n", plain_first.c_str(),
                plain_commit.c_str());
    require(plain_commit == plain_first, "selecting the first candidate did not commit it");

    // 2. The trigger key opens the band the configuration names (MoQi, not the default
    //    Stroke), and the table is loaded by then — the filter below has data to work on.
    driver.type("hao");
    driver.key(0x60); // grave, the auxiliary-filter trigger
    print("hao+grave", driver);
    require(driver.page.aux_up.find("[MoQi Filtering]") != std::string::npos,
            "the trigger did not open the configured MoQi filter");

    // 3. The two-letter MoQi code of 好 narrows the list to the candidates whose code
    //    starts with it, and the first of them commits as that character.
    driver.type("nz");
    print("hao+nz", driver);
    require(!driver.page.entries.empty(), "the MoQi code filtered every candidate away");
    require(driver.page.entries.front().text == "\u597d",
            "the MoQi code nz does not lead with 好");
    const std::string moqi_commit = driver.committed(driver.select(0));
    std::printf("[hao+nz] committed=[%s]\n", moqi_commit.c_str());
    require(moqi_commit == "\u597d", "the MoQi-filtered candidate did not commit 好");

  }
  std::printf("[moqi] profile %s\n", profile.string().c_str());
}

// 4. The same engine with the English candidates switched off: a Latin word no longer
//    produces an English candidate, and the Chinese path is untouched.
void section_spell_disabled() {
  Driver driver(true);
  driver.type("hello");
  print("hello (SpellEnabled=false)", driver);
  for (const auto &text : driver.texts())
    require(text != "hello", "an English candidate appeared although SpellEnabled is false");
  driver.send(BackendStimulus{driver.binding, 0, BackendStimulus::Type::Escape});
  driver.type("nihao");
  print("nihao (SpellEnabled=false)", driver);
  require(!driver.page.entries.empty(), "the Chinese candidates disappeared with the switch off");
}

int main() {
  try {
    const auto profile = prepare_profile("AuxiliaryFilter=MoQi\n");
    section_moqi(profile);

    // The second configuration is read by the next engine this process creates.
    prepare_profile("AuxiliaryFilter=Stroke\nSpellEnabled=False\n");
    section_spell_disabled();

    std::error_code ec;
    std::filesystem::remove_all(profile, ec);
    std::printf("PASS MoQi auxiliary filter and the SpellEnabled switch on the product engine\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("FAIL %s\n", error.what());
    return 1;
  }
}
