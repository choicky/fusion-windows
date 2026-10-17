#pragma once

// Stage 5: how the Fcitx preedit contract (segments + TextFormatFlag bits + byte
// cursor) is presented in a TSF composition. Portable (no windows.h) so the
// mapping and the UTF-16 layout are proven in the Mock-only tests; the TSF DLL
// realizes it with ITfContext::SetSelection and GUID_PROP_ATTRIBUTE.
//
// Only Fcitx display semantics that a TSF display attribute can express are
// mapped: HighLight (the engine marks the text up to its cursor / the active
// part) and Underline (composition text). Bold / Italic / Strike have no
// TF_DISPLAYATTRIBUTE field, and DontCommit is a commit property, not a display
// one, so they carry no TSF attribute.

#include "fusion/backend.hpp"
#include "fusion/text_utf16.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace fusion::tsf {

enum class PreeditStyle { None, Input, Highlighted };

inline PreeditStyle preedit_style_for_format(std::uint32_t format) {
  if (format & text_format::highlight)
    return PreeditStyle::Highlighted;
  if (format & text_format::underline)
    return PreeditStyle::Input;
  return PreeditStyle::None;
}

struct StyledRange {
  std::size_t start = 0;
  std::size_t end = 0;
  PreeditStyle style = PreeditStyle::None;
};

// The TSF-side layout of one displayed preedit: UTF-16 ranges with their style
// and the caret position. caret == nullopt means "no engine cursor to show":
// the composition caret then stays at the end, as before Stage 5.
struct PreeditPresentation {
  std::vector<StyledRange> ranges;
  std::optional<std::size_t> caret;
};

// nullopt when the projection does not map onto `text` (see
// project_preedit_utf16); the controller has already rejected such a batch.
inline std::optional<PreeditPresentation> layout_preedit(std::string_view text,
                                                         const FormattedPreedit &preedit) {
  std::vector<Utf16Segment> segments;
  std::optional<std::size_t> cursor;
  if (!project_preedit_utf16(text, preedit, segments, cursor))
    return std::nullopt;
  PreeditPresentation presentation;
  presentation.caret = cursor;
  for (const auto &segment : segments)
    presentation.ranges.push_back(
        {segment.start, segment.end, preedit_style_for_format(segment.format)});
  return presentation;
}

} // namespace fusion::tsf
