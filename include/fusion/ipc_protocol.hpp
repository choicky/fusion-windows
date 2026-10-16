#pragma once

// Stage 4A out-of-process IPC protocol (UNFROZEN / not final wire format).
//
// Length-prefixed binary framing over a local Named Pipe. Each wire message is:
//   [4 bytes: payload_length (uint32_t, little-endian)]
//   [1 byte:  message_type]
//   [N bytes: payload]
// where payload_length == N. The header is 5 bytes total.
//
// Strict request/response alternation per connection. No pipelining.
// All deserialization is fail-closed: malformed input yields std::nullopt.
// All deserialize_* functions reject trailing bytes.

#include "fusion/backend.hpp"
#include "fusion/core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace fusion::ipc {

inline constexpr std::uint32_t protocol_version = 1;

inline constexpr std::size_t max_message_payload = 65536;
inline constexpr std::size_t max_string_length = 8192;
inline constexpr std::size_t max_vector_length = 64;
inline constexpr std::size_t header_size = 5;

enum class MessageType : std::uint8_t {
  HelloRequest = 0x01,
  HelloResponse = 0x02,
  BindRequest = 0x03,
  BindResponse = 0x04,
  ResetRequest = 0x05,
  ResetResponse = 0x06,
  UnbindRequest = 0x07,
  UnbindResponse = 0x08,
  SendRequest = 0x09,
  SendResponse = 0x0A,
};

struct WireFrame {
  MessageType type{};
  std::vector<std::uint8_t> payload;
};

