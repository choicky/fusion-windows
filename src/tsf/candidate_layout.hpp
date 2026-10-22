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
//
// Everything here is a device-pixel decision: the caller derives the metrics from the
// popup font's real text height at the target monitor's DPI (layout_metrics_for), so a
// slot is always at least as tall as the text it contains and the two layout directions
// differ only in how slots are arranged, never in whether text fits.

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
  // Aligned with page.entries: 1 when the slot's text does not fit its rect and is drawn
  // with an ellipsis. The popup offers the full text for such a slot; the candidate
  // itself is never shortened — what is committed is always the engine's own string.
  std::vector<std::uint8_t> clipped;
  LayoutRect prev;               // pager arrows, empty when the engine has no prev/next
  LayoutRect next;
  int width = 0;
  int height = 0;
  bool slot_clipped(std::size_t index) const {
    return index < clipped.size() && clipped[index] != 0;
  }
};

// Device-pixel metrics. The values that are not typographic (margins, gaps, arrows) keep
// the proportions they had at 96 dpi, scaled by the target DPI in one place instead of
// being multiplied a second time somewhere else.
struct LayoutMetrics {
  int line_height = 20; // measured text height of the popup font, device pixels
  int item_padding = 3; // vertical padding inside a slot, above and below the text
  int row_height = 0;   // line_height + 2 * item_padding; computed when left 0
  int side_margin = 6;
  int item_gap = 12;
  int pager_width = 20;
  int min_vertical_width = 180;
  int min_width = 40;
  int min_slot_width = 24; // a slot never shrinks below this, even on a narrow work area
};

// Metrics for a font whose drawn text height is `line_height` device pixels on a display
// at `dpi`. 96 dpi reproduces the proportions the popup shipped with.
inline LayoutMetrics layout_metrics_for(int line_height, int dpi) {
  const auto scaled = [dpi](int value) { return value * dpi / 96; };
  LayoutMetrics metrics;
  metrics.line_height = (std::max)(1, line_height);
  metrics.item_padding = (std::max)(2, scaled(3));
  metrics.row_height = metrics.line_height + 2 * metrics.item_padding;
  metrics.side_margin = (std::max)(3, scaled(6));
  metrics.item_gap = (std::max)(4, scaled(12));
  metrics.pager_width = (std::max)(12, scaled(20));
  metrics.min_vertical_width = (std::max)(60, scaled(180));
  metrics.min_width = (std::max)(24, scaled(40));
  metrics.min_slot_width = (std::max)(16, scaled(24));
  return metrics;
}

// Bounds the popup must respect. Both are device pixels and 0 means "unbounded", so a
// caller with no screen to fit into (a geometry test) gets the plain intrinsic layout.
struct LayoutLimits {
  int max_width = 0;      // the target monitor's work area width
  int max_slot_width = 0; // an upper bound for one slot, independent of the row
};

// `measure` returns the drawn pixel width of a UTF-8 string.
inline CandidatePanelLayout
layout_candidate_panel(const CandidatePage &page,
                       const std::function<int(const std::string &)> &measure,
                       const LayoutMetrics &metrics = {}, const LayoutLimits &limits = {}) {
  CandidatePanelLayout out;
  out.slots.assign(page.entries.size(), LayoutRect{});
  out.clipped.assign(page.entries.size(), 0);
  const int line_height = (std::max)(1, metrics.line_height);
  const int padding = (std::max)(0, metrics.item_padding);
  const int row_height =
      metrics.row_height > 0 ? metrics.row_height : line_height + 2 * padding;
  const int side_margin = (std::max)(0, metrics.side_margin);
  const int item_gap = (std::max)(0, metrics.item_gap);
  const int pager_width = (std::max)(0, metrics.pager_width);

  int top = 0;
  int width = (std::max)(1, metrics.min_width);
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
    // One row, in page order. Each slot is as wide as its own text until something has to
    // give: a single very long candidate must not stretch the panel past the screen, so a
    // slot is capped by the work area's per-slot share (and by an explicit cap when the
    // caller has one). A capped slot is drawn with an ellipsis and offered in full; the
    // candidate it stands for is unchanged.
    std::size_t count = 0;
    for (const auto &entry : page.entries)
      if (!entry.placeholder)
        ++count;
    int chrome = 2 * side_margin;
    if (count > 1)
      chrome += static_cast<int>(count - 1) * item_gap;
    if (page.has_prev)
      chrome += pager_width + item_gap;
    if (page.has_next)
      chrome += pager_width;
    int cap = limits.max_slot_width;
    if (limits.max_width > 0 && count > 0) {
      const int share = (limits.max_width - chrome) / static_cast<int>(count);
      cap = cap > 0 ? (std::min)(cap, share) : share;
    }
    if (cap > 0)
      cap = (std::max)(cap, metrics.min_slot_width);

    int x = side_margin;
    if (page.has_prev) {
      out.prev = LayoutRect{x, top, x + pager_width, top + row_height};
      x += pager_width + item_gap;
    }
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue; // index kept, no displayed slot
      const int available = measure(slot_full_text(page.entries[index]));
      const int w = cap > 0 ? (std::min)(available, cap) : available;
      out.slots[index] = LayoutRect{x, top, x + w, top + row_height};
      out.clipped[index] = available > w ? 1 : 0;
      x += w + item_gap;
    }
    if (page.has_next) {
      out.next = LayoutRect{x, top, x + pager_width, top + row_height};
      x += pager_width;
    }
    width = (std::max)(width, x + side_margin);
    top += row_height;
  } else {
    const int band = pager ? row_height : 0;
    int w = (std::max)(1, metrics.min_vertical_width);
    for (const auto &entry : page.entries)
      if (!entry.placeholder)
        w = (std::max)(w, measure(slot_full_text(entry)) + 2 * side_margin + pager_width);
    if (limits.max_width > 0) {
      w = (std::min)(w, (std::max)(limits.max_width, metrics.min_width));
    }
    width = (std::max)(width, w);
    const int slot_left = side_margin;
    const int slot_right = (std::max)(slot_left + 1, width - side_margin);
    int y = top + band;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue;
      out.slots[index] = LayoutRect{slot_left, y, slot_right, y + row_height};
      out.clipped[index] = measure(slot_full_text(page.entries[index])) > slot_right - slot_left
                               ? 1
                               : 0;
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
