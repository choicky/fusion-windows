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
//   - the scrolling grid sizes each column by its own widest cell, so one long candidate
//     does not draw four single characters at its width, and the columns stay aligned — while
//     each cell is drawn at its own label-adjusted width, so an unnumbered cell on a row other
//     than the engine's page is capped at eight ideographs too;
//   - the one "too long" cap is eight ideographs of candidate text in both arrangements,
//     with the label and the slot's inset added on top;
//   - a larger configured font really produces a taller panel, and a font family that does
//     not exist still draws (GDI substitution) instead of failing;
//   - the metrics scale with DPI in one place, so 192 dpi is exactly twice 96 dpi;
//   - R49: the popup is owned by the application window it was given and topmost, a new owner
//     re-creates it, its owner's destruction takes it away instead of leaving a dangling HWND,
//     and a re-place request reaches the service through the popup's own window message.
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

#include <algorithm>
#include <cstddef>
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

// A scroll window: `rows` engine pages of `page_size` candidates, with the engine sitting on
// row `current_row`. This is the shape the Host projects (the engine's own paging facts), so
// the layout is exercised with the numbers it really receives.
fusion::CandidatePage make_scroll_page(int page_size, int rows, int current_row) {
  fusion::CandidatePage page;
  page.arrangement = fusion::CandidateArrangement::Scroll;
  page.layout = fusion::CandidateLayout::Horizontal;
  page.page_size = page_size;
  page.window_first_page = 1;
  page.current_page = 1 + current_row;
  page.total_pages = 12;
  page.has_prev = true;
  page.has_next = true;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < page_size; ++column) {
      fusion::CandidateEntry entry;
      // Labels exist only on the engine's own page — it is the row the number keys act on.
      if (row == current_row)
        entry.label = std::to_string(column + 1) + ".";
      entry.text = "候选" + std::to_string(row * page_size + column + 1);
      page.entries.push_back(std::move(entry));
    }
  }
  page.cursor_index = current_row * page_size;
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

RECT window_rect(HWND hwnd) {
  RECT rect{};
  if (hwnd != nullptr)
    GetWindowRect(hwnd, &rect);
  return rect;
}

bool overlaps(const RECT &a, const RECT &b) {
  return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

bool in_rect(const RECT &rect, POINT point) {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
         point.y < rect.bottom;
}

// A point in the space between the two windows: it belongs to neither, which is exactly
// where the pointer is while it travels from a candidate to the full text.
POINT point_between(const RECT &panel, const RECT &tip) {
  if (tip.top >= panel.bottom)
    return POINT{(std::max)(panel.left, tip.left) + 2, (panel.bottom + tip.top) / 2};
  if (tip.bottom <= panel.top)
    return POINT{(std::max)(panel.left, tip.left) + 2, (tip.bottom + panel.top) / 2};
  if (tip.left >= panel.right)
    return POINT{(panel.right + tip.left) / 2, panel.top + 2};
  return POINT{(tip.right + panel.left) / 2, panel.top + 2};
}

std::wstring to_wide(const std::string &text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), count);
  return result;
}

