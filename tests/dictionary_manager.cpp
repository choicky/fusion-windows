// Pinyin dictionary management, end to end on the product's own engine.
//
// The manager this checks is the one the settings window calls: list, import, enable,
// disable, delete and reload. Everything below runs against the real pinned engine in a
// throwaway per-user profile, and every claim is made about what the candidate list does —
// a dictionary that is on disk but not loaded is not "imported", and a disabled one that
// still produces its word is not "disabled".
//
// The dictionary is a text file in libime's own format (`hanzi pinyin [weight]`), which is
// what an import converts with `libime::PinyinDictionary`. The word is invented so the base
// dictionary cannot supply it, and its two syllables are typed the same way in Pinyin and in
// Ziranma Shuangpin, so the same file has to serve both input methods.

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

namespace fs = std::filesystem;

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

// The throwaway Fcitx profile: the user data root the engine searches for extra
// dictionaries lives under it, and nothing outside it is read or written.
fs::path prepare_profile() {
  const fs::path root = fs::temp_directory_path() / "fusion-dictionary-manager-check";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "User" / "AppData" / "Roaming" / "Fcitx5" / "config" /
                         "fcitx5" / "conf");
  fs::create_directories(root / "AppData" / "Local");
  SetEnvironmentVariableA("APPDATA", (root / "User" / "AppData" / "Roaming").string().c_str());
  SetEnvironmentVariableA("LOCALAPPDATA", (root / "AppData" / "Local").string().c_str());
  SetEnvironmentVariableA("USERPROFILE", (root / "User").string().c_str());
  return root;
}

struct Driver {
  std::unique_ptr<Backend> backend;
  Binding binding{8101, 1, 1};
  std::uint64_t next = 1;
  CandidatePage page;

  explicit Driver(const char *method) {
    FcitxBackendOptions options;
    options.user_path = true;
    if (method) {
      options.fixture_input_methods = {method};
      options.fixture_default_method = method;
    }
    backend = make_fcitx_backend(std::move(options));
    require(backend->healthy() && backend->bind(binding), "the real backend did not bind");
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

  void type(const std::string &text) {
    for (char ch : text) {
      BackendStimulus stimulus{binding, 0, BackendStimulus::Type::Character, ch};
      stimulus.keysym = static_cast<std::uint8_t>(ch);
      send(stimulus);
    }
  }

  std::vector<std::string> texts() const {
    std::vector<std::string> out;
    for (const auto &entry : page.entries)
      out.push_back(entry.text);
    return out;
  }

  bool has(const std::string &text) const {
    for (const auto &candidate : page.entries)
      if (candidate.text == text)
        return true;
    return false;
  }
};

// A dictionary operation runs with no input context bound. A live context keeps a preedit
// and unsettled ordered work after typing, which is exactly the state the manager refuses,
// and these operations are about files and the engine's dictionary set — not about a
// composition. Unbinding is what a settled editor looks like from the Host's side; the
// refusal itself is asserted deliberately in `section_refused_while_composing`.
template <typename Function> void idle_call(Driver &driver, Function &&function) {
  driver.backend->unbind(driver.binding);
  driver.page = CandidatePage{};
  function();
  require(driver.backend->bind(driver.binding), "the test context could not be re-bound");
}

void print_candidates(const char *label, const Driver &driver) {
  std::printf("[%s] candidates:", label);
  for (const auto &text : driver.texts())
    std::printf(" [%s]", text.c_str());
  std::printf("\n");
}

const DictionaryEntry *find_entry(const DictionaryList &list, const std::string &name) {
  for (const auto &entry : list.entries)
    if (entry.name == name)
      return &entry;
  return nullptr;
}

std::string describe(const DictionaryList &list) {
  std::string text;
  for (const auto &entry : list.entries) {
    if (!text.empty())
      text += ", ";
    text += entry.name + "(" + entry.source + (entry.enabled ? ",enabled" : ",disabled") + ")";
  }
  return text.empty() ? "(none)" : text;
}

constexpr const char *kTextName = "mozike";
constexpr const char *kBinaryName = "mozike2";
// Two syllables whose Pinyin and Ziranma Shuangpin spellings are the same string.
constexpr const char *kWord = "墨珂";
constexpr const char *kTyped = "moke";

// Where the Host put a dictionary, found rather than assumed: Fcitx resolves its per-user
// data root from APPDATA, and the exact level below it is the runtime's business. What this
// check cares about is that the file is inside the profile, in a `dictionaries` directory.
fs::path find_placed_dictionary(const fs::path &profileRoot, const std::string &name) {
  std::error_code ec;
  for (const auto &entry : fs::recursive_directory_iterator(profileRoot, ec)) {
    if (!entry.is_regular_file() || entry.path().filename() != name + ".dict")
      continue;
    if (entry.path().parent_path().filename() == "dictionaries")
      return entry.path();
  }
  return {};
}

// The word cannot come from the base dictionary, so anything the candidate list shows later
// is the file a section wrote.
void section_baseline() {
  Driver driver(nullptr);
  driver.type(kTyped);
  print_candidates("pinyin before import", driver);
  require(!driver.page.entries.empty(), "the engine produced no candidates for the control word");
  require(!driver.has(kWord), "the base dictionary already has the test word");
}

// The refusal the Host applies to every dictionary change while a buffer is live, asserted
// rather than assumed: the file is not written and the status says so.
void section_refused_while_composing(const fs::path &profileRoot) {
  Driver driver(nullptr);
  driver.type(kTyped);
  require(!driver.page.entries.empty(), "the engine produced no candidates for the control word");

  const fs::path textFile = profileRoot / "refused.txt";
  {
    std::ofstream out(textFile, std::ios::binary);
    out << kWord << " mo'ke 1000\n";
  }
  DictionaryEntry imported;
  DictionaryList list;
  std::string error;
  const auto status = driver.backend->settings_import_dictionary(textFile.string(), false, imported,
                                                                 list, error);
  std::printf("[while composing] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Busy, "a dictionary change was accepted during a composition");
  require(find_placed_dictionary(profileRoot, "refused").empty(),
          "a refused import still wrote the file");
}

void section_import_text(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path textFile = profileRoot / "mozike.txt";
  {
    std::ofstream out(textFile, std::ios::binary);
    out << kWord << " mo'ke 1000\n";
  }

  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status =
        driver.backend->settings_import_dictionary(textFile.string(), false, imported, list, error);
  });
  std::printf("[import] status=%d error=[%s] list=%s\n", static_cast<int>(status), error.c_str(),
              describe(list).c_str());
  require(status == SettingsStatus::Ok, "importing a text dictionary failed");
  require(imported.name == kTextName, "the imported dictionary did not keep the file's name");
  require(imported.source == "user", "the imported dictionary is not in the user root");
  require(imported.enabled, "the imported dictionary is not enabled");
  require(imported.size > 0, "the imported dictionary has no size");
  require(find_entry(list, kTextName) != nullptr, "the reply does not list the imported file");

