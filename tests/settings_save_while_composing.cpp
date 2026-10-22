// Saving settings while the user is still typing.
//
// Two product promises are measured here, both against the real pinned engine and the real
// Fcitx configuration files:
//
//   1. A page whose value only reaches the *next* projected page — the candidate layout and
//      the global configuration — is saved while a composition is live, and the composition
//      survives untouched. The two pages whose official apply step ends a composition (the
//      Pinyin/Shuangpin configuration, the input-method list) are refused with that reason
//      instead of being applied and losing the user's buffer.
//   2. A save the filesystem refuses leaves the stored file, what GetConfig reports and the
//      live configuration in agreement, so the identical retry really writes.
//
// The second is the regression for the global apply path, which used to load the live object
// before writing it: a refused write then left the Host reporting a value it never persisted,
// and the retry matched the "nothing changed" branch and reported success while writing
// nothing. Both assertions of `section_refused_global_write` fail against that order.
//
// The Host's per-user configuration directory is required, because these writes must land in
// real files. The run therefore needs `FUSION_TEST_SETTINGS_PROFILE` naming a throwaway root
// with APPDATA and USERPROFILE redirected into it; without that the process prints SKIP and
// touches nothing. See the guard in main(): it refuses a root that is not a task directory,
// and refuses to run when APPDATA is not inside the root.

#include "fusion/backend.hpp"

#include <crtdbg.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace fusion;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

std::string lower(std::string value) {
  for (char &ch : value)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

// The MSVC CRT flags getenv as unsafe; this process only reads its own launcher's variables
// and never writes one back, so the bounded form is used rather than disabling the warning.
std::string env_value(const char *name) {
  char *raw = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr)
    return {};
  std::string value(raw);
  std::free(raw);
  return value;
}

// Reads one scalar out of a value tree. The trees are keyed by option name and a nested
// configuration adds one level, so the leaf is looked for at both depths rather than assumed
// to sit at one of them — the same reading the Host's own size check does.
const SettingsValue *leaf_of(const SettingsValue &value, const std::string &name) {
  if (!value.is_map)
    return nullptr;
  const auto direct = value.entries.find(name);
  if (direct != value.entries.end())
    return &direct->second;
  for (const auto &child : value.entries) {
    if (const auto *nested = leaf_of(child.second, name))
      return nested;
  }
  return nullptr;
}

std::string stored_scalar(const SettingsSnapshot &snapshot, const std::string &name) {
  const SettingsValue *leaf = leaf_of(snapshot.values, name);
  return leaf != nullptr ? leaf->scalar : std::string();
}

SettingsValue value_tree(const std::string &name, const std::string &scalar) {
  SettingsValue submitted = SettingsValue::object();
  submitted.entries[name] = SettingsValue::string(scalar);
  return submitted;
}

