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
#include <iterator>
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

// The settings protocol carries a path as UTF-8, and that is what the backend converts back
// to a wide path. `fs::path::string()` would hand over the ANSI code page instead, which is
// the exact boundary a Chinese file name is lost at, so the checks pass paths this way.
std::string utf8(const fs::path &path) {
  const auto wide = path.wstring();
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string text(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length,
                      nullptr, nullptr);
  return text;
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path &path, const std::string &contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << contents;
}

// Where the Host put a dictionary, found rather than assumed: Fcitx resolves its per-user
// data root from APPDATA, and the exact level below it is the runtime's business. What this
// check cares about is that the file is inside the profile, in a `dictionaries` directory.
fs::path find_placed_dictionary(const fs::path &profileRoot, const std::wstring &name) {
  std::error_code ec;
  for (const auto &entry : fs::recursive_directory_iterator(profileRoot, ec)) {
    if (!entry.is_regular_file() || entry.path().filename() != name + L".dict")
      continue;
    if (entry.path().parent_path().filename() == "dictionaries")
      return entry.path();
  }
  return {};
}

fs::path find_placed_dictionary(const fs::path &profileRoot, const std::string &name) {
  return find_placed_dictionary(profileRoot, fs::path(name).wstring());
}

// The staging files an import is allowed to leave behind: none.
std::vector<fs::path> staging_leftovers(const fs::path &dictionaryDirectory) {
  std::vector<fs::path> found;
  std::error_code ec;
  if (!fs::is_directory(dictionaryDirectory, ec)) {
    return found;
  }
  for (const auto &entry : fs::directory_iterator(dictionaryDirectory, ec)) {
    const auto name = entry.path().filename().wstring();
    if (name.rfind(L"fusion-import-", 0) == 0) {
      found.push_back(entry.path());
    }
  }
  return found;
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
  const auto status = driver.backend->settings_import_dictionary(utf8(textFile), false, imported,
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
        driver.backend->settings_import_dictionary(utf8(textFile), false, imported, list, error);
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
        driver.backend->settings_import_dictionary(utf8(textFile), false, imported, list, error);
  });
  std::printf("[import again] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Exists, "a second import of the same name was not refused");
  require(!error.empty(), "the refusal carries no explanation");

  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(textFile), true, imported, list,
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
        driver.backend->settings_import_dictionary(utf8(broken), false, imported, list, error);
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
    status = driver.backend->settings_import_dictionary(utf8(copy), false, imported, list, error);
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

// The file an import converts into is not a name a dictionary can have. `.<name>.import.dict`
// was both a legal dictionary name and a `.dict` file the engine loads, so importing
// `collide.txt` could overwrite — and a failed import could delete — an installed dictionary
// called `.collide.import`.
void section_staging_does_not_collide(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path source = profileRoot / "staging-source.txt";
  write_file(source, std::string(kWord) + " mo'ke 1000\n");
  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(source), false, imported, list, error);
  });
  require(status == SettingsStatus::Ok, "the staging source dictionary could not be imported");

  const fs::path placed = find_placed_dictionary(profileRoot, "staging-source");
  require(!placed.empty(), "the imported dictionary is not in the user root");
  const fs::path squatter = placed.parent_path() / L".collide.import.dict";
  std::error_code ec;
  fs::copy_file(placed, squatter, fs::copy_options::overwrite_existing, ec);
  require(!ec, "the squatting dictionary could not be written");
  const auto before = read_file(squatter);

  idle_call(driver, [&] {
    std::string reloadError;
    require(driver.backend->settings_reload_dictionaries(list, reloadError) == SettingsStatus::Ok,
            "the squatting dictionary could not be loaded");
  });
  require(find_entry(list, ".collide.import") != nullptr,
          "the squatting dictionary is not listed, so this check would prove nothing");

  const fs::path collide = profileRoot / "collide.txt";
  write_file(collide, std::string(kWord) + " mo'ke 1000\n");
  idle_call(driver, [&] {
    status =
        driver.backend->settings_import_dictionary(utf8(collide), false, imported, list, error);
  });
  std::printf("[staging] status=%d error=[%s] list=%s\n", static_cast<int>(status), error.c_str(),
              describe(list).c_str());
  require(status == SettingsStatus::Ok,
          "an import next to a dictionary named like the staging file failed");
  require(fs::exists(squatter), "the import deleted a dictionary that had the staging file's name");
  require(read_file(squatter) == before, "the import overwrote that dictionary");
  require(find_entry(list, ".collide.import") != nullptr, "that dictionary left the list");
  require(staging_leftovers(placed.parent_path()).empty(), "the import left a staging file behind");
}

