#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fusion {

// Transport representation of Fcitx's runtime Configuration::save() tree.
// It carries values; it does not describe or hard-code any option schema.
struct SettingsValue {
  bool is_map = false;
  std::string scalar;
  std::map<std::string, SettingsValue> entries;

  static SettingsValue string(std::string value) {
    SettingsValue result;
    result.scalar = std::move(value);
    return result;
  }
  static SettingsValue object() {
    SettingsValue result;
    result.is_map = true;
    return result;
  }

  bool operator==(const SettingsValue &) const = default;
};

// A lossless transport view of Configuration::dumpDescription(). The settings
// process reconstructs the upstream FcitxQtConfigType objects from these fields.
struct SettingsOptionDescription {
  std::string name;
  std::string type;
  std::string description;
  SettingsValue default_value;
  SettingsValue properties = SettingsValue::object();
};

struct SettingsConfigType {
  std::string name;
  std::vector<SettingsOptionDescription> options;
};

struct SettingsSnapshot {
  SettingsValue values = SettingsValue::object();
  std::vector<SettingsConfigType> types;
};

struct SettingsSelection {
  std::string selected;
  std::vector<std::string> available;
};

// Stage A input-method list management (settings_api_version 2). This models one
// Fcitx input-method group's membership using official semantics, not a Fusion
// concept. `active` is the group's Items/0 (the method a bound context follows);
// `fallback` is Fcitx's DefaultIM (NOT the active method). `enabled` is the
// ordered Items list (Items/0..N, so enabled.front() == active when non-empty);
// `available` is every input method the packaged runtime actually defines. An
// add/remove/reorder writes `enabled` + `active` back through the manager's
// setGroup/save; it never deletes a method definition file or learning data.
struct ImList {
  std::string active;
  std::string fallback;
  std::vector<std::string> enabled;
  std::vector<std::string> available;

  bool operator==(const ImList &) const = default;
};

// Pinyin dictionary management (settings_api_version 3). One entry per dictionary the
// engine can actually load: the file stem the engine matches `<stem>.disable` against,
// which root it came from, whether that marker is present, and the file's size. No word
// count is carried — nothing on disk reports one without loading the dictionary, and a
// number computed here would not be the engine's.
struct DictionaryEntry {
  std::string name;
  // "user" is the writable per-user data root (deletable, importable); "bundled" is the
  // packaged data root, which this manager never writes or deletes.
  std::string source;
  bool enabled = true;
  std::uint64_t size = 0;

  bool operator==(const DictionaryEntry &) const = default;
};

struct DictionaryList {
  std::vector<DictionaryEntry> entries;

  bool operator==(const DictionaryList &) const = default;
};

enum class SettingsStatus : std::uint8_t {
  Ok = 0,
  Error = 1,
  Busy = 2,
  Unsupported = 3,
  Invalid = 4,
  Timeout = 5,
  Indeterminate = 6,
  VersionMismatch = 7,
  // A dictionary with the requested name already exists. Nothing was written; the same
  // request with overwrite set replaces it. Separate from Invalid so the window can ask
  // instead of reporting a failure.
  Exists = 8,
};

} // namespace fusion
