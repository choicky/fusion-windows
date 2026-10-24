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
//
// Candidate presentation raised it 4 -> 5: CandidatePage carries the font the
// Windows popup draws with (family + point size), so the user's font choice takes
// effect on the next page instead of needing its own request/reply channel or a
// reconnect. The page is a payload inside SendResponse, so this is again a shape
// change rather than a new message, and an old/new pairing must fail the handshake.
//
// Automatic scrolling raised it 5 -> 6: CandidatePage carries the engine's paging
// facts (arrangement, page size, current/first/total page), so one page can describe a
// bounded window of several engine pages, and BackendStimulus gained
// SelectCandidateGlobal so a candidate in another row of that window is selected by the
// engine's own global index instead of by a page-local one that does not exist. Both are
// payload shapes inside the same messages; an old/new pairing fails the handshake.
//
// Automatic scrolling's second pass raised it 6 -> 7: BackendStimulus carries the number of
// complete candidate rows the presentation can show, because that number is decided by the
// window's work area, DPI and font and the Host cannot see any of them. The Host groups the
// scroll window by it, so a row step and a page turn stay the same thing on a short screen.
// Same message, same framing; the stimulus payload grew by one field.
//
// The settings messages below stay additive on top of this version for the reason
// they always were: FusionSettings.exe uses its own connection and its own
// settings_api_version. The numbers below remain engineering constants: D072 keeps
// wire format and numerical thresholds UNFROZEN.

#include "fusion/backend.hpp"
#include "fusion/core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace fusion::ipc {

inline constexpr std::uint32_t protocol_version = 8;

// Size of the Stage 4B response envelope (one host_incarnation field).
inline constexpr std::size_t response_envelope_size = 8;

inline constexpr std::size_t max_message_payload = 65536;
inline constexpr std::size_t max_string_length = 8192;
inline constexpr std::size_t max_vector_length = 64;
// The largest global candidate index a stimulus may name: a bound, not a page size. The
// engine's own total decides what is selectable; this only keeps the payload sane.
inline constexpr std::size_t max_candidate_index = 100000;
// Upper bounds for the paging facts, checked before they are used in any multiplication.
inline constexpr std::uint32_t max_page_size = 100;
inline constexpr std::uint32_t max_page_count = 100000;
// The most candidate rows a presentation may claim it can show. Five is the confirmed
// maximum, so anything above it is a payload this Host does not implement.
inline constexpr std::uint32_t max_candidate_rows = 5;
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
  // Additive control messages for FusionSettings.exe. The settings client starts
  // with SettingsHelloRequest rather than HelloRequest and performs one settings
  // request per connection. Existing v4 TIP clients send only 0x01-0x0A, so this
  // extension intentionally leaves protocol_version at 4 and never changes their
  // handshake or message shapes.
  SettingsHelloRequest = 0x0B,
  SettingsHelloResponse = 0x0C,
  SettingsGetConfigRequest = 0x0D,
  SettingsGetConfigResponse = 0x0E,
  SettingsSetConfigRequest = 0x0F,
  SettingsSetConfigResponse = 0x10,
  SettingsGetSelectionRequest = 0x11,
  SettingsGetSelectionResponse = 0x12,
  SettingsSelectInputMethodRequest = 0x13,
  SettingsSelectInputMethodResponse = 0x14,
  // Stage A input-method list management (settings_api_version 2). GetImList
  // returns the ordered enabled list plus the full available set; SetImList
  // writes an ordered enabled list back through the manager's setGroup/save.
  // Additive on v4, one request per connection, so existing v4 TIP and v1
  // settings clients are unaffected.
  SettingsGetImListRequest = 0x15,
  SettingsGetImListResponse = 0x16,
  SettingsSetImListRequest = 0x17,
  SettingsSetImListResponse = 0x18,
  // Pinyin dictionary management (settings_api_version 3). The list reads the two data
  // roots the engine itself searches; import converts a file through libime and places it
  // in the per-user root; enable/disable writes the engine's own `<stem>.disable` marker;
  // delete removes a user dictionary and its marker; reload calls the addon's official
  // `dictmanager` sub-config, which is what re-runs `loadExtraDict()`.
  // Additive on v4: a settings client that never sends these is unaffected.
  SettingsListDictionariesRequest = 0x19,
  SettingsListDictionariesResponse = 0x1A,
  SettingsImportDictionaryRequest = 0x1B,
  SettingsImportDictionaryResponse = 0x1C,
  SettingsSetDictionaryEnabledRequest = 0x1D,
  SettingsSetDictionaryEnabledResponse = 0x1E,
  SettingsDeleteDictionaryRequest = 0x1F,
  SettingsDeleteDictionaryResponse = 0x20,
  SettingsReloadDictionariesRequest = 0x21,
  SettingsReloadDictionariesResponse = 0x22,
};