// A successful parse is not a dictionary. libime skips every line it cannot read and says
// nothing, so plain text, an empty file or a file of invalid lines parses "successfully" into
// an empty trie — which used to be written over the installed dictionary.
void section_invalid_text_does_not_replace(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path keep = profileRoot / "keepme.txt";
  write_file(keep, std::string(kWord) + " mo'ke 1000\n");
  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(keep), false, imported, list, error);
  });
  require(status == SettingsStatus::Ok, "the dictionary this check protects could not be imported");
  const fs::path placed = find_placed_dictionary(profileRoot, "keepme");
  require(!placed.empty(), "the protected dictionary is not in the user root");
  const auto before = read_file(placed);

  write_file(keep, "this file is not a dictionary at all, it is a sentence\n");
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(keep), true, imported, list, error);
  });
  std::printf("[plain text] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Invalid, "a text file with no dictionary entry was accepted");
  require(read_file(placed) == before, "the refused import replaced the installed dictionary");

  write_file(keep, "");
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(keep), true, imported, list, error);
  });
  std::printf("[empty text] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Invalid, "an empty text file was accepted as a dictionary");
  require(read_file(placed) == before, "the empty file replaced the installed dictionary");

  driver.type(kTyped);
  require(driver.has(kWord), "the installed dictionary stopped supplying its word");

  // An invalid line beside a valid one is still accepted: libime's own tolerance is not
  // turned into a stricter format policy here.
  write_file(keep, "justonetoken\n" + std::string(kWord) + " mo'ke 1000\n");
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(keep), true, imported, list, error);
  });
  std::printf("[partial] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Ok,
          "a file with one invalid line and one valid entry was refused");
}

// A Chinese file name is UTF-8 on the wire and a wide path on disk, and both ends of that
// conversion are the product's: the name it reports and the file it writes.
void section_chinese_name(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const std::wstring wideName = L"中文词库";
  const std::string expected = utf8(fs::path(wideName));
  const fs::path file = profileRoot / (wideName + L".txt");
  write_file(file, std::string(kWord) + " mo'ke 1000\n");

  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(file), false, imported, list, error);
  });
  std::printf("[chinese] status=%d name=[%s] expected=[%s] error=[%s]\n", static_cast<int>(status),
              imported.name.c_str(), expected.c_str(), error.c_str());
  require(status == SettingsStatus::Ok, "importing a dictionary with a Chinese name failed");
  require(imported.name == expected, "the imported dictionary's name is not the file's name");
  require(find_entry(list, expected) != nullptr, "the Chinese-named dictionary is not listed");
  const fs::path placed = find_placed_dictionary(profileRoot, wideName);
  require(!placed.empty(), "the Chinese-named dictionary is not on disk under its own name");

  driver.type(kTyped);
  require(driver.has(kWord), "the engine did not load the Chinese-named dictionary");

  idle_call(driver, [&] {
    status = driver.backend->settings_set_dictionary_enabled(expected, false, list, error);
  });
  require(status == SettingsStatus::Ok, "disabling a Chinese-named dictionary failed");
  const auto *entry = find_entry(list, expected);
  require(entry != nullptr && !entry->enabled, "it does not read back disabled");
  const fs::path marker = placed.parent_path() / (wideName + L".dict.disable");
  require(fs::exists(marker), "the disable marker was not written under the Chinese name");

  idle_call(driver, [&] {
    status = driver.backend->settings_set_dictionary_enabled(expected, true, list, error);
  });
  require(status == SettingsStatus::Ok, "enabling a Chinese-named dictionary failed");
  entry = find_entry(list, expected);
  require(entry != nullptr && entry->enabled, "it does not read back enabled");

  idle_call(driver, [&] {
    status = driver.backend->settings_delete_dictionary(expected, list, error);
  });
  std::printf("[chinese delete] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Ok, "deleting a Chinese-named dictionary failed");
  require(find_entry(list, expected) == nullptr, "it is still listed after being deleted");
  require(find_placed_dictionary(profileRoot, wideName).empty(), "its file is still on disk");
}

