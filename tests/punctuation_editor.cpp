// Real-engine checks for the zh_CN punctuation subconfiguration exposed through the
// settings transport. It needs a writable throwaway profile (FUSION_TEST_PUNCTUATION_PROFILE
// must contain the directory StandardPaths resolves Fcitx's per-user data to); otherwise it
// prints SKIP, naming that directory, and touches nothing. Every change goes through
// Backend::settings_set_config, the same Host entry point the WinUI client uses, and every
// assertion about a save also checks the canonical Fcitx data file: an in-memory readback
// alone would not prove that anything was stored.

#include "fusion/backend.hpp"

#include <aclapi.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcitx-utils/standardpaths.h>

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

// The addon writes through StandardPaths, so the one authority to ask where the canonical file
// is — and the only path the assertions may name — is that same StandardPaths. Redirecting
// APPDATA alone is not enough on Windows: the roaming folder is resolved through the shell from
// the user profile, so a throwaway APPDATA with the real USERPROFILE still lands in the product
// profile, and the test would then assert one file while the engine wrote another.
std::filesystem::path data_file() {
  return fcitx::StandardPaths::global().userDirectory(fcitx::StandardPathsType::PkgData) /
         "punctuation" / "punc.mb.zh_CN";
}

bool inside(const std::filesystem::path &root, const std::filesystem::path &candidate) {
  auto candidatePart = candidate.begin();
  for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++candidatePart) {
    if (rootPart->empty() && rootPart == root.begin()) {
      continue;
    }
    if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
      return false;
    }
  }
  return true;
}

std::string file_text() {
  std::ifstream file(data_file(), std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Fcitx's atomic save leaves nothing behind when it fails, and a temporary file here would be
// a second copy of a table that is not the stored one.
std::size_t temp_leftovers() {
  const auto directory = data_file().parent_path();
  if (!std::filesystem::is_directory(directory)) {
    return 0;
  }
  std::size_t count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with("punc.mb.zh_CN_")) {
      ++count;
    }
  }
  return count;
}

// Denying "create files" on the punctuation directory makes Fcitx's atomic save fail before it
// can create its temporary file — the write half of a failed save. Holding the target open
// without delete sharing (below) fails later, at the atomic replacement, so both halves of
// StandardPaths::safeSave are exercised. The previous DACL is restored on destruction.
class DenyDirectoryWrites {
public:
  explicit DenyDirectoryWrites(const std::filesystem::path &directory) : directory_(directory) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    require(::GetNamedSecurityInfoW(directory_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, &acl, nullptr,
                                    &descriptor) == ERROR_SUCCESS,
            "the punctuation directory's ACL is readable");
    descriptor_.assign(static_cast<const unsigned char *>(descriptor),
                       static_cast<const unsigned char *>(descriptor) +
                           ::GetSecurityDescriptorLength(descriptor));
    ::LocalFree(descriptor);

    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sid);
    require(::CreateWellKnownSid(WinWorldSid, nullptr, sid, &sidSize) != FALSE,
            "the Everyone SID is available");
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    entry.grfAccessMode = DENY_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    ::BuildTrusteeWithSidW(&entry.Trustee, sid);
    PACL denied = nullptr;
    require(::SetEntriesInAclW(1, &entry, acl, &denied) == ERROR_SUCCESS,
            "the deny entry can be built");
    const auto status = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(directory_.c_str()),
                                                SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                nullptr, denied, nullptr);
    ::LocalFree(denied);
    require(status == ERROR_SUCCESS, "the deny entry can be applied");
  }
  ~DenyDirectoryWrites() { restore(); }
  DenyDirectoryWrites(const DenyDirectoryWrites &) = delete;
  DenyDirectoryWrites &operator=(const DenyDirectoryWrites &) = delete;

  void restore() {
    if (descriptor_.empty()) {
      return;
    }
    PACL previous = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    ::GetSecurityDescriptorDacl(reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor_.data()),
                                &present, &previous, &defaulted);
    ::SetNamedSecurityInfoW(const_cast<LPWSTR>(directory_.c_str()), SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION, nullptr, nullptr,
                            present != FALSE ? previous : nullptr, nullptr);
    descriptor_.clear();
  }

