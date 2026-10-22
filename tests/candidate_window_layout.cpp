// The candidate popup is a real window, and the presentation settings have to change what
// it draws and how big it is. A stored enum, or a screenshot of the settings window, proves
// nothing about that: this check creates the actual popup — the same window class, the same
// layout_candidate_panel, the same SetWindowPos and paint path the TSF service uses — shows
// it with projected pages in each geometry, and reads the real window rectangle back.
//
// What it covers:
//   - the window is sized by the page's effective layout (wide/short vs tall/narrow), and
//     keeps the non-activating extended styles;
//   - a candidate slot is never shorter than the text drawn into it, in either direction
//     (the horizontal row used to be half a row tall, which cut the glyphs in half);
//   - one very long candidate cannot stretch the panel past the work area: its slot is
//     capped, drawn ellipsized, and its full text is available from the tip popup — by
//     keyboard highlight and by hovering, without taking focus;
//   - a larger configured font really produces a taller panel, and a font family that does
//     not exist still draws (GDI substitution) instead of failing;
//   - the metrics scale with DPI in one place, so 192 dpi is exactly twice 96 dpi.
//
// What it does NOT cover: a composition inside a real application reaching this popup.
// That path (anchor, composition, TIP registration) is unchanged by this work and would
// require an installed TIP, which this task must not touch. It also does not move a real
// window between two physical monitors: the DPI arithmetic is checked directly, and a
// mixed-DPI visual check remains owner-side validation.
//
// It runs as an isolated process: no TIP is registered and no product installation is
// used, so it can run on a machine whose product Host is in daily use.

#include "tsf/candidate_view.hpp"
#include "tsf/service.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace fusion::tsf {

// The popup's click path is the one part of CandidateView this check cannot exercise: it
// needs a TextService, and a TextService needs a TSF context in a real application. The
// symbol still has to resolve for the object to link, so it is defined here and aborts if
// it is ever reached. A silent no-op would turn "the click was not tested" into "the click
// did nothing", which is a different and false claim.
void TextService::on_candidate_hit(void *entry, std::uint64_t generation, CandidateAction action,
                                  int index) {
  (void)entry;
  (void)generation;
  (void)action;
  (void)index;
  std::fprintf(stderr, "candidate_window_layout: the click path must not be reached\n");
  std::abort();
}

} // namespace fusion::tsf

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

// Short CJK candidates with a label and a comment: the shape the Pinyin engine produces.
fusion::CandidatePage make_page(fusion::CandidateLayout layout, int entries) {
  fusion::CandidatePage page;
  page.layout = layout;
  page.cursor_index = 0;
  for (int index = 0; index < entries; ++index) {
    fusion::CandidateEntry entry;
    entry.label = std::to_string(index + 1) + ".";
    entry.text = "候选" + std::to_string(index + 1);
    entry.comment = "c" + std::to_string(index);
    page.entries.push_back(std::move(entry));
  }
  return page;
}

// One candidate far longer than any work area: the case that used to widen the panel
// without limit.
fusion::CandidatePage make_long_page(fusion::CandidateLayout layout, int entries, int long_index) {
  fusion::CandidatePage page = make_page(layout, entries);
  page.cursor_index = -1; // nothing highlighted unless the caller says so
  if (long_index >= 0 && long_index < entries) {
    std::string text;
    for (int i = 0; i < 40; ++i)
      text += "很长很长的候选文本";
    page.entries[static_cast<std::size_t>(long_index)].text = std::move(text);
  }
  return page;
}

// A page where *every* candidate is long: the case a single long candidate does not cover,
// because the budget has to divide between competitors rather than absorb one outlier.
fusion::CandidatePage make_all_long_page(fusion::CandidateLayout layout, int entries, int repeat) {
  fusion::CandidatePage page = make_page(layout, entries);
  page.cursor_index = -1;
  for (auto &entry : page.entries) {
    std::string text;
    for (int i = 0; i < repeat; ++i)
      text += "超长候选文本";
    entry.text = std::move(text);
    entry.comment = "same-comment-for-every-entry";
  }
  return page;
}

