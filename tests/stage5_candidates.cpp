// Stage 5 candidate projection + action tests (platform-free controller logic).
//
// These drive the real SemanticController real-frontend path against a
// deterministic scripted Backend double. They assert the projection/fencing
// decisions only: candidate page STATE, placeholder index alignment, the
// select-to-EFFECT ordered settlement, paging highlight, stale-page refusal after
// an incarnation move, out-of-range refusal, truncated-page cursor
// normalization, and the focus-loss/presentation-failure invariants (hide only,
// semantics intact). The Win32 presentation surface (src/tsf/candidate_view.cpp)
// is exercised by the real product-path smoke, not here; nothing in this file
// needs Fcitx, TSF, or windows.h.

#include "fusion/backend.hpp"
#include "mock_backend.hpp"
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

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: stage5_candidates <page|select|refuse|paging|truncation|focus>\n");
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
  else {
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
  }
  std::printf("stage5_candidates %s: PASS\n", mode.c_str());
  return 0;
}
