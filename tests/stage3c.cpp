#include "fusion/backend.hpp"
#include "tsf/semantic.hpp"
#include <crtdbg.h>
#include <iostream>
#include <stdexcept>
using namespace fusion;
using namespace fusion::tsf;
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Document {
  std::string committed, preedit;
  void apply(SemanticController &controller, const SemanticDecision &decision) {
    for (const auto &work : decision.frontend) {
      std::cout << "callback " << static_cast<int>(work.operation.type) << " ["
                << work.operation.text << "] seq=" << work.key.sequence << '\n';
      if (work.operation.type == BackendOperation::Type::SetPreedit) {
        preedit = work.operation.text;
        require(controller.settle_state(work.key, EditResult::AppliedReadback) == Result::Accepted,
                "ordered STATE settlement");
      } else {
        auto ticket = controller.open_effect(work.key);
        require(ticket.has_value(), "ordered EFFECT ticket");
        committed += work.operation.text;
        preedit.clear();
        require(controller.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
                "ordered EFFECT settlement");
        require(controller.settle_effect(*ticket, EditResult::AppliedReadback) != Result::Accepted,
                "duplicate EFFECT accepted");
      }
    }
  }
};
KeyAction physical(char character, bool shift = false) {
  return translate_key(character == ','   ? 0xbc
                       : character == '@' ? '2'
                       : character == '/' ? 0xbf
                       : character == '.' ? 0xbe
                       : character == ':' ? 0xba
                                          : static_cast<unsigned char>(character),
                       false, false, false, character, shift);
}
void type(SemanticController &controller, Document &doc, const std::string &text) {
  for (char c : text) {
    auto d = controller.begin_key(physical(c), true);
    require(d.eaten && d.backend_session_id &&
                d.route != SemanticDecision::Route::CancelledRecovery,
            "Pinyin key did not reach Fcitx");
    doc.apply(controller, d);
  }
}
// Focused boundary fixture: immutable callbacks, refusal, uncertainty and Forward.
class CallbackBackend final : public Backend {
public:
  BackendBatch next;
  bool bind(Binding) override { return true; }
  bool reset(Binding) override { return true; }
  void unbind(Binding) override {}
  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return 42; }
  BackendBatch send(const BackendStimulus &input) override {
    auto batch = next;
    batch.backend_session_id = 42;
    batch.binding = input.binding;
    batch.stimulus_id = input.stimulus_id;
    return batch;
  }
};
void fallback() {
  CallbackBackend backend;
  backend.next.disposition = BackendBatch::Disposition::Consumed;
  backend.next.operations = {{BackendOperation::Type::CommitText, "first"},
                             {BackendOperation::Type::SetPreedit, ""},
                             {BackendOperation::Type::CommitText, "second"}};
  SemanticController controller(950, 64, backend);
  auto d = controller.begin_key(physical(','), true);
  require(d.frontend.size() == 3, "fixture callbacks lost");
  for (const auto &work : d.frontend) {
    if (work.operation.type == BackendOperation::Type::CommitText)
      controller.enqueue_effect(work.key, work.operation.text);
    else
      controller.enqueue_state(work.key, work.operation.text);
  }
  require(!controller.finish_pass(), "Forward overtook queued effects");
  auto first = controller.open_effect(d.frontend[0].key);
  require(first.has_value(), "first effect ticket");
  require(controller.settle_effect(*first, EditResult::RefusedNoMutation) == Result::Accepted,
          "refusal accounting");
  require(controller.pending().size() == 3 && !controller.state_applicable(d.frontend[1].key) &&
              !controller.open_effect(d.frontend[2].key),
          "refusal reordered callbacks");
  first = controller.open_effect(d.frontend[0].key);
  require(first.has_value(), "known no-mutation refusal did not permit retry");
  controller.settle_effect(*first, EditResult::AppliedReadback);
  require(controller.state_applicable(d.frontend[1].key), "ordered preedit blocked");
  controller.settle_front_state(d.frontend[1].key, EditResult::RefusedNoMutation);
  require(controller.pending().size() == 2 && !controller.open_effect(d.frontend[2].key),
          "preedit refusal dropped ordering watermark");
  controller.settle_front_state(d.frontend[1].key, EditResult::AppliedReadback);
  auto last = controller.open_effect(d.frontend[2].key);
  require(last.has_value(), "last effect missing after consumed preedit");
  controller.settle_effect(*last, EditResult::Indeterminate);
  require(!controller.open_effect(d.frontend[2].key) && !controller.finish_pass(),
          "indeterminate retry or Forward");
  controller.destroy();
  SemanticController forward(951, 64, backend);
  backend.next.operations = {{BackendOperation::Type::CommitText, "before-forward"},
                             {BackendOperation::Type::ForwardKey, "Tab", {}, true}};
  auto f = forward.begin_key(translate_key(9, false, false, false), true);
  require(f.forward && !f.eaten && f.frontend.size() == 1 && !forward.finish_pass(),
          "original forward callback not preserved behind effect");
  Document doc;
  doc.apply(forward, f);
  require(forward.finish_pass(), "original forward did not settle");
  backend.next.operations[1].original_key = false;
  auto unsupported = forward.begin_key(physical('x'), true);
  require(unsupported.route == SemanticDecision::Route::CancelledRecovery && unsupported.eaten,
          "non-original forward silently replayed");
  forward.destroy();
  std::cout << "PASS callback fallback, watermark, uncertainty and original Forward contract\n";
}
int main() {
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  try {
    fallback();
    auto backend = make_fcitx_backend();
    SemanticController controller(901, 64, *backend);
    Document doc;
    type(controller, doc, "nihao");
    require(!doc.preedit.empty(), "nihao preedit");
    auto comma = physical(',');
    require(controller.routes_key(comma), "comma excluded at T eligibility");
    auto result = controller.begin_key(comma, true);
    require(result.eaten && !result.forward && result.frontend.size() == 3,
            "comma did not produce three ordered callbacks");
    require(result.frontend[0].operation.type == BackendOperation::Type::CommitText &&
                result.frontend[0].operation.text == "你好" &&
                result.frontend[1].operation.type == BackendOperation::Type::SetPreedit &&
                result.frontend[1].operation.text.empty() &&
                result.frontend[2].operation.type == BackendOperation::Type::CommitText &&
                result.frontend[2].operation.text == "，",
            "comma callback oracle");
    require(!controller.open_effect(result.frontend[2].key), "later commit overtook first");
    doc.apply(controller, result);
    require(doc.committed == "你好，" && doc.preedit.empty(), "comma final text");
    std::cout << "PASS real nihao, -> 你好，; callback order and duplicate fencing\n";
    // A genuine physical Tab is returned naturally only after all prior effects.
    auto tab = translate_key(9, false, false, false);
    require(controller.routes_key(tab), "Tab excluded before real dispatch");
    auto forward = controller.begin_key(tab, true);
    require(forward.backend_session_id && forward.forward && !forward.eaten,
            "Fcitx did not determine genuine Tab Forward");
    doc.apply(controller, forward);
    require(controller.finish_pass(), "Forward did not settle prior effects");
    std::cout << "PASS genuine Tab Forward; no owned key or replay\n";
    for (const char c : std::string("/@.:")) {
      controller.note_composition_terminated();
      auto action = physical(c, c == '@' || c == ':');
      require(controller.routes_key(action), "ordinary punctuation excluded");
      auto d = controller.begin_key(action, true);
      require(d.backend_session_id && d.route != SemanticDecision::Route::CancelledRecovery &&
                  d.route != SemanticDecision::Route::FailOpen,
              "ordinary punctuation not dispatched");
      std::cout << "key " << c << " disposition=" << (d.forward ? "Forward" : "Consumed") << '\n';
      doc.apply(controller, d);
      require(!d.forward || controller.finish_pass(), "punctuation Forward barrier");
    }
    controller.note_composition_terminated();
    // Idle Return: no composition, nothing pending. The engine must not consume the
    // physical key, so it passes to the application (the Notepad-- symptom is that it
    // does not arrive). This is the one Return case the earlier checks never covered.
    auto idle_return = controller.begin_key(translate_key(kVkReturn, false, false, false), true);
    std::cout << "idle Return forward=" << (idle_return.forward ? 1 : 0)
              << " route=" << static_cast<int>(idle_return.route)
              << " ops=" << idle_return.frontend.size() << '\n';
    require(!controller.return_has_engine_work(),
            "idle Return is not engine work, so the shell must not claim it");
    require(idle_return.forward && idle_return.route == SemanticDecision::Route::Pass,
            "idle Return must pass to the application");
    doc.apply(controller, idle_return);
    require(controller.finish_pass(), "idle Return Forward barrier");
    type(controller, doc, "nihao");
    auto back = controller.begin_key(translate_key(kVkBack, false, false, false), true);
    require(back.eaten && !back.frontend.empty(), "real Backspace");
    doc.apply(controller, back);
    auto ret = controller.begin_key(translate_key(kVkReturn, false, false, false), true);
    require(controller.return_has_engine_work(),
            "composing Return stays engine work, so the shell claims it");
    require(ret.backend_session_id && ret.route != SemanticDecision::Route::CancelledRecovery,
            "real Return composition");
    doc.apply(controller, ret);
    controller.note_composition_terminated();
    type(controller, doc, "nihao");
    auto raw_return = controller.begin_key(translate_key(kVkReturn, false, false, false), true);
    require(!raw_return.forward && !raw_return.frontend.empty() &&
                raw_return.frontend.front().operation.type == BackendOperation::Type::CommitText &&
                raw_return.frontend.front().operation.text == "nihao",
            "nihao Return mature raw commit");
    doc.apply(controller, raw_return);
    auto unsupported = controller.begin_key(classify_key('C', true, false, false), true);
    require(!unsupported.eaten && !unsupported.backend_session_id &&
                unsupported.route == SemanticDecision::Route::FailOpen,
            "system modifier swallowed");
    auto denied = controller.begin_key(physical(','), false);
    require(!denied.backend_session_id && !denied.eaten, "hard policy dispatched");
    controller.destroy();
    std::cout << "PASS real Backspace/Return and hard-policy gate\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
