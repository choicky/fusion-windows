#pragma once

#include "fusion/ipc_protocol.hpp"
#include "fusion/settings.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fusion::ipc {

// Every settings message is additive: a later version keeps the shape of the
// messages that already existed and only adds types. A Host therefore serves any
// client in [min, current] — a v1 client simply never sends the newer types — and
// a client requires a Host at least as new as itself, while a newer Host stays
// usable because it must keep the older messages intact.
//
// 3 adds pinyin dictionary management (list/import/enable/delete/reload). A v2
// client still works: it never sends the new types, and every message it does send
// keeps its shape.
inline constexpr std::uint32_t settings_api_version = 3;
inline constexpr std::uint32_t settings_api_min_version = 1;
inline constexpr std::size_t max_settings_payload = 60 * 1024;
inline constexpr std::size_t max_settings_nodes = 4096;
inline constexpr std::size_t max_settings_map_entries = 512;
inline constexpr std::size_t max_settings_types = 64;
inline constexpr std::size_t max_settings_options = 512;
inline constexpr std::size_t max_settings_depth = 16;

struct SettingsHello {
  std::uint32_t protocol = 0;
  std::uint32_t api = 0;
};

struct SettingsReplyPrefix {
  SettingsStatus status = SettingsStatus::Error;
  std::string error;
};

namespace settings_detail {

inline bool write_string(std::vector<std::uint8_t> &out, const std::string &value) {
  if (value.size() > max_string_length) {
    return false;
  }
  detail::write_string(out, value);
  return true;
}

inline bool write_value(std::vector<std::uint8_t> &out, const SettingsValue &value,
                        std::size_t depth, std::size_t &nodes) {
  if (depth > max_settings_depth || ++nodes > max_settings_nodes) {
    return false;
  }
  detail::write_u8(out, value.is_map ? 1 : 0);
  if (!value.is_map) {
    return write_string(out, value.scalar);
  }
  if (value.entries.size() > max_settings_map_entries) {
    return false;
  }
  detail::write_u32(out, static_cast<std::uint32_t>(value.entries.size()));
  for (const auto &[key, child] : value.entries) {
    if (!write_string(out, key) || !write_value(out, child, depth + 1, nodes)) {
      return false;
    }
  }
  return true;
}

inline std::optional<SettingsValue> read_value(detail::Reader &reader, std::size_t depth,
                                                std::size_t &nodes) {
  if (depth > max_settings_depth || ++nodes > max_settings_nodes) {
    return std::nullopt;
  }
  const auto tag = reader.read_u8();
  if (!tag || *tag > 1) {
    return std::nullopt;
  }
  if (*tag == 0) {
    auto scalar = reader.read_string();
    if (!scalar) {
      return std::nullopt;
    }
    return SettingsValue::string(std::move(*scalar));
  }
  auto count = reader.read_u32();
  if (!count || *count > max_settings_map_entries || nodes + *count > max_settings_nodes) {
    return std::nullopt;
  }
  SettingsValue value = SettingsValue::object();
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto key = reader.read_string();
    auto child = read_value(reader, depth + 1, nodes);
    if (!key || !child || !value.entries.emplace(std::move(*key), std::move(*child)).second) {
      return std::nullopt;
    }
  }
  return value;
}

inline bool write_option(std::vector<std::uint8_t> &out,
                         const SettingsOptionDescription &option, std::size_t &nodes) {
  if (!write_string(out, option.name) || !write_string(out, option.type) ||
      !write_string(out, option.description) ||
      !write_value(out, option.default_value, 0, nodes) ||
      !write_value(out, option.properties, 0, nodes)) {
    return false;
  }
  return true;
}

inline std::optional<SettingsOptionDescription> read_option(detail::Reader &reader,
                                                             std::size_t &nodes) {
  auto name = reader.read_string();
  auto type = reader.read_string();
  auto description = reader.read_string();
  auto default_value = read_value(reader, 0, nodes);
  auto properties = read_value(reader, 0, nodes);
  if (!name || !type || !description || !default_value || !properties || !properties->is_map) {
    return std::nullopt;
  }
  SettingsOptionDescription option;
  option.name = std::move(*name);
  option.type = std::move(*type);
  option.description = std::move(*description);
  option.default_value = std::move(*default_value);
  option.properties = std::move(*properties);
  return option;
}

