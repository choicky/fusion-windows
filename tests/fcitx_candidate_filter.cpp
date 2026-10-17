// Blocker-1 regression against the real Fcitx5 engine (system libraries, no
// doubles): the candidate display projection must observe
// Instance::outputFilter exactly the way the mature classic-ui panel path does
// (src/ui/classic/inputwindow.cpp at the pinned revision: label, candidate
// text and comment each pass through instance->outputFilter before display).
// Proves:
//   1. with no OutputFilter consumer the engine call is the identity and the
//      page projects unchanged (so nothing is filtered twice anywhere);
//   2. with a consumer connected, every label/text/comment of the projection
//      carries the engine-filtered display text, and the consumer receives
//      this input context;
//   3. filtering disturbs nothing else: engine index identity, placeholder
//      slots, cursor and pager truth are preserved exactly as before.
// CMake registers this target only on non-Windows builds and only when the
// Fcitx5 development libraries are present; the Windows product path covers
// the same code inside FusionHost.exe.

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include "fcitx/candidate_projection.hpp"

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

class TestFrontend final : public fcitx::InputContext {
public:
  explicit TestFrontend(fcitx::Instance &instance)
      : fcitx::InputContext(instance.inputContextManager(), "fusion-filter-test") {
    created();
  }
  ~TestFrontend() override { destroy(); }
  const char *frontend() const override { return "fusion-filter-test"; }
  void commitStringImpl(const std::string &) override {}
  void deleteSurroundingTextImpl(int, unsigned) override {}
  void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
  void updatePreeditImpl() override {}
};

fcitx::Text text(const std::string &raw) {
  fcitx::Text value;
  value.append(raw);
  return value;
}

// Engines mark placeholder slots from their own CandidateWord subclasses
// (setPlaceHolder is protected); the test mirror does the same.
class PlaceholderWord final : public fcitx::DisplayOnlyCandidateWord {
public:
  PlaceholderWord(fcitx::Text text, fcitx::Text comment)
      : fcitx::DisplayOnlyCandidateWord(std::move(text), std::move(comment)) {
    setPlaceHolder(true);
  }
};

std::shared_ptr<fcitx::CommonCandidateList> make_list() {
  auto list = std::make_shared<fcitx::CommonCandidateList>();
  auto first = std::make_unique<fcitx::DisplayOnlyCandidateWord>(text("你好"), text("nh"));
  auto second = std::make_unique<PlaceholderWord>(text("逆"), text("n"));
  auto third = std::make_unique<fcitx::DisplayOnlyCandidateWord>(text("尼"), text("n"));
  list->append(std::move(first));
  list->append(std::move(second));
  list->append(std::move(third));
  list->setCursorIndex(0);
  return list;
}

} // namespace

int main() {
  char program[] = "fusion-filter-test";
  char disable[] = "--disable=all";
  char *argv[] = {program, disable};
  fcitx::Instance instance(2, argv);
  TestFrontend frontend(instance);
  auto list = make_list();
  // The engine owns label text (selection-key derived); the projection must
  // carry exactly what the engine hands the display path, filtered or not.
  const std::string label0 = list->label(0).toString();

  // 1. No consumer connected: Instance::outputFilter is the identity.
  {
    const auto page = fusion::project_candidate_page(instance, &frontend, list, 10, 4096, 4096);
    CHECK(page.has_value());
    CHECK(page->entries.size() == 3);
    CHECK(page->entries[0].text == "你好");
    CHECK(page->entries[0].comment == "nh");
    CHECK(page->entries[0].label == label0);
    CHECK(page->entries[1].placeholder);
    CHECK(page->entries[2].text == "尼");
    CHECK(page->cursor_index == 0);
    CHECK(page->layout ==
          static_cast<fusion::CandidateLayout>(static_cast<std::uint8_t>(list->layoutHint())));
    CHECK(!page->truncated);
  }

  // 2. A connected consumer rewrites every display Text it is given; the
  //    projection must carry the filtered values for label, text and comment.
  std::vector<fcitx::InputContext *> seen;
  auto connection = instance.connect<fcitx::Instance::OutputFilter>(
      std::function<void(fcitx::InputContext *, fcitx::Text &)>(
          [&](fcitx::InputContext *ic, fcitx::Text &orig) {
            seen.push_back(ic);
            orig = text("F:" + orig.toString());
          }));
  {
    const auto page = fusion::project_candidate_page(instance, &frontend, list, 10, 4096, 4096);
    CHECK(page.has_value());
    CHECK(page->entries.size() == 3);
    CHECK(page->entries[0].text == "F:你好");
    CHECK(page->entries[0].comment == "F:nh");
    CHECK(page->entries[0].label == "F:" + label0);
    CHECK(page->entries[2].text == "F:尼");
    // Each retained entry had its fields filtered exactly once (no double
    // filtering): 3 entries x (label? first ten entries filtered) + text +
    // comment == 9 calls, all for this context.
    CHECK(seen.size() == 9);
    for (const auto *ic : seen)
      CHECK(ic == &frontend);
    // 3. Index identity, placeholder slot and cursor survive the filtering.
    CHECK(page->entries[1].placeholder);
    CHECK(page->entries[1].text == "F:逆");
    CHECK(page->cursor_index == 0);
    CHECK(page->has_next == list->toPageable()->hasNext());
    CHECK(page->has_prev == list->toPageable()->hasPrev());
  }

  // Bounds behavior is unchanged by the repair: an entry bound that cuts the
  // page records truncation and normalizes a dropped cursor to no-highlight.
  {
    list->setCursorIndex(2);
    const auto page = fusion::project_candidate_page(instance, &frontend, list, 1, 4096, 4096);
    CHECK(page.has_value());
    CHECK(page->truncated);
    CHECK(page->entries.size() == 1);
    CHECK(page->entries[0].text == "F:你好");
    CHECK(page->cursor_index == -1);
  }

  if (failures != 0) {
    std::fprintf(stderr, "fcitx_candidate_filter: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf("fcitx_candidate_filter: all checks passed\n");
  return 0;
}