namespace detail {

inline void write_u8(std::vector<std::uint8_t> &buf, std::uint8_t v) { buf.push_back(v); }

inline void write_u32(std::vector<std::uint8_t> &buf, std::uint32_t v) {
  buf.push_back(static_cast<std::uint8_t>(v));
  buf.push_back(static_cast<std::uint8_t>(v >> 8));
  buf.push_back(static_cast<std::uint8_t>(v >> 16));
  buf.push_back(static_cast<std::uint8_t>(v >> 24));
}

inline void write_u64(std::vector<std::uint8_t> &buf, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    buf.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void write_string(std::vector<std::uint8_t> &buf, const std::string &s) {
  write_u32(buf, static_cast<std::uint32_t>(s.size()));
  buf.insert(buf.end(), s.begin(), s.end());
}

inline void write_binding(std::vector<std::uint8_t> &buf, const Binding &b) {
  write_u64(buf, b.context);
  write_u64(buf, b.lifetime);
  write_u64(buf, b.epoch);
}

struct Reader {
  const std::uint8_t *data;
  std::size_t size;
  std::size_t pos = 0;

  bool remaining(std::size_t n) const { return pos + n <= size; }
  bool at_end() const { return pos == size; }

  std::optional<std::uint8_t> read_u8() {
    if (!remaining(1))
      return std::nullopt;
    return data[pos++];
  }

  std::optional<std::uint32_t> read_u32() {
    if (!remaining(4))
      return std::nullopt;
    std::uint32_t v = 0;
    std::memcpy(&v, data + pos, 4);
    pos += 4;
    return v;
  }

  std::optional<std::uint64_t> read_u64() {
    if (!remaining(8))
      return std::nullopt;
    std::uint64_t v = 0;
    std::memcpy(&v, data + pos, 8);
    pos += 8;
    return v;
  }

  std::optional<std::string> read_string() {
    auto len = read_u32();
    if (!len || *len > max_string_length || !remaining(*len))
      return std::nullopt;
    std::string s(reinterpret_cast<const char *>(data + pos), *len);
    pos += *len;
    return s;
  }

  std::optional<Binding> read_binding() {
    auto ctx = read_u64();
    auto lt = read_u64();
    auto ep = read_u64();
    if (!ctx || !lt || !ep)
      return std::nullopt;
    return Binding{*ctx, *lt, *ep};
  }
};

} // namespace detail

inline void serialize(std::vector<std::uint8_t> &buf, const Binding &b) {
  detail::write_binding(buf, b);
}

inline std::optional<Binding> deserialize_binding(const std::uint8_t *data, std::size_t size) {
  detail::Reader r{data, size};
  auto v = r.read_binding();
  if (v && !r.at_end())
    return std::nullopt;
  return v;
}

inline void serialize(std::vector<std::uint8_t> &buf, const BackendStimulus &s) {
  detail::write_binding(buf, s.binding);
  detail::write_u64(buf, s.stimulus_id);
  detail::write_u8(buf, static_cast<std::uint8_t>(s.type));
  detail::write_u8(buf, static_cast<std::uint8_t>(s.character));
  detail::write_u32(buf, s.keysym);
  detail::write_u32(buf, s.modifiers);
}

inline std::optional<BackendStimulus> deserialize_stimulus(const std::uint8_t *data,
                                                           std::size_t size) {
  detail::Reader r{data, size};
  auto binding = r.read_binding();
  auto stimulus_id = r.read_u64();
  auto type_byte = r.read_u8();
  auto character = r.read_u8();
  auto keysym = r.read_u32();
  auto modifiers = r.read_u32();
  if (!binding || !stimulus_id || !type_byte || !character || !keysym || !modifiers || !r.at_end())
    return std::nullopt;
  if (*type_byte > static_cast<std::uint8_t>(BackendStimulus::Type::Return))
    return std::nullopt;
  BackendStimulus s;
  s.binding = *binding;
  s.stimulus_id = *stimulus_id;
  s.type = static_cast<BackendStimulus::Type>(*type_byte);
  s.character = static_cast<char>(*character);
  s.keysym = *keysym;
  s.modifiers = *modifiers;
  return s;
}

inline void serialize(std::vector<std::uint8_t> &buf, const BackendOperation &op) {
  detail::write_u8(buf, static_cast<std::uint8_t>(op.type));
  detail::write_string(buf, op.text);
  detail::write_u32(buf, static_cast<std::uint32_t>(op.candidates.size()));
  for (const auto &c : op.candidates)
    detail::write_string(buf, c);
  detail::write_u8(buf, op.original_key ? 1 : 0);
}

inline std::optional<BackendOperation> deserialize_operation(detail::Reader &r) {
  auto type_byte = r.read_u8();
  if (!type_byte || *type_byte > static_cast<std::uint8_t>(BackendOperation::Type::ForwardKey))
    return std::nullopt;
  auto text = r.read_string();
  auto cand_count = r.read_u32();
  if (!text || !cand_count || *cand_count > max_vector_length)
    return std::nullopt;
  std::vector<std::string> candidates;
  candidates.reserve(*cand_count);
  for (std::uint32_t i = 0; i < *cand_count; ++i) {
    auto c = r.read_string();
    if (!c)
      return std::nullopt;
    candidates.push_back(std::move(*c));
  }
  auto original_key = r.read_u8();
  if (!original_key)
    return std::nullopt;
  BackendOperation op;
  op.type = static_cast<BackendOperation::Type>(*type_byte);
  op.text = std::move(*text);
  op.candidates = std::move(candidates);
  op.original_key = *original_key != 0;
  return op;
}

inline void serialize(std::vector<std::uint8_t> &buf, const BackendBatch &batch) {
  detail::write_u64(buf, batch.backend_session_id);
  serialize(buf, batch.binding);
  detail::write_u64(buf, batch.stimulus_id);
  detail::write_u8(buf, static_cast<std::uint8_t>(batch.disposition));
  detail::write_u32(buf, static_cast<std::uint32_t>(batch.operations.size()));
  for (const auto &op : batch.operations)
    serialize(buf, op);
}

inline std::optional<BackendBatch> deserialize_batch(const std::uint8_t *data, std::size_t size) {
  detail::Reader r{data, size};
  auto session_id = r.read_u64();
  auto binding = r.read_binding();
  auto stimulus_id = r.read_u64();
  auto disp_byte = r.read_u8();
  auto op_count = r.read_u32();
  if (!session_id || !binding || !stimulus_id || !disp_byte || !op_count ||
      *op_count > max_vector_length)
    return std::nullopt;
  if (*disp_byte > static_cast<std::uint8_t>(BackendBatch::Disposition::Reject))
    return std::nullopt;
  std::vector<BackendOperation> operations;
  operations.reserve(*op_count);
  for (std::uint32_t i = 0; i < *op_count; ++i) {
    auto op = deserialize_operation(r);
    if (!op)
      return std::nullopt;
    operations.push_back(std::move(*op));
  }
  if (!r.at_end())
    return std::nullopt;
  BackendBatch batch;
  batch.backend_session_id = *session_id;
  batch.binding = *binding;
  batch.stimulus_id = *stimulus_id;
  batch.disposition = static_cast<BackendBatch::Disposition>(*disp_byte);
  batch.operations = std::move(operations);
  return batch;
}

inline void serialize(std::vector<std::uint8_t> &buf, bool value) {
  detail::write_u8(buf, value ? 1 : 0);
}

inline std::optional<bool> deserialize_bool(const std::uint8_t *data, std::size_t size) {
  detail::Reader r{data, size};
  auto v = r.read_u8();
  if (!v || !r.at_end())
    return std::nullopt;
  return *v != 0;
}

inline void serialize(std::vector<std::uint8_t> &buf, std::uint64_t value) {
  detail::write_u64(buf, value);
}

inline std::optional<std::uint64_t> deserialize_u64(const std::uint8_t *data, std::size_t size) {
  detail::Reader r{data, size};
  auto v = r.read_u64();
  if (v && !r.at_end())
    return std::nullopt;
  return v;
}

inline std::vector<std::uint8_t> frame_message(MessageType type,
                                               const std::vector<std::uint8_t> &payload) {
  std::vector<std::uint8_t> frame;
  frame.reserve(header_size + payload.size());
  detail::write_u32(frame, static_cast<std::uint32_t>(payload.size()));
  detail::write_u8(frame, static_cast<std::uint8_t>(type));
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

inline std::optional<WireFrame> parse_header(const std::uint8_t *data, std::size_t size) {
  if (size < header_size)
    return std::nullopt;
  std::uint32_t payload_len = 0;
  std::memcpy(&payload_len, data, 4);
  if (payload_len > max_message_payload)
    return std::nullopt;
  if (size < header_size + payload_len)
    return std::nullopt;
  auto type_byte = data[4];
  if (type_byte < 0x01 || type_byte > 0x0A)
    return std::nullopt;
  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  frame.payload.assign(data + header_size, data + header_size + payload_len);
  return frame;
}

} // namespace fusion::ipc