inline bool write_snapshot(std::vector<std::uint8_t> &out, const SettingsSnapshot &snapshot) {
  if (!snapshot.values.is_map || snapshot.types.size() > max_settings_types) {
    return false;
  }
  std::size_t nodes = 0;
  if (!write_value(out, snapshot.values, 0, nodes)) {
    return false;
  }
  detail::write_u32(out, static_cast<std::uint32_t>(snapshot.types.size()));
  for (const auto &type : snapshot.types) {
    if (!write_string(out, type.name) || type.options.size() > max_settings_options) {
      return false;
    }
    detail::write_u32(out, static_cast<std::uint32_t>(type.options.size()));
    for (const auto &option : type.options) {
      if (!write_option(out, option, nodes)) {
        return false;
      }
    }
  }
  return out.size() <= max_settings_payload;
}

inline bool read_snapshot(detail::Reader &reader, SettingsSnapshot &snapshot) {
  std::size_t nodes = 0;
  auto values = read_value(reader, 0, nodes);
  auto type_count = reader.read_u32();
  if (!values || !values->is_map || !type_count || *type_count > max_settings_types) {
    return false;
  }
  snapshot.values = std::move(*values);
  snapshot.types.clear();
  snapshot.types.reserve(*type_count);
  for (std::uint32_t i = 0; i < *type_count; ++i) {
    auto name = reader.read_string();
    auto option_count = reader.read_u32();
    if (!name || !option_count || *option_count > max_settings_options ||
        nodes + *option_count * 2 > max_settings_nodes) {
      return false;
    }
    SettingsConfigType type;
    type.name = std::move(*name);
    type.options.reserve(*option_count);
    for (std::uint32_t j = 0; j < *option_count; ++j) {
      auto option = read_option(reader, nodes);
      if (!option) {
        return false;
      }
      type.options.push_back(std::move(*option));
    }
    snapshot.types.push_back(std::move(type));
  }
  return true;
}

inline bool write_selection(std::vector<std::uint8_t> &out,
                            const SettingsSelection &selection) {
  if (selection.available.size() > max_vector_length || !write_string(out, selection.selected)) {
    return false;
  }
  detail::write_u32(out, static_cast<std::uint32_t>(selection.available.size()));
  for (const auto &method : selection.available) {
    if (!write_string(out, method)) {
      return false;
    }
  }
  return out.size() <= max_settings_payload;
}

inline bool read_selection(detail::Reader &reader, SettingsSelection &selection) {
  auto selected = reader.read_string();
  auto count = reader.read_u32();
  if (!selected || !count || *count > max_vector_length) {
    return false;
  }
  selection.selected = std::move(*selected);
  selection.available.clear();
  selection.available.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto method = reader.read_string();
    if (!method) {
      return false;
    }
    selection.available.push_back(std::move(*method));
  }
  return true;
}

inline bool write_string_list(std::vector<std::uint8_t> &out,
                              const std::vector<std::string> &items) {
  if (items.size() > max_vector_length) {
    return false;
  }
  detail::write_u32(out, static_cast<std::uint32_t>(items.size()));
  for (const auto &item : items) {
    if (!write_string(out, item)) {
      return false;
    }
  }
  return true;
}

inline bool read_string_list(detail::Reader &reader, std::vector<std::string> &items) {
  auto count = reader.read_u32();
  if (!count || *count > max_vector_length) {
    return false;
  }
  items.clear();
  items.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto item = reader.read_string();
    if (!item) {
      return false;
    }
    items.push_back(std::move(*item));
  }
  return true;
}

// ImList: active, fallback, ordered enabled list, available list. Every string is
// capped by max_string_length and every list by max_vector_length, so a bound
// peer cannot drive unbounded allocation; readers fail closed on a short buffer
// or trailing bytes.
inline bool write_im_list(std::vector<std::uint8_t> &out, const ImList &list) {
  return write_string(out, list.active) && write_string(out, list.fallback) &&
         write_string_list(out, list.enabled) && write_string_list(out, list.available) &&
         out.size() <= max_settings_payload;
}

