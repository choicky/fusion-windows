#pragma once

// Stage 4A/4B out-of-process IPC protocol (UNFROZEN / not final wire format).
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
//
// Stage 4B response envelope: every Host response payload starts with the
// responding Host incarnation
//   [8 bytes: host_incarnation (uint64_t, little-endian)] [response payload]
// so a client can never accept a result as belonging to a different Host
// incarnation than the one its connection established (D072 §13 incarnation
// fencing). Requests carry no envelope: a request can only be written on a
// connection whose own Hello established that connection's incarnation, and one
// pipe instance is served by exactly one Host process incarnation.
// `protocol_version` was raised 1 -> 2 for this envelope so an old/new pairing
// fails the handshake instead of misparsing.
//
// Stage 5 raised it 2 -> 3: BackendStimulus gained the non-key candidate action
// types plus a page-local index, and CandidateSnapshot's payload became a
// CandidatePage (labelled entries + page-local cursor + pager truth) instead of a
// flat string list. Same framing, same Send/Batch channel, same fencing — there is
// no candidate-specific transport — but the payload shape changed, so a mixed
// Stage 4B/Stage 5 pairing must fail the handshake rather than misparse it.
//
// Stage 5 closure raised it 3 -> 4: SetPreedit carries the displayed preedit's
// fcitx::Text contract (segment byte lengths + TextFormatFlag bits + byte
// cursor), and CandidatePage carries the panel rows (auxUp, panel preedit,
// auxDown). Same framing and fencing; only the payload shapes grew.
// The numbers below remain engineering constants: D072 keeps wire format and
// numerical thresholds UNFROZEN.

#include "fusion/backend.hpp"
#include "fusion/core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace fusion::ipc {

inline constexpr std::uint32_t protocol_version = 4;

// Size of the Stage 4B response envelope (one host_incarnation field).
inline constexpr std::size_t response_envelope_size = 8;

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
  detail::write_u32(buf, static_cast<std::uint32_t>(s.candidate_index));
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
  auto index_bits = r.read_u32();
  if (!binding || !stimulus_id || !type_byte || !character || !keysym || !modifiers ||
      !index_bits || !r.at_end())
    return std::nullopt;
  if (*type_byte > static_cast<std::uint8_t>(BackendStimulus::Type::PagePrev))
    return std::nullopt;
  const auto type = static_cast<BackendStimulus::Type>(*type_byte);
  const auto index = static_cast<std::int32_t>(*index_bits);
  // Fail closed on an index that cannot address a page slot, and on a key
  // stimulus that claims one.
  if (type == BackendStimulus::Type::SelectCandidate) {
    if (index < 0 || static_cast<std::size_t>(index) >= max_vector_length)
      return std::nullopt;
  } else if (index != -1) {
    return std::nullopt;
  }
  BackendStimulus s;
  s.binding = *binding;
  s.stimulus_id = *stimulus_id;
  s.type = type;
  s.character = static_cast<char>(*character);
  s.keysym = *keysym;
  s.modifiers = *modifiers;
  s.candidate_index = index;
  return s;
}

inline void serialize(std::vector<std::uint8_t> &buf, const CandidateEntry &entry) {
  detail::write_string(buf, entry.label);
  detail::write_string(buf, entry.text);
  detail::write_string(buf, entry.comment);
  detail::write_u8(buf, entry.placeholder ? 1 : 0);
}

inline std::optional<CandidateEntry> deserialize_entry(detail::Reader &r) {
  auto label = r.read_string();
  auto text = r.read_string();
  auto comment = r.read_string();
  auto placeholder = r.read_u8();
  if (!label || !text || !comment || !placeholder)
    return std::nullopt;
  CandidateEntry entry;
  entry.label = std::move(*label);
  entry.text = std::move(*text);
  entry.comment = std::move(*comment);
  entry.placeholder = *placeholder != 0;
  return entry;
}

inline void serialize(std::vector<std::uint8_t> &buf, const CandidatePage &page) {
  detail::write_u32(buf, static_cast<std::uint32_t>(page.entries.size()));
  for (const auto &entry : page.entries)
    serialize(buf, entry);
  detail::write_u32(buf, static_cast<std::uint32_t>(page.cursor_index));
  detail::write_u8(buf, static_cast<std::uint8_t>(page.layout));
  detail::write_u8(buf, page.has_prev ? 1 : 0);
  detail::write_u8(buf, page.has_next ? 1 : 0);
  detail::write_u8(buf, page.truncated ? 1 : 0);
  detail::write_string(buf, page.aux_up);
  detail::write_string(buf, page.panel_preedit);
  detail::write_string(buf, page.aux_down);
}