private:
  std::filesystem::path directory_;
  std::vector<unsigned char> descriptor_;
};

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

  // The addon refuses the rows it cannot store, so a successful answer always means every
  // submitted row is in the file: a column holding a space cannot survive the space-separated
  // file, and an empty mapping is a row set() would drop.
  for (const auto &rejected : std::vector<Row>{{"、", "a b", ""}, {"。", "", ""}}) {
    auto withRejected = rows;
    withRejected.push_back(rejected);
    require(backend.settings_set_config(uri, value_of(withRejected), snapshot, error) ==
                SettingsStatus::Invalid && !error.empty(),
            "a row the addon cannot store is rejected with a reason");
    require(file_text() == beforeInvalid, "and the rejected row wrote nothing");
  }

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
  require(temp_leftovers() == 0, "and the failed replacement left no temporary file behind");

  // The same edit succeeds once the file can be replaced, which also proves the error was not
  // a rejected value.
  require(backend.settings_set_config(uri, value_of(attempted), snapshot, error) ==
              SettingsStatus::Ok,
          "the same edit succeeds after the lock is released");
  require(file_text().find("= ＝\n") != std::string::npos,
          "and the new row is on disk rather than only in memory");
  rows = attempted;

  // The other half of a failed atomic save: the directory cannot take the temporary file, so
  // the write never starts. The previous file and the live map must still be exactly as they
  // were, and Fcitx's own cleanup must leave no temporary file beside it.
  const auto beforeUnwritable = file_text();
  {
    DenyDirectoryWrites denied(path.parent_path());
    auto unwritable = rows;
    unwritable.push_back({"；", "；", ""});
    require(backend.settings_set_config(uri, value_of(unwritable), snapshot, error) ==
                SettingsStatus::Error && !error.empty(),
            "a write that cannot create its temporary file is reported as unsaved");
    denied.restore();
    require(file_text() == beforeUnwritable, "and the previous file is untouched");
    require(temp_leftovers() == 0, "and no temporary file is left beside it");
  }
  SettingsSnapshot afterUnwritable;
  require(backend.settings_get_config(uri, afterUnwritable, error) == SettingsStatus::Ok &&
              rows_of(afterUnwritable.values) == rows,
          "the live map still holds the previous rows after a failed write");

  // A deliberately empty table is a table, not a missing one: the addon writes zero rows, the
  // canonical file becomes empty, and the Host reports the empty list rather than the defaults.
  require(backend.settings_set_config(uri, value_of({}), snapshot, error) == SettingsStatus::Ok,
          "an explicitly empty table is accepted");
  require(file_text().empty(), "the canonical file is empty after saving no rows");
  SettingsSnapshot emptyState;
  require(backend.settings_get_config(uri, emptyState, error) == SettingsStatus::Ok,
          "the empty table can be read back");
  const auto emptyEntries = emptyState.values.entries.find("Entries");
  require(emptyEntries != emptyState.values.entries.end() && !emptyEntries->second.is_map &&
              emptyEntries->second.scalar.empty(),
          "and the Host reports it as the empty list Fcitx stores for no rows");

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
  const auto canonical = data_file().lexically_normal();
  if (!profile || !inside(std::filesystem::path(profile).lexically_normal(),
                          canonical)) {
    std::cout << "SKIP punctuation_editor: set FUSION_TEST_PUNCTUATION_PROFILE so that it "
                 "contains "
              << canonical.string() << '\n';
    return 0;
  }
  try {
    const auto backend = make_fcitx_backend(FcitxBackendOptions{/*user_path=*/true});
    require(backend->healthy(), "real backend not ready");
    run(*backend);
    std::cout << "PASS punctuation editor: official subconfig, checked save, one-press #, paired "
                 "row, refused rows, failed write, failed replacement, empty table and defaults\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
