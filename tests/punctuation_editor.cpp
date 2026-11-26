// Real-engine checks for the zh_CN punctuation subconfiguration exposed through the
// settings transport. It needs a writable throwaway profile (FUSION_TEST_PUNCTUATION_PROFILE,
// with APPDATA inside it); otherwise it prints SKIP and touches nothing. The product profile
// is never used. Every change goes through Backend::settings_set_config, the same Host entry
// point the WinUI client uses, and every assertion about a save also checks the canonical
// Fcitx data file: an in-memory readback alone would not prove that anything was stored.

#include "fusion/backend.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fusion;

namespace {

constexpr char uri[] = "fcitx://config/addon/punctuation/punctuationmap/zh_CN";

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

struct Row {
  std::string key;
  std::string mapping;
  std::string alt;
  bool operator==(const Row &) const = default;
};

std::vector<Row> rows_of(const SettingsValue &values) {
  const auto entries = values.entries.find("Entries");
  if (entries == values.entries.end() || !entries->second.is_map) {
    throw std::runtime_error("the snapshot has no Entries list");
  }
  std::vector<Row> rows;
  for (std::size_t index = 0; index < entries->second.entries.size(); ++index) {
    const auto item = entries->second.entries.find(std::to_string(index));
    if (item == entries->second.entries.end() || !item->second.is_map) {
      throw std::runtime_error("the snapshot has a non-contiguous Entries list");
    }
    auto field = [&item](const char *name) {
      const auto found = item->second.entries.find(name);
      if (found == item->second.entries.end() || found->second.is_map) {
        throw std::runtime_error("a punctuation row is malformed");
      }
      return found->second.scalar;
    };
    rows.push_back({field("Key"), field("Mapping"), field("AltMapping")});
  }
  return rows;
}

int count_rows(const std::vector<Row> &rows, const std::string &key,
               const std::string &mapping) {
  return static_cast<int>(std::count_if(rows.begin(), rows.end(), [&](const Row &row) {
           return row.key == key && row.mapping == mapping;
         }));
}

SettingsValue value_of(const std::vector<Row> &rows) {
  SettingsValue entries = SettingsValue::object();
  for (std::size_t index = 0; index < rows.size(); ++index) {
    SettingsValue row = SettingsValue::object();
    row.entries["Key"] = SettingsValue::string(rows[index].key);
    row.entries["Mapping"] = SettingsValue::string(rows[index].mapping);
    row.entries["AltMapping"] = SettingsValue::string(rows[index].alt);
    entries.entries[std::to_string(index)] = std::move(row);
  }
  SettingsValue root = SettingsValue::object();
  root.entries["Entries"] = std::move(entries);
  return root;
}

// Measured on this runtime: PunctuationProfile::save writes PkgData/punctuation/punc.mb.zh_CN
// under %APPDATA%\Fcitx5, not under the Host's separate data directory. The test's own isolated
// APPDATA is the authority here, so it cannot accidentally name the product profile.
std::filesystem::path data_file() {
  const char *appdata = std::getenv("APPDATA");
  require(appdata != nullptr, "APPDATA is not set");
  return std::filesystem::path(appdata) / "Fcitx5" / "punctuation" / "punc.mb.zh_CN";
}

std::string file_text() {
  std::ifstream file(data_file(), std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string last_commit(const BackendBatch &batch) {
  std::string text;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CommitText) {
      text = operation.text;
    }
  }
  return text;
}

std::size_t commit_count(const BackendBatch &batch) {
  return static_cast<std::size_t>(std::count_if(
      batch.operations.begin(), batch.operations.end(), [](const BackendOperation &operation) {
        return operation.type == BackendOperation::Type::CommitText;
      }));
}

class Scenario {
public:
  Scenario(Backend &backend, std::uint64_t context)
      : backend_(&backend), binding_{context, 1, 1} {
    require(backend.bind(binding_), "fixture bind failed");
  }
  ~Scenario() { backend_->unbind(binding_); }
  Scenario(const Scenario &) = delete;
  Scenario &operator=(const Scenario &) = delete;

