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

enum class SettingsStatus : std::uint8_t {
  Ok = 0,
  Error = 1,
  Busy = 2,
  Unsupported = 3,
  Invalid = 4,
  Timeout = 5,
  Indeterminate = 6,
  VersionMismatch = 7,
};

} // namespace fusion
