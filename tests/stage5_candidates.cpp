// Stage 5 candidate projection + action tests (platform-free controller logic).
//
// These drive the real SemanticController real-frontend path against a
// deterministic scripted Backend double. They assert the projection/fencing
// decisions only: candidate page STATE, placeholder index alignment, the
// select-to-EFFECT ordered settlement, paging highlight, stale-page refusal after
// an incarnation move, and out-of-range refusal. The Win32 presentation surface
// (src/tsf/candidate_view.cpp) is exercised by the real product-path smoke, not
// here; nothing in this file needs Fcitx, TSF, or windows.h.

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
  BackendOperation operation{BackendOperation::Type::CandidateSnapshot};
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

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: stage5_candidates <page|select|refuse|paging>\n");
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
  else {
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
  }
  std::printf("stage5_candidates %s: PASS\n", mode.c_str());
  return 0;
}