  BackendBatch character(char value) {
    BackendStimulus stimulus{binding_, next_stimulus_++, BackendStimulus::Type::Character,
                             value};
    return backend_->send(stimulus);
  }

private:
  Backend *backend_;
  Binding binding_;
  std::uint64_t next_stimulus_ = 1;
};

void run(Backend &backend) {
  SettingsSnapshot snapshot;
  std::string error;
  require(backend.settings_get_config(uri, snapshot, error) == SettingsStatus::Ok,
          "the Host serves the official zh_CN punctuation subconfiguration");
  require(!snapshot.types.empty() && snapshot.types.front().name == "PunctuationMapConfig",
          "the schema comes from the punctuation addon itself");
  const auto type = snapshot.types.front();
  auto option = std::find_if(type.options.begin(), type.options.end(), [](const auto &candidate) {
    return candidate.name == "Entries";
  });
  require(option != type.options.end() &&
              option->type == "List|Entries$PunctuationMapEntryConfig",
          "Entries is an ordered list of the addon's own row type");
  require(option->default_value.is_map, "the schema carries the shipped map as its default");

  auto rows = rows_of(snapshot.values);
  require(count_rows(rows, "#", "#") == 1 && count_rows(rows, "#", "＃") == 1,
          "the shipped default has both ASCII and full-width candidates for #");
  const auto paired = std::find_if(rows.begin(), rows.end(), [](const Row &row) {
    return row.key == "\"" && row.mapping == "“" && row.alt == "”";
  });
  require(paired != rows.end(), "a paired punctuation row keeps Mapping and AltMapping");

  // An invalid row must be refused before the addon silently drops it, and must leave the
  // live map and the canonical file untouched.
  auto invalid = rows;
  invalid.push_back({"ab", "×", ""});
  const auto beforeInvalid = file_text();
  require(backend.settings_set_config(uri, value_of(invalid), snapshot, error) ==
              SettingsStatus::Invalid &&
              !error.empty(),
          "a multi-character key is rejected with a reason");
  SettingsSnapshot afterInvalid;
  require(backend.settings_get_config(uri, afterInvalid, error) == SettingsStatus::Ok &&
              rows_of(afterInvalid.values).size() == rows.size(),
          "the rejected write did not change the live map");
  require(file_text() == beforeInvalid, "and wrote no file");

  // Remove only the full-width # row. Repeated keys and their order are preserved, so the
  // remaining ASCII row is the one the engine will use.
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [](const Row &row) { return row.key == "#" && row.mapping == "＃"; }),
             rows.end());
  require(backend.settings_set_config(uri, value_of(rows), snapshot, error) == SettingsStatus::Ok,
          "the edited map was saved");
  const auto storedHash = file_text();
  require(storedHash.find("# #\n") != std::string::npos &&
              storedHash.find("# ＃\n") == std::string::npos,
          "the canonical Fcitx file contains the edit, not just an in-memory copy");
  SettingsSnapshot afterEdit;
  require(backend.settings_get_config(uri, afterEdit, error) == SettingsStatus::Ok &&
              count_rows(rows_of(afterEdit.values), "#", "＃") == 0,
          "and the Host reads the edited map back");

  // The product path: one press of # now has a single candidate and commits ASCII #. A pair
  // row still produces both of its candidates in Fcitx's order, with no shell-side table.
  {
    Scenario scenario(backend, 481);
    const auto hash = scenario.character('#');
    require(hash.disposition == BackendBatch::Disposition::Consumed && commit_count(hash) == 1 &&
                last_commit(hash) == "#",
            "one press of # commits ASCII '#' after the full-width row is removed");
  }
  {
    // The shipped PunctuationConfig leaves TypePairedPunctuationsTogether off, so the addon
    // alternates the two halves of this one row across presses. That is the observable proof
    // that AltMapping survived as a pair rather than becoming a second candidate row.
    Scenario scenario(backend, 482);
    const auto open = scenario.character('"');
    const auto close = scenario.character('"');
    require(last_commit(open) == "“" && last_commit(close) == "”",
            "the paired punctuation row still commits both halves through the engine");
  }

  // A genuine write refusal: hold the Fcitx data file open without delete sharing, submit a
  // valid additional row, and require an honest failure with no live or on-disk change.
  const auto path = data_file();
  auto handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "the stored file can be locked for the failure test");
  auto attempted = rows;
  attempted.push_back({"=", "＝", ""});
  const auto refused = backend.settings_set_config(uri, value_of(attempted), snapshot, error);
  ::CloseHandle(handle);
  require(refused == SettingsStatus::Error && !error.empty(),
          "a filesystem refusal is reported as an unsaved error");
  SettingsSnapshot afterRefusal;
  require(backend.settings_get_config(uri, afterRefusal, error) == SettingsStatus::Ok &&
              rows_of(afterRefusal.values).size() == rows.size(),
          "the live map still holds the previous rows");
  require(file_text() == storedHash, "and the canonical file is byte-for-byte unchanged");

  // The same edit succeeds once the file can be replaced, which also proves the error was not
  // a rejected value.
  require(backend.settings_set_config(uri, value_of(attempted), snapshot, error) ==
              SettingsStatus::Ok,
          "the same edit succeeds after the lock is released");
  require(file_text().find("= ＝\n") != std::string::npos,
          "and the new row is on disk rather than only in memory");

  // Restore defaults from the schema's DefaultValue: both # candidates and the pair return.
  SettingsValue restore = SettingsValue::object();
  restore.entries["Entries"] = option->default_value;
  require(backend.settings_set_config(uri, restore, snapshot, error) == SettingsStatus::Ok,
          "restore defaults is submitted through the same official setter");
  SettingsSnapshot restored;
  require(backend.settings_get_config(uri, restored, error) == SettingsStatus::Ok,
          "the restored map can be read back");
  SettingsValue defaultRoot = SettingsValue::object();
  defaultRoot.entries["Entries"] = option->default_value;
  const auto defaultRows = rows_of(defaultRoot);
  const auto restoredRows = rows_of(restored.values);
  require(restoredRows == defaultRows && count_rows(restoredRows, "#", "#") == 1 &&
              count_rows(restoredRows, "#", "＃") == 1,
          "restoring defaults brings back the shipped rows, including both # candidates");
  const auto restoredFile = file_text();
  require(restoredFile.find("# #\n") != std::string::npos &&
              restoredFile.find("# ＃\n") != std::string::npos &&
              restoredFile.find("\" “ ”\n") != std::string::npos &&
              restoredFile.find("= ＝\n") == std::string::npos,
          "and the canonical file is the shipped map, without the experimental extra row");
}

} // namespace

int main() {
  const char *profile = std::getenv("FUSION_TEST_PUNCTUATION_PROFILE");
  const char *appdata = std::getenv("APPDATA");
  if (!profile || !appdata || !std::filesystem::path(appdata).lexically_normal().string().starts_with(
                                  std::filesystem::path(profile).lexically_normal().string())) {
    std::cout << "SKIP punctuation_editor: set FUSION_TEST_PUNCTUATION_PROFILE and APPDATA inside "
                 "it\n";
    return 0;
  }
  try {
    const auto backend = make_fcitx_backend(FcitxBackendOptions{/*user_path=*/true});
    require(backend->healthy(), "real backend not ready");
    run(*backend);
    std::cout << "PASS punctuation editor: official subconfig, file-backed save, one-press #, "
                 "paired row, refused write and defaults\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