inline std::optional<CandidatePage> deserialize_page(detail::Reader &r) {
  auto count = r.read_u32();
  if (!count || *count > max_vector_length)
    return std::nullopt;
  CandidatePage page;
  page.entries.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto entry = deserialize_entry(r);
    if (!entry)
      return std::nullopt;
    page.entries.push_back(std::move(*entry));
  }
  auto cursor_bits = r.read_u32();
  auto layout = r.read_u8();
  auto has_prev = r.read_u8();
  auto has_next = r.read_u8();
  auto truncated = r.read_u8();
  auto aux_up = r.read_string();
  auto panel_preedit = r.read_string();
  auto aux_down = r.read_string();
  if (!cursor_bits || !layout || !has_prev || !has_next || !truncated || !aux_up ||
      !panel_preedit || !aux_down)
    return std::nullopt;
  if (*layout > static_cast<std::uint8_t>(CandidateLayout::Horizontal))
    return std::nullopt;
  const auto cursor = static_cast<std::int32_t>(*cursor_bits);
  // -1 (nothing highlighted) is the only legal negative value.
  if (cursor < -1 ||
      (!page.entries.empty() && cursor >= static_cast<std::int32_t>(page.entries.size())))
    return std::nullopt;
  if (page.entries.empty() && cursor >= 0)
    return std::nullopt;
  page.cursor_index = cursor;
  page.layout = static_cast<CandidateLayout>(*layout);
  page.has_prev = *has_prev != 0;
  page.has_next = *has_next != 0;
  page.truncated = *truncated != 0;
  page.aux_up = std::move(*aux_up);
  page.panel_preedit = std::move(*panel_preedit);
  page.aux_down = std::move(*aux_down);
  return page;
}

// SetPreedit's fcitx::Text contract. An absent projection travels as "no
// segments, hidden cursor" and is read back as absent, so plain-text producers
// (Mock, test doubles) keep their caret-at-end meaning.
inline void serialize(std::vector<std::uint8_t> &buf, const std::optional<FormattedPreedit> &p) {
  detail::write_u8(buf, p ? 1 : 0);
  if (!p)
    return;
  detail::write_u32(buf, static_cast<std::uint32_t>(p->segments.size()));
  for (const auto &segment : p->segments) {
    detail::write_u32(buf, segment.bytes);
    detail::write_u32(buf, segment.format);
  }
  detail::write_u32(buf, static_cast<std::uint32_t>(p->cursor));
}

inline std::optional<std::optional<FormattedPreedit>>
deserialize_preedit_format(detail::Reader &r, const std::string &text) {
  auto present = r.read_u8();
  if (!present || *present > 1)
    return std::nullopt;
  if (*present == 0)
    return std::optional<FormattedPreedit>{};
  auto count = r.read_u32();
  if (!count || *count > max_vector_length)
    return std::nullopt;
  FormattedPreedit p;
  p.segments.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto bytes = r.read_u32();
    auto format = r.read_u32();
    if (!bytes || !format || (*format & ~text_format::known) != 0)
      return std::nullopt;
    p.segments.push_back({*bytes, *format});
  }
  auto cursor_bits = r.read_u32();
  if (!cursor_bits)
    return std::nullopt;
  p.cursor = static_cast<std::int32_t>(*cursor_bits);
  // Fail closed: the segments must tile the text and the cursor must be hidden
  // or inside it (code-point alignment is checked where it is converted).
  if (!p.consistent_with(text))
    return std::nullopt;
  return std::optional<FormattedPreedit>{std::move(p)};
}

inline void serialize(std::vector<std::uint8_t> &buf, const BackendOperation &op) {
  detail::write_u8(buf, static_cast<std::uint8_t>(op.type));
  detail::write_string(buf, op.text);
  // The page is only present for CandidateSnapshot, so the reader knows the shape
  // from the type byte alone.
  if (op.type == BackendOperation::Type::CandidateSnapshot) {
    if (op.page)
      serialize(buf, *op.page);
    else
      serialize(buf, CandidatePage{});
  }
  if (op.type == BackendOperation::Type::SetPreedit)
    serialize(buf, op.preedit);
  detail::write_u8(buf, op.original_key ? 1 : 0);
}

inline std::optional<BackendOperation> deserialize_operation(detail::Reader &r) {
  auto type_byte = r.read_u8();
  if (!type_byte || *type_byte > static_cast<std::uint8_t>(BackendOperation::Type::ForwardKey))
    return std::nullopt;
  auto text = r.read_string();
  if (!text)
    return std::nullopt;
  std::optional<CandidatePage> page;
  if (*type_byte == static_cast<std::uint8_t>(BackendOperation::Type::CandidateSnapshot)) {
    auto parsed = deserialize_page(r);
    if (!parsed)
      return std::nullopt;
    page = std::move(*parsed);
  }
  std::optional<FormattedPreedit> preedit;
  if (*type_byte == static_cast<std::uint8_t>(BackendOperation::Type::SetPreedit)) {
    auto parsed = deserialize_preedit_format(r, *text);
    if (!parsed)
      return std::nullopt;
    preedit = std::move(*parsed);
  }
  auto original_key = r.read_u8();
  if (!original_key)
    return std::nullopt;
  BackendOperation op;
  op.type = static_cast<BackendOperation::Type>(*type_byte);
  op.text = std::move(*text);
  op.page = std::move(page);
  op.original_key = *original_key != 0;
  op.preedit = std::move(preedit);
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

// Stage 4B response envelope. The Host prefixes every response payload with its
// own incarnation; the client validates the prefix against the incarnation its
// connection established before it parses the response body.
inline std::vector<std::uint8_t> envelop_response(std::uint64_t host_incarnation,
                                                  const std::vector<std::uint8_t> &payload) {
  std::vector<std::uint8_t> enveloped;
  enveloped.reserve(8 + payload.size());
  detail::write_u64(enveloped, host_incarnation);
  enveloped.insert(enveloped.end(), payload.begin(), payload.end());
  return enveloped;
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
