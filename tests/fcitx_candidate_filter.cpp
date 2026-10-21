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
// Stage 5 closure adds the preedit and panel boundaries:
//   4. the displayed client preedit is the engine-filtered clientPreedit() Text
//      (dbusfrontend updatePreeditImpl), with its segments, format flags and byte
//      cursor; an unusable cursor is hidden; the engine's own password masking
//      applies, which it does only for the InputPanel's own Text reference;
//   5. the panel rows (auxUp, server-side preedit, auxDown) are engine-filtered
//      like classic-ui, and an empty InputPanel projects no rows.
// CMake registers this target on Windows FUSION_WITH_FCITX5 builds (pinned
// runtime) and on non-Windows builds when Fcitx5 development libraries exist.

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fcitx-utils/capabilityflags.h>
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
    // This list reports no layout hint (CommonCandidateList defaults to NotSet), so the
    // stored preference is what the page carries — and the default preference is vertical,
    // which is why the presentation is unchanged for a user who never configures it.
    CHECK(list->layoutHint() == fcitx::CandidateLayoutHint::NotSet);
    const auto page = fusion::project_candidate_page(instance, &frontend, list,
                                                     fusion::CandidateLayout::Vertical, 10, 4096,
                                                     4096);
    CHECK(page.has_value());
    CHECK(page->entries.size() == 3);
    CHECK(page->entries[0].text == "你好");
    CHECK(page->entries[0].comment == "nh");
    CHECK(page->entries[0].label == label0);
    CHECK(page->entries[1].placeholder);
    CHECK(page->entries[2].text == "尼");
    CHECK(page->cursor_index == 0);
    CHECK(page->layout == fusion::CandidateLayout::Vertical);
    CHECK(!page->truncated);
  }

  // 1b. Layout precedence, which is classicui's own rule (inputwindow.cpp:598-603, and
  //     again at 938-943, pinned fcitx5 a7f2528): an engine hint that names a layout wins
  //     and the stored preference applies only when the hint is NotSet. Asserted through
  //     the projection rather than the helper alone, so it is the page the renderer
  //     receives that is being checked.
  {
    using fusion::CandidateLayout;
    struct Case {
      fcitx::CandidateLayoutHint hint;
      CandidateLayout preference;
      CandidateLayout expected;
    };
    const Case cases[] = {
        {fcitx::CandidateLayoutHint::NotSet, CandidateLayout::Vertical, CandidateLayout::Vertical},
        {fcitx::CandidateLayoutHint::NotSet, CandidateLayout::Horizontal,
         CandidateLayout::Horizontal},
        {fcitx::CandidateLayoutHint::Vertical, CandidateLayout::Horizontal,
         CandidateLayout::Vertical},
        {fcitx::CandidateLayoutHint::Horizontal, CandidateLayout::Vertical,
         CandidateLayout::Horizontal},
    };
    for (const auto &one : cases) {
      list->setLayoutHint(one.hint);
      const auto page = fusion::project_candidate_page(instance, &frontend, list, one.preference,
                                                       10, 4096, 4096);
      CHECK(page.has_value());
      CHECK(page->layout == one.expected);
    }
    // A hint is never downgraded to NotSet, so a renderer that treats NotSet as "vertical"
    // can never turn a semantic hint into the wrong geometry.
    CHECK(fusion::resolve_candidate_layout(CandidateLayout::NotSet,
                                           CandidateLayout::Horizontal) ==
          CandidateLayout::Horizontal);
    CHECK(fusion::resolve_candidate_layout(CandidateLayout::Vertical,
                                           CandidateLayout::Horizontal) ==
          CandidateLayout::Vertical);
    list->setLayoutHint(fcitx::CandidateLayoutHint::NotSet); // restore the list's own default
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
    const auto page = fusion::project_candidate_page(instance, &frontend, list,
                                                     fusion::CandidateLayout::Vertical, 10, 4096,
                                                     4096);
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
    const auto page = fusion::project_candidate_page(instance, &frontend, list,
                                                     fusion::CandidateLayout::Vertical, 1, 4096,
                                                     4096);
    CHECK(page.has_value());
    CHECK(page->truncated);
    CHECK(page->entries.size() == 1);
    CHECK(page->entries[0].text == "F:你好");
    CHECK(page->cursor_index == -1);
  }

  // 4/5. Preedit and panel rows. Replace the rewriting consumer with a
  //      structure-preserving one (ASCII upper-casing keeps every segment length
  //      and the byte cursor), so the checks see both "filtered" and "intact".
  connection.disconnect();
  int filtered_calls = 0;
  auto structural = instance.connect<fcitx::Instance::OutputFilter>(
      std::function<void(fcitx::InputContext *, fcitx::Text &)>(
          [&](fcitx::InputContext *, fcitx::Text &orig) {
            ++filtered_calls;
            fcitx::Text upper;
            for (int i = 0, e = static_cast<int>(orig.size()); i < e; ++i) {
              std::string piece = orig.stringAt(i);
              for (auto &ch : piece)
                if (ch >= 'a' && ch <= 'z')
                  ch = static_cast<char>(ch - 'a' + 'A');
              upper.append(std::move(piece), orig.formatAt(i));
            }
            upper.setCursor(orig.cursor());
            orig = std::move(upper);
          }));
  (void)structural;
  {
    fcitx::Text client;
    client.append("ni", fcitx::TextFormatFlags{fcitx::TextFormatFlag::HighLight,
                                               fcitx::TextFormatFlag::Underline});
    client.append("你hao", fcitx::TextFormatFlag::Underline);
    client.setCursor(2);
    frontend.inputPanel().setClientPreedit(client);
    filtered_calls = 0;
    const auto projected = fusion::project_client_preedit(instance, &frontend, 4096, 16);
    CHECK(projected.has_value());
    CHECK(filtered_calls == 1);                    // filtered exactly once
    CHECK(projected->text == "NI你HAO");           // the engine-filtered display text
    CHECK(projected->format.segments.size() == 2); // segments survive the filter
    CHECK(projected->format.segments[0].bytes == 2);
    CHECK(projected->format.segments[0].format ==
          (fusion::text_format::highlight | fusion::text_format::underline));
    CHECK(projected->format.segments[1].bytes == 6);
    CHECK(projected->format.segments[1].format == fusion::text_format::underline);
    CHECK(projected->format.cursor == 2); // engine byte cursor
    CHECK(projected->format.consistent_with(projected->text));

    client.setCursor(-1);
    frontend.inputPanel().setClientPreedit(client);
    CHECK(fusion::project_client_preedit(instance, &frontend, 4096, 16)->format.cursor == -1);
    client.setCursor(3); // inside 你 (bytes 2..4): not displayable, shown hidden
    frontend.inputPanel().setClientPreedit(client);
    CHECK(fusion::project_client_preedit(instance, &frontend, 4096, 16)->format.cursor == -1);
    client.setCursor(100); // past the end: hidden (classic-ui rule)
    frontend.inputPanel().setClientPreedit(client);
    CHECK(fusion::project_client_preedit(instance, &frontend, 4096, 16)->format.cursor == -1);
    client.setCursor(8); // end
    frontend.inputPanel().setClientPreedit(client);
    CHECK(fusion::project_client_preedit(instance, &frontend, 4096, 16)->format.cursor == 8);

    // Bounds fail closed rather than truncating the composition.
    CHECK(!fusion::project_client_preedit(instance, &frontend, 4, 16).has_value());
    CHECK(!fusion::project_client_preedit(instance, &frontend, 4096, 1).has_value());

    // The engine masks a password field's preedit only for the panel's own
    // Text reference: masking proves the projection hands it that reference.
    frontend.setCapabilityFlags(fcitx::CapabilityFlag::Password);
    const auto masked = fusion::project_client_preedit(instance, &frontend, 4096, 16);
    CHECK(masked.has_value());
    CHECK(masked->text.find("NI") == std::string::npos);
    CHECK(masked->text.find("\xE2\x80\xA2") != std::string::npos);
    frontend.setCapabilityFlags(fcitx::CapabilityFlags{});
  }
  {
    frontend.inputPanel().reset();
    fusion::CandidatePage empty_rows;
    filtered_calls = 0;
    CHECK(fusion::project_panel_text(instance, &frontend, empty_rows, 4096));
    CHECK(filtered_calls == 0); // an empty InputPanel displays nothing
    CHECK(empty_rows.aux_up.empty() && empty_rows.panel_preedit.empty() &&
          empty_rows.aux_down.empty());
    CHECK(empty_rows.panel_empty());

    frontend.inputPanel().setAuxUp(text("[stroke]h"));
    frontend.inputPanel().setPreedit(text("ni hao"));
    frontend.inputPanel().setAuxDown(text("down"));
    fusion::CandidatePage rows;
    CHECK(fusion::project_panel_text(instance, &frontend, rows, 4096));
    CHECK(filtered_calls == 3);
    CHECK(rows.aux_up == "[STROKE]H");
    CHECK(rows.panel_preedit == "NI HAO");
    CHECK(rows.aux_down == "DOWN");
    CHECK(rows.empty());        // no candidate slot ...
    CHECK(!rows.panel_empty()); // ... yet the panel is displayed

    fusion::CandidatePage bounded;
    CHECK(!fusion::project_panel_text(instance, &frontend, bounded, 3)); // fail closed
    frontend.inputPanel().reset();
  }

  if (failures != 0) {
    std::fprintf(stderr, "fcitx_candidate_filter: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf("fcitx_candidate_filter: all checks passed\n");
  return 0;
}
