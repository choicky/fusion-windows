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
  // Vertical only: how many candidate rows this page has, how many of them the popup shows,
  // and which one is the first shown. A column that fits leaves first_row 0 and
  // visible_rows == total_rows. The rows outside the window keep their index and get an
  // empty rect (no draw, no hit), so what the pointer can hit is exactly what is on screen.
  int first_row = 0;
  int visible_rows = 0;
  int total_rows = 0;
  // How many complete rows of this arrangement the work area can hold at the font and DPI the
  // panel was laid out with (at most five). The host groups scroll pages by this number, so
  // what the paging keys call a page and what the popup shows are the same thing; 0 means the
  // layout had no ceiling to apply (an unbounded test layout).
  int row_capacity = 0;
  bool rows_hidden_above() const { return first_row > 0; }
  bool rows_hidden_below() const { return first_row + visible_rows < total_rows; }
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

  // The automatic scrolling arrangement: a grid whose columns are the engine's own page
  // size and whose rows are the engine's own pages. `entries` already is a contiguous run of
  // engine pages (see CandidateScrollWindow), so placement is pure index arithmetic over the
  // engine's order — no re-sorting, no renumbering, and a cell's position in entries is the
  // only identity the hit test needs.
  if (page.arrangement == CandidateArrangement::Scroll && page.page_size > 0 &&
      !page.entries.empty()) {
    const int columns =
        (std::max)(1, (std::min)(page.page_size, static_cast<int>(page.entries.size())));
    const int rows = (static_cast<int>(page.entries.size()) + columns - 1) / columns;
    const int band = pager ? row_height : 0;

    // One width for every cell on every row, taken from the widest text in the window and
    // then divided by the budget: columns line up, a row cannot be squeezed into a different
    // shape by its own text, and the panel is inside the screen by construction.
    std::vector<int> text_width(page.entries.size(), 0);
    int natural = 0;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue;
      text_width[index] = measure(slot_full_text(page.entries[index]));
      natural = (std::max)(natural, text_width[index]);
    }
    int cell = natural;
    if (bounded_width) {
      const int content = limits.max_width - 2 * side_margin - (columns - 1) * item_gap;
      const int share = content > 0 ? content / columns : 1;
      cell = (std::min)(cell, share);
    }
    if (limits.max_slot_width > 0)
      cell = (std::min)(cell, limits.max_slot_width);
    cell = (std::max)(1, cell);

    int panel_width = 2 * side_margin + columns * cell + (columns - 1) * item_gap;
    // The pager arrows live in the band above the grid, so they are not taken out of any
    // row's width — but the panel still has to be wide enough to hold them.
    if (pager)
      panel_width = (std::max)(panel_width, 2 * side_margin + 2 * pager_width + item_gap);
    if (bounded_width)
      panel_width = (std::min)(panel_width, (std::max)(limits.max_width, metrics.min_width));
    width = (std::max)(width, panel_width);

    out.total_rows = rows;
    // Five rows is the window's own size; a work area that cannot hold them shows fewer,
    // anchored so the row the paging keys are on stays visible. The font is never shrunk to
    // make more fit, and a row outside the window keeps its index with an empty rect.
    // The capacity is what the panel can hold, measured with the same numbers it places rows
    // with — the text rows above it and the pager band included, so a host that groups by
    // this number never asks for a row that would not fit.
    constexpr int max_scroll_rows = 5;
    int capacity = max_scroll_rows;
    if (bounded_height) {
      const int available = limits.max_height - top - band - side_margin;
      capacity = (std::clamp)(available / (std::max)(1, row_height), 1, max_scroll_rows);
    }
    out.row_capacity = capacity;
    int visible = (std::min)(rows, capacity);
    int first = 0;
    if (visible < rows) {
      const int anchor = (std::min)(page.current_row(), rows - 1);
      first = (std::clamp)(anchor - (visible - 1), 0, rows - visible);
    }
    out.first_row = first;
    out.visible_rows = visible;

    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue; // keeps its cell, draws nothing
      const int row = static_cast<int>(index) / columns;
      const int column = static_cast<int>(index) % columns;
      const int shown = row - first;
      if (shown < 0 || shown >= visible)
        continue;
      const int left = side_margin + column * (cell + item_gap);
      const int y = top + band + shown * row_height;
      out.slots[index] = LayoutRect{left, y, left + cell, y + row_height};
      out.clipped[index] = text_width[index] > cell ? 1 : 0;
    }
    if (page.has_prev)
      out.prev = LayoutRect{side_margin, top, side_margin + pager_width, top + band};
    if (page.has_next)
      out.next = LayoutRect{width - side_margin - pager_width, top, width - side_margin,
                            top + band};
    top = top + band + visible * row_height + side_margin;
    out.width = width;
    out.height = top;
    return out;
  }

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
    // One column, in page order — always one column. A column taller than the space it is
    // given is shown through a bounded viewport that keeps the highlighted candidate in
    // view, rather than continuing sideways: a second column would change the reading order
    // the engine's own PageSize defines (down the list, one page at a time), and the
    // product does not redefine that. The user's font is never shrunk and no candidate is
    // dropped; a row outside the window keeps its index with an empty rect.
    const int band = pager ? row_height : 0;
    int column_width = (std::max)(1, metrics.min_vertical_width);
    for (const auto &entry : page.entries)
      if (!entry.placeholder)
        column_width = (std::max)(column_width, measure(slot_full_text(entry)) + 2 * side_margin +
                                                      pager_width);
    if (bounded_width)
      column_width = (std::min)(column_width, (std::max)(limits.max_width, metrics.min_width));
    width = (std::max)(width, column_width);
    if (bounded_width)
      width = (std::min)(width, (std::max)(limits.max_width, metrics.min_width));

    const int rows = static_cast<int>(count);
    out.total_rows = rows;
    // How many whole rows the budget can show. At least one: a window that cannot show a
    // single candidate would have nothing to scroll and no way to reach any of them.
    int visible = rows;
    if (bounded_height && rows > 1) {
      const int available = limits.max_height - top - band - side_margin;
      const int fits = available / row_height;
      if (fits < rows)
        visible = (std::max)(1, fits);
    }
    // The window keeps the highlighted row visible, and it keeps the rows that precede it
    // as context: the highlighted row sits at the bottom of the window once the list is
    // longer than the window, which is the arrangement the paging keys move one row at a
    // time. With nothing highlighted the window starts at the first row.
    int first = 0;
    if (visible < rows) {
      int anchor = 0;
      for (std::size_t index = 0;
           index < page.entries.size() && static_cast<std::int32_t>(index) < page.cursor_index;
           ++index)
        if (!page.entries[index].placeholder)
          ++anchor;
      first = (std::clamp)(anchor - (visible - 1), 0, rows - visible);
    }
    out.first_row = first;
    out.visible_rows = visible;

    int row = 0;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue;
      const int shown = row - first;
      if (shown >= 0 && shown < visible) {
        const int left = side_margin;
        const int right = width - side_margin;
        const int y = top + band + shown * row_height;
        out.slots[index] = LayoutRect{left, y, (std::max)(left + 1, right), y + row_height};
        out.clipped[index] = measure(slot_full_text(page.entries[index])) >
                                     (std::max)(left + 1, right) - left
                                 ? 1
                                 : 0;
      }
      ++row;
    }
    if (page.has_prev)
      out.prev = LayoutRect{side_margin, top, side_margin + pager_width, top + band};
    if (page.has_next)
      out.next =
          LayoutRect{width - side_margin - pager_width, top, width - side_margin, top + band};
    top = top + band + visible * row_height + side_margin;
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
// The returned rect is empty when the work area is degenerate or when the panel leaves no
// free band at all; an empty result means "show nothing", never "show it over the panel".
inline LayoutRect place_fulltext_tip(const LayoutRect &panel, const LayoutRect &work, int width,
                                     int height, int gap) {
  const int l = (std::max)(0, width);
  const int h = (std::max)(0, height);
  const int g = (std::max)(0, gap);
  if (work.right <= work.left || work.bottom <= work.top)
    return LayoutRect{};

  // The four regions the panel leaves free, already clipped to the work area. Each is by
  // construction disjoint from the panel (they start or end a `gap` clear of it).
  const LayoutRect below{work.left, (std::min)(work.bottom, panel.bottom + g), work.right,
                         work.bottom};
  const LayoutRect above{work.left, work.top, work.right, (std::max)(work.top, panel.top - g)};
  const LayoutRect right{
      (std::min)(work.right, panel.right + g), work.top, work.right, work.bottom};
  const LayoutRect left_band{work.left, work.top, (std::max)(work.left, panel.left - g),
                             work.bottom};

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
    // The panel leaves no region at all (it fills the work area). There is then no honest
    // place for the text: an empty rect means "do not show it", which the caller turns into
    // a hidden box. Covering the candidates instead would swallow the click aimed at one of
    // them, which is exactly what this function exists to prevent.
    return LayoutRect{};
  }
  const int ew = (std::min)(l, best->right - best->left);
  const int eh = (std::min)(h, best->bottom - best->top);
  const int x = (std::clamp)(panel.left, best->left, (std::max)(best->left, best->right - ew));
  const int y = (std::clamp)(panel.top, best->top, (std::max)(best->top, best->bottom - eh));
  return LayoutRect{x, y, x + ew, y + eh};
}

} // namespace fusion::tsf
