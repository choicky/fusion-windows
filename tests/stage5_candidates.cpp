// Stage 5 candidate projection + action tests (platform-free controller logic).
//
// These drive the real SemanticController real-frontend path against a
// deterministic scripted Backend double. They assert the projection/fencing
// decisions only: candidate page STATE, placeholder index alignment, the
// select-to-EFFECT ordered settlement, paging highlight, stale-page refusal after
// an incarnation move, out-of-range refusal, truncated-page cursor
// normalization, and the focus-loss/presentation-failure invariants (hide only,
// semantics intact). Stage 5 closure adds the preedit contract (UTF-8 byte cursor
// -> UTF-16, segments/format -> TSF styles, fail-closed malformed projection),
// the panel rows with the whole-panel hide rule, and the popup layout/hit-test
// contract with the comment presented on its own slot (tsf/candidate_layout.hpp,
// which candidate_view.cpp only paints). Nothing in this file needs Fcitx, TSF,
// or windows.h.

#include "fusion/backend.hpp"
#include "fusion/text_utf16.hpp"
#include "mock_backend.hpp"
#include "tsf/candidate_layout.hpp"
#include "tsf/preedit_presentation.hpp"
#include "tsf/semantic.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace fusion;
using namespace fusion::tsf;

namespace {

void fail(const char *what) {
  std::fprintf(stderr, "stage5 candidate FAIL: %s\n", what);
  std::exit(1);
}
#define CHECK(cond) ((void)((cond) || (fail(#cond), 0)))

CandidateEntry entry(std::string label, std::string text, bool placeholder = false) {
  CandidateEntry value;
  value.label = std::move(label);
  value.text = std::move(text);
  value.placeholder = placeholder;
  return value;
}

BackendOperation snapshot(CandidatePage page) {
  BackendOperation operation;
  operation.type = BackendOperation::Type::CandidateSnapshot;
  operation.page = std::move(page);
  return operation;
}

BackendOperation preedit(std::string text) {
  return BackendOperation{BackendOperation::Type::SetPreedit, std::move(text), {}, false};
}

BackendOperation styled_preedit(std::string text, std::vector<TextSegment> segments,
                                std::int32_t cursor) {
  BackendOperation operation = preedit(std::move(text));
  operation.preedit = FormattedPreedit{std::move(segments), cursor};
  return operation;
}

BackendOperation commit(std::string text) {
  return BackendOperation{BackendOperation::Type::CommitText, std::move(text), {}, false};
}

// A scripted Backend: each send() pops the next queued response, filling the
// identity fields from the stimulus so a well-formed batch always validates.
class ScriptBackend final : public Backend {
public:
  explicit ScriptBackend(std::uint64_t session = 4001) : session_(session) {}

  bool bind(Binding binding) override {
    bound_ = binding;
    ++bind_calls_;
    return true;
  }
  bool reset(Binding) override {
    ++reset_calls_;
    return true;
  }
  void unbind(Binding) override {}
  bool healthy() const override { return healthy_; }
  std::uint64_t session_id() const override { return session_; }
  LinkStatus link_status() const override {
    return LinkStatus{LinkState::Established, incarnation_, connection_};
  }

  void queue(std::vector<BackendOperation> operations,
             BackendBatch::Disposition disposition = BackendBatch::Disposition::Consumed) {
    responses_.push_back(Response{std::move(operations), disposition});
  }
  BackendStimulus last_stimulus() const { return last_; }
  int sends() const { return sends_; }

  BackendBatch send(const BackendStimulus &stimulus) override {
    last_ = stimulus;
    ++sends_;
    BackendBatch batch;
    batch.backend_session_id = session_;
    batch.binding = stimulus.binding;
    batch.stimulus_id = stimulus.stimulus_id;
    batch.disposition = BackendBatch::Disposition::Reject;
    if (!responses_.empty()) {
      batch.disposition = responses_.front().disposition;
      batch.operations = std::move(responses_.front().operations);
      responses_.erase(responses_.begin());
    }
    return batch;
  }