// The transport admits frames up to the last message type it defines. Adding a type
// without raising this bound makes the frame unreadable ("invalid data") rather than
// unknown, which is why the two move together.
inline constexpr std::uint8_t max_message_type =
    static_cast<std::uint8_t>(MessageType::SettingsReloadDictionariesResponse);

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
  detail::write_u32(buf, static_cast<std::uint32_t>(s.window_rows));
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
  auto window_rows = r.read_u32();
  if (!binding || !stimulus_id || !type_byte || !character || !keysym || !modifiers ||
      !index_bits || !window_rows || !r.at_end())
    return std::nullopt;
  // A capacity is a count of rows, not a command: bound it the way a page size is bounded,
  // and treat 0 as "no opinion".
  if (*window_rows > max_candidate_rows)
    return std::nullopt;
  // The known range, not merely the first types: a value outside it is a payload this
  // Host does not implement and is rejected rather than guessed at. KeyRelease is the
  // key-up half of a modifier-only key; it carries no index and its keysym is its
  // identity, exactly like the key-down that preceded it.
  if (*type_byte > static_cast<std::uint8_t>(BackendStimulus::Type::CursorNext))
    return std::nullopt;
  const auto type = static_cast<BackendStimulus::Type>(*type_byte);
  const auto index = static_cast<std::int32_t>(*index_bits);
  // Fail closed on an index that cannot address a slot, and on a key stimulus that claims
  // one. A page-local index is bounded by a page; a global one by the whole list, so its
  // ceiling is larger — the Host re-checks both against the engine's own numbers.
  if (type == BackendStimulus::Type::SelectCandidate) {
    if (index < 0 || static_cast<std::size_t>(index) >= max_vector_length)
      return std::nullopt;
  } else if (type == BackendStimulus::Type::SelectCandidateGlobal) {
    if (index < 0 || static_cast<std::size_t>(index) >= max_candidate_index)
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
  s.window_rows = static_cast<std::int32_t>(*window_rows);
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
  // Presentation, after the page's own fields so the shape reads in the same order as
  // CandidatePage. Bounded by the same string limit every other field uses.
  detail::write_string(buf, page.font.family);
  detail::write_u32(buf, static_cast<std::uint32_t>(page.font.size_pt));
  // The window, after the font for the same reason: appended fields keep the reader's
  // order the same as CandidatePage's own.
  detail::write_u8(buf, static_cast<std::uint8_t>(page.arrangement));
  detail::write_u32(buf, static_cast<std::uint32_t>(page.page_size));
  detail::write_u32(buf, static_cast<std::uint32_t>(page.current_page));
  detail::write_u32(buf, static_cast<std::uint32_t>(page.window_first_page));
  detail::write_u32(buf, static_cast<std::uint32_t>(page.total_pages));
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
  auto font_family = r.read_string();
  auto font_size = r.read_u32();
  if (!cursor_bits || !layout || !has_prev || !has_next || !truncated || !aux_up ||
      !panel_preedit || !aux_down || !font_family || !font_size)
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
  // Fail closed on a font size outside any usable range: the popup multiplies it by the
  // monitor DPI, and an implausible value would be a payload this Host does not implement
  // rather than something to clamp silently.
  if (*font_size > 400)
    return std::nullopt;
  page.font.family = std::move(*font_family);
  page.font.size_pt = static_cast<int>(*font_size);

  auto arrangement = r.read_u8();
  auto page_size = r.read_u32();
  auto current_page = r.read_u32();
  auto window_first_page = r.read_u32();
  auto total_pages = r.read_u32();
  if (!arrangement || !page_size || !current_page || !window_first_page || !total_pages)
    return std::nullopt;
  if (*arrangement > static_cast<std::uint8_t>(CandidateArrangement::Scroll))
    return std::nullopt;
  // Bounds first, arithmetic second, relations last — the arithmetic below multiplies, and a
  // value is only allowed to take part in it once it is inside its own range.
  if (*page_size > max_page_size || *total_pages > max_page_count)
    return std::nullopt;

  const auto scroll = static_cast<std::uint8_t>(CandidateArrangement::Scroll);
  if (*arrangement == scroll) {
    // A window has to describe *this* entry list, and the one thing it may never lose is the
    // page the engine is on: the numbers the keys act on come from it. So the current page
    // must exist, must lie inside the pages the entries actually cover, and the entries may
    // not reach past the list's own last page.
    if (*page_size < 1 || *total_pages < 1)
      return std::nullopt;
    if (*current_page >= *total_pages)
      return std::nullopt;
    if (*window_first_page > *current_page)
      return std::nullopt;
    const std::uint64_t window_start =
        static_cast<std::uint64_t>(*window_first_page) * *page_size;
    const std::uint64_t window_end = window_start + page.entries.size();
    const std::uint64_t current_start = static_cast<std::uint64_t>(*current_page) * *page_size;
    const std::uint64_t list_end = static_cast<std::uint64_t>(*total_pages) * *page_size;
    if (current_start < window_start || current_start >= window_end)
      return std::nullopt; // the highlighted page is not in the payload
    if (window_end > list_end)
      return std::nullopt; // entries past the last page cannot be addressed
  } else {
    // The ordinary page shape carries none of the scrolling fields: a page that claims some
    // would be a payload from a build this one does not implement, and guessing which half
    // to believe is worse than refusing it.
    if (*page_size != 0 || *current_page != 0 || *window_first_page != 0 || *total_pages != 0)
      return std::nullopt;
  }
  page.arrangement = static_cast<CandidateArrangement>(*arrangement);
  page.page_size = static_cast<std::int32_t>(*page_size);
  page.current_page = static_cast<std::int32_t>(*current_page);
  page.window_first_page = static_cast<std::int32_t>(*window_first_page);
  page.total_pages = static_cast<std::int32_t>(*total_pages);
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
  if (type_byte < 0x01 || type_byte > max_message_type)
    return std::nullopt;
  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  frame.payload.assign(data + header_size, data + header_size + payload_len);
  return frame;
}

} // namespace fusion::ipc