inline bool read_im_list(detail::Reader &reader, ImList &list) {
  auto active = reader.read_string();
  auto fallback = reader.read_string();
  if (!active || !fallback) {
    return false;
  }
  list.active = std::move(*active);
  list.fallback = std::move(*fallback);
  return read_string_list(reader, list.enabled) && read_string_list(reader, list.available);
}

// Dictionary list (settings_api_version 3). The size travels as u32: a dictionary this
// engine can load is a pinyin trie of a few tens of megabytes, and the value is display
// information only, so saturation is honest where an overflow error would not be.
inline void write_dictionary_entry(std::vector<std::uint8_t> &out, const DictionaryEntry &entry) {
  detail::write_string(out, entry.name);
  detail::write_string(out, entry.source);
  detail::write_u8(out, entry.enabled ? 1 : 0);
  detail::write_u32(out, static_cast<std::uint32_t>(entry.size > UINT32_MAX ? UINT32_MAX
                                                                           : entry.size));
}

inline bool write_dictionary_list(std::vector<std::uint8_t> &out, const DictionaryList &list) {
  if (list.entries.size() > max_settings_map_entries) {
    return false;
  }
  detail::write_u32(out, static_cast<std::uint32_t>(list.entries.size()));
  for (const auto &entry : list.entries) {
    write_dictionary_entry(out, entry);
  }
  return out.size() <= max_settings_payload;
}

inline bool read_dictionary_entry(detail::Reader &reader, DictionaryEntry &entry) {
  auto name = reader.read_string();
  auto source = reader.read_string();
  auto enabled = reader.read_u8();
  auto size = reader.read_u32();
  if (!name || !source || !enabled || !size || *enabled > 1) {
    return false;
  }
  entry = DictionaryEntry{std::move(*name), std::move(*source), *enabled == 1, *size};
  return true;
}

inline bool read_dictionary_list(detail::Reader &reader, DictionaryList &list) {
  auto count = reader.read_u32();
  if (!count || *count > max_settings_map_entries) {
    return false;
  }
  list.entries.clear();
  list.entries.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    DictionaryEntry entry;
    if (!read_dictionary_entry(reader, entry)) {
      return false;
    }
    list.entries.push_back(std::move(entry));
  }
  return true;
}

} // namespace settings_detail

inline std::vector<std::uint8_t> serialize_settings_hello(std::uint32_t protocol,
                                                           std::uint32_t api) {
  std::vector<std::uint8_t> payload;
  detail::write_u32(payload, protocol);
  detail::write_u32(payload, api);
  return payload;
}

inline std::optional<SettingsHello> deserialize_settings_hello(const std::uint8_t *data,
                                                               std::size_t size) {
  detail::Reader reader{data, size};
  auto protocol = reader.read_u32();
  auto api = reader.read_u32();
  if (!protocol || !api || !reader.at_end()) {
    return std::nullopt;
  }
  return SettingsHello{*protocol, *api};
}

inline std::vector<std::uint8_t> serialize_settings_get_request(const std::string &uri) {
  std::vector<std::uint8_t> payload;
  if (!settings_detail::write_string(payload, uri)) {
    return {};
  }
  return payload;
}

inline std::optional<std::string> deserialize_settings_get_request(const std::uint8_t *data,
                                                                   std::size_t size) {
  detail::Reader reader{data, size};
  auto uri = reader.read_string();
  if (!uri || !reader.at_end()) {
    return std::nullopt;
  }
  return uri;
}

inline bool serialize_settings_set_request(std::vector<std::uint8_t> &payload,
                                           const std::string &uri,
                                           const SettingsValue &values) {
  payload.clear();
  std::size_t nodes = 0;
  if (!values.is_map || !settings_detail::write_string(payload, uri) ||
      !settings_detail::write_value(payload, values, 0, nodes)) {
    payload.clear();
    return false;
  }
  if (payload.size() > max_settings_payload) {
    payload.clear();
    return false;
  }
  return true;
}