  std::uint64_t incarnation_ = 1;
  std::uint64_t connection_ = 1;
  bool healthy_ = true;
  int bind_calls_ = 0;
  int reset_calls_ = 0;

private:
  struct Response {
    std::vector<BackendOperation> operations;
    BackendBatch::Disposition disposition;
  };
  std::uint64_t session_;
  Binding bound_{};
  BackendStimulus last_{};
  int sends_ = 0;
  std::vector<Response> responses_;
};

KeyAction char_key(char ch) {
  KeyAction action;
  action.type = KeyAction::Type::AppendChar;
  action.ch = ch;
  action.keysym = static_cast<std::uint32_t>(ch);
  return action;
}

CandidatePage make_page(std::vector<CandidateEntry> entries, std::int32_t cursor, bool has_next,
                        bool has_prev = false) {
  CandidatePage page;
  page.entries = std::move(entries);
  page.cursor_index = cursor;
  page.has_next = has_next;
  page.has_prev = has_prev;
  return page;
}

// Page projection: a key dispatch that carries a candidate snapshot makes the
// page the displayed STATE, preserves placeholder indices, and an empty snapshot
// hides it.
void mode_page() {
  ScriptBackend backend;
  SemanticController controller(/*context_id=*/1, 64, backend);
  std::vector<CandidateEntry> entries = {entry("1", "你"), entry("", "", true), entry("2", "祢")};
  backend.queue({preedit("ni"), snapshot(make_page(entries, 0, true))});
  const auto decision = controller.begin_key(char_key('n'), true);
  CHECK(decision.route == SemanticDecision::Route::State);
  CHECK(decision.page.has_value());
  CHECK(decision.page->entries.size() == 3);
  CHECK(decision.page->entries[1].placeholder); // placeholder keeps index 1
  CHECK(controller.displayed_page().has_value());
  CHECK(controller.displayed_page()->entries.size() == 3);
  CHECK(controller.displayed_page()->cursor_index == 0);

  // A subsequent dispatch with an empty page is the authoritative hide signal.
  backend.queue({preedit(""), snapshot(CandidatePage{})});
  const auto hidden = controller.begin_key(char_key('i'), true);
  CHECK(hidden.page.has_value());
  CHECK(hidden.page->empty());
  CHECK(!controller.displayed_page().has_value()); // nothing to show or act on
}

// A select action commits and hides; it carries no key identity and its EFFECT
// is ordered after the preceding STATE.
void mode_select() {
  ScriptBackend backend;
  SemanticController controller(1, 64, backend);
  backend.queue(
      {preedit("ni"), snapshot(make_page({entry("1", "你"), entry("2", "祢")}, 1, true))});
  controller.begin_key(char_key('n'), true);
  const std::size_t records_after_state = controller.core().records().size();

  backend.queue({commit("你"), snapshot(CandidatePage{})});
  const auto decision = controller.begin_candidate_action(CandidateAction::Select, 0);
  CHECK(decision.route == SemanticDecision::Route::Effect);
  CHECK(decision.frontend.size() == 1);
  CHECK(decision.frontend[0].operation.type == BackendOperation::Type::CommitText);
  CHECK(decision.page.has_value() && decision.page->empty());
  CHECK(!controller.displayed_page().has_value()); // hidden after the projection update
  const BackendStimulus &stimulus = backend.last_stimulus();
  CHECK(stimulus.type == BackendStimulus::Type::SelectCandidate);
  CHECK(stimulus.candidate_index == 0);
  CHECK(stimulus.keysym == 0 && stimulus.modifiers == 0); // never a synthetic key
  const auto &records = controller.core().records();
  CHECK(records.size() == records_after_state + 1);
  CHECK(records.back().kind == Kind::Effect); // the commit is owned after the state
}

// Out-of-range and stale-fence refusals never dispatch a stimulus.
void mode_refuse() {
  {
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    backend.queue({preedit("ni"), snapshot(make_page({entry("1", "你")}, 0, false))});
    controller.begin_key(char_key('n'), true);
    const int sends_before = backend.sends();
    const auto decision = controller.begin_candidate_action(CandidateAction::Select, 7);
    CHECK(decision.route == SemanticDecision::Route::FailOpen);
    CHECK(backend.sends() == sends_before); // refused before dispatch
  }
  {
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    backend.queue({preedit("ni"), snapshot(make_page({entry("1", "你")}, 0, true))});
    controller.begin_key(char_key('n'), true);
    CHECK(controller.displayed_page().has_value());
    backend.incarnation_ = 2; // the link moved to a new Host incarnation
    const int sends_before = backend.sends();
    const auto decision = controller.begin_candidate_action(CandidateAction::Select, 0);
    CHECK(decision.route == SemanticDecision::Route::FailOpen);
    CHECK(!controller.displayed_page().has_value()); // stale page fenced, not resurrected
    CHECK(backend.sends() == sends_before);
  }
}

// Paging is a non-key action that replaces the projected page and resets the
// highlight to the new page's cursor.
void mode_paging() {
  ScriptBackend backend;
  SemanticController controller(1, 64, backend);
  backend.queue({preedit("nihao"), snapshot(make_page({entry("1", "你")}, 0, true))});
  controller.begin_key(char_key('n'), true);
  backend.queue({snapshot(make_page({entry("1", "泥"), entry("2", "倪")}, 0, false, true))});
  const auto decision = controller.begin_candidate_action(CandidateAction::PageNext, -1);
  CHECK(decision.route == SemanticDecision::Route::State ||
        decision.route == SemanticDecision::Route::Pass);
  CHECK(controller.displayed_page().has_value());
  CHECK(controller.displayed_page()->cursor_index == 0);
  CHECK(controller.displayed_page()->has_prev);
  CHECK(controller.displayed_page()->has_next == false);
  CHECK(backend.last_stimulus().type == BackendStimulus::Type::PageNext);
  CHECK(backend.last_stimulus().candidate_index == -1);
}

// F3 regression: when a transport/product bound truncates the page to a
// retained prefix, the cursor must never refer outside that prefix. A valid
// cursor is preserved; a removed cursor target becomes no-highlight (-1), never
// a different highlighted entry; the engine's own cursor is not touched.
void mode_truncation() {
  {
    CandidatePage page;
    page.entries = {entry("1", "你"), entry("2", "祢"), entry("3", "倪")};
    page.cursor_index = 1; // inside the retained prefix
    page.truncated = true;
    page.normalize_cursor_to_retained_prefix();
    CHECK(page.cursor_index == 1);
  }
  {
    CandidatePage page;
    page.entries = {entry("1", "你"), entry("2", "祢")};
    page.cursor_index = 2; // exactly at the truncation boundary: not retained
    page.truncated = true;
    page.normalize_cursor_to_retained_prefix();
    CHECK(page.cursor_index == -1);
  }
  {
    CandidatePage page;
    page.entries = {entry("1", "你")};
    page.cursor_index = 7; // beyond the retained prefix
    page.truncated = true;
    page.normalize_cursor_to_retained_prefix();
    CHECK(page.cursor_index == -1);
  }
  {
    CandidatePage page; // no cursor stays no-highlight
    page.entries = {entry("1", "你"), entry("2", "祢")};
    page.cursor_index = -1;
    page.normalize_cursor_to_retained_prefix();
    CHECK(page.cursor_index == -1);
  }
  {
    CandidatePage page; // empty page: the hide signal can carry no addressable cursor
    page.cursor_index = 0;
    page.normalize_cursor_to_retained_prefix();
    CHECK(page.empty());
    CHECK(page.cursor_index == -1);
  }
  // Pipeline level: the controller displays a normalized truncated page and a
  // still-valid cursor survives the projection unchanged.
  {
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    CandidatePage page = make_page({entry("1", "你"), entry("2", "祢"), entry("3", "倪")}, 2, true);
    page.truncated = true;
    page.normalize_cursor_to_retained_prefix();
    backend.queue({preedit("ni"), snapshot(std::move(page))});
    const auto decision = controller.begin_key(char_key('n'), true);
    CHECK(decision.route == SemanticDecision::Route::State);
    CHECK(controller.displayed_page().has_value());
    CHECK(controller.displayed_page()->truncated);
    CHECK(controller.displayed_page()->cursor_index == 2);
    CHECK(controller.displayed_page()->cursor_index <
          static_cast<std::int32_t>(controller.displayed_page()->entries.size()));
  }
}

// F2 regression (controller-side invariant): focus loss hides the candidate
// presentation in the TSF shell and that is the whole obligation — presentation
// only. The semantic controller receives no call, so context identity, Host
// binding, records and the projected page survive (focus != semantic
// incarnation, D072 §9); no engine event is invented. A normal dispatch after
// focus returns may re-present, and actions still fence on. A presentation
// failure (no active view for the anchor) is the same shape: hide, keep
// semantics, invent nothing.
void mode_focus() {
  ScriptBackend backend;
  SemanticController controller(1, 64, backend);
  backend.queue(
      {preedit("ni"), snapshot(make_page({entry("1", "你"), entry("2", "祢")}, 0, true))});
  CHECK(controller.begin_key(char_key('n'), true).route == SemanticDecision::Route::State);
  const std::uint64_t context_id = controller.context_id();
  const Binding binding = controller.core().binding();
  const std::size_t records = controller.core().records().size();
  const int sends = backend.sends();
  const int binds = backend.bind_calls_;
  const int resets = backend.reset_calls_;

  // Focus loss: the shell hides the popup; nothing reaches the controller.
  CHECK(controller.displayed_page().has_value()); // projected, just not shown
  CHECK(controller.context_id() == context_id);   // no semantic incarnation advance
  CHECK(controller.core().binding() == binding);  // Host context neither unbound nor rebound
  CHECK(controller.core().records().size() == records);
  CHECK(backend.sends() == sends); // no engine event invented by the transition
  CHECK(backend.bind_calls_ == binds && backend.reset_calls_ == resets);

  // Focus returns: a normal state/update re-presents and selection still works.
  backend.queue(
      {preedit("ni"), snapshot(make_page({entry("1", "你"), entry("2", "祢")}, 1, false))});
  const auto again = controller.begin_key(char_key('i'), true);
  CHECK(again.route == SemanticDecision::Route::State);
  CHECK(again.page.has_value());
  CHECK(controller.displayed_page()->cursor_index == 1);
  backend.queue({commit("祢"), snapshot(CandidatePage{})});
  const auto select = controller.begin_candidate_action(CandidateAction::Select, 1);
  CHECK(select.route == SemanticDecision::Route::Effect);
}

// Blocker 1: Fcitx byte cursor -> TSF UTF-16 caret. Never inside a code point,
// never between surrogate halves; out-of-range or malformed input is refused.
void mode_utf16() {
  const std::string ascii = "nihao";
  CHECK(utf16_offset_of_utf8_byte(ascii, 0) == std::size_t{0});
  CHECK(utf16_offset_of_utf8_byte(ascii, 2) == std::size_t{2});
  CHECK(utf16_offset_of_utf8_byte(ascii, 5) == std::size_t{5});
  CHECK(!utf16_offset_of_utf8_byte(ascii, 6));
  const std::string cjk = "你好"; // 2 x 3 bytes, 2 BMP units
  CHECK(utf16_offset_of_utf8_byte(cjk, 0) == std::size_t{0});
  CHECK(utf16_offset_of_utf8_byte(cjk, 3) == std::size_t{1});
  CHECK(utf16_offset_of_utf8_byte(cjk, 6) == std::size_t{2});
  CHECK(!utf16_offset_of_utf8_byte(cjk, 1)); // inside 你
  CHECK(!utf16_offset_of_utf8_byte(cjk, 4)); // inside 好
  const std::string mixed = "你hao";         // partial selection shape: selected hanzi + pinyin
  CHECK(utf16_offset_of_utf8_byte(mixed, 3) == std::size_t{1});
  CHECK(utf16_offset_of_utf8_byte(mixed, 4) == std::size_t{2});
  CHECK(utf16_offset_of_utf8_byte(mixed, 6) == std::size_t{4});
  const std::string astral = "a\xF0\x9F\x98\x80"
                             "b"; // U+1F600: 4 bytes, one surrogate pair
  CHECK(utf16_offset_of_utf8_byte(astral, 1) == std::size_t{1});
  CHECK(utf16_offset_of_utf8_byte(astral, 5) == std::size_t{3}); // after both halves
  CHECK(utf16_offset_of_utf8_byte(astral, 6) == std::size_t{4});
  for (std::size_t inside = 2; inside <= 4; ++inside)
    CHECK(!utf16_offset_of_utf8_byte(astral, inside));      // never between the halves
  CHECK(!utf16_offset_of_utf8_byte("\xC0\x80", 2));         // overlong
  CHECK(!utf16_offset_of_utf8_byte("\xED\xA0\x80", 3));     // UTF-16 surrogate in UTF-8
  CHECK(!utf16_offset_of_utf8_byte("\xF4\x90\x80\x80", 4)); // > U+10FFFF
  CHECK(!utf16_offset_of_utf8_byte("\xE4\xBD", 2));         // truncated

  // Whole-preedit mapping: segments become UTF-16 ranges, cursor becomes caret.
  std::vector<Utf16Segment> segments;
  std::optional<std::size_t> cursor;
  const FormattedPreedit begin{
      {{3, text_format::highlight | text_format::underline}, {3, text_format::underline}}, 0};
  CHECK(project_preedit_utf16(mixed, begin, segments, cursor));
  CHECK(segments.size() == 2);
  CHECK(segments[0].start == 0 && segments[0].end == 1);
  CHECK(segments[1].start == 1 && segments[1].end == 4);
  CHECK(cursor == std::size_t{0});
  const FormattedPreedit middle{{{6, text_format::underline}}, 4};
  CHECK(project_preedit_utf16(mixed, middle, segments, cursor) && cursor == std::size_t{2});
  const FormattedPreedit end{{{6, text_format::underline}}, 6};
  CHECK(project_preedit_utf16(mixed, end, segments, cursor) && cursor == std::size_t{4});
  const FormattedPreedit hidden{{{6, text_format::underline}}, -1};
  CHECK(project_preedit_utf16(mixed, hidden, segments, cursor) && !cursor);
  const FormattedPreedit split{{{1, 0}, {5, 0}}, -1}; // segment boundary inside 你
  CHECK(!project_preedit_utf16(mixed, split, segments, cursor));
  const FormattedPreedit short_tiling{{{3, 0}}, -1};
  CHECK(!project_preedit_utf16(mixed, short_tiling, segments, cursor));
  const FormattedPreedit mid_cursor{{{6, 0}}, 1};
  CHECK(!project_preedit_utf16(mixed, mid_cursor, segments, cursor));
}

// Blocker 1 at the controller: the preedit contract travels unchanged from the
// batch to the materialization queue, maps onto TSF styles/caret, and a
// projection that cannot map onto the text is refused (never re-interpreted).
void mode_preedit() {
  {
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    // Windows-default pinyin shape: HighLight|Underline up to the engine cursor,
    // Underline after it, embedded caret fixed at the beginning (cursor 0).
    const std::vector<TextSegment> segments = {{5, text_format::highlight | text_format::underline},
                                               {1, text_format::underline}};
    backend.queue({styled_preedit("ni hao", segments, 0),
                   snapshot(make_page({entry("1", "你好")}, 0, false))});
    const auto decision = controller.begin_key(char_key('o'), true);
    CHECK(decision.route == SemanticDecision::Route::State);
    CHECK(decision.frontend.size() == 1);
    const auto &op = decision.frontend[0].operation;
    CHECK(op.preedit.has_value());
    CHECK(op.preedit->cursor == 0);
    CHECK(op.preedit->segments.size() == 2);
    controller.enqueue_state(decision.frontend[0].key, op.text, op.preedit);
    CHECK(controller.pending().size() == 1);
    CHECK(controller.pending()[0].preedit.has_value());
    const auto layout =
        layout_preedit(controller.pending()[0].text, *controller.pending()[0].preedit);
    CHECK(layout.has_value());
    CHECK(layout->caret == std::size_t{0});
    CHECK(layout->ranges.size() == 2);
    CHECK(layout->ranges[0].style == PreeditStyle::Highlighted);
    CHECK(layout->ranges[0].start == 0 && layout->ranges[0].end == 5);
    CHECK(layout->ranges[1].style == PreeditStyle::Input);
    CHECK(layout->ranges[1].start == 5 && layout->ranges[1].end == 6);
  }
  {
    // Partial selection + continued composition: "你" selected, "hao" left, the
    // engine cursor after the selection; the mixed text maps by code point.
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    backend.queue({styled_preedit("你hao", {{6, text_format::underline}}, 3),
                   snapshot(make_page({entry("1", "好")}, 0, false))});
    const auto decision = controller.begin_key(char_key('h'), true);
    CHECK(decision.route == SemanticDecision::Route::State);
    const auto layout = layout_preedit(decision.frontend[0].operation.text,
                                       *decision.frontend[0].operation.preedit);
    CHECK(layout.has_value() && layout->caret == std::size_t{1});
    CHECK(layout->ranges.size() == 1 && layout->ranges[0].end == 4);
    backend.queue({commit("你好"), snapshot(CandidatePage{})});
    CHECK(controller.begin_candidate_action(CandidateAction::Select, 0).route ==
          SemanticDecision::Route::Effect);
  }
  // Format bits without a TSF display meaning carry no attribute.
  CHECK(preedit_style_for_format(0) == PreeditStyle::None);
  CHECK(preedit_style_for_format(text_format::bold | text_format::italic | text_format::strike |
                                 text_format::dont_commit) == PreeditStyle::None);
  CHECK(preedit_style_for_format(text_format::underline) == PreeditStyle::Input);
  CHECK(preedit_style_for_format(text_format::highlight) == PreeditStyle::Highlighted);
  {
    // A cursor inside a code point is a malformed batch: fail closed.
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    backend.queue({styled_preedit("你", {{3, text_format::underline}}, 1),
                   snapshot(make_page({entry("1", "你")}, 0, false))});
    const auto decision = controller.begin_key(char_key('n'), true);
    CHECK(decision.route == SemanticDecision::Route::CancelledRecovery);
    CHECK(!controller.displayed_page().has_value());
  }
  {
    // Plain-text producers (no projection) keep the pre-Stage-5 caret-at-end path.
    ScriptBackend backend;
    SemanticController controller(1, 64, backend);
    backend.queue({preedit("ni")});
    const auto decision = controller.begin_key(char_key('n'), true);
    CHECK(decision.route == SemanticDecision::Route::State);
    CHECK(!decision.frontend[0].operation.preedit.has_value());
  }
}

// Blocker 2: panel rows ride the same STATE snapshot; the presentation hides
// only when the whole displayed panel is empty, never merely because the
// candidate list is; candidate actions still need a candidate slot.
void mode_panel() {
  ScriptBackend backend;
  SemanticController controller(1, 64, backend);
  // Stroke filter shape: candidates plus the typed strokes in auxUp.
  CandidatePage stroke = make_page({entry("1", "你")}, 0, false);
  stroke.aux_up = "[Stroke Filtering]h";
  backend.queue({preedit("ni"), snapshot(stroke)});
  CHECK(controller.begin_key(char_key('`'), true).route == SemanticDecision::Route::State);
  CHECK(controller.displayed_page().has_value());
  CHECK(controller.displayed_page()->aux_up == "[Stroke Filtering]h");

  // QuickPhrase shape: auxUp only, no candidate, empty client preedit.
  CandidatePage quick;
  quick.aux_up = "Quick Phrase: ";
  CHECK(quick.empty() && !quick.panel_empty());
  backend.queue({preedit(""), snapshot(quick)});
  controller.begin_key(char_key(';'), true);
  CHECK(controller.displayed_page().has_value()); // stays visible without candidates
  CHECK(controller.displayed_page()->aux_up == "Quick Phrase: ");
  const int sends = backend.sends();
  CHECK(controller.begin_candidate_action(CandidateAction::Select, 0).route ==
        SemanticDecision::Route::FailOpen); // no candidate slot to act on
  CHECK(controller.begin_candidate_action(CandidateAction::PageNext, -1).route ==
        SemanticDecision::Route::FailOpen);
  CHECK(backend.sends() == sends);

  // auxDown alone and the server-side preedit row alone also keep it visible.
  CandidatePage lower;
  lower.aux_down = "down";
  CHECK(!lower.panel_empty());
  CandidatePage server;
  server.panel_preedit = "ni hao";
  CHECK(!server.panel_empty());
  // Placeholders only are nothing to display (classic-ui counts real slots).
  CandidatePage placeholders = make_page({entry("", "", true)}, -1, false);
  CHECK(placeholders.panel_empty());

  // The whole panel empty: the hide signal.
  backend.queue({preedit(""), snapshot(CandidatePage{})});
  controller.begin_key(char_key('x'), true);
  CHECK(!controller.displayed_page().has_value());
}

// Blocker 3: the comment is presented on its own slot; index alignment,
// placeholders, hit testing and the pager are unchanged by it.
void mode_comment() {
  const auto measure = [](const std::string &text) { return static_cast<int>(text.size()) * 8; };
  CandidateEntry first = entry("1", "你");
  first.comment = "(ni)";
  CandidateEntry second = entry("2", "你");
  second.comment = "(nǐ)";                 // same text, distinguished only by the engine comment
  CandidateEntry third = entry("3", "尼"); // empty comment: nothing appended
  CHECK(slot_full_text(first) == "1 你 (ni)");
  CHECK(slot_full_text(third) == "3 尼");
  CHECK(slot_main_text(second) == "2 你");

  for (const auto layout_kind : {CandidateLayout::Horizontal, CandidateLayout::Vertical}) {
    CandidatePage page = make_page({first, entry("", "", true), second, third}, 2, true);
    page.layout = layout_kind;
    page.aux_up = "[Stroke Filtering]";
    const auto layout = layout_candidate_panel(page, measure);
    CHECK(layout.slots.size() == 4);
    CHECK(!layout.upper.empty());
    CHECK(layout.slots[1].empty()); // placeholder keeps index 1, no slot
    for (const std::size_t index : {std::size_t{0}, std::size_t{2}, std::size_t{3}}) {
      const auto &slot = layout.slots[index];
      CHECK(!slot.empty());
      CHECK(slot.top >= layout.upper.bottom); // candidates below the panel rows
      // A click anywhere in the slot (text or comment part) resolves to it.
      const auto left = hit_test_candidate_panel(layout, slot.left + 1, slot.top + 1);
      const auto right = hit_test_candidate_panel(layout, slot.right - 1, slot.bottom - 1);
      CHECK(left && left->kind == PanelHit::Kind::Select &&
            left->index == static_cast<std::int32_t>(index));
      CHECK(right && right->kind == PanelHit::Kind::Select &&
            right->index == static_cast<std::int32_t>(index));
    }
    if (layout_kind == CandidateLayout::Horizontal) {
      // A slot is the text plus the inset it reserves on both sides, and the rectangle the
      // painter draws into is exactly the text's own width. Asserting the slot *is* the text
      // width is what the painting defect looked like from the layout's side: the painter
      // then started 2 px further in and every candidate that fit was ellipsized.
      CHECK(layout.text_inset > 0);
      CHECK(layout.slots[0].right - layout.slots[0].left ==
            measure(slot_full_text(first)) + 2 * layout.text_inset);
      CHECK(layout.slots[3].right - layout.slots[3].left ==
            measure(slot_full_text(third)) + 2 * layout.text_inset);
      CHECK(layout.text_rect(0).right - layout.text_rect(0).left == measure(slot_full_text(first)));
      CHECK(layout.text_rect(3).right - layout.text_rect(3).left == measure(slot_full_text(third)));
      CHECK(!layout.slot_clipped(0) && !layout.slot_clipped(3));
    }
    // Panel text rows are not actionable; the pager still is.
    CHECK(!hit_test_candidate_panel(layout, layout.upper.left + 1, layout.upper.top + 1));
    const auto next = hit_test_candidate_panel(layout, layout.next.left + 1, layout.next.top + 1);
    CHECK(next && next->kind == PanelHit::Kind::PageNext && next->index == -1);
    CHECK(layout.prev.empty()); // has_prev false: no arrow
  }

  // Controller level: the comment reaches the displayed page on the right index
  // and selecting that index dispatches exactly that index.
  ScriptBackend backend;
  SemanticController controller(1, 64, backend);
  backend.queue({preedit("ni"), snapshot(make_page({first, second}, 0, false))});
  controller.begin_key(char_key('n'), true);
  CHECK(controller.displayed_page()->entries[1].comment == "(nǐ)");
  backend.queue({commit("你"), snapshot(CandidatePage{})});
  controller.begin_candidate_action(CandidateAction::Select, 1);
  CHECK(backend.last_stimulus().candidate_index == 1);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: stage5_candidates "
                 "<page|select|refuse|paging|truncation|focus|utf16|preedit|panel|comment>\n");
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "page")
    mode_page();
  else if (mode == "select")
    mode_select();
  else if (mode == "refuse")
    mode_refuse();
  else if (mode == "paging")
    mode_paging();
  else if (mode == "truncation")
    mode_truncation();
  else if (mode == "focus")
    mode_focus();
  else if (mode == "utf16")
    mode_utf16();
  else if (mode == "preedit")
    mode_preedit();
  else if (mode == "panel")
    mode_panel();
  else if (mode == "comment")
    mode_comment();
  else {
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
  }
  std::printf("stage5_candidates %s: PASS\n", mode.c_str());
  return 0;
}
