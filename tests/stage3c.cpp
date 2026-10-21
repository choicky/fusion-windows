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
    // R36: the owner's exact sequence — type, commit the candidate with Space, then
    // Enter with nothing pending. The commit clears the client preedit projection, so
    // the engine has no work for Return and would only forward it; the shell must
    // therefore not claim the key in OnTestKeyDown, because an application that honours
    // only that callback (Notepad--) drops the key. Earlier checks never covered an idle
    // Return *after* a commit: they forced an external termination first, which clears
    // the projection by itself and would hide exactly this precondition.
    type(controller, doc, "nihao");
    auto commit = controller.begin_key(translate_key(kVkSpace, false, false, false), true);
    require(commit.eaten && !commit.forward && !commit.frontend.empty() &&
                commit.frontend.front().operation.type == BackendOperation::Type::CommitText &&
                commit.frontend.front().operation.text == "你好",
            "Space did not commit the candidate");
    doc.apply(controller, commit);
    require(controller.finish_pass(), "commit barrier");
    require(controller.raw_buffer().empty(), "commit clears the client preedit projection");
    require(!controller.engine_has_work_for(translate_key(kVkReturn, false, false, false)),
            "idle Return after a commit is not engine work, so the shell must not claim it");
    require(controller.routes_key(translate_key(kVkReturn, false, false, false)),
            "Return is still T-routable while the engine is ready");
    auto idle_return = controller.begin_key(translate_key(kVkReturn, false, false, false), true);
    std::cout << "idle Return forward=" << (idle_return.forward ? 1 : 0)
              << " route=" << static_cast<int>(idle_return.route)
              << " ops=" << idle_return.frontend.size() << '\n';
    require(idle_return.forward && idle_return.frontend.empty() &&
                idle_return.route == SemanticDecision::Route::Pass,
            "idle Return must only be forwarded to the application");
    doc.apply(controller, idle_return);
    require(controller.finish_pass(), "idle Return Forward barrier");
    // R37: the same shape for the editing keys. After a commit the engine has no work
    // for them — pinyin handles them only inside its non-empty input branch — so it
    // answers them with ForwardKey only. The oracle here is that engine answer, not the
    // fixed expression: the key must come back as the application's own key, and only
    // then must T stop claiming it. WeChat reported exactly these keys after a commit
    // while they worked with the TIP unloaded (owner report, 2026-10-22), which is what
    // an overstated T claim looks like in an application that honours only that callback.
    // Up/Down are measured here rather than assumed: the owner reported "the arrows", and
    // the same engine branch decides them.
    for (const auto &[vk, name] : {std::pair{0x08u, "Backspace"}, {0x2eu, "Delete"},
                                   {0x25u, "Left"}, {0x27u, "Right"}, {0x26u, "Up"},
                                   {0x28u, "Down"}, {0x24u, "Home"}, {0x23u, "End"}}) {
      const auto idle_key = translate_key(vk, false, false, false);
      require(controller.routes_key(idle_key), "editing key is no longer an engine key class");
      const auto idle = controller.begin_key(idle_key, true);
      std::cout << "idle " << name << " forward=" << (idle.forward ? 1 : 0)
                << " route=" << static_cast<int>(idle.route)
                << " ops=" << idle.frontend.size() << '\n';
      require(idle.forward && !idle.eaten && idle.frontend.empty() &&
                  idle.route == SemanticDecision::Route::Pass,
              "the engine did not hand an idle editing key back to the application");
      require(controller.finish_pass(), "idle editing key Forward barrier");
      require(!controller.engine_has_work_for(idle_key),
              "OnTestKeyDown would claim an editing key the engine only forwards");
    }
    // A held key arrives as further key-downs of the same key, each with its own T/K
    // pair: every repeat must reach the application on its own, not only the first.
    for (int repeat = 0; repeat < 3; ++repeat) {
      const auto held = controller.begin_key(translate_key(kVkBack, false, false, false), true);
      require(held.forward && !held.eaten && held.frontend.empty(),
              "a repeated idle Backspace did not reach the application");
      require(controller.finish_pass(), "repeated idle Backspace Forward barrier");
    }
    type(controller, doc, "nihao");
    // While composing, the same keys stay engine work: the engine deletes, moves and
    // navigates the composition with them.
    for (const auto &[vk, name] : {std::pair{0x08u, "Backspace"}, {0x2eu, "Delete"},
                                   {0x25u, "Left"}, {0x27u, "Right"}, {0x26u, "Up"},
                                   {0x28u, "Down"}, {0x24u, "Home"}, {0x23u, "End"}}) {
      (void)name;
      require(controller.engine_has_work_for(translate_key(vk, false, false, false)),
              "composing editing key left the engine, so the shell would stop claiming it");
    }
    auto back = controller.begin_key(translate_key(kVkBack, false, false, false), true);
    require(back.eaten && !back.frontend.empty(), "real Backspace");
    doc.apply(controller, back);
    auto ret = controller.begin_key(translate_key(kVkReturn, false, false, false), true);
    require(controller.engine_has_work_for(translate_key(kVkReturn, false, false, false)),
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
    // R37 control, the other half of the same rule: a configured Fcitx global hotkey is
    // engine work with **nothing** composing, so the shell must keep claiming it. F6 is
    // installed as this isolated instance's own "toggle input method" hotkey
    // (Hotkey/TriggerKeys) — a global action this runtime has two methods for, so the
    // engine really acts on it. The composition is committed first, so no composition, no
    // candidate page and a clear pass barrier are in place when F6 arrives. Asserting
    // only routes_key would prove nothing about the path — the point is that the engine
    // acts on the key and that T lets it through.
    {
      auto hotkey_backend = make_fcitx_backend(FcitxBackendOptions{false, "F6"});
      require(hotkey_backend != nullptr, "global hotkey fixture backend");
      SemanticController hotkey(952, 64, *hotkey_backend);
      Document hotkey_doc;
      type(hotkey, hotkey_doc, "nihao");
      auto hotkey_commit = hotkey.begin_key(translate_key(kVkSpace, false, false, false), true);
      require(hotkey_commit.eaten && !hotkey_commit.frontend.empty(), "fixture commit");
      hotkey_doc.apply(hotkey, hotkey_commit);
      require(hotkey.finish_pass(), "fixture commit barrier");
      require(hotkey.raw_buffer().empty(), "fixture commit left a projection");
      const auto f6 = translate_key(0x75, false, false, false); // VK_F6
      require(f6.keysym == 0xffc3 && !f6.modified, "F6 translation");
      require(hotkey.routes_key(f6), "a configured global hotkey is still an engine key class");
      require(hotkey.engine_has_work_for(f6),
              "T must keep claiming a configured global hotkey with nothing composing");
      const auto global = hotkey.begin_key(f6, true);
      std::cout << "idle F6 eaten=" << (global.eaten ? 1 : 0)
                << " forward=" << (global.forward ? 1 : 0)
                << " route=" << static_cast<int>(global.route)
                << " ops=" << global.frontend.size() << '\n';
      require(global.eaten && !global.forward && global.frontend.empty(),
              "the engine did not handle the configured global hotkey");
      require(hotkey.finish_pass(), "global hotkey barrier");
      hotkey.destroy();
      std::cout << "PASS configured global hotkey reached the engine with nothing composing\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
