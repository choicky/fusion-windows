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
  return metrics;
}

// Bounds the popup must respect. All are device pixels and 0 means "unbounded", so a
// caller with no screen to fit into (a geometry test) gets the plain intrinsic layout.
//
// They are budgets, not hints: the layout below divides the available space before it
// places anything, so the panel's own width and height come out inside these numbers
// rather than being clamped afterwards (clamping a window never made its contents fit).
struct LayoutLimits {
  int max_width = 0;      // the target monitor's work area width
  int max_height = 0;     // and its height
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
  const bool bounded_width = limits.max_width > 0;
  const bool bounded_height = limits.max_height > 0;

  // One content column: the panel never draws wider than the budget, and an auxiliary row
  // is ellipsized inside it like a slot is. Nothing here can be wider than the screen, so
  // there is no state in which a long preedit or prompt pushes the panel off the display.
  int top = 0;
  int width = (std::max)(1, metrics.min_width);
  const auto place_text_row = [&](const std::string &text, LayoutRect &rect) {
    const int natural = measure(text) + side_margin;
    int right = side_margin + natural;
    if (bounded_width)
      right = (std::min)(right, (std::max)(side_margin + 1, limits.max_width - side_margin));
    rect = LayoutRect{side_margin, top, right, top + row_height};
    width = (std::max)(width, right + side_margin);
    top += row_height;
  };
  const std::string upper = panel_upper_text(page);
  if (!upper.empty())
    place_text_row(upper, out.upper);
  if (!page.aux_down.empty())
    place_text_row(page.aux_down, out.lower);

  bool any_slot = false;
  for (const auto &entry : page.entries)
    any_slot = any_slot || !entry.placeholder;
  const bool pager = page.has_prev || page.has_next;
  if (!any_slot && !pager) {
    out.width = width;
    out.height = top;
    return out;
  }

  std::size_t count = 0;
  for (const auto &entry : page.entries)
    if (!entry.placeholder)
      ++count;

  if (page.layout == CandidateLayout::Horizontal) {
    // One row, in page order. The row has a fixed budget: margins, gaps, the pager arrows
    // and every cell come out of the same width, so K cells can never add up to more than
    // the screen even when every one of them is long. A cell that has to give is drawn
    // with an ellipsis and offered in full; the candidate it stands for is unchanged, and
    // it keeps its rect (so it keeps a click target).
    int reserve = 0;
    if (page.has_prev)
      reserve += pager_width + item_gap;
    // The next arrow also sits after a gap, so its gap is part of what it costs.
    if (page.has_next)
      reserve += pager_width + item_gap;
    const int gaps = count > 1 ? static_cast<int>(count - 1) * item_gap : 0;
    int cap = limits.max_slot_width;
    if (bounded_width && count > 0) {
      const int content = limits.max_width - 2 * side_margin - reserve - gaps;
      const int per = content > 0 ? content / static_cast<int>(count) : 1;
      cap = cap > 0 ? (std::min)(cap, per) : per;
      cap = (std::max)(cap, 1);
    }

    int x = side_margin;
    if (page.has_prev) {
      out.prev = LayoutRect{x, top, x + pager_width, top + row_height};
      x += pager_width + item_gap;
    }
    int remaining = static_cast<int>(count);
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue; // index kept, no displayed slot
      const int natural = measure(slot_full_text(page.entries[index]));
      const int w = cap > 0 ? (std::min)(natural, cap) : natural;
      out.slots[index] = LayoutRect{x, top, x + w, top + row_height};
      out.clipped[index] = natural > w ? 1 : 0;
      // A gap belongs *between* things: one after the last cell would be charged to the
      // budget and then left empty, which is how a reserved arrow ends up outside the panel.
      --remaining;
      x += w;
      if (remaining > 0 || page.has_next)
        x += item_gap;
    }
    if (page.has_next) {
      out.next = LayoutRect{x, top, x + pager_width, top + row_height};
      x += pager_width;
    }
    width = (std::max)(width, x + side_margin);
    if (bounded_width)
      width = (std::min)(width, (std::max)(limits.max_width, metrics.min_width));
    top += row_height;
  } else {
    // One column, in page order — until the column would not fit the screen at the size
    // the user chose. Then the same page continues in a second column rather than being
    // cut off or shrunk: every candidate keeps a visible, clickable rect, and the order is
    // still the engine's (down one column, then the next). A single column is the normal
    // case and the only one for a page that fits.
    const int band = pager ? row_height : 0;
    const int rows_available =
        bounded_height ? (limits.max_height - top - band - side_margin) / row_height : 0;
    const int max_rows = bounded_height ? (std::max)(1, rows_available) : 0;
    int columns = 1;
    if (max_rows > 0 && static_cast<int>(count) > max_rows) {
      columns = (static_cast<int>(count) + max_rows - 1) / max_rows;
    }
    const int rows_per_column =
        columns > 0 ? (static_cast<int>(count) + columns - 1) / columns : static_cast<int>(count);

    int column_width = (std::max)(1, metrics.min_vertical_width);
    for (const auto &entry : page.entries)
      if (!entry.placeholder)
        column_width = (std::max)(column_width, measure(slot_full_text(entry)) + 2 * side_margin +
                                                      pager_width);
    if (bounded_width) {
      const int share = (std::max)(1, limits.max_width / columns);
      column_width = (std::min)(column_width, (std::max)(share, metrics.min_width));
    }
    width = (std::max)(width, column_width * columns);
    if (bounded_width)
      width = (std::min)(width, (std::max)(limits.max_width, metrics.min_width));

    int column = 0;
    int row = 0;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue;
      if (row >= rows_per_column) {
        ++column;
        row = 0;
      }
      const int left = column * column_width + side_margin;
      const int right = (column + 1) * column_width - side_margin;
      const int y = top + band + row * row_height;
      out.slots[index] = LayoutRect{left, y, (std::max)(left + 1, right), y + row_height};
      out.clipped[index] = measure(slot_full_text(page.entries[index])) >
                                   (std::max)(left + 1, right) - left
                               ? 1
                               : 0;
      ++row;
    }
    if (page.has_prev)
      out.prev = LayoutRect{side_margin, top, side_margin + pager_width, top + band};
    if (page.has_next)
      out.next =
          LayoutRect{width - side_margin - pager_width, top, width - side_margin, top + band};
    top = top + band + rows_per_column * row_height + side_margin;
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