// An independent height for a wrapped text: the test's own font handle, its own GDI
// measurement and the width the popup says it wrapped at. Nothing here reuses the popup's
// arithmetic, which is the point — comparing its counters with each other would prove only
// that they agree.
int wrapped_height_with(const fusion::CandidatePage &page, int dpi, const std::wstring &text,
                        int width) {
  HDC dc = GetDC(nullptr);
  if (dc == nullptr)
    return 0;
  const std::string family = page.font.family.empty() ? "Microsoft YaHei UI" : page.font.family;
  const int size_pt = page.font.size_pt > 0 ? page.font.size_pt : 11;
  HFONT font = CreateFontW(-MulDiv(size_pt, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, FF_DONTCARE, to_wide(family).c_str());
  HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
  RECT cell{0, 0, (std::max)(1, width), 0};
  DrawTextW(dc, text.c_str(), -1, &cell, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  const int height = cell.bottom - cell.top;
  if (previous != nullptr)
    SelectObject(dc, previous);
  if (font != nullptr)
    DeleteObject(font);
  ReleaseDC(nullptr, dc);
  return height;
}

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

// Whether a slot's text fits the rectangle the popup paints it into, asked of GDI with this
// test's own device context and font rather than of the layout's arithmetic.
//
// This is the check the painting defect needed: the layout used to size a slot as exactly the
// measured text and the painter then start drawing 2 px further right, so every candidate
// that fit its own measurement was ellipsized. Comparing the drawn rectangle against an
// independent measurement is a question about the geometry that reaches the screen; comparing
// the layout with itself would only prove it agrees with itself.
int natural_width(const fusion::CandidatePage &page, const std::string &utf8, int dpi) {
  HDC dc = GetDC(nullptr);
  if (dc == nullptr)
    return -1;
  const std::string family = page.font.family.empty() ? "Microsoft YaHei UI" : page.font.family;
  const int size_pt = page.font.size_pt > 0 ? page.font.size_pt : 11;
  HFONT font = CreateFontW(-MulDiv(size_pt, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, FF_DONTCARE, to_wide(family).c_str());
  HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
  const std::wstring text = to_wide(utf8);
  // `GetTextExtentPoint32W` rather than `DT_CALCRECT`: the popup sizes a slot with this call,
  // and DrawText's own calculation adds cell overhang that belongs to the drawing, not to the
  // text's width. The question here is whether the rectangle the popup paints into is the
  // width the text needs — a 2 px offset is what must not come back, and this comparison still
  // sees it with several pixels to spare.
  SIZE extent{};
  GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
  const int width = static_cast<int>(extent.cx);
  if (previous != nullptr)
    SelectObject(dc, previous);
  if (font != nullptr)
    DeleteObject(font);
  ReleaseDC(nullptr, dc);
  return width;
}

bool slots_draw_their_text_whole(const fusion::tsf::CandidateView &view,
                                 const fusion::CandidatePage &page, int dpi) {
  const auto &layout = view.layout();
  for (std::size_t index = 0; index < page.entries.size(); ++index) {
    if (index >= layout.slots.size() || layout.slots[index].empty())
      continue;
    const auto &entry = page.entries[index];
    const int available = layout.text_rect(index).right - layout.text_rect(index).left;
    const int needed = natural_width(page, fusion::tsf::slot_full_text(entry), dpi);
    if (needed < 0 || needed > available)
      return false;
  }
  return true;
}

// R49: an owner window is a real top-level window of the process that hosts the popup. The
// product path takes it from the context view (or from the focus); here it is this test's own
// window, because the ownership and the topmost order are properties of the window pair, not
// of TSF.
HWND make_owner_window() {
  static const bool registered = [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FusionTestOwnerWindow";
    return RegisterClassExW(&wc) != 0;
  }();
  if (!registered)
    return nullptr;
  return CreateWindowExW(0, L"FusionTestOwnerWindow", L"Fusion test owner", WS_POPUP, 10, 10, 20,
                         20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// What the popup does when its window procedure is asked to re-place itself.
int reposition_calls = 0;
void *reposition_entry = nullptr;
std::uint64_t reposition_generation = 0;

// Drains this thread's messages until the recorded call arrives, so the posted request is
// dispatched by the same path the application's own loop would use.
void pump_until_repositioned(int expected) {
  for (int i = 0; i < 2000 && reposition_calls < expected; ++i) {
    MSG msg{};
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got == 0 || got == -1)
      return;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

// R49: the popup routes the re-position through a TextService pointer, so the check that drives
// it needs a pointer of that type — but not a real service. Constructing a TextService here would
// pull its whole COM vtable — every TSF override service.cpp defines — into a link that has none
// of them, and the stub below never touches `this`. An object-sized block of storage is enough to
// prove that the popup's own message reaches the service with the identity it was created for.
alignas(std::max_align_t) unsigned char service_storage[sizeof(fusion::tsf::TextService)];
fusion::tsf::TextService *const any_service =
    reinterpret_cast<fusion::tsf::TextService *>(service_storage);

} // namespace

namespace fusion::tsf {

void TextService::place_candidate_from_anchor(void *entry, std::uint64_t generation) {
  ++reposition_calls;
  reposition_entry = entry;
  reposition_generation = generation;
}

} // namespace fusion::tsf

int main() {
  constexpr int entries = 8;
  fusion::tsf::CandidateView view;

  // 1. The real popup window is created, and it is the non-activating tool window the
  //    Stage 5 surface was defined as: the presentation settings must not change that.
  CHECK(view.ensure_created(nullptr, nullptr, 0, nullptr, false));
  CHECK(view.window() != nullptr);
  if (view.window() == nullptr) {
    std::fprintf(stderr, "candidate_window_layout: the popup could not be created\n");
    return 1;
  }

  const LONG_PTR extended = GetWindowLongPtrW(view.window(), GWL_EXSTYLE);
  CHECK((extended & WS_EX_NOACTIVATE) != 0);
  CHECK((extended & WS_EX_TOOLWINDOW) != 0);

  // The DPI the popup actually drew for, so the independent measurements below use the same
  // font size the layout did.
  const UINT panel_dpi = GetDpiForWindow(view.window());
  const int dpi = panel_dpi != 0 ? static_cast<int>(panel_dpi) : 96;

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
  CHECK(slots_draw_their_text_whole(view, horizontal, dpi));
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
  CHECK(slots_draw_their_text_whole(view, vertical, dpi));

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
    // Leaving the slot does not hide it on the spot: the pointer may be on its way into the
    // box across the gap, so the close is deferred and the cursor's position at the end of
    // the delay decides. With the pointer away from both windows it goes.
    SendMessageW(view.window(), WM_MOUSELEAVE, 0, 0);
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    SetCursorPos(work.left + 4, work.top + 4);
    SendMessageW(view.window(), WM_TIMER, fusion::tsf::CandidateView::tip_close_timer_id, 0);
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

  // 14. The largest size the schema allows, with a full page of candidates. On this display
  //     the column may or may not fit, so the invariants here are the ones that hold either
  //     way: one column, inside the screen, every drawn candidate clickable, and the
  //     highlighted candidate always drawn. The overflow case itself is proven below with a
  //     budget that cannot hold the page, which does not depend on the screen size.
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
    CHECK(layout.total_rows == 10);
    // One column only: no two visible slots share a row, and every visible slot starts at
    // the same left edge.
    int left_edge = -1;
    for (std::size_t a = 0; a < layout.slots.size(); ++a) {
      if (layout.slots[a].empty())
        continue;
      if (left_edge < 0)
        left_edge = layout.slots[a].left;
      CHECK(layout.slots[a].left == left_edge);
      for (std::size_t b = a + 1; b < layout.slots.size(); ++b)
        CHECK(layout.slots[b].empty() || layout.slots[b].top != layout.slots[a].top);
    }
    int placed = 0;
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      if (layout.slots[index].empty())
        continue;
      ++placed;
      CHECK(layout.slots[index].bottom - layout.slots[index].top >= line);
      CHECK(layout.slots[index].top >= 0);
      CHECK(layout.slots[index].bottom <= height_of(rect));
      const auto hit = fusion::tsf::hit_test_candidate_panel(
          layout, layout.slots[index].left + 1,
          (layout.slots[index].top + layout.slots[index].bottom) / 2);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }
    CHECK(placed == layout.visible_rows);
    CHECK(placed > 0);
    // Nothing outside the window takes a click: a row that is not drawn has no rect.
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      if (!layout.slots[index].empty())
        continue;
      const auto hit = fusion::tsf::hit_test_candidate_panel(layout, 1, layout.height - 1);
      CHECK(!hit || hit->kind != fusion::tsf::PanelHit::Kind::Select ||
            hit->index != static_cast<std::int32_t>(index));
    }
    // The highlighted candidate is always inside the window: that is what makes the rows
    // outside it reachable at all (the keyboard moves the window).
    for (std::int32_t cursor = 0; cursor < 10; ++cursor) {
      page.cursor_index = cursor;
      show_and_measure(view, page);
      const auto &current = view.layout();
      CHECK(!current.slots[static_cast<std::size_t>(cursor)].empty());
      CHECK(current.slots[static_cast<std::size_t>(cursor)].top >= 0);
      CHECK(current.slots[static_cast<std::size_t>(cursor)].bottom <= current.height);
    }
  }

  // 14b. The same page against a budget that cannot hold it (a short work area): the panel
  //      becomes a viewport over one column instead of continuing into a second column, and
  //      the window follows the highlighted row.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(64, 96); // 48 pt at 100 %
    const auto measure = [](const std::string &text) {
      return static_cast<int>(text.size()) * 30;
    };
    auto page = make_page(fusion::CandidateLayout::Vertical, 10);
    page.font.size_pt = 48;
    fusion::tsf::LayoutLimits limits;
    limits.max_width = 900;
    limits.max_height = 420;
    page.cursor_index = 0;
    const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
    CHECK(layout.total_rows == 10);
    CHECK(layout.visible_rows > 0 && layout.visible_rows < layout.total_rows);
    CHECK(layout.first_row == 0);
    CHECK(layout.rows_hidden_below());
    CHECK(!layout.rows_hidden_above());
    CHECK(layout.height <= 420);
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      if (layout.slots[index].empty())
        continue;
      CHECK(layout.slots[index].bottom <= 420);
      CHECK(layout.slots[index].top >= 0);
    }
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

  // 18. The same page on a work area that cannot hold the whole column: a small laptop
  //     screen at 125 %, ten candidates at the largest size the schema allows. The column
  //     stays a single column and the panel becomes a bounded viewport over it: the rows
  //     that fit are drawn and clickable, the rest are reached by moving the highlight, and
  //     no second column appears — the engine's page order and PageSize are not rewritten by
  //     the presentation.
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
    CHECK(layout.total_rows == 10);
    CHECK(layout.visible_rows < 10);
    int placed = 0;
    int column_left = -1;
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      const auto &slot = layout.slots[index];
      if (slot.empty())
        continue;
      ++placed;
      CHECK(slot.top >= 0 && slot.bottom <= 600);
      CHECK(slot.left >= 0 && slot.right <= 800);
      CHECK(slot.bottom - slot.top >= metrics.line_height);
      if (column_left < 0)
        column_left = slot.left;
      CHECK(slot.left == column_left); // one column, one left edge
      const auto hit = fusion::tsf::hit_test_candidate_panel(layout, slot.left + 1,
                                                             (slot.top + slot.bottom) / 2);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }
    CHECK(placed == layout.visible_rows);
    CHECK(placed > 0);
    // No two visible candidates share a row: the second-column layout this replaced put
    // them side by side, which is exactly what no longer happens.
    for (std::size_t a = 0; a < layout.slots.size(); ++a)
      for (std::size_t b = a + 1; b < layout.slots.size(); ++b)
        CHECK(layout.slots[a].empty() || layout.slots[b].empty() ||
              layout.slots[a].top != layout.slots[b].top);
    // Walking the highlight from the first candidate to the last walks the window over the
    // whole column: every candidate is visible for exactly the cursor position that names
    // it, and always inside the panel.
    for (std::int32_t cursor = 0; cursor < 10; ++cursor) {
      page.cursor_index = cursor;
      const auto scrolled = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
      CHECK(scrolled.first_row + scrolled.visible_rows <= scrolled.total_rows);
      CHECK(!scrolled.slots[static_cast<std::size_t>(cursor)].empty());
      CHECK(scrolled.slots[static_cast<std::size_t>(cursor)].bottom <= scrolled.height);
    }
    // The window starts at the first row with nothing highlighted and moves down with the
    // cursor, so the first and last candidates are reachable by the same mechanism.
    page.cursor_index = -1;
    const auto top = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
    CHECK(top.first_row == 0 && !top.rows_hidden_above());
    page.cursor_index = 9;
    const auto bottom = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
    CHECK(bottom.rows_hidden_below() == false);
    CHECK(bottom.first_row == bottom.total_rows - bottom.visible_rows);
    CHECK(bottom.rows_hidden_above());
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
      CHECK(aware.ensure_created(nullptr, nullptr, 0, nullptr, false));
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

  // 22. The reading path as the pointer actually makes it: candidate → the gap between the
  //     two windows → the full text → wheel → back to the candidates → away from both. Each
  //     step is a real position and a real message, not a jump into the box followed by a
  //     wheel: the gap is the part that used to destroy the box, because it belongs to
  //     neither window and the panel leaves it on its way in.
  {
    auto page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 3);
    page.entries[3].text.clear();
    for (int i = 0; i < 2400; ++i)
      page.entries[3].text += "很长的候选内容";
    show_and_measure(view, page);
    const auto &slot = view.layout().slots[3];
    CHECK(!slot.empty());
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    CHECK(view.tip_scrollable());

    if (view.tip_window() != nullptr) {
      const RECT panel = window_rect(view.window());
      const RECT tip = window_rect(view.tip_window());
      // The two windows really are apart: without a gap this path would not exist.
      CHECK(!overlaps(panel, tip));
      const POINT gap = point_between(panel, tip);
      CHECK(!in_rect(panel, gap) && !in_rect(tip, gap));

      // 1. The pointer leaves the candidate into the gap. The panel reports the leave; the
      //    box must still be there, and must still be there after the delay expires while
      //    the pointer is in the gap.
      SetCursorPos(gap.x, gap.y);
      SendMessageW(view.window(), WM_MOUSELEAVE, 0, 0);
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      SendMessageW(view.window(), WM_TIMER, fusion::tsf::CandidateView::tip_close_timer_id, 0);
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));

      // 2. The pointer arrives in the box and wheels it. The panel's pending close is
      //    cancelled by the box's own move, and the wheel works from this position.
      const POINT inside{tip.left + width_of(tip) / 2, tip.top + height_of(tip) / 2};
      SetCursorPos(inside.x, inside.y);
      SendMessageW(view.tip_window(), WM_MOUSEMOVE, 0, 0);
      SendMessageW(view.window(), WM_TIMER, fusion::tsf::CandidateView::tip_close_timer_id, 0);
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      CHECK(view.tip_scroll() == 0);
      SendMessageW(view.tip_window(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
      CHECK(view.tip_scroll() > 0);

      // 3. Back over the candidates: the wheel position is kept only while the same slot is
      //    described, and a different slot replaces the text (it is not stale).
      const auto &other = view.layout().slots[4];
      CHECK(!other.empty());
      SetCursorPos(panel.left + (other.left + other.right) / 2,
                   panel.top + (other.top + other.bottom) / 2);
      SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                   MAKELPARAM((other.left + other.right) / 2, (other.top + other.bottom) / 2));
      // Slot 4 is short, so nothing is clipped there: the box goes away rather than describe
      // the candidate the pointer left.
      CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));

      // 4. Hover the long slot again, then leave both windows entirely: after the delay the
      //    box is gone, so nothing stale is left behind.
      SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                   MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      SetCursorPos(panel.left - scaled_for_test(600), panel.top - scaled_for_test(600));
      SendMessageW(view.window(), WM_MOUSELEAVE, 0, 0);
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      SendMessageW(view.window(), WM_TIMER, fusion::tsf::CandidateView::tip_close_timer_id, 0);
      CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));

      // 5. From the box back to the candidates the other way: the box is showing, the panel
      //    takes the pointer, and the panel's own move cancels the close and re-decides.
      SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                   MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      const RECT tip_again = window_rect(view.tip_window());
      SetCursorPos(tip_again.left + 4, tip_again.top + 4);
      SendMessageW(view.tip_window(), WM_MOUSEMOVE, 0, 0);
      SetCursorPos(panel.left + (slot.left + slot.right) / 2,
                   panel.top + (slot.top + slot.bottom) / 2);
      SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                   MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      SendMessageW(view.window(), WM_TIMER, fusion::tsf::CandidateView::tip_close_timer_id, 0);
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    }

    // 6. A refreshed page and a hidden panel both clear the box: a detail must not outlive
    //    the page it describes.
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    show_and_measure(view, make_page(fusion::CandidateLayout::Horizontal, entries));
    CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));
    view.hide();
    CHECK(!view.visible());
  }

  // 23. The text is measured at the width it is drawn at, and its end is reachable. The check
  //     re-measures the same string with its own font handle at the width the popup reports,
  //     so the numbers are compared with an independent measurement of the real window rather
  //     than with the popup's own.
  {
    auto page = make_long_page(fusion::CandidateLayout::Horizontal, entries, 3);
    page.cursor_index = 3; // the keyboard case: no pointer involved
    page.entries[3].text.clear();
    for (int i = 0; i < 2000; ++i)
      page.entries[3].text += "很长的候选内容";
    show_and_measure(view, page);
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    if (view.tip_window() != nullptr) {
      RECT client{};
      GetClientRect(view.tip_window(), &client);
      const int wrap = view.tip_wrap_width();
      CHECK(wrap > 0);
      CHECK(wrap < width_of(client)); // the padding is outside the measured width
      const std::wstring text = to_wide(fusion::tsf::slot_full_text(page.entries[3]));
      const int independent = wrapped_height_with(page, view.dpi(), text, wrap);
      CHECK(independent > 0);
      // The height the popup counted is the height this width really produces.
      CHECK(independent == view.tip_content_height());
      // The premise of the rest: this text is taller than the box it gets. It is stated,
      // not assumed — the band a popup is allowed is the *work area*, so a text that just
      // fits would make "it scrolls" a false claim about a fine implementation (and did:
      // 600 repeats of this string fit a 144-dpi display's band exactly).
      CHECK(independent > view.tip_view_height());
      CHECK(view.tip_scrollable());

      // At maximum scroll the whole text is inside the window: the last line is not past
      // the bottom edge.
      for (int i = 0; i < 4000 && view.tip_scroll() < view.tip_scroll_max(); ++i)
        SendMessageW(view.tip_window(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
      CHECK(view.tip_scroll() == view.tip_scroll_max());
      CHECK(view.tip_scroll() + view.tip_view_height() >= independent);
      CHECK(independent - view.tip_scroll() <= height_of(client));
      // One notch back moves it, so the end is not a one-way trap.
      SendMessageW(view.tip_window(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
      CHECK(view.tip_scroll() < view.tip_scroll_max());
    }
  }

  // 24. Placement never covers the candidates, not even when there is nowhere to go: a panel
  //     that fills the work area leaves no band, and the honest answer is an empty rect (the
  //     caller shows nothing) rather than a box dropped on top of the list. The three sizes
  //     below are the cases around that boundary.
  {
    using fusion::tsf::LayoutRect;
    const LayoutRect work{0, 0, 1000, 800};
    const LayoutRect full{0, 0, 1000, 800}; // the panel is the work area
    CHECK(fusion::tsf::place_fulltext_tip(full, work, 300, 200, 4).empty());

    // A panel narrower than the work area on a nearly-full screen: the only band left is the
    // narrow one beside it, so the box is narrowed to that band — still disjoint, still on
    // screen, and no wider than what the band can hold.
    const LayoutRect tall{100, 0, 900, 800};
    const LayoutRect inband = fusion::tsf::place_fulltext_tip(tall, work, 600, 780, 4);
    CHECK(!inband.empty());
    CHECK(!overlaps(RECT{inband.left, inband.top, inband.right, inband.bottom},
                    RECT{tall.left, tall.top, tall.right, tall.bottom}));
    CHECK(inband.right - inband.left <= 600);
    CHECK(inband.left >= work.left && inband.right <= work.right);

    // A degenerate work area has no honest answer either.
    CHECK(fusion::tsf::place_fulltext_tip(tall, LayoutRect{}, 300, 200, 4).empty());
  }

  // 25. Why the measure has to run twice, measured on the text itself: placement can narrow
  //     the box (only a side band is free), and the same text needs more lines in a narrower
  //     box. Both heights are measured here with this check's own font and GDI, so the gap
  //     between them is a property of the text and the two widths, not of the popup.
  {
    using fusion::tsf::LayoutRect;
    const LayoutRect work{0, 0, 1000, 800};
    const LayoutRect tall_panel{100, 0, 900, 800}; // full height: only a side band is free
    const LayoutRect placed = fusion::tsf::place_fulltext_tip(tall_panel, work, 600, 780, 4);
    CHECK(!placed.empty());
    const int placed_width = placed.right - placed.left;
    CHECK(placed_width < 600); // narrowed by the band
    CHECK(!overlaps(RECT{placed.left, placed.top, placed.right, placed.bottom},
                    RECT{tall_panel.left, tall_panel.top, tall_panel.right, tall_panel.bottom}));

    fusion::CandidatePage page; // default font, as a page without one is drawn
    std::string text;
    for (int i = 0; i < 120; ++i)
      text += "很长很长的候选内容";
    const std::wstring wide = to_wide(text);
    const int requested_height = wrapped_height_with(page, 96, wide, 600 - 12);
    const int placed_height = wrapped_height_with(page, 96, wide, placed_width - 12);
    CHECK(requested_height > 0);
    // A single-pass measure would set the scroll extent from the first number and stop
    // short of the real end, which is exactly the defect.
    CHECK(placed_height > requested_height);
  }

  // 25. Automatic scrolling is a grid, and it is drawn by the same popup: columns are the
  //     engine's own page size, every cell has one width, and each row is one engine page.
  {
    const auto page = make_scroll_page(5, 5, 2);
    const RECT rect = show_and_measure(view, page);
    CHECK(view.visible());
    const auto &layout = view.layout();
    CHECK(layout.total_rows == 5);
    CHECK(layout.visible_rows >= 1 && layout.visible_rows <= 5);
    CHECK(layout.slots.size() == page.entries.size());
    CHECK(slots_hold_the_text(view));

    // Row 0's five cells share a top and are consecutive in x; the sixth cell starts a row.
    // Each column has its own width (see 30 for the case where they differ), so the shared
    // number here is the column's, not the grid's.
    CHECK(layout.slots[0].right - layout.slots[0].left > 0);
    for (std::size_t column = 1; column < 5; ++column) {
      CHECK(layout.slots[column].top == layout.slots[0].top);
      CHECK(layout.slots[column].left > layout.slots[column - 1].left);
    }
    CHECK(layout.slots[5].top > layout.slots[0].top);
    // Column alignment, not per-row packing: the cell below column c starts at the same x as
    // the column it belongs to. Its right edge is its own (a numbered cell is wider than an
    // unnumbered one in the same column), so only the left edge is shared.
    for (std::size_t index = 5; index < 25; ++index) {
      const std::size_t column = index % 5;
      CHECK(layout.slots[index].left == layout.slots[column].left);
    }
    // Every visible cell is its own click target, and the index the hit reports is the one
    // the page carries — the page-local index the semantic layer turns into a global one.
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      if (layout.slots[index].empty())
        continue;
      const auto hit = fusion::tsf::hit_test_candidate_panel(
          layout, layout.slots[index].left + 1,
          (layout.slots[index].top + layout.slots[index].bottom) / 2);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }
    // The window fits the work area and is wider than it is tall.
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(POINT{120, 120}, MONITOR_DEFAULTTONEAREST), &info))
      work = info.rcWork;
    CHECK(width_of(rect) <= width_of(work) && height_of(rect) <= height_of(work));
  }

  // 26. A different PageSize is a different number of columns, with no setting of ours in
  //     between: seven per row is seven per row, and the eighth candidate starts row two.
  {
    const auto page = make_scroll_page(7, 3, 0);
    const RECT rect = show_and_measure(view, page);
    CHECK(view.visible());
    const auto &layout = view.layout();
    CHECK(layout.total_rows == 3);
    CHECK(layout.slots.size() == 21);
    CHECK(layout.slots[6].top == layout.slots[0].top);
    CHECK(layout.slots[7].top > layout.slots[0].top);
    CHECK(width_of(rect) > 0);
  }

  // 26b. The two column counts the setting offers — five and nine per row — with numbers only
  //      on the row the engine's keys act on. A one-character, two-character or ordinary short
  //      word is drawn whole in every cell: numbered or not, and whatever the column count.
  {
    const UINT raw = GetDpiForWindow(view.window());
    const int shown_dpi = raw != 0 ? static_cast<int>(raw) : 96;

    for (const int page_size : {5, 9}) {
      const auto page = make_scroll_page(page_size, 3, 1);
      show_and_measure(view, page);
      CHECK(view.visible());
      const auto &layout = view.layout();
      CHECK(layout.slots.size() == page.entries.size());
      // Columns are the engine's page size, and the second row starts where row one ends.
      CHECK(layout.slots[static_cast<std::size_t>(page_size)].top > layout.slots[0].top);
      for (std::size_t index = 0; index < page.entries.size(); ++index) {
        // The numbered row and the unnumbered ones alike: a short word fits, so nothing is
        // reported as clipped and nothing is shortened by the painter either.
        CHECK(!layout.slot_clipped(index));
      }
      CHECK(slots_draw_their_text_whole(view, page, shown_dpi));

      // The numbers are on the engine's row only, and that is what the check above covered:
      // one row numbered, the others not, every cell whole either way.
      CHECK(page.entries[0].label.empty());
      CHECK(!page.entries[static_cast<std::size_t>(page_size)].label.empty());
    }

    // The same question for the horizontal row, which numbers every cell: five and nine
    // candidates across one line, each at its own measured width.
    for (const int count : {5, 9}) {
      const auto page = make_page(fusion::CandidateLayout::Horizontal, count);
      show_and_measure(view, page);
      const auto &layout = view.layout();
      CHECK(layout.slots.size() == page.entries.size());
      for (std::size_t index = 0; index < page.entries.size(); ++index) {
        CHECK(!layout.slot_clipped(index));
        // A slot is exactly its text plus the inset on both sides, so the rectangle the
        // painter draws into is the width the text was measured at.
        CHECK(layout.slots[index].right - layout.slots[index].left ==
              layout.text_rect(index).right - layout.text_rect(index).left +
                  2 * fusion::tsf::layout_metrics_for(view.line_height(), shown_dpi).item_inset);
        CHECK(layout.text_rect(index).right - layout.text_rect(index).left ==
              natural_width(page, fusion::tsf::slot_full_text(page.entries[index]), shown_dpi));
      }
      CHECK(slots_draw_their_text_whole(view, page, shown_dpi));
    }
  }

  // 27. A short last row keeps its columns: two candidates sit under columns one and two, and
  //     nothing is invented for the empty cells.
  {
    fusion::CandidatePage page = make_scroll_page(5, 5, 4);
    page.entries.resize(22);
    page.cursor_index = 20;
    show_and_measure(view, page);
    const auto &layout = view.layout();
    CHECK(layout.slots.size() == 22);
    const auto &last = layout.slots[21];
    const auto &first_of_row = layout.slots[20];
    CHECK(last.top == first_of_row.top);
    CHECK(last.left > first_of_row.left);
    // Column one is where column one starts on the row above (16 = row 3, column 1); the
    // widths are each column's own, so only the left edge is shared.
    CHECK(last.left == layout.slots[16].left);
    // No candidate exists for columns three to five, so there is nothing to click there.
    const auto nowhere = fusion::tsf::hit_test_candidate_panel(
        layout, last.right + 4, (last.top + last.bottom) / 2);
    CHECK(!nowhere || nowhere->kind != fusion::tsf::PanelHit::Kind::Select);
  }

  // 28. A work area that cannot hold five rows shows fewer, and the row the paging keys are
  //     on is one of them. The user's font is not touched to make more fit.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return static_cast<int>(text.size()) * 12;
    };
    for (const int budget : {160, 220, 400}) {
      fusion::tsf::LayoutLimits limits;
      limits.max_width = 900;
      limits.max_height = budget;
      for (const int current_row : {0, 2, 4}) {
        const auto page = make_scroll_page(5, 5, current_row);
        const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
        CHECK(layout.total_rows == 5);
        CHECK(layout.visible_rows >= 1 && layout.visible_rows <= 5);
        CHECK(layout.height <= budget);
        // The current row's first cell is visible (it has a rect).
        const std::size_t anchor = static_cast<std::size_t>(current_row) * 5;
        CHECK(!layout.slots[anchor].empty());
        // Rows outside the window keep their index and have no rect.
        int shown = 0;
        for (std::size_t index = 0; index < layout.slots.size(); ++index)
          shown += layout.slots[index].empty() ? 0 : 1;
        CHECK(shown == layout.visible_rows * 5);
      }
    }
    // At the smallest budget the row count really is reduced, not merely clipped.
    fusion::tsf::LayoutLimits tight;
    tight.max_width = 900;
    tight.max_height = 160;
    const auto layout = fusion::tsf::layout_candidate_panel(make_scroll_page(5, 5, 4), measure,
                                                            metrics, tight);
    CHECK(layout.visible_rows < 5);
  }

  // 28b. The capacity the popup reports is the number of rows it can really draw: the host
  //      groups scroll pages by it, so a disagreement here is the "sliding window" defect —
  //      a page of five rows drawn as three, whose last visible row is not the page's last.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return static_cast<int>(text.size()) * 12;
    };
    for (const int budget : {152, 190, 266, 342, 1000}) {
      fusion::tsf::LayoutLimits limits;
      limits.max_width = 900;
      limits.max_height = budget;
      const auto page = make_scroll_page(5, 5, 0);
      const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);
      CHECK(layout.row_capacity >= 1 && layout.row_capacity <= 5);
      // What it says it can hold is what it did hold: with a five-row page and a capacity of
      // n, exactly n rows are on screen and no row is silently dropped.
      CHECK(layout.visible_rows == layout.row_capacity);
      CHECK(layout.height <= budget);
      // The capacity is a ceiling on rows, not a squeeze on their size: every visible row is
      // still a whole row of the metrics above.
      std::vector<int> row_tops;
      for (const auto &slot : layout.slots) {
        if (slot.empty())
          continue;
        if (std::find(row_tops.begin(), row_tops.end(), slot.top) == row_tops.end())
          row_tops.push_back(slot.top);
        CHECK(slot.bottom - slot.top >= metrics.line_height);
      }
      CHECK(static_cast<int>(row_tops.size()) == layout.row_capacity);
    }
    // A bigger font buys fewer rows, never smaller text.
    const auto large = fusion::tsf::layout_metrics_for(60, 96);
    fusion::tsf::LayoutLimits short_area;
    short_area.max_width = 900;
    short_area.max_height = 260;
    const auto big = fusion::tsf::layout_candidate_panel(make_scroll_page(5, 5, 0), measure,
                                                         large, short_area);
    CHECK(big.row_capacity < 5);
    CHECK(big.visible_rows == big.row_capacity);
    for (const auto &slot : big.slots) {
      if (!slot.empty())
        CHECK(slot.bottom - slot.top >= large.line_height);
    }
  }

  // 29. A long candidate in a scroll cell is ellipsized like any other, and its full text is
  //     still reachable from the same detail popup — one mechanism, not a second one.
  {
    auto page = make_scroll_page(5, 3, 0);
    for (int i = 0; i < 40; ++i)
      page.entries[6].text += "很长的候选内容";
    page.cursor_index = -1;
    show_and_measure(view, page);
    const auto &slot = view.layout().slots[6];
    CHECK(!slot.empty());
    CHECK(view.layout().slot_clipped(6));
    SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                 MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
    CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
    // Leaving the cell hides it again; the popped box belongs to the cell it describes.
    SendMessageW(view.window(), WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
    CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));
  }

  // 30. The scrolling grid gives every column its own width. The owner's page (自动卷轴,
  //     0.0.0-winui3+3325514): one long candidate and single characters beside it. Every
  //     cell used to be as wide as the longest text in the window, so four one-character
  //     candidates drew as wide as a twelve-character one.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    // Ten pixels per UTF-8 byte, so one ideograph (three bytes) is 30: the arithmetic below
    // is the test's own, not a copy of GDI's numbers.
    const auto measure = [](const std::string &text) {
      return 10 * static_cast<int>(text.size());
    };
    fusion::CandidatePage page;
    page.arrangement = fusion::CandidateArrangement::Scroll;
    page.layout = fusion::CandidateLayout::Horizontal;
    page.page_size = 5;
    page.window_first_page = 1;
    page.current_page = 2; // the engine sits on the window's second row
    page.total_pages = 9;
    page.has_prev = true;
    page.has_next = true;
    const char *cells[3][5] = {
        {"藏", "苍茫的填鸭式我的爱啊啊啊", "仓", "舱", "藏"},
        {"藏", "仓", "舱", "藏", "仓"},
        {"藏", "仓", "舱", "藏", "仓"},
    };
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 5; ++column) {
        fusion::CandidateEntry entry;
        // Labels exist only on the row the engine's number keys act on.
        if (row == 1)
          entry.label = std::to_string(column + 1) + ".";
        entry.text = cells[row][column];
        page.entries.push_back(std::move(entry));
      }
    }
    page.cursor_index = 5;

    fusion::tsf::LayoutLimits limits;
    limits.max_width = 1600;
    limits.max_height = 800;
    const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);

    const int inset = layout.text_inset;
    CHECK(inset == metrics.item_inset);
    const int one = measure("藏") + 2 * inset;         // a single character
    const int numbered = measure("1. 仓") + 2 * inset; // and one carrying its label
    const int label = measure("1. ");                  // every label here is "N. "
    // The cap: eight ideographs of text, plus the inset. The long candidate in row 0 has no
    // label, so that is the whole width its cell may take.
    const int capped = measure("中中中中中中中中") + 2 * inset;

    // A column is as wide as the widest cell it holds — column 1 is the capped long candidate,
    // the others are a numbered single character — but each cell is drawn at its **own** width:
    // the label allowance belongs only to the entries that carry a label (the current engine
    // page's row), so an unnumbered cell in the same column is narrower.
    const int expected_column[5] = {numbered, capped, numbered, numbered, numbered};
    for (int column = 0; column < 5; ++column) {
      for (int row = 0; row < 3; ++row) {
        const std::size_t index = static_cast<std::size_t>(row * 5 + column);
        CHECK(!layout.slots[index].empty());
        // Aligned across rows: same left edge on every row; the width is the cell's own.
        CHECK(layout.slots[index].left == layout.slots[static_cast<std::size_t>(column)].left);
        const int natural = measure(fusion::tsf::slot_full_text(page.entries[index])) + 2 * inset;
        const int limit = capped + (row == 1 ? label : 0);
        CHECK(layout.slots[index].right - layout.slots[index].left == (std::min)(natural, limit));
      }
    }
    CHECK(expected_column[1] > 3 * expected_column[0]); // a wide column, not an accident
    // The same single character is narrower on the row without numbers.
    CHECK(layout.slots[0].right - layout.slots[0].left == one);
    CHECK(layout.slots[5].right - layout.slots[5].left == numbered);

    // The panel is margins + the five column widths + the four gaps between them.
    const int sum = expected_column[0] + expected_column[1] + expected_column[2] +
                    expected_column[3] + expected_column[4];
    CHECK(layout.width == 2 * metrics.side_margin + sum + 4 * metrics.item_gap);

    // The long cell is ellipsized at the cap and says so; twelve ideographs do not fit eight.
    CHECK(layout.slot_clipped(1));
    for (const std::size_t index :
         {std::size_t{0}, std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{5}})
      CHECK(!layout.slot_clipped(index));

    // Nothing about placement changed: a cell is found where it is drawn, a step down is the
    // same column one row lower, and a step right is the next column.
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      const auto &slot = layout.slots[index];
      const auto hit = fusion::tsf::hit_test_candidate_panel(layout, slot.left + 1, slot.top + 1);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }
    for (std::size_t index = 0; index + 5 < layout.slots.size(); ++index) {
      CHECK(layout.slots[index + 5].left == layout.slots[index].left);
      CHECK(layout.slots[index + 5].top > layout.slots[index].top);
    }
    for (std::size_t index = 0; index % 5 != 4; ++index)
      CHECK(layout.slots[index + 1].left > layout.slots[index].left);
  }

  // 31. One "too long" cap for both arrangements: eight ideographs of candidate text,
  //     measured in the candidate font, with the label and the inset added on top rather
  //     than taken out of the eight. Exactly eight is drawn whole; nine is ellipsized at the
  //     width the eight-character cell gets, and its whole text is in the detail popup.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return 10 * static_cast<int>(text.size());
    };
    const std::string eight = "中中中中中中中中";
    const std::string nine = "中中中中中中中中中";
    const std::string twelve = "中中中中中中中中中中中中";
    const int inset = metrics.item_inset;
    const int cap = measure(eight) + 2 * inset;
    const int label = measure("2. ");

    fusion::CandidateEntry short_one;
    short_one.text = eight;
    fusion::CandidateEntry numbered;
    numbered.label = "2.";
    numbered.text = twelve;
    fusion::CandidateEntry one_too_many;
    one_too_many.text = nine;

    // Horizontal, no work area to divide: only the cap decides.
    {
      fusion::CandidatePage page;
      page.layout = fusion::CandidateLayout::Horizontal;
      page.entries = {short_one, numbered, one_too_many};
      const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics);
      CHECK(layout.slots.size() == 3);
      CHECK(layout.slots[0].right - layout.slots[0].left == cap);
      CHECK(!layout.slot_clipped(0)); // exactly eight: nothing is hidden
      // Twelve ideographs and a label: the cap plus the label plus the inset, not the cap
      // shrunk by the label.
      CHECK(layout.slots[1].right - layout.slots[1].left == cap + label);
      CHECK(layout.slot_clipped(1));
      // Nine ideographs: one more than the cap allows, at the same width as eight.
      CHECK(layout.slots[2].right - layout.slots[2].left == cap);
      CHECK(layout.slot_clipped(2));
    }

    // The same three cells as one row of the grid: the cap sets a column width the same way.
    {
      fusion::CandidatePage page;
      page.arrangement = fusion::CandidateArrangement::Scroll;
      page.layout = fusion::CandidateLayout::Horizontal;
      page.page_size = 3;
      page.entries = {short_one, numbered, one_too_many};
      const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics);
      CHECK(layout.total_rows == 1);
      CHECK(layout.slots[0].right - layout.slots[0].left == cap);
      CHECK(layout.slots[1].right - layout.slots[1].left == cap + label);
      CHECK(layout.slots[2].right - layout.slots[2].left == cap);
      CHECK(!layout.slot_clipped(0) && layout.slot_clipped(1) && layout.slot_clipped(2));
    }

    // On the real popup, with the popup's own font: the cap is eight ideographs of that
    // font, and the full text of a clipped cell reaches the detail box whole.
    {
      auto page = make_page(fusion::CandidateLayout::Horizontal, 2);
      page.cursor_index = -1;
      // No label and no comment on either cell, so the cap is the only thing deciding: a
      // comment would legitimately add to the width.
      page.entries[0].label.clear();
      page.entries[0].comment.clear();
      page.entries[0].text = eight;
      page.entries[1].label.clear();
      page.entries[1].comment.clear();
      for (int i = 0; i < 200; ++i)
        page.entries[1].text += "中";
      show_and_measure(view, page);
      const int popup_dpi = view.dpi();
      CHECK(!view.layout().slot_clipped(0));
      CHECK(view.layout().text_rect(0).right - view.layout().text_rect(0).left ==
            natural_width(page, eight, popup_dpi));
      CHECK(view.layout().slot_clipped(1));
      // The cell is the cap, measured by this check's own GDI rather than by the popup.
      CHECK(view.layout().text_rect(1).right - view.layout().text_rect(1).left ==
            natural_width(page, eight, popup_dpi));

      const auto &slot = view.layout().slots[1];
      SendMessageW(view.window(), WM_MOUSEMOVE, 0,
                   MAKELPARAM((slot.left + slot.right) / 2, (slot.top + slot.bottom) / 2));
      CHECK(view.tip_window() != nullptr && IsWindowVisible(view.tip_window()));
      // The box carries the *whole* candidate, not the ellipsized eight: its content height
      // is what this check's own GDI wraps the full text into at the width the box wrapped at.
      const int expected =
          wrapped_height_with(page, popup_dpi, to_wide(page.entries[1].text),
                              view.tip_wrap_width());
      CHECK(expected > 0);
      CHECK(std::abs(view.tip_content_height() - expected) <= view.line_height());
      SendMessageW(view.window(), WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
      CHECK(view.tip_window() == nullptr || !IsWindowVisible(view.tip_window()));
    }
  }

  // 32. The work area still wins where it is tighter than the cap: five long cells in a
  //     narrow budget each get their share, the panel stays inside the budget, and every cell
  //     is still a click target. (The budget tests above are unchanged by the cap.)
  {
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return 10 * static_cast<int>(text.size());
    };
    std::string long_text;
    for (int i = 0; i < 40; ++i)
      long_text += "中";
    std::vector<fusion::CandidateEntry> grid_cells;
    for (int i = 0; i < 10; ++i) {
      fusion::CandidateEntry entry;
      entry.label = std::to_string(i % 5 + 1) + ".";
      entry.text = long_text;
      grid_cells.push_back(std::move(entry));
    }

    const int budget = 500;
    const int share = (budget - 2 * metrics.side_margin - 4 * metrics.item_gap) / 5;
    fusion::tsf::LayoutLimits limits;
    limits.max_width = budget;
    limits.max_height = 600;

    fusion::CandidatePage grid;
    grid.arrangement = fusion::CandidateArrangement::Scroll;
    grid.layout = fusion::CandidateLayout::Horizontal;
    grid.page_size = 5;
    grid.entries = grid_cells;
    const auto scrolled = fusion::tsf::layout_candidate_panel(grid, measure, metrics, limits);
    CHECK(scrolled.width <= budget);
    for (std::size_t index = 0; index < scrolled.slots.size(); ++index) {
      const auto &slot = scrolled.slots[index];
      if (slot.empty())
        continue;
      CHECK(slot.right - slot.left <= share);
      CHECK(slot.left >= 0 && slot.right <= budget);
      CHECK(scrolled.slot_clipped(index)); // the budget is tighter than the text
      const auto hit = fusion::tsf::hit_test_candidate_panel(scrolled, slot.left + 1, slot.top + 1);
      CHECK(hit && hit->index == static_cast<std::int32_t>(index));
    }

    fusion::CandidatePage row = grid;
    row.arrangement = fusion::CandidateArrangement::Page;
    row.page_size = 0;
    row.entries.assign(grid_cells.begin(), grid_cells.begin() + 5);
    const auto across = fusion::tsf::layout_candidate_panel(row, measure, metrics, limits);
    CHECK(across.width <= budget);
    for (const auto &slot : across.slots)
      CHECK(slot.empty() || slot.right <= budget);
  }

  // 33. The cap belongs to the cell, not to the column. Only the current engine page's entries
  //     carry a label (candidate_projection.cpp), so a column can hold a numbered nine-character
  //     candidate and, on another row, an unnumbered one. Sizing the unnumbered cell at the
  //     column's width would hand it the label allowance it does not have and let all nine
  //     characters through unclipped — the cap says eight.
  {
    const auto metrics = fusion::tsf::layout_metrics_for(20, 96);
    const auto measure = [](const std::string &text) {
      return 10 * static_cast<int>(text.size());
    };
    const std::string eight = "中中中中中中中中";
    const std::string nine = "中中中中中中中中中";
    const int inset = metrics.item_inset;
    const int cap = measure(eight) + 2 * inset;
    const int label = measure("1. ");

    fusion::CandidatePage page;
    page.arrangement = fusion::CandidateArrangement::Scroll;
    page.layout = fusion::CandidateLayout::Horizontal;
    page.page_size = 3;
    page.window_first_page = 0;
    page.current_page = 0; // the engine sits on the first row: only that row is numbered
    page.total_pages = 4;
    page.has_prev = true;
    page.has_next = true;
    // Row 0 (the engine's page): numbered. Row 1: unnumbered, the row the report is about.
    const char *cells[2][3] = {
        {"苍茫的填鸭式我的爱啊啊啊", "藏", "仓"},
        {"苍茫的填鸭式我的爱啊啊啊", "中中中中中中中中", "舱"},
    };
    for (int row = 0; row < 2; ++row) {
      for (int column = 0; column < 3; ++column) {
        fusion::CandidateEntry entry;
        if (row == 0)
          entry.label = std::to_string(column + 1) + ".";
        entry.text = cells[row][column];
        page.entries.push_back(std::move(entry));
      }
    }
    page.cursor_index = 0;

    fusion::tsf::LayoutLimits limits;
    limits.max_width = 1600;
    limits.max_height = 800;
    const auto layout = fusion::tsf::layout_candidate_panel(page, measure, metrics, limits);

    // Column 0 is as wide as its widest cell — the numbered one, which owns the label allowance.
    CHECK(layout.slots[0].right - layout.slots[0].left == cap + label);
    // The unnumbered cell in the same column is drawn at its own limit: eight ideographs.
    CHECK(layout.slots[3].right - layout.slots[3].left == cap);
    CHECK(layout.text_rect(3).right - layout.text_rect(3).left == measure(eight));
    // Both nine-character cells are ellipsized, and both start at the column's left edge.
    CHECK(layout.slot_clipped(0));
    CHECK(layout.slot_clipped(3));
    CHECK(layout.slots[3].left == layout.slots[0].left);
    // The cell is narrower than its column, so the column width is still the widest cell's.
    CHECK(layout.slots[3].right < layout.slots[0].right);
    // Exactly eight characters, unnumbered: nothing is hidden.
    CHECK(!layout.slot_clipped(4));
    CHECK(layout.slots[4].right - layout.slots[4].left == cap);
    // The panel width still comes from the column widths, not from the cells: column 0 is the
    // numbered long candidate, column 1 the eight ideographs, column 2 a numbered character.
    const int sum = (cap + label) + cap + (measure("3. 舱") + 2 * inset);
    CHECK(layout.width == 2 * metrics.side_margin + sum + 2 * metrics.item_gap);
    // A click anywhere in a cell is still that cell.
    for (std::size_t index = 0; index < layout.slots.size(); ++index) {
      const auto &slot = layout.slots[index];
      const auto hit = fusion::tsf::hit_test_candidate_panel(layout, slot.left + 1, slot.top + 1);
      CHECK(hit && hit->kind == fusion::tsf::PanelHit::Kind::Select &&
            hit->index == static_cast<std::int32_t>(index));
    }

    // The same shape on the real popup, with the popup's own font: the unnumbered cell's text
    // rectangle is exactly the eight ideographs this check measures for itself.
    {
      auto real = page;
      real.entries[0].text = nine;   // numbered, nine characters
      real.entries[3].text = nine;   // unnumbered, nine characters
      real.entries[4].text = eight;  // unnumbered, exactly eight
      show_and_measure(view, real);
      const int popup_dpi = view.dpi();
      const auto &drawn = view.layout();
      CHECK(drawn.slot_clipped(0));
      CHECK(drawn.slot_clipped(3));
      CHECK(!drawn.slot_clipped(4));
      CHECK(drawn.slots[3].left == drawn.slots[0].left);
      CHECK(drawn.text_rect(3).right - drawn.text_rect(3).left ==
            natural_width(real, eight, popup_dpi));
      // The numbered cell is allowed its label on top of the eight, and nothing more.
      const int label_width = natural_width(real, "1. ", popup_dpi);
      const int numbered_width = drawn.text_rect(0).right - drawn.text_rect(0).left;
      const int unnumbered_width = drawn.text_rect(3).right - drawn.text_rect(3).left;
      CHECK(numbered_width - unnumbered_width >= label_width - 1);
      CHECK(numbered_width - unnumbered_width <= label_width + 1);
    }
  }

  // 34. R49: the popup is owned by the application's own window and topmost. Owned means the
  //     window follows the application it belongs to (a WS_POPUP with a parent is an owner, not
  //     a child), and topmost is what keeps a candidate list above a window that never lets the
  //     IME activate anything. Both are read back from the created window, and the two lifetime
  //     cases are real: a new owner re-creates the popup, and destroying the owner takes it away
  //     rather than leaving a window owned by a dead HWND.
  {
    const HWND owner = make_owner_window();
    CHECK(owner != nullptr);
    auto page = make_page(fusion::CandidateLayout::Vertical, entries);
    fusion::tsf::CandidateView owned;
    int marker = 0;
    CHECK(owned.ensure_created(nullptr, &marker, 7, owner, true));
    const HWND popup = owned.window();
    CHECK(popup != nullptr);
    CHECK(owned.owner() == owner);
    CHECK(owned.owner_from_view());
    CHECK(GetWindow(popup, GW_OWNER) == owner);
    CHECK((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    owned.show(page, POINT{140, 140});
    CHECK(owned.visible());
    // A show re-asserts the order instead of relying on the style alone: the window still
    // reports itself topmost afterwards, and it still belongs to the same owner.
    CHECK((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    CHECK(GetWindow(popup, GW_OWNER) == owner);

    // The same context with a different owner window gets a popup owned by the new one: the
    // service decides the owner per context, so a second document window is never served by a
    // window owned by the first.
    const HWND second = make_owner_window();
    CHECK(second != nullptr);
    CHECK(owned.ensure_created(nullptr, &marker, 7, second, false));
    CHECK(owned.window() != nullptr);
    CHECK(owned.window() != popup);
    CHECK(!IsWindow(popup));
    CHECK(GetWindow(owned.window(), GW_OWNER) == second);
    CHECK(!owned.owner_from_view());

    // Destroying the owner destroys the popup it owns, and the next show creates a fresh one.
    // This is the dangling-HWND case: after this the window list must hold a live window, not
    // the one that died with its owner.
    const HWND dying = owned.window();
    CHECK(DestroyWindow(second) != 0);
    CHECK(owned.window() == nullptr);
    CHECK(!IsWindow(dying));
    CHECK(owned.ensure_created(nullptr, &marker, 7, owner, true));
    CHECK(owned.window() != nullptr);
    CHECK(owned.window() != dying);
    owned.show(page, POINT{160, 160});
    CHECK(owned.visible());
    CHECK(GetWindow(owned.window(), GW_OWNER) == owner);

    // A layout change asks the popup's own thread to re-place it: the request travels as the
    // window message the popup was given, and reaches the service with the identity the popup
    // was created for. Nothing here moves a window from inside an edit session.
    reposition_calls = 0;
    reposition_entry = nullptr;
    reposition_generation = 0;
    CHECK(owned.ensure_created(any_service, &marker, 7, owner, true));
    CHECK(PostMessageW(owned.window(), fusion::tsf::CandidateView::reposition_message, 0, 0) != 0);
    pump_until_repositioned(1);
    CHECK(reposition_calls == 1);
    CHECK(reposition_entry == &marker);
    CHECK(reposition_generation == 7);
    owned.destroy();
    DestroyWindow(owner);
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