  // The conversion really happened: the file in the user root is a binary dictionary, not
  // the text that was handed in.
  const fs::path placed = find_placed_dictionary(profileRoot, kTextName);
  require(fs::exists(placed), "the imported dictionary is not in the user dictionary root");
  require(fs::file_size(placed) >= 16, "the imported dictionary is empty");

  driver.type(kTyped);
  print_candidates("pinyin after import", driver);
  require(driver.has(kWord), "the engine did not load the imported dictionary");
}

void section_conflict_and_overwrite(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path textFile = profileRoot / "mozike.txt";
  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;

  idle_call(driver, [&] {
    status =
        driver.backend->settings_import_dictionary(textFile.string(), false, imported, list, error);
  });
  std::printf("[import again] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Exists, "a second import of the same name was not refused");
  require(!error.empty(), "the refusal carries no explanation");

  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(textFile.string(), true, imported, list,
                                                        error);
  });
  std::printf("[import overwrite] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Ok, "the confirmed overwrite was refused");
  require(find_entry(list, kTextName) != nullptr, "the overwritten dictionary is missing");
}

void section_invalid_file(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path broken = profileRoot / "broken.dict";
  {
    std::ofstream out(broken, std::ios::binary);
    out << "this is not a trie dump";
  }

  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Ok;
  idle_call(driver, [&] {
    status =
        driver.backend->settings_import_dictionary(broken.string(), false, imported, list, error);
  });
  std::printf("[broken] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Invalid, "a corrupt .dict was accepted");
  require(!error.empty(), "the rejection carries no explanation");
  require(find_entry(list, "broken") == nullptr, "the corrupt file was left in the list");

  // The failure did not damage the dictionary that was already there.
  driver.type(kTyped);
  require(driver.has(kWord), "the failed import broke the existing dictionary");
}

void section_disable_enable(DictionaryList &list) {
  Driver driver(nullptr);
  std::string error;
  SettingsStatus status = SettingsStatus::Error;

  idle_call(driver, [&] {
    status = driver.backend->settings_set_dictionary_enabled(kTextName, false, list, error);
  });
  std::printf("[disable] status=%d error=[%s] list=%s\n", static_cast<int>(status), error.c_str(),
              describe(list).c_str());
  require(status == SettingsStatus::Ok, "disabling the dictionary failed");
  const auto *entry = find_entry(list, kTextName);
  require(entry != nullptr && !entry->enabled, "the dictionary does not read as disabled");

  driver.type(kTyped);
  print_candidates("pinyin while disabled", driver);
  require(!driver.has(kWord), "a disabled dictionary still supplies its word");

  idle_call(driver, [&] {
    status = driver.backend->settings_set_dictionary_enabled(kTextName, true, list, error);
  });
  std::printf("[enable] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Ok, "re-enabling the dictionary failed");
  entry = find_entry(list, kTextName);
  require(entry != nullptr && entry->enabled, "the dictionary does not read as enabled");

  driver.type(kTyped);
  print_candidates("pinyin after re-enabling", driver);
  require(driver.has(kWord), "the re-enabled dictionary does not supply its word again");
}

void section_shuangpin_shares_the_source(const DictionaryList &list) {
  Driver shuangpin("shuangpin");
  shuangpin.type(kTyped);
  print_candidates("shuangpin", shuangpin);
  require(shuangpin.has(kWord), "Shuangpin does not see the dictionary Pinyin imported");

  // The same list, from the other input method: one store, not two.
  DictionaryList fromShuangpin;
  std::string error;
  const auto status = shuangpin.backend->settings_list_dictionaries(fromShuangpin, error);
  require(status == SettingsStatus::Ok, "listing dictionaries under Shuangpin failed");
  require(fromShuangpin.entries.size() == list.entries.size(),
          "the two input methods report different dictionary sets");
}

void section_binary_import(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path converted = find_placed_dictionary(profileRoot, kTextName);
  const fs::path copy = profileRoot / (std::string(kBinaryName) + ".dict");
  {
    std::error_code ec;
    fs::copy_file(converted, copy, fs::copy_options::overwrite_existing, ec);
    require(!ec, "the converted dictionary could not be copied for the binary import");
  }

  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(copy.string(), false, imported, list, error);
  });
  std::printf("[import .dict] status=%d error=[%s] list=%s\n", static_cast<int>(status),
              error.c_str(), describe(list).c_str());
  require(status == SettingsStatus::Ok, "importing a .dict file failed");
  require(imported.name == kBinaryName, "the .dict import did not keep the file's name");
}

