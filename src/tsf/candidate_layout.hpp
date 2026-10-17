#pragma once

// Stage 5 candidate popup geometry, kept portable (no windows.h) so the
// display/index/hit-test contract is proven in the Mock-only tests and the Win32
// surface (candidate_view.cpp) only paints it.
//
// Rows follow the mature classic-ui panel (src/ui/classic/inputwindow.cpp at the
// pinned revision): an upper row with auxUp followed by the panel preedit, a
// lower row with auxDown, then the candidate slots. A candidate slot displays
// "label text comment" (classic draws the comment after the text with one space,
// its spaceBetweenComment default). Every slot rect stays aligned with
// CandidatePage::entries[i] == CandidateList::candidate(i); a placeholder keeps
// its index with an empty, non-hit rect.

#include "fusion/backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fusion::tsf {

struct LayoutRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool empty() const { return right <= left || bottom <= top; }
  bool contains(int x, int y) const {
    return !empty() && x >= left && x < right && y >= top && y < bottom;
  }
};

// Display pieces of one slot: main = "label text" (label omitted when empty),
// comment = the engine comment, drawn after main with one space.
inline std::string slot_main_text(const CandidateEntry &entry) {
  std::string value = entry.label;
  if (!value.empty())
    value += ' ';
  value += entry.text;
  return value;
}
inline std::string slot_full_text(const CandidateEntry &entry) {
  std::string value = slot_main_text(entry);
  if (!entry.comment.empty()) {
    value += ' ';
    value += entry.comment;
  }
  return value;
}
// classic-ui upper row: auxUp then the panel (server-side) preedit.
inline std::string panel_upper_text(const CandidatePage &page) {
  return page.aux_up + page.panel_preedit;
}

struct CandidatePanelLayout {
  LayoutRect upper;              // empty when there is no upper-row text
  LayoutRect lower;              // empty when there is no auxDown
  std::vector<LayoutRect> slots; // aligned with page.entries
  LayoutRect prev;               // pager arrows, empty when the engine has no prev/next
  LayoutRect next;
  int width = 0;
  int height = 0;
};

namespace layout_metrics {
inline constexpr int row_height = 24;
inline constexpr int side_margin = 6;
inline constexpr int item_gap = 12;
inline constexpr int pager_width = 20;
inline constexpr int min_vertical_width = 180;
inline constexpr int min_width = 40;
} // namespace layout_metrics

// `measure` returns the drawn pixel width of a UTF-8 string.
inline CandidatePanelLayout
layout_candidate_panel(const CandidatePage &page,
                       const std::function<int(const std::string &)> &measure) {
  using namespace layout_metrics;
  CandidatePanelLayout out;
  out.slots.assign(page.entries.size(), LayoutRect{});
  int top = 0;
  int width = min_width;
  const std::string upper = panel_upper_text(page);
  if (!upper.empty()) {
    const int w = measure(upper) + 2 * side_margin;
    out.upper = LayoutRect{side_margin, top, w - side_margin, top + row_height};
    width = (std::max)(width, w);
    top += row_height;
  }
  if (!page.aux_down.empty()) {
    const int w = measure(page.aux_down) + 2 * side_margin;
    out.lower = LayoutRect{side_margin, top, w - side_margin, top + row_height};
    width = (std::max)(width, w);
    top += row_height;
  }
  bool any_slot = false;
  for (const auto &entry : page.entries)
    any_slot = any_slot || !entry.placeholder;
  const bool pager = page.has_prev || page.has_next;
  if (!any_slot && !pager) {
    out.width = width;
    out.height = top;
    return out;
  }
  if (page.layout == CandidateLayout::Horizontal) {
    int x = side_margin;
    if (page.has_prev) {
      out.prev = LayoutRect{x, top + side_margin, x + pager_width, top + row_height - side_margin};
      x += pager_width + item_gap;
    }
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue; // index kept, no displayed slot
      const int w = measure(slot_full_text(page.entries[index]));
      out.slots[index] = LayoutRect{x, top + side_margin, x + w, top + row_height - side_margin};
      x += w + item_gap;
    }
    if (page.has_next) {
      out.next = LayoutRect{x, top + side_margin, x + pager_width, top + row_height - side_margin};
      x += pager_width;
    }
    width = (std::max)(width, x + side_margin);
    top += row_height;
  } else {
    const int band = pager ? row_height : 0;
    int w = min_vertical_width;
    for (const auto &entry : page.entries)
      if (!entry.placeholder)
        w = (std::max)(w, measure(slot_full_text(entry)) + 2 * side_margin + pager_width);
    width = (std::max)(width, w);
    int y = top + band;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue;
      out.slots[index] = LayoutRect{side_margin, y, width - side_margin, y + row_height};
      y += row_height;
    }
    if (page.has_prev)
      out.prev = LayoutRect{side_margin, top, side_margin + pager_width, top + band};
    if (page.has_next)
      out.next =
          LayoutRect{width - side_margin - pager_width, top, width - side_margin, top + band};
    top = y + side_margin;
  }
  out.width = width;
  out.height = top;
  return out;
}

// Pointer hit on the laid-out panel: a pager arrow, or the page-local index of
// the displayed slot under the point. Panel text rows are not actionable.
struct PanelHit {
  enum class Kind { Select, PageNext, PagePrev } kind = Kind::Select;
  std::int32_t index = -1;
};
inline std::optional<PanelHit> hit_test_candidate_panel(const CandidatePanelLayout &layout, int x,
                                                        int y) {
  if (layout.prev.contains(x, y))
    return PanelHit{PanelHit::Kind::PagePrev, -1};
  if (layout.next.contains(x, y))
    return PanelHit{PanelHit::Kind::PageNext, -1};
  for (std::size_t slot = 0; slot < layout.slots.size(); ++slot)
    if (layout.slots[slot].contains(x, y))
      return PanelHit{PanelHit::Kind::Select, static_cast<std::int32_t>(slot)};
  return std::nullopt;
}

} // namespace fusion::tsf