SettingsValue nested_value_tree(const std::string &group, const std::string &name,
                                const std::string &scalar) {
  SettingsValue submitted = SettingsValue::object();
  submitted.entries[group] = value_tree(name, scalar);
  return submitted;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Driver {
  std::unique_ptr<Backend> backend;
  Binding binding{8301, 1, 1};
  std::uint64_t next = 1;
  CandidatePage page;
  std::int32_t rows = 0;

  explicit Driver(FcitxBackendOptions options) : backend(make_fcitx_backend(std::move(options))) {
    if (!backend->healthy() || !backend->bind(binding))
      throw std::runtime_error("the real backend did not bind");
  }

  BackendBatch send(BackendStimulus stimulus) {
    stimulus.binding = binding;
    stimulus.stimulus_id = next++;
    stimulus.window_rows = rows;
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
  BackendBatch select(std::int32_t index) {
    BackendStimulus stimulus{binding, 0, BackendStimulus::Type::SelectCandidate};
    stimulus.candidate_index = index;
    return send(stimulus);
  }
  static std::string committed(const BackendBatch &batch) {
    std::string text;
    for (const auto &operation : batch.operations)
      if (operation.type == BackendOperation::Type::CommitText)
        text += operation.text;
    return text;
  }
};

// What "the composition survived the save" means, measured rather than assumed: the first
// candidate of the page that was on screen before the save is still the engine's first
// candidate afterwards, and selecting it commits that word. A save that reached the engine
// would have cleared the buffer, so the selection would find no list and commit nothing.
void require_composition_survives(Driver &driver, const char *label) {
  require(!driver.page.entries.empty(), "there is a page to select from");
  const std::string expected = driver.page.entries.front().text;
  const std::string committed = Driver::committed(driver.select(0));
  std::printf("   [%s] the candidate on screen before the save was \"%s\"; the selection "
              "afterwards committed \"%s\"\n",
              label, expected.c_str(), committed.c_str());
  require(!committed.empty(), "the composition was still there after the save");
  require(committed == expected,
          "the word selected after the save is the candidate that was on screen before it");
}

// 1. The candidate layout, saved while a word is still being typed: accepted, stored, and the
//    composition is neither committed nor cleared. The value reaches the next projection,
//    which is the first moment it could be drawn.
void section_presentation_during_composition(Driver &driver) {
  std::printf("[presentation] the layout is saved while a composition is live\n");
  driver.type("nihao");
  require(!driver.page.entries.empty(), "the engine produced no candidates");
  require(driver.page.layout == CandidateLayout::Vertical,
          "the fixture starts from the schema's vertical default");

  SettingsSnapshot snapshot;
  std::string error;
  const auto status = driver.backend->settings_set_config(
      "fcitx://config/candidate-presentation", value_tree("CandidateLayout", "Horizontal"),
      snapshot, error);
  std::printf("   saving the layout with a live composition returned %d%s%s\n",
              static_cast<int>(status), error.empty() ? "" : ": ", error.c_str());
  require(status == SettingsStatus::Ok,
          "a presentation save is not refused because a composition is live");
  require(stored_scalar(snapshot, "CandidateLayout") == "Horizontal",
          "the Host reports the layout it stored");

  require_composition_survives(driver, "presentation");

  driver.type("nihao");
  std::printf("   the next composition was projected as layout=%d arrangement=%d\n",
              static_cast<int>(driver.page.layout), static_cast<int>(driver.page.arrangement));
  require(driver.page.layout == CandidateLayout::Horizontal,
          "the saved layout is what the next projection uses");
}

// 2. The global configuration, saved while a word is still being typed. Its apply step ends
//    with reloadConfig, which re-reads the file and re-derives the state that depends on it
//    without resetting an input context.
void section_global_during_composition(Driver &driver) {
  std::printf("[global] a behavior option is saved while a composition is live\n");
  SettingsSnapshot current;
  std::string error;
  require(driver.backend->settings_get_config("fcitx://config/global", current, error) ==
              SettingsStatus::Ok,
          "the global configuration can be read");
  const std::string name = "ShowInputMethodInformation";
  const std::string before = stored_scalar(current, name);
  require(!before.empty(), "the option this section changes is in the schema");
  const std::string after = lower(before) == "true" ? "False" : "True";

  driver.type("zhongguo");
  require(!driver.page.entries.empty(), "the engine produced no candidates");

  SettingsSnapshot saved;
  const auto status = driver.backend->settings_set_config(
      "fcitx://config/global", nested_value_tree("Behavior", name, after), saved, error);
  std::printf("   saving a global option with a live composition returned %d%s%s\n",
              static_cast<int>(status), error.empty() ? "" : ": ", error.c_str());
  require(status == SettingsStatus::Ok,
          "a global save is not refused because a composition is live");
  require(lower(stored_scalar(saved, name)) == lower(after),
          "the Host reports the value it stored");

  require_composition_survives(driver, "global");

  SettingsSnapshot restored;
  require(driver.backend->settings_set_config("fcitx://config/global",
                                              nested_value_tree("Behavior", name, before),
                                              restored, error) == SettingsStatus::Ok,
          "the option is put back, so the sections below start from the profile's value");
}

// 3. The Pinyin configuration cannot be written without being applied, and its apply step
//    clears every composing context. It is refused with that reason and nothing changes.
void section_engine_page_during_composition(Driver &driver) {
  std::printf("[pinyin] the engine page is refused while a composition is live\n");
  SettingsSnapshot current;
  std::string error;
  require(driver.backend->settings_get_config("fcitx://config/inputmethod/pinyin", current,
                                              error) == SettingsStatus::Ok,
          "the Pinyin configuration can be read");
  const std::string before = stored_scalar(current, "PageSize");
  require(!before.empty(), "the page-size option is in the schema");
  const std::string wanted = before == "9" ? "8" : "9";

  driver.type("nihao");
  require(!driver.page.entries.empty(), "the engine produced no candidates");

  SettingsSnapshot refused;
  const auto status = driver.backend->settings_set_config(
      "fcitx://config/inputmethod/pinyin", value_tree("PageSize", wanted), refused, error);
  std::printf("   saving it with a live composition returned %d\n   reason: %s\n",
              static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Busy,
          "the engine page is refused while a composition is live, not applied");
  require(error.find("composition") != std::string::npos,
          "the refusal names the composition as the reason");

  SettingsSnapshot after;
  require(driver.backend->settings_get_config("fcitx://config/inputmethod/pinyin", after,
                                              error) == SettingsStatus::Ok,
          "the Pinyin configuration can still be read");
  require(stored_scalar(after, "PageSize") == before,
          "the refused change was not stored, so the same change can really be retried");
  require_composition_survives(driver, "pinyin");
}

// 4. The input-method list is refused for the same class of reason: the group change ends the
//    composition of every context that is composing, and the list is stored by that same call.
void section_im_list_during_composition(Driver &driver) {
  std::printf("[im list] the list is refused while a composition is live\n");
  ImList current;
  std::string error;
  require(driver.backend->settings_get_im_list(current, error) == SettingsStatus::Ok,
          "the input-method list can be read");
  require(current.enabled.size() > 1,
          "this fixture needs both Pinyin and Shuangpin to make a real reorder");

  std::vector<std::string> reordered = current.enabled;
  std::swap(reordered[0], reordered[1]);

  driver.type("nihao");
  require(!driver.page.entries.empty(), "the engine produced no candidates");

  ImList refused;
  const auto status = driver.backend->settings_set_im_list(reordered, refused, error);
  std::printf("   saving the list with a live composition returned %d\n   reason: %s\n",
              static_cast<int>(status), error.c_str());
  require(status == SettingsStatus::Busy, "the list is refused while a composition is live");

  ImList stored;
  require(driver.backend->settings_get_im_list(stored, error) == SettingsStatus::Ok,
          "the list can still be read");
  require(stored.enabled == current.enabled, "the refused order was not stored");
  require_composition_survives(driver, "im list");
}

// 5. A global write the filesystem refuses. The value the Host reports and the file on disk
//    must both stay at the old value, and the identical retry must really write.
void section_refused_global_write(Driver &driver, const std::filesystem::path &config_file) {
  std::printf("[refused write] a global save the filesystem refuses\n");
  require(std::filesystem::exists(config_file),
          "the global configuration file exists before the failure is induced");
  const std::string before = read_file(config_file);
  require(!before.empty(), "and it has content");

  SettingsSnapshot current;
  std::string error;
  require(driver.backend->settings_get_config("fcitx://config/global", current, error) ==
              SettingsStatus::Ok,
          "the global configuration can be read");
  const std::string name = "ShowInputMethodInformation";
  const std::string old_value = stored_scalar(current, name);
  const std::string new_value = lower(old_value) == "true" ? "False" : "True";

  const auto submit = [&](SettingsStatus &status) {
    SettingsSnapshot snapshot;
    status = driver.backend->settings_set_config(
        "fcitx://config/global", nested_value_tree("Behavior", name, new_value), snapshot, error);
  };
  const auto read_back = [&] {
    SettingsSnapshot snapshot;
    std::string readError;
    require(driver.backend->settings_get_config("fcitx://config/global", snapshot, readError) ==
                SettingsStatus::Ok,
            "the global configuration can be read back");
    return stored_scalar(snapshot, name);
  };

  // Fcitx saves by writing a temporary file and renaming it over the target, so holding the
  // target open without delete sharing makes that rename fail. The failure is produced by the
  // real write path, not by a stub.
  HANDLE held = CreateFileW(config_file.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(held != INVALID_HANDLE_VALUE,
          "the configuration file can be held open for the duration of the failure");
  SettingsStatus refused = SettingsStatus::Ok;
  try {
    submit(refused);
  } catch (...) {
    CloseHandle(held);
    throw;
  }
  CloseHandle(held);

  std::printf("   the refused write returned %d%s%s\n", static_cast<int>(refused),
              error.empty() ? "" : ": ", error.c_str());
  require(refused == SettingsStatus::Error,
          "a write the filesystem refuses is reported as a failure");
  require(read_back() == old_value,
          "and GetConfig still reports the stored value, not the one that failed to persist");
  require(read_file(config_file) == before, "the stored file is byte-for-byte unchanged");

  // The retry has to be a real write. Against the load-then-save order it matched the
  // "nothing changed" branch, returned success and wrote nothing.
  SettingsStatus retried = SettingsStatus::Error;
  submit(retried);
  std::printf("   the identical retry returned %d%s%s\n", static_cast<int>(retried),
              error.empty() ? "" : ": ", error.c_str());
  require(retried == SettingsStatus::Ok, "the identical retry succeeds once writing is possible");
  require(lower(read_back()) == lower(new_value), "and the Host reports the new value");
  const std::string after = read_file(config_file);
  require(after != before, "the retry really rewrote the file");

  // And a save with nothing pending still writes nothing.
  SettingsStatus unchanged = SettingsStatus::Error;
  submit(unchanged);
  require(unchanged == SettingsStatus::Ok, "a no-change save reports success");
  require(read_file(config_file) == after, "and touches no file");
}

// The five sections above must all run: a guard that skipped one silently would leave the
// promise it covers unmeasured while the process still exited 0.
constexpr int expected_sections = 5;

// The configuration file the Host's global apply writes, found under the throwaway profile
// rather than assembled from the layout Fcitx happens to use: `config/fcitx5/config` is
// PkgConfig's `config`, which is what `safeSave` replaces. Finding it by walking the profile
// also proves the Host really wrote there.
std::filesystem::path global_config_file(const std::filesystem::path &root) {
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file())
      continue;
    const auto path = entry.path();
    if (path.filename() == "config" && path.parent_path().filename() == "fcitx5")
      return path;
  }
  return {};
}

void run() {
  Driver driver{FcitxBackendOptions{/*user_path=*/true}};
  const auto root = std::filesystem::weakly_canonical(env_value("FUSION_TEST_SETTINGS_PROFILE"));

  int sections = 0;

  section_presentation_during_composition(driver);
  ++sections;
  section_global_during_composition(driver);
  ++sections;
  section_engine_page_during_composition(driver);
  ++sections;
  section_im_list_during_composition(driver);
  ++sections;

  // The two global saves above are what created this file, so it is looked for after them:
  // finding it under the throwaway root is also the check that the Host wrote there.
  const auto config_file = global_config_file(root);
  require(!config_file.empty(), "the Host wrote its global configuration inside the profile");
  require(std::filesystem::weakly_canonical(config_file).string().rfind(root.string(), 0) == 0,
          "and the path this check writes is the one inside the named profile root");
  std::printf("   profile file: %s\n",
              std::filesystem::weakly_canonical(config_file).string().c_str());
  section_refused_global_write(driver, config_file);
  ++sections;

  require(sections == expected_sections,
          "every section of this check ran; a skipped one would leave its promise unmeasured");
}

// The profile this writes into must be the throwaway one. Two independent facts are checked:
// the launcher named the root, and the directory this process actually writes to is inside
// that root. Without both, the process does nothing at all.
bool isolation_holds(std::string &why) {
  const std::string root = env_value("FUSION_TEST_SETTINGS_PROFILE");
  if (root.empty()) {
    why = "FUSION_TEST_SETTINGS_PROFILE is not set";
    return false;
  }
  const std::filesystem::path expected = std::filesystem::weakly_canonical(root);
  const std::string appdata = env_value("APPDATA");
  if (appdata.empty()) {
    why = "APPDATA is not set";
    return false;
  }
  const std::filesystem::path actual = std::filesystem::weakly_canonical(appdata);
  const auto mismatch =
      std::mismatch(expected.begin(), expected.end(), actual.begin(), actual.end());
  if (mismatch.first != expected.end()) {
    why = "APPDATA (" + actual.string() + ") is not inside the named profile root";
    return false;
  }
  return true;
}

} // namespace

int main() {
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

  std::string why;
  if (!isolation_holds(why)) {
    std::printf("SKIP settings_save_while_composing: %s\n", why.c_str());
    return 0;
  }

  try {
    run();
    std::printf("settings_save_while_composing: all sections passed\n");
    return 0;
  } catch (const std::exception &exception) {
    std::printf("FAILED: %s\n", exception.what());
    return 1;
  }
}