// The test places the popup in the coordinates the process actually uses, so the inset is
// scaled by the DPI the window reports rather than assumed to be pixels.
int scaled_for_test(int value) {
  const UINT dpi = GetDpiForSystem();
  return static_cast<int>(value * (dpi == 0 ? 96 : dpi) / 96);
}

int width_of(const RECT &rect) { return static_cast<int>(rect.right - rect.left); }

int height_of(const RECT &rect) { return static_cast<int>(rect.bottom - rect.top); }

RECT show_and_measure(fusion::tsf::CandidateView &view, const fusion::CandidatePage &page) {
  view.show(page, POINT{120, 120});
  RECT rect{};
  if (view.window() != nullptr) {
    GetWindowRect(view.window(), &rect);
  }
  return rect;
}

// Every displayed slot has to be at least as tall as the text the popup measured: this is
// the invariant that was broken in the horizontal direction.
bool slots_hold_the_text(const fusion::tsf::CandidateView &view) {
  // Named for the caller above as well: one implementation, two spellings would drift.

  const auto &layout = view.layout();
  for (const auto &slot : layout.slots) {
    if (slot.empty())
      continue;
    if (slot.bottom - slot.top < view.line_height())
      return false;
  }
  return true;
}

} // namespace

int main() {
  constexpr int entries = 8;
  fusion::tsf::CandidateView view;

  // 1. The real popup window is created, and it is the non-activating tool window the
  //    Stage 5 surface was defined as: the presentation settings must not change that.
  CHECK(view.ensure_created(nullptr, nullptr, 0));
  CHECK(view.window() != nullptr);
  if (view.window() == nullptr) {
    std::fprintf(stderr, "candidate_window_layout: the popup could not be created\n");
    return 1;
  }

  const LONG_PTR extended = GetWindowLongPtrW(view.window(), GWL_EXSTYLE);
  CHECK((extended & WS_EX_NOACTIVATE) != 0);
  CHECK((extended & WS_EX_TOOLWINDOW) != 0);

  // 2. A horizontal page makes a wide, short panel — and every slot is tall enough for the
  //    text it contains.
  const auto horizontal = make_page(fusion::CandidateLayout::Horizontal, entries);
  const RECT horizontal_rect = show_and_measure(view, horizontal);
  const int horizontal_width = width_of(horizontal_rect);
  const int horizontal_height = height_of(horizontal_rect);
  CHECK(view.visible());
  CHECK(horizontal_width > 0 && horizontal_height > 0);
  CHECK(horizontal_width > horizontal_height);
  CHECK(horizontal_width > 200);
  CHECK(view.line_height() > 0);
  CHECK(slots_hold_the_text(view));
  CHECK(horizontal_height >= view.line_height());

  // 3. A vertical page makes a tall, narrow panel — from the same popup and the same
  //    entries, so the only thing that differs is the page's layout value. Its slots hold
  //    the text too: the two directions must not disagree about that.
  const auto vertical = make_page(fusion::CandidateLayout::Vertical, entries);
  const RECT vertical_rect = show_and_measure(view, vertical);
  const int vertical_width = width_of(vertical_rect);
  const int vertical_height = height_of(vertical_rect);
  CHECK(view.visible());
  CHECK(vertical_width > 0 && vertical_height > 0);
  CHECK(vertical_height > vertical_width);
  CHECK(slots_hold_the_text(view));

  // 4. The two geometries are actually different, in the direction each one implies:
  //    the row is wider than the column, and the column is taller than the row.
  CHECK(horizontal_width > vertical_width);
  CHECK(vertical_height > horizontal_height);

  // 5. Switching back re-lays-out the same window, so a saved preference takes effect on
  //    the next projected page rather than only on a newly opened popup.
  const RECT again = show_and_measure(view, horizontal);
  CHECK(width_of(again) == horizontal_width);
  CHECK(height_of(again) == horizontal_height);

  // 6. Nothing to show still hides the popup, unchanged by the presentation work.
  fusion::CandidatePage empty;
  empty.layout = fusion::CandidateLayout::Horizontal;
  view.show(empty, POINT{120, 120});
  CHECK(!view.visible());

  // 7. A single very long candidate does not stretch the panel past the work area. Its
  //    slot is capped and flagged, the short candidates are not, and the full text is
  //    reachable — but only for the slot that needs it.
  {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{120, 120}, MONITOR_DEFAULTTONEAREST), &info))
      work = info.rcWork;
    const int work_width = width_of(work);

    const auto long_page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 2);
    const RECT long_rect = show_and_measure(view, long_page);
    CHECK(view.visible());
    CHECK(width_of(long_rect) <= work_width);
    CHECK(view.layout().slot_clipped(2));
    CHECK(!view.layout().slot_clipped(0));
    CHECK(slots_hold_the_text(view));
    // Nothing is highlighted, so the tip stays out of the way.
    CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));

    // Hovering the long slot offers its full text; the tip is a separate, non-activating,
    // click-through window that never selects anything.
    const auto &slot = view.layout().slots[2];
    CHECK(!slot.empty());
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr);
    if (view.tip_window() != nullptr) {
      CHECK(IsWindowVisible(view.tip_window()));
      const LONG_PTR tip_style = GetWindowLongPtrW(view.tip_window(), GWL_EXSTYLE);
      CHECK((tip_style & WS_EX_NOACTIVATE) != 0);
      CHECK((tip_style & WS_EX_TRANSPARENT) != 0);
      RECT tip{};
      GetWindowRect(view.tip_window(), &tip);
      CHECK(tip.left >= work.left && tip.right <= work.right);
      CHECK(tip.top >= work.top && tip.bottom <= work.bottom);
    }
    // Leaving the slot hides it again rather than leaving a stale tooltip on screen.
    SendMessageW(view.window(), WM_MOUSELEAVE, 0, 0);
    CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));

    // A page with nothing clipped hides the tip as well.
    view.show(horizontal, POINT{120, 120});
    CHECK(!view.layout().slot_clipped(0));
    CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));
  }

  // 8. The keyboard case: a highlighted candidate too long for its cell shows its full text
  //    without the pointer having to find it.
  {
    auto page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 1);
    page.cursor_index = 1;
    show_and_measure(view, page);
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    page.cursor_index = 0;
    show_and_measure(view, page);
    CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));
  }

  // 9. The configured font really decides the text height, and therefore the panel: a
  //    larger point size makes a taller window, and the stored value is points, so the
  //    device size follows the DPI rather than being the number itself.
  {
    auto page = make_page(fusion::CandidateLayout::Horizontal, entries);
    page.font.family = "Microsoft YaHei UI";
    page.font.size_pt = 11;
    const RECT small_rect = show_and_measure(view, page);
    const int small_line = view.line_height();
    page.font.size_pt = 24;
    const RECT large_rect = show_and_measure(view, page);
    const int large_line = view.line_height();
    CHECK(view.visible());
    CHECK(small_line > 0 && large_line > small_line);
    CHECK(height_of(large_rect) > height_of(small_rect));
    CHECK(height_of(large_rect) >= large_line);
    CHECK(slots_hold_the_text(view));
  }

  // 10. A family that does not exist still draws: GDI substitutes, and the popup keeps the
  //     panel usable instead of collapsing to nothing.
  {
    auto page = make_page(fusion::CandidateLayout::Vertical, entries);
    page.font.family = "No Such Family 12345";
    page.font.size_pt = 11;
    const RECT rect = show_and_measure(view, page);
    CHECK(view.visible());
    CHECK(width_of(rect) > 0 && height_of(rect) > 0);
    CHECK(view.line_height() > 0);
    CHECK(slots_hold_the_text(view));
  }

  // 11. The metrics scale with DPI in one place: 192 dpi is exactly twice 96 dpi, so a
  //     panel measured on a 200 % display keeps the same proportions, and nothing is
  //     scaled a second time by the drawing code.
  {
    const auto base = fusion::tsf::layout_metrics_for(20, 96);
    const auto doubled = fusion::tsf::layout_metrics_for(40, 192);
    CHECK(doubled.row_height == 2 * base.row_height);
    CHECK(doubled.side_margin == 2 * base.side_margin);
    CHECK(doubled.item_gap == 2 * base.item_gap);
    CHECK(doubled.pager_width == 2 * base.pager_width);
    const auto mid = fusion::tsf::layout_metrics_for(25, 120);
    CHECK(mid.row_height > base.row_height);
    CHECK(mid.row_height < doubled.row_height);
  }

  // 12. A click still resolves to the candidate the user sees, in both directions.
  {
    for (const auto kind :
         {fusion::CandidateLayout::Horizontal, fusion::CandidateLayout::Vertical}) {
      auto page = make_page(kind, entries);
      show_and_measure(view, page);
      const auto &layout = view.layout();
      for (std::size_t index = 0; index < layout.slots.size(); ++index) {
        const auto &slot = layout.slots[index];
        if (slot.empty())
          continue;
        const auto hit = fusion::tsf::hit_test_candidate_panel(layout, slot.left + 1,
                                                               (slot.top + slot.bottom) / 2);
        CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
              hit->index == static_cast<std::int32_t>(index));
      }
    }
  }

  // 13. Every candidate long at once: the budget is divided, not absorbed by one outlier,
  //     so the panel still fits and *every* slot keeps a rect that can be clicked.
  {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{120, 120}, MONITOR_DEFAULTTONEAREST), &info))
      work = info.rcWork;
    const int work_width = width_of(work);
    const int work_height = height_of(work);

    for (const auto kind :
         {fusion::CandidateLayout::Horizontal, fusion::CandidateLayout::Vertical}) {
      const auto page = make_all_long_page(kind, entries, 24);
      const RECT rect = show_and_measure(view, page);
      CHECK(view.visible());
      CHECK(width_of(rect) <= work_width);
      CHECK(height_of(rect) <= work_height);
      const auto &layout = view.layout();
      for (std::size_t index = 0; index < layout.slots.size(); ++index) {
        // Present, on screen, flagged as shortened, and still the target of its own click.
        CHECK(!layout.slots[index].empty());
        // Horizontal has to divide one row between competitors, so every cell is
        // shortened; vertical is allowed to be as wide as the text (it is bounded by the
        // work area), so clipping there is a consequence, not a requirement.
        if (kind == fusion::CandidateLayout::Horizontal)
          CHECK(layout.slot_clipped(index));
        CHECK(layout.slots[index].left >= 0 && layout.slots[index].right <= width_of(rect));
        CHECK(layout.slots[index].top >= 0 && layout.slots[index].bottom <= height_of(rect));
        const auto hit = fusion::tsf::hit_test_candidate_panel(
            layout, layout.slots[index].left + 1,
            (layout.slots[index].top + layout.slots[index].bottom) / 2);
        CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
              hit->index == static_cast<std::int32_t>(index));
      }
      CHECK(slots_hold_the_text(view));
    }
  }

  // 14. The largest size the schema allows, with a full page of candidates: the vertical
  //     column cannot fit the work area at 48 pt, and the answer is still bounded — every
  //     candidate present and clickable, and the user's size untouched.
  {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{120, 120}, MONITOR_DEFAULTTONEAREST), &info))
      work = info.rcWork;
    const int work_width = width_of(work);
    const int work_height = height_of(work);

    auto page = make_page(fusion::CandidateLayout::Vertical, 10);
    page.font.family = "Microsoft YaHei UI";
    page.font.size_pt = 48;
    const RECT rect = show_and_measure(view, page);
    CHECK(view.visible());
    CHECK(width_of(rect) <= work_width);
    CHECK(height_of(rect) <= work_height);
    const int line = view.line_height();
    CHECK(line > 0);
    const auto &layout = view.layout();
    CHECK(layout.slots.size() == 10);
    int placed = 0;
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      if (layout.slots[index].empty())
        continue;
      ++placed;
      CHECK(layout.slots[index].bottom - layout.slots[index].top >= line);
      const auto hit = fusion::tsf::hit_test_candidate_panel(
          layout, layout.slots[index].left + 1,
          (layout.slots[index].top + layout.slots[index].bottom) / 2);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }
    CHECK(placed == 10);
  }

  // 15. A narrow work area with a pager and an auxiliary row: the budget is shared by every
  //     part of the panel, so nothing sticks out to the right of the screen.
  {
    const auto page = make_all_long_page(fusion::CandidateLayout::Horizontal, entries, 12);
    fusion::CandidatePage bounded = page;
    bounded.aux_up = "笔画筛选：横 竖 撇 点 折 以及一段很长的提示文字";
    bounded.has_next = true;
    bounded.has_prev = true;

    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return static_cast<int>(text.size()) * 12;
    };
    for (const int budget : {320, 640, 1024}) {
      fusion::tsf::LayoutLimits limits;
      limits.max_width = budget;
      limits.max_height = 600;
      const auto layout = fusion::tsf::layout_candidate_panel(bounded, measure, metrics, limits);
      CHECK(layout.width <= budget);
      CHECK(layout.height <= 600);
      CHECK(!layout.upper.empty() && layout.upper.right <= budget);
      CHECK(!layout.prev.empty() && !layout.next.empty());
      CHECK(layout.next.right <= budget);
      int placed = 0;
      for (std::size_t index = 0; index < layout.slots.size(); ++index) {
        const auto &slot = layout.slots[index];
        if (slot.empty())
          continue;
        ++placed;
        CHECK(slot.left >= 0 && slot.right <= budget);
        const auto hit = fusion::tsf::hit_test_candidate_panel(
            layout, slot.left + 1, (slot.top + slot.bottom) / 2);
        CHECK(hit && hit->index == static_cast<std::int32_t>(index));
      }
      CHECK(placed == entries);
      CHECK(layout.prev.left >= 0);
    }
  }

  // 16. The last page has no next arrow: the reserve it needed is gone and the cells may use
  //     it, but nothing overflows and nothing is lost.
  {
    const auto all_long = make_all_long_page(fusion::CandidateLayout::Horizontal, entries, 24);
    fusion::CandidatePage page = all_long;
    page.has_next = true; // a middle page, with the arrow that has to be paid for
    page.has_prev = false;
    fusion::CandidatePage last = all_long; // the last page: no next arrow to reserve
    last.has_next = false;
    last.has_prev = true;
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return static_cast<int>(text.size()) * 12;
    };
    fusion::tsf::LayoutLimits limits;
    limits.max_width = 800;
    limits.max_height = 400;
    const auto with_next =
        fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
    const auto without_next = fusion::tsf::layout_candidate_panel(last, measure, metrics, limits);
    CHECK(with_next.width <= 800 && without_next.width <= 800);
    CHECK(without_next.next.empty() && !without_next.prev.empty());
    CHECK(without_next.slots[0].right - without_next.slots[0].left >=
          with_next.slots[0].right - with_next.slots[0].left);
  }

  // 17. A very long candidate's full text is readable to the end: the box is bounded, and
  //     the pointer's wheel moves the text inside it until the last line is in view.
  {
    auto page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 3);
    page.cursor_index = -1;
    page.entries[3].text.clear();
    for (int i = 0; i < 2400; ++i)
      page.entries[3].text += "很长的候选内容";
    show_and_measure(view, page);
    const auto &slot = view.layout().slots[3];
    CHECK(!slot.empty());
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    // The text really is taller than the box, so there is something to reach.
    CHECK(view.tip_scrollable());
    CHECK(view.tip_content_height() > view.tip_view_height());
    CHECK(view.tip_scroll() == 0);

    // One wheel notch moves it, and repeated notches reach the end; the clamp is exact, so
    // the end of the text lands inside the visible box rather than past it.
    SendMessageW(view.tip_window(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    CHECK(view.tip_scroll() > 0);
    for (int i = 0; i < 500 && view.tip_scroll() < view.tip_scroll_max(); ++i)
      SendMessageW(view.tip_window(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    CHECK(view.tip_scroll() == view.tip_scroll_max());
    CHECK(view.tip_scroll() + view.tip_view_height() >= view.tip_content_height());

    // And it scrolls back, so nothing is one-way.
    SendMessageW(view.tip_window(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    CHECK(view.tip_scroll() < view.tip_scroll_max());

    // A text that fits is not scrollable and stays fully click-through, so it can never
    // intercept a pointer aimed at the candidates.
    auto short_page = make_page(fusion::CandidateLayout::Horizontal, entries);
    short_page.entries[0].text += std::string(120, 'x');
    show_and_measure(view, short_page);
    const auto &short_slot = view.layout().slots[0];
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((short_slot.left + short_slot.right) / 2,
                            (short_slot.top + short_slot.bottom) / 2));
    if (view.tip_window() != nullptr && IsWindowVisible(view.tip_window())) {
      CHECK(!view.tip_scrollable());
      const LONG_PTR style = GetWindowLongPtrW(view.tip_window(), GWL_EXSTYLE);
      CHECK((style & WS_EX_TRANSPARENT) != 0);
    }
  }

  // 18. The same page on a work area that cannot hold a single column: a small laptop
  //     screen at 125 %, ten candidates at the largest size the schema allows. The column
  //     has to continue sideways — all ten present, inside the screen, still clickable —
  //     instead of running off the bottom where nobody can reach it.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(75, 96); // 48 pt text at 100 %
    const auto measure = [](const std::string &text) {
      return static_cast<int>(text.size()) * 14;
    };
    auto page = make_page(fusion::CandidateLayout::Vertical, 10);
    page.font.size_pt = 48;

    fusion::tsf::LayoutLimits limits;
    limits.max_width = 800;
    limits.max_height = 600;
    const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);

    CHECK(layout.height <= 600);
    CHECK(layout.width <= 800);
    int placed = 0;
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      const auto &slot = layout.slots[index];
      if (slot.empty())
        continue;
      ++placed;
      CHECK(slot.top >= 0 && slot.bottom <= 600);
      CHECK(slot.left >= 0 && slot.right <= 800);
      CHECK(slot.bottom - slot.top >= metrics.line_height);
      const auto hit = fusion::tsf::hit_test_candidate_panel(layout, slot.left + 1,
                                                             (slot.top + slot.bottom) / 2);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }
    CHECK(placed == 10);
    // More than one column: two candidates share a row, which can only happen when the
    // single column would not have fitted. Without that, this page could not be on screen.
    bool shared_row = false;
    for (std::size_t a = 0; a < layout.slots.size(); ++a)
      for (std::size_t b = a + 1; b < layout.slots.size(); ++b)
        if (!layout.slots[a].empty() && !layout.slots[b].empty() &&
            layout.slots[a].top == layout.slots[b].top &&
            layout.slots[a].left != layout.slots[b].left)
          shared_row = true;
    CHECK(shared_row);
  }

  // 19. The popup's own DPI is the one it measured with, and its own rectangle obeys the
  //     work area: a caret near the bottom-right corner produces a window that is above the
  //     caret and inside the screen, not one hanging off the edge.
  {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{120, 120}, MONITOR_DEFAULTTONEAREST), &info))
      work = info.rcWork;

    const int awareness = static_cast<int>(GetAwarenessFromDpiAwarenessContext(
        GetThreadDpiAwarenessContext()));
    std::printf("dpi awareness of this process: %d (0 unaware, 1 system, 2 per-monitor)\n",
                awareness);

    auto page = make_page(fusion::CandidateLayout::Vertical, entries);
    const POINT corner{work.right - scaled_for_test(40), work.bottom - scaled_for_test(40)};
    view.show(page, corner);
    CHECK(view.visible());
    // Measurement, window and font agree on one DPI.
    using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
    const auto get_dpi =
        reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                           "GetDpiForWindow"));
    if (get_dpi != nullptr) {
      const int window_dpi = static_cast<int>(get_dpi(view.window()));
      CHECK(window_dpi == view.dpi());
    }
    RECT rect{};
    GetWindowRect(view.window(), &rect);
    CHECK(rect.left >= work.left && rect.right <= work.right);
    CHECK(rect.top >= work.top && rect.bottom <= work.bottom);
    // No room below the caret at the bottom of the screen, so it opened above it.
    CHECK(rect.bottom <= corner.y);
  }

  // 20. The same popup created from a per-monitor-aware context — the awareness a modern
  //     host application gives the TIP. The window and the measurement have to agree on the
  //     DPI in that mode too, and a point size has to come out as that many points at the
  //     DPI the window reports (not at the 96 the unaware path above is pinned to).
  {
    const DPI_AWARENESS_CONTEXT previous =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (previous == nullptr) {
      std::printf("per-monitor context: not available on this system\n");
    } else {
      fusion::tsf::CandidateView aware;
      CHECK(aware.ensure_created(nullptr, nullptr, 0));
      auto page = make_page(fusion::CandidateLayout::Vertical, 5);
      page.font.family = "Microsoft YaHei UI";
      page.font.size_pt = 11;
      aware.show(page, POINT{200, 200});
      CHECK(aware.visible());

      using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
      const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
          GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
      const int window_dpi =
          get_dpi != nullptr ? static_cast<int>(get_dpi(aware.window())) : 96;
      std::printf("per-monitor context: window dpi=%d, measured dpi=%d, line=%d\n", window_dpi,
                  aware.dpi(), aware.line_height());
      CHECK(window_dpi == aware.dpi());
      // 11 pt is 11/72 inch: the drawn line has to be at least that many device pixels, at
      // whatever DPI this display actually reports.
      CHECK(aware.line_height() >= 11 * window_dpi / 72);
      // And the panel is still placed inside its monitor's work area at that scale.
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      if (GetMonitorInfoW(MonitorFromWindow(aware.window(), MONITOR_DEFAULTTONEAREST), &info))
        work = info.rcWork;
      RECT rect{};
      GetWindowRect(aware.window(), &rect);
      CHECK(rect.left >= work.left && rect.right <= work.right);
      CHECK(rect.top >= work.top && rect.bottom <= work.bottom);
      CHECK(slots_hold_the_text(aware));
      aware.destroy();
      CHECK(aware.window() == nullptr);
      SetThreadDpiAwarenessContext(previous);
    }
  }

  // 21. place_fulltext_tip is a pure geometric decision: the box it returns is always on
  //     screen and never on top of the panel, and when the text wanted more than any gap can
  //     hold, the box shrinks to the largest non-overlapping band instead of spilling over
  //     the candidates. A tall 400px popup beside a panel that already fills a short work
  //     area is the exact case the old inline fallback (y = work.top) put on top of the list.
  {
    using fusion::tsf::LayoutRect;
    const LayoutRect work{0, 0, 1200, 300};
    const LayoutRect panel{300, 40, 900, 260}; // only ~40px of free vertical room at the top
    const LayoutRect big = fusion::tsf::place_fulltext_tip(panel, work, 400, 400, 2);
    CHECK(big.left >= work.left && big.right <= work.right);
    CHECK(big.top >= work.top && big.bottom <= work.bottom);
    // Disjoint from the panel on at least one axis.
    CHECK(big.right <= panel.left || big.left >= panel.right || big.bottom <= panel.top ||
          big.top >= panel.bottom);
    CHECK(!big.empty());

    // When a band fits the whole text, the requested size is kept and it opens below first.
    const LayoutRect roomy{0, 0, 1000, 1000};
    const LayoutRect small_panel{100, 100, 300, 200};
    const LayoutRect below = fusion::tsf::place_fulltext_tip(small_panel, roomy, 150, 100, 4);
    CHECK(below.right - below.left == 150 && below.bottom - below.top == 100);
    CHECK(below.top == small_panel.bottom + 4);
    CHECK(!(below.left < small_panel.right && below.right > small_panel.left &&
            below.top < small_panel.bottom && below.bottom > small_panel.top));
  }

  // 21b. The same rule proven on real windows: a candidate so long its full text scrolls,
  //      hovered over, must not leave the box covering the panel it describes. This is the
  //      case that failed on the old placement — the fallback pinned the tall box at the top
  //      of the work area, right across the candidate list.
  {
    auto page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 3);
    page.entries[3].text.clear();
    for (int i = 0; i < 2400; ++i)
      page.entries[3].text += "很长的候选内容";
    show_and_measure(view, page);
    const auto &slot = view.layout().slots[3];
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    CHECK(view.tip_scrollable());
    if (view.tip_window() != nullptr) {
      RECT panel_rect{};
      RECT tip_rect{};
      GetWindowRect(view.window(), &panel_rect);
      GetWindowRect(view.tip_window(), &tip_rect);
      const bool overlaps = tip_rect.left < panel_rect.right && tip_rect.right > panel_rect.left &&
                            tip_rect.top < panel_rect.bottom && tip_rect.bottom > panel_rect.top;
      CHECK(!overlaps);
      // The scroll extent is measured against the box that was actually placed: the visible
      // height is at most the window minus its padding, and the maximum scroll is exactly the
      // text that does not fit in that visible height. A stale pre-placement extent would let
      // the end fall past the real box.
      CHECK(height_of(tip_rect) >= view.tip_view_height());
      CHECK(view.tip_scroll_max() == view.tip_content_height() - view.tip_view_height());
    }
  }

  // 22. The full-text box survives the pointer moving from the clipped slot *into* the box
  //     so it can be wheeled, and hides only once the pointer has left the box too. The old
  //     code tore the box down on the panel's leave, which made a long candidate impossible
  //     to read.
  {
    auto page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 3);
    page.entries[3].text.clear();
    for (int i = 0; i < 2400; ++i)
      page.entries[3].text += "很长的候选内容";
    show_and_measure(view, page);
    const auto &slot = view.layout().slots[3];
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    CHECK(view.tip_scrollable());

    if (view.tip_window() != nullptr) {
      RECT tip{};
      GetWindowRect(view.tip_window(), &tip);
      const POINT inside{tip.left + (tip.right - tip.left) / 2, tip.top + (tip.bottom - tip.top) / 2};
      SetCursorPos(inside.x, inside.y);
      // The panel reports the pointer leaving it while the pointer is inside the box: keep it.
      SendMessageW(view.window(), WM_MOUSELEAVE, 0, 0);
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));

      // The pointer leaves the box itself: only now does the box go away.
      SendMessageW(view.tip_window(), WM_MOUSEMOVE, 0, 0);
      SetCursorPos(tip.left - scaled_for_test(400), tip.top);
      SendMessageW(view.tip_window(), WM_MOUSELEAVE, 0, 0);
      CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));
    }
  }

  view.destroy();
  CHECK(view.window() == nullptr);

  if (failures == 0) {
    std::printf("candidate_window_layout: horizontal %dx%d, vertical %dx%d\n", horizontal_width,
                horizontal_height, vertical_width, vertical_height);
  }
  std::fprintf(stderr, "candidate_window_layout: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
