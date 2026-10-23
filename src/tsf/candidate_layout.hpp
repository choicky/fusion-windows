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

// The one "too long" cap shared by every arrangement that lays cells out in a row: a cell
// shows at most this many CJK ideographs of its candidate text, measured in the candidate
// font with the same `measure` the layout sizes with. The label ("1. ") and the slot's own
// inset are added on top of the cap rather than taken out of it, so a labelled cell is not
// two characters narrower than an unlabelled one. Past the cap the cell is ellipsized,
// `clipped` is set, and the detail popup offers the whole string: what the engine commits is
// never shortened, only what is drawn.
inline constexpr int kLongCandidateIdeographs = 8;

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
  // The padding inside every slot, on the left and on the right, that the layout already
  // reserved when it sized the slot. The painter subtracts exactly this to get the rectangle
  // it draws into, so the width the text was measured at and the width it is drawn in are the
  // same number — the painting side must not add an inset of its own.
  int text_inset = 0;
  // The rectangle a slot's text is drawn into: the slot minus `text_inset` on both sides, never
  // narrower than one pixel. One definition, used by the painter and by `clipped` above.
  LayoutRect text_rect(std::size_t index) const {
    if (index >= slots.size())
      return LayoutRect{};
    const LayoutRect &slot = slots[index];
    if (slot.empty())
      return LayoutRect{};
    const int left = slot.left + text_inset;
    const int right = slot.right - text_inset;
    return LayoutRect{left, slot.top, (std::max)(left + 1, right), slot.bottom};
  }
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
  // Horizontal padding inside a slot, on each side. A slot is sized as
  // `text + 2 * item_inset`, and the text is drawn into exactly `slot - 2 * item_inset`, so a
  // candidate that fits its own measurement is never ellipsized for want of two pixels. Zero
  // would be consistent but leaves the text touching the highlight rectangle's edge.
  int item_inset = 2;
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
  metrics.item_inset = (std::max)(2, scaled(2));
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
  // The text inside a slot never touches its edge, and the space that costs is reserved once
  // here, so measuring, sizing and clipping all describe the same rectangle.
  const int inset = (std::max)(1, metrics.item_inset);
  out.text_inset = inset;
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

  // The cap is measured once per page, in the font the layout is laying out with. `budget`
  // is the work area's share for one cell (0 when there is no budget to apply); the smaller
  // of the two caps wins, so the panel is still inside the screen when the share is tighter
  // than eight ideographs.
  std::string ideographs;
  ideographs.reserve(static_cast<std::size_t>(kLongCandidateIdeographs) * 3);
  for (int index = 0; index < kLongCandidateIdeographs; ++index)
    ideographs += "中";
  const int long_text_cap = measure(ideographs);
  const auto slot_limit = [&](const CandidateEntry &entry, int budget) {
    int limit = long_text_cap + 2 * inset;
    if (!entry.label.empty())
      limit += measure(entry.label + " ");
    if (budget > 0)
      limit = (std::min)(limit, budget);
    return (std::max)(1, limit);
  };

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

    // One width per column, not one for the whole grid: a column is as wide as the widest
    // cell it holds, so a page with one long candidate beside single characters does not
    // draw four empty blocks. The widest cell of a column is taken over every row of the
    // scroll window — all of `entries`, not only the rows on screen — so the grid keeps its
    // shape while the highlight walks from row to row. Columns still line up because a cell
    // is placed at its own column's left edge; index arithmetic, hit testing and the
    // row/column navigation are unchanged.
    //
    // The column width is the alignment grid, not the width each cell is drawn at. A cell is
    // as wide as its *own* limit: the label allowance belongs to the entries that carry a
    // label, and only the current engine page's entries do (candidate_projection.cpp). Sizing
    // every cell of the column at the widest — including a label it does not have — would let
    // an unlabelled nine-ideograph candidate on another row show all nine characters and stay
    // unclipped, which is exactly what the eight-ideograph cap forbids.
    std::vector<int> natural(page.entries.size(), 0);
    std::vector<int> cell_width(page.entries.size(), 0);
    std::vector<int> column_width(static_cast<std::size_t>(columns), 0);
    int share = 0; // the work area's share for one column; 0 = no budget to apply
    if (bounded_width) {
      const int content = limits.max_width - 2 * side_margin - (columns - 1) * item_gap;
      share = content > 0 ? content / columns : 1;
    }
    if (limits.max_slot_width > 0)
      share = share > 0 ? (std::min)(share, limits.max_slot_width) : limits.max_slot_width;
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      if (page.entries[index].placeholder)
        continue;
      const CandidateEntry &entry = page.entries[index];
      natural[index] = measure(slot_full_text(entry)) + 2 * inset;
      // This cell's own limit: the cap plus its own label, and no more than the budget. The
      // column takes the widest of these; the cell keeps its own, left-aligned in the column.
      cell_width[index] = (std::min)(natural[index], slot_limit(entry, share));
      int &column = column_width[index % static_cast<std::size_t>(columns)];
      column = (std::max)(column, cell_width[index]);
    }
    for (auto &column : column_width)
      column = (std::max)(1, column);

    // Column left edges from the widths above: column c starts one gap after column c-1 ends.
    std::vector<int> column_left(static_cast<std::size_t>(columns), side_margin);
    for (int column = 1; column < columns; ++column)
      column_left[static_cast<std::size_t>(column)] =
          column_left[static_cast<std::size_t>(column - 1)] +
          column_width[static_cast<std::size_t>(column - 1)] + item_gap;

    int panel_width = column_left[static_cast<std::size_t>(columns - 1)] +
                      column_width[static_cast<std::size_t>(columns - 1)] + side_margin;
    // The pager arrows live in the band above the grid, so they are not taken out of any
    // column's width — but the panel still has to be wide enough to hold them.
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
      const std::size_t column = index % static_cast<std::size_t>(columns);
      const int row = static_cast<int>(index) / columns;
      const int shown = row - first;
      if (shown < 0 || shown >= visible)
        continue;
      const int left = column_left[column];
      const int y = top + band + shown * row_height;
      // Left-aligned in its column, but only as wide as this cell's own limit — so the
      // rectangle the painter draws into, and `clipped`, follow this entry's label, not the
      // widest one in the column.
      out.slots[index] = LayoutRect{left, y, left + cell_width[index], y + row_height};
      out.clipped[index] = natural[index] > cell_width[index] ? 1 : 0;
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
    // One row, in page order. Each cell is as wide as its own text, up to two caps: the
    // "too long" cap above, and the work area's share of this row. Without the first, one
    // long candidate would decide the width of a cell that shows eight characters; without
    // the second, K cells could add up to more than the screen. A cell that has to give is
    // drawn with an ellipsis and offered in full; the candidate it stands for is unchanged,
    // and it keeps its rect (so it keeps a click target).
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
      const int natural = measure(slot_full_text(page.entries[index])) + 2 * inset;
      const int w = (std::min)(natural, slot_limit(page.entries[index], cap));
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
        // The same rectangle the painter uses: the slot minus its inset on both sides.
        const int text_width = (std::max)(left + 1, right) - left - 2 * inset;
        out.clipped[index] =
            measure(slot_full_text(page.entries[index])) > text_width ? 1 : 0;
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