// Where the full-text popup goes, in screen coordinates. Two invariants, both absolute:
// the returned rect stays inside `work`, and it never overlaps `panel`. A popup that sat on
// the candidate list would eat the click the user aimed at a candidate, and once the text
// is long enough to scroll the popup also takes the pointer (it can no longer be
// click-through), so an overlap would trap it over the very list it is describing.
//
// `width`/`height` are the size the text wanted; `gap` separates the popup from the panel.
// Preference order is the reader's: directly below, then directly above, then beside on the
// side that has room. When none of those bands can hold the *full* height, the popup is
// bound to the largest non-overlapping band — the leftover text scrolls, exactly as when it
// was bounded by the whole work area, rather than spilling over the panel or off the screen.
// The returned rect is empty only when the work area itself is degenerate.
inline LayoutRect place_fulltext_tip(const LayoutRect &panel, const LayoutRect &work, int width,
                                     int height, int gap) {
  const int l = (std::max)(0, width);
  const int h = (std::max)(0, height);
  const int g = (std::max)(0, gap);
  if (work.right <= work.left || work.bottom <= work.top)
    return LayoutRect{};

  // The four regions the panel leaves free, already clipped to the work area. Each is by
  // construction disjoint from the panel (they start or end a `gap` clear of it).
  const LayoutRect below{work.left, (std::min)(work.bottom, panel.bottom + g), work.right, work.bottom};
  const LayoutRect above{work.left, work.top, work.right, (std::max)(work.top, panel.top - g)};
  const LayoutRect right{
      (std::min)(work.right, panel.right + g), work.top, work.right, work.bottom};
  const LayoutRect left_band{work.left, work.top, (std::max)(work.left, panel.left - g), work.bottom};

  // A band holds the popup whole only if it is at least as large in both dimensions.
  const auto holds = [&](const LayoutRect &band) {
    return !band.empty() && band.right - band.left >= l && band.bottom - band.top >= h;
  };
  // x chosen so [x, x+l] stays inside work, preferring to start under the panel's left edge.
  const auto clamp_x = [&](int preferred) {
    return (std::clamp)(preferred, work.left, (std::max)(work.left, work.right - l));
  };
  // y chosen so [y, y+h] stays inside work, preferring to hug the panel.
  const auto clamp_y = [&](int preferred) {
    return (std::clamp)(preferred, work.top, (std::max)(work.top, work.bottom - h));
  };
  const auto rect = [&](int x, int y) { return LayoutRect{x, y, x + l, y + h}; };

  if (holds(below))
    return rect(clamp_x(panel.left), below.top);
  if (holds(above))
    return rect(clamp_x(panel.left), above.bottom - h);
  if (holds(right))
    return rect(right.left, clamp_y(panel.top));
  if (holds(left_band))
    return rect(left_band.right - l, clamp_y(panel.top));

  // Nothing fits the full text: bind the popup to the largest non-overlapping band and let
  // the rest scroll. Its size is capped to the band, so it cannot reach past the work area
  // or back over the panel.
  auto widest = [](const LayoutRect &b) { return (b.right - b.left) * (b.bottom - b.top); };
  const LayoutRect *best = &below;
  for (const LayoutRect *band : {&below, &above, &right, &left_band})
    if (!band->empty() && widest(*band) > widest(*best))
      best = band;
  if (best->empty()) {
    // Even the bands are zero (the panel fills the work area): clamp the popup to the work
    // area so at least its top-left corner is reachable.
    const int ew = (std::min)(l, work.right - work.left);
    const int eh = (std::min)(h, work.bottom - work.top);
    return LayoutRect{work.left, work.top, work.left + ew, work.top + eh};
  }
  const int ew = (std::min)(l, best->right - best->left);
  const int eh = (std::min)(h, best->bottom - best->top);
  const int x = (std::clamp)(panel.left, best->left, (std::max)(best->left, best->right - ew));
  const int y = (std::clamp)(panel.top, best->top, (std::max)(best->top, best->bottom - eh));
  return LayoutRect{x, y, x + ew, y + eh};
}

} // namespace fusion::tsf
