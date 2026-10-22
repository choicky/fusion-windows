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

  view.destroy();
  CHECK(view.window() == nullptr);

  if (failures == 0) {
    std::printf("candidate_window_layout: horizontal %dx%d, vertical %dx%d\n", horizontal_width,
                horizontal_height, vertical_width, vertical_height);
  }
  std::fprintf(stderr, "candidate_window_layout: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