inline bool deserialize_settings_set_request(const std::uint8_t *data, std::size_t size,
                                             std::string &uri, SettingsValue &values) {
  if (size > max_settings_payload) {
    return false;
  }
  detail::Reader reader{data, size};
  auto parsed_uri = reader.read_string();
  std::size_t nodes = 0;
  auto parsed_values = settings_detail::read_value(reader, 0, nodes);
  if (!parsed_uri || !parsed_values || !parsed_values->is_map || !reader.at_end()) {
    return false;
  }
  uri = std::move(*parsed_uri);
  values = std::move(*parsed_values);
  return true;
}

// Stage A: the input-method list-set request carries the desired ordered enabled
// list (Items/0..N). The active method is enabled.front(); the fallback and each
// entry's keyboard layout are preserved server-side, so they are not sent here.
inline std::vector<std::uint8_t> serialize_settings_set_im_list_request(
    const std::vector<std::string> &enabled) {
  std::vector<std::uint8_t> payload;
  if (!settings_detail::write_string_list(payload, enabled) ||
      payload.size() > max_settings_payload) {
    payload.clear();
  }
  return payload;
}

inline bool deserialize_settings_set_im_list_request(const std::uint8_t *data, std::size_t size,
                                                     std::vector<std::string> &enabled) {
  if (size > max_settings_payload) {
    return false;
  }
  detail::Reader reader{data, size};
  if (!settings_detail::read_string_list(reader, enabled) || !reader.at_end()) {
    return false;
  }
  return true;
}

// A dictionary import carries the local file path, never its contents: the Host reads the
// file itself, so nothing large ever travels through the settings pipe. `overwrite` is the
// answer to a previous `Exists`, and is the only way an existing dictionary is replaced.
inline bool deserialize_settings_import_dictionary_request(const std::uint8_t *data,
                                                           std::size_t size, std::string &path,
                                                           bool &overwrite) {
  if (size > max_settings_payload) {
    return false;
  }
  detail::Reader reader{data, size};
  auto read_path = reader.read_string();
  auto flag = reader.read_u8();
  if (!read_path || !flag || *flag > 1 || !reader.at_end()) {
    return false;
  }
  path = std::move(*read_path);
  overwrite = *flag == 1;
  return true;
}

inline bool deserialize_settings_set_dictionary_enabled_request(const std::uint8_t *data,
                                                               std::size_t size,
                                                               std::string &name,
                                                               bool &enabled) {
  if (size > max_settings_payload) {
    return false;
  }
  detail::Reader reader{data, size};
  auto read_name = reader.read_string();
  auto flag = reader.read_u8();
  if (!read_name || !flag || *flag > 1 || !reader.at_end()) {
    return false;
  }
  name = std::move(*read_name);
  enabled = *flag == 1;
  return true;
}

inline bool deserialize_settings_dictionary_name_request(const std::uint8_t *data,
                                                         std::size_t size, std::string &name) {
  if (size > max_settings_payload) {
    return false;
  }
  detail::Reader reader{data, size};
  auto read_name = reader.read_string();
  if (!read_name || !reader.at_end()) {
    return false;
  }
  name = std::move(*read_name);
  return true;
}

inline std::vector<std::uint8_t> serialize_settings_status(SettingsStatus status,
                                                           const std::string &error) {
  std::vector<std::uint8_t> payload;
  detail::write_u8(payload, static_cast<std::uint8_t>(status));
  (void)settings_detail::write_string(payload, error);
  return payload;
}

inline std::optional<SettingsReplyPrefix>
deserialize_settings_status(detail::Reader &reader) {
  auto status = reader.read_u8();
  auto error = reader.read_string();
  if (!status || *status > static_cast<std::uint8_t>(SettingsStatus::Exists) || !error) {
    return std::nullopt;
  }
  return SettingsReplyPrefix{static_cast<SettingsStatus>(*status), std::move(*error)};
}

} // namespace fusion::ipc