// The conversion finishes before the final step, so a composition that is live when the
// import is applied must stop the replacement rather than have the candidate source swapped
// underneath it. The installed file has to be exactly what it was.
void section_busy_after_conversion(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path file = profileRoot / "busy.txt";
  write_file(file, std::string(kWord) + " mo'ke 1000\n");
  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(file), false, imported, list, error);
  });
  require(status == SettingsStatus::Ok, "the dictionary this check protects could not be imported");
  const fs::path placed = find_placed_dictionary(profileRoot, "busy");
  require(!placed.empty(), "it is not in the user root");
  const auto before = read_file(placed);

  // A different word under the same name: this is what the engine would serve if the
  // replacement had gone through.
  write_file(file, "墨客 mo'ke 1000\n");

  driver.type(kTyped);
  const auto refused =
      driver.backend->settings_import_dictionary(utf8(file), true, imported, list, error);
  std::printf("[busy after conversion] status=%d error=[%s]\n", static_cast<int>(refused),
              error.c_str());
  require(refused == SettingsStatus::Busy, "an import was applied under a live composition");
  require(read_file(placed) == before, "the refused import replaced the installed dictionary");
  require(staging_leftovers(placed.parent_path()).empty(),
          "the refused import left a staging file behind");

  // What the user still has is what the engine serves.
  driver.backend->unbind(driver.binding);
  driver.page = CandidatePage{};
  require(driver.backend->bind(driver.binding), "the test context could not be re-bound");
  driver.type(kTyped);
  require(driver.has(kWord), "the refused import changed what the engine serves");
}

// A file operation that fails must say so. A delete that cannot remove the dictionary has to
// leave the dictionary and its marker alone, and enabling — which is the removal of the
// marker — has to fail when the marker cannot be removed.
void section_file_failures(DictionaryList &list, const fs::path &profileRoot) {
  Driver driver(nullptr);
  const fs::path file = profileRoot / "locked.txt";
  write_file(file, std::string(kWord) + " mo'ke 1000\n");
  DictionaryEntry imported;
  std::string error;
  SettingsStatus status = SettingsStatus::Error;
  idle_call(driver, [&] {
    status = driver.backend->settings_import_dictionary(utf8(file), false, imported, list, error);
  });
  require(status == SettingsStatus::Ok, "the dictionary this check locks could not be imported");
  const fs::path placed = find_placed_dictionary(profileRoot, "locked");
  require(!placed.empty(), "it is not in the user root");
  const fs::path marker = placed.parent_path() / L"locked.dict.disable";

  idle_call(driver, [&] {
    status = driver.backend->settings_set_dictionary_enabled("locked", false, list, error);
  });
  require(status == SettingsStatus::Ok, "disabling it failed");
  require(fs::exists(marker), "the disable marker is not there");

  // Held open without delete sharing, so Windows refuses the delete.
  const HANDLE hold = CreateFileW(placed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(hold != INVALID_HANDLE_VALUE, "the dictionary file could not be held open");
  idle_call(driver, [&] {
    status = driver.backend->settings_delete_dictionary("locked", list, error);
  });
  std::printf("[delete locked] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status != SettingsStatus::Ok,
          "deleting a file that could not be deleted reported success");
  CloseHandle(hold);

  DictionaryList after;
  std::string listError;
  require(driver.backend->settings_list_dictionaries(after, listError) == SettingsStatus::Ok,
          "the list could not be read after the failed delete");
  const auto *entry = find_entry(after, "locked");
  require(entry != nullptr, "the failed delete removed the dictionary from the list");
  require(!entry->enabled, "the failed delete changed the dictionary's enabled state");
  require(fs::exists(marker), "the failed delete removed the disable marker");

  const HANDLE markerHold = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(markerHold != INVALID_HANDLE_VALUE, "the disable marker could not be held open");
  idle_call(driver, [&] {
    status = driver.backend->settings_set_dictionary_enabled("locked", true, list, error);
  });
  std::printf("[enable locked] status=%d error=[%s]\n", static_cast<int>(status), error.c_str());
  require(status != SettingsStatus::Ok,
          "enabling with a marker that cannot be removed reported success");
  CloseHandle(markerHold);

  require(driver.backend->settings_list_dictionaries(after, listError) == SettingsStatus::Ok,
          "the list could not be read after the failed enable");
  entry = find_entry(after, "locked");
  require(entry != nullptr && !entry->enabled,
          "the failed enable reported the dictionary as enabled");

  // With nothing held the same delete succeeds, so the refusals above turned on the lock.
  idle_call(driver, [&] {
    status = driver.backend->settings_delete_dictionary("locked", list, error);
  });
  require(status == SettingsStatus::Ok, "the same delete failed with the file released");
  require(find_entry(list, "locked") == nullptr, "the dictionary is still listed");
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
    section_staging_does_not_collide(list, profileRoot);
    section_invalid_text_does_not_replace(list, profileRoot);
    section_chinese_name(list, profileRoot);
    section_busy_after_conversion(list, profileRoot);
    section_file_failures(list, profileRoot);

    std::printf("dictionary manager: PASS\n");
    return 0;
  } catch (const std::exception &e) {
    std::printf("dictionary manager: FAIL %s\n", e.what());
    return 1;
  }
}
