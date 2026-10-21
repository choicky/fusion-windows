// The candidate popup is a real window, and the layout preference has to change what it
// draws. A stored enum, or a screenshot of the settings window, proves nothing about that:
// this check creates the actual popup — the same window class, the same
// layout_candidate_panel, the same SetWindowPos and paint path the TSF service uses —
// shows it with a projected page in each geometry and reads the real window rectangle back.
//
// What it covers: the window the user sees is sized by the page's effective layout, so a
// horizontal page produces a wide, short panel and a vertical page a tall, narrow one, and
// the popup keeps the non-activating extended styles it had before this change.
//
// What it does NOT cover: a composition inside a real application reaching this popup.
// That path (anchor, composition, TIP registration) is unchanged by this work and would
// require an installed TIP, which this task must not touch.
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

int width_of(const RECT &rect) { return rect.right - rect.left; }

int height_of(const RECT &rect) { return rect.bottom - rect.top; }

RECT show_and_measure(fusion::tsf::CandidateView &view, const fusion::CandidatePage &page) {
  view.show(page, POINT{120, 120});
  RECT rect{};
  if (view.window() != nullptr) {
    GetWindowRect(view.window(), &rect);
  }
  return rect;
}

} // namespace

int main() {
  constexpr int entries = 8;
  fusion::tsf::CandidateView view;

  // 1. The real popup window is created, and it is the non-activating tool window the
  //    Stage 5 surface was defined as: a layout option must not change that.
  CHECK(view.ensure_created(nullptr, nullptr, 0));
  CHECK(view.window() != nullptr);
  if (view.window() == nullptr) {
    std::fprintf(stderr, "candidate_window_layout: the popup could not be created\n");
    return 1;
  }

  const LONG_PTR extended = GetWindowLongPtrW(view.window(), GWL_EXSTYLE);
  CHECK((extended & WS_EX_NOACTIVATE) != 0);
  CHECK((extended & WS_EX_TOOLWINDOW) != 0);

  // 2. A horizontal page makes a wide, short panel.
  const auto horizontal = make_page(fusion::CandidateLayout::Horizontal, entries);
  const RECT horizontal_rect = show_and_measure(view, horizontal);
  const int horizontal_width = width_of(horizontal_rect);
  const int horizontal_height = height_of(horizontal_rect);
  CHECK(view.visible());
  CHECK(horizontal_width > 0 && horizontal_height > 0);
  CHECK(horizontal_width > horizontal_height);
  CHECK(horizontal_width > 200);

  // 3. A vertical page makes a tall, narrow panel — from the same popup and the same
  //    entries, so the only thing that differs is the page's layout value.
  const auto vertical = make_page(fusion::CandidateLayout::Vertical, entries);
  const RECT vertical_rect = show_and_measure(view, vertical);
  const int vertical_width = width_of(vertical_rect);
  const int vertical_height = height_of(vertical_rect);
  CHECK(view.visible());
  CHECK(vertical_width > 0 && vertical_height > 0);
  CHECK(vertical_height > vertical_width);

  // 4. The two geometries are actually different, in the direction each one implies:
  //    the row is wider than the column, and the column is taller than the row.
  CHECK(horizontal_width > vertical_width);
  CHECK(vertical_height > horizontal_height);

  // 5. Switching back re-lays-out the same window, so a saved preference takes effect on
  //    the next projected page rather than only on a newly opened popup.
  const RECT again = show_and_measure(view, horizontal);
  CHECK(width_of(again) == horizontal_width);
  CHECK(height_of(again) == horizontal_height);

  // 6. Nothing to show still hides the popup, unchanged by the layout work.
  fusion::CandidatePage empty;
  empty.layout = fusion::CandidateLayout::Horizontal;
  view.show(empty, POINT{120, 120});
  CHECK(!view.visible());

  if (failures == 0) {
    std::printf("candidate_window_layout: horizontal %dx%d, vertical %dx%d\n", horizontal_width,
                horizontal_height, vertical_width, vertical_height);
  }
  std::fprintf(stderr, "candidate_window_layout: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
