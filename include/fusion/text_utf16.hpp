#pragma once

// Fcitx addresses preedit text in UTF-8 bytes (fcitx::Text::cursor()); TSF ranges
// count UTF-16 code units. These helpers convert one into the other without ever
// landing inside a UTF-8 sequence or between the two halves of a surrogate pair.
// Portable on purpose: the TSF DLL uses them and the Mock-only tests prove them.

#include "fusion/backend.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace fusion {

// UTF-16 code-unit offset of `byte_offset` in `utf8`. nullopt when the offset is
// past the end, falls inside a code point, or the bytes before it are not
// well-formed UTF-8 (overlong, surrogate, > U+10FFFF, truncated).
inline std::optional<std::size_t> utf16_offset_of_utf8_byte(std::string_view utf8,
                                                            std::size_t byte_offset) {
  if (byte_offset > utf8.size())
    return std::nullopt;
  std::size_t units = 0;
  std::size_t i = 0;
  while (i < byte_offset) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    std::size_t length = 0;
    std::uint32_t code = 0;
    if (lead < 0x80) {
      length = 1;
      code = lead;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
      code = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      length = 3;
      code = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4;
      code = lead & 0x07u;
    } else {
      return std::nullopt;
    }
    if (i + length > utf8.size() || i + length > byte_offset)
      return std::nullopt; // truncated sequence, or the offset splits this code point
    for (std::size_t k = 1; k < length; ++k) {
      const auto trail = static_cast<unsigned char>(utf8[i + k]);
      if ((trail & 0xC0u) != 0x80u)
        return std::nullopt;
      code = (code << 6) | (trail & 0x3Fu);
    }
    if ((length == 3 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) ||
        (length == 4 && (code < 0x10000 || code > 0x10FFFF)))
      return std::nullopt;
    units += length == 4 ? 2 : 1;
    i += length;
  }
  return units;
}

// One preedit segment as a UTF-16 range [start, end) with its Fcitx format bits.
struct Utf16Segment {
  std::size_t start = 0;
  std::size_t end = 0;
  std::uint32_t format = 0;
};

// Map a FormattedPreedit onto UTF-16: every segment boundary and the cursor.
// cursor_out is nullopt for a hidden cursor. Returns false when the projection is
// inconsistent with `text` or any boundary is not a code-point boundary.
inline bool project_preedit_utf16(std::string_view text, const FormattedPreedit &preedit,
                                  std::vector<Utf16Segment> &segments_out,
                                  std::optional<std::size_t> &cursor_out) {
  segments_out.clear();
  cursor_out.reset();
  std::size_t byte = 0;
  std::size_t unit = 0;
  for (const auto &segment : preedit.segments) {
    const std::size_t end_byte = byte + segment.bytes;
    if (end_byte > text.size())
      return false;
    const auto end_unit = utf16_offset_of_utf8_byte(text, end_byte);
    if (!end_unit)
      return false;
    if (*end_unit > unit)
      segments_out.push_back({unit, *end_unit, segment.format});
    byte = end_byte;
    unit = *end_unit;
  }
  if (byte != text.size())
    return false;
  if (preedit.cursor >= 0) {
    const auto at = utf16_offset_of_utf8_byte(text, static_cast<std::size_t>(preedit.cursor));
    if (!at)
      return false;
    cursor_out = *at;
  } else if (preedit.cursor != -1) {
    return false;
  }
  return true;
}

} // namespace fusion