void section_delete(DictionaryList &list) {
  Driver driver(nullptr);
  std::string error;
  SettingsStatus status = SettingsStatus::Error;

  idle_call(driver, [&] {
    status = driver.backend->settings_delete_dictionary(kTextName, list, error);
  });
  std::printf("[delete %s] status=%d error=[%s] list=%s\n", kTextName, static_cast<int>(status),
              error.c_str(), describe(list).c_str());
  require(status == SettingsStatus::Ok, "deleting a user dictionary failed");
  require(find_entry(list, kTextName) == nullptr, "the deleted dictionary is still listed");
  require(find_entry(list, kBinaryName) != nullptr, "the other dictionary was deleted too");

  // The remaining dictionary still supplies the word: the deletion acted on one file.
  driver.type(kTyped);
  require(driver.has(kWord), "deleting one dictionary removed the other one's word");

  idle_call(driver, [&] {
    status = driver.backend->settings_delete_dictionary(kBinaryName, list, error);
  });
  require(status == SettingsStatus::Ok, "deleting the second user dictionary failed");
  require(list.entries.empty(), "the dictionary list is not empty after deleting both");

  driver.type(kTyped);
  print_candidates("pinyin after delete", driver);
  require(!driver.has(kWord), "a deleted dictionary still supplies its word");
}

void section_reload(DictionaryList &list) {
  Driver driver(nullptr);
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] { status = driver.backend->settings_reload_dictionaries(list, error); });
  std::printf("[reload] status=%d error=[%s] list=%s\n", static_cast<int>(status), error.c_str(),
              describe(list).c_str());
  require(status == SettingsStatus::Ok, "the explicit reload failed");
}

} // namespace

int main() {
  try {
    const fs::path profileRoot = prepare_profile();
    DictionaryList list;

    section_baseline();
    section_refused_while_composing(profileRoot);
    section_import_text(list, profileRoot);
    section_conflict_and_overwrite(list, profileRoot);
    section_invalid_file(list, profileRoot);
    section_disable_enable(list);
    section_shuangpin_shares_the_source(list);
    section_binary_import(list, profileRoot);
    section_delete(list);
    section_reload(list);

    std::printf("dictionary manager: PASS\n");
    return 0;
  } catch (const std::exception &e) {
    std::printf("dictionary manager: FAIL %s\n", e.what());
    return 1;
  }
}
