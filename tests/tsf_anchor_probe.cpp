// R46: the anchor decision on its own.
//
// The candidate page has to be placed at the text it belongs to, and a text store can refuse to
// say where that is (TS_E_NOLAYOUT) for a range it has not laid out yet — measured twice, in
// Firefox's contenteditable editor and in the taskbar search box, where it happened on every
// keystroke while composition and commit worked. Hiding the page for that one answer is what
// made the window vanish mid-word. The rule the fix rests on is small enough to test without
// TSF: a rectangle wins, a refusal with a previous good place keeps that place, and anything
// else defers. This check feeds the decision the shapes the probe produces and reads the
// consequences back; the TSF shell that calls it is unchanged arithmetic around these answers.
//
// What it does NOT cover: which call produced each observation (that is the probe's own
// plumbing, and the owner checklist reads its log lines), and a real application's layout
// notification arriving at the right moment.

#include "tsf/anchor_probe.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

fusion::tsf::AnchorObservation with_rect(LONG left, LONG top, LONG right, LONG bottom,
                                         const char *source, BOOL clipped = FALSE) {
  fusion::tsf::AnchorObservation obs;
  obs.rect = RECT{left, top, right, bottom};
  obs.has_rect = true;
  obs.source = source;
  obs.clipped = clipped;
  return obs;
}

fusion::tsf::AnchorObservation failed(HRESULT hr, const char *source) {
  fusion::tsf::AnchorObservation obs;
  obs.hr = hr;
  obs.source = source;
  return obs;
}

using fusion::tsf::decide_anchor;
using fusion::tsf::LastGoodAnchor;
using fusion::tsf::ProbeTrigger;

// The value Firefox and the search box return, spelled from the same constant the probe
// compares against: a test that wrote its own number could drift from the code under test.
constexpr HRESULT kNoLayout = fusion::tsf::kTsENoLayout;

void rect_wins_and_becomes_last_good() {
  const auto obs = with_rect(100, 200, 140, 224, "selection");
  const auto decision = decide_anchor(obs, LastGoodAnchor{}, ProbeTrigger::Key);
  CHECK(decision.placed);
  CHECK(decision.anchor.x == 140 && decision.anchor.y == 224);
  CHECK(decision.becomes_last_good);
  CHECK(!decision.retained);
  CHECK(!decision.no_layout);
  // A key-path rectangle reports the range it came from, and says nothing else.
  CHECK(std::string(decision.source) == "selection");
  CHECK(decision.range == nullptr);
}

void a_refusal_without_a_previous_place_defers() {
  const auto decision =
      decide_anchor(failed(kNoLayout, "selection"), LastGoodAnchor{}, ProbeTrigger::Key);
  // Nothing is invented: no anchor means the caller hides the page, which is the behaviour
  // before this context ever had a place.
  CHECK(!decision.placed);
  CHECK(decision.no_layout);
  CHECK(!decision.retained);
  CHECK(!decision.becomes_last_good);
  // The code that failed is reported, so a log line can tell R46 from any other refusal.
  CHECK(decision.source[0] == '\0');
}

void a_refusal_with_a_previous_place_keeps_it() {
  const LastGoodAnchor last_good{POINT{300, 410}, true};
  const auto decision = decide_anchor(failed(kNoLayout, "selection"), last_good, ProbeTrigger::Key);
  CHECK(decision.placed);
  CHECK(decision.anchor.x == 300 && decision.anchor.y == 410);
  CHECK(decision.retained);
  CHECK(std::string(decision.source) == "last-good");
  // The retained point is not a measurement, so it must not overwrite the last-good anchor:
  // a context that fails twice in a row still sits at the one place that was real.
  CHECK(!decision.becomes_last_good);
  CHECK(decision.no_layout);
}

void another_failure_code_is_not_the_missing_layout_case() {
  const LastGoodAnchor last_good{POINT{300, 410}, true};
  const auto decision = decide_anchor(failed(E_FAIL, "selection"), last_good, ProbeTrigger::Key);
  // The R46 rule is about a store that has not laid the text out yet. Any other failure stays
  // exactly as it was: the page is deferred, not placed on a stored guess.
  CHECK(!decision.placed);
  CHECK(!decision.no_layout);
  CHECK(!decision.retained);
}

void a_layout_change_names_itself_and_its_range() {
  const auto obs = with_rect(50, 60, 90, 82, "composition", TRUE);
  const auto decision = decide_anchor(obs, LastGoodAnchor{}, ProbeTrigger::LayoutChange);
  CHECK(decision.placed);
  CHECK(decision.anchor.x == 90 && decision.anchor.y == 82);
  // Two words, because two things are true: the notification re-probed, and the rectangle came
  // from the composition range rather than the selection.
  CHECK(std::string(decision.source) == "layout-change");
  CHECK(std::string(decision.range) == "composition");
  CHECK(decision.becomes_last_good);
  CHECK(!decision.retained);
  CHECK(obs.clipped == TRUE);
}

void the_caret_fallback_is_a_source_like_any_other() {
  // R25's fallback range, after the selection and the composition both failed to resolve.
  const auto obs = with_rect(0, 0, 12, 20, "composition-caret");
  const auto decision =
      decide_anchor(obs, LastGoodAnchor{POINT{999, 999}, true}, ProbeTrigger::Key);
  CHECK(decision.placed);
  CHECK(std::string(decision.source) == "composition-caret");
  // A fresh rectangle outranks a stored anchor even when a stored anchor exists.
  CHECK(!decision.retained);
  CHECK(decision.anchor.x == 12 && decision.anchor.y == 20);
}

void an_empty_rect_still_places_at_its_corner() {
  // The probe's own guarantee is a resolved rectangle; a zero-width range at the start of an
  // empty line is real, and the popup opens below it like anywhere else.
  const auto decision =
      decide_anchor(with_rect(77, 88, 77, 88, "selection"), LastGoodAnchor{}, ProbeTrigger::Key);
  CHECK(decision.placed);
  CHECK(decision.anchor.x == 77 && decision.anchor.y == 88);
}

} // namespace

int main() {
  static_assert(kNoLayout == MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0206),
                "the code under test must be the documented TS_E_NOLAYOUT");
  rect_wins_and_becomes_last_good();
  a_refusal_without_a_previous_place_defers();
  a_refusal_with_a_previous_place_keeps_it();
  another_failure_code_is_not_the_missing_layout_case();
  a_layout_change_names_itself_and_its_range();
  the_caret_fallback_is_a_source_like_any_other();
  an_empty_rect_still_places_at_its_corner();
  if (failures == 0)
    std::printf("tsf_anchor_probe: ok\n");
  std::fprintf(stderr, "tsf_anchor_probe: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
