#include "fusion/backend.hpp"
#include "tsf/semantic.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace fusion;
using namespace fusion::tsf;
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
namespace {
// Every section that completes prints exactly one `PASS` line through this function, and
// the count is checked on the way out. A section that never runs is therefore a failure
// rather than a shorter log: an earlier round of this file silently returned after five of
// its sections and still exited 0, which made "88/88 and unchanged oracles" look like
// coverage it did not have.
constexpr int kExpectedSections = 29;
int sections_completed = 0;
bool failure_reported = false;

void section(std::string line) {
  ++sections_completed;
  std::cout << line << '\n';
}

// A destructor runs on every exit path, including an early `return` from main; a check at the
// end of main does not.
struct SectionCoverage {
  ~SectionCoverage() {
    if (failure_reported || sections_completed == kExpectedSections)
      return;
    std::cout.flush();
    std::cerr << "FAIL stage3c completed " << sections_completed << " of " << kExpectedSections
              << " sections; a scheduled section did not run\n";
    std::_Exit(1);
  }
};
} // namespace
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
  section("PASS callback fallback, watermark, uncertainty and original Forward contract");
}

// R51: a backend that counts the resets it is asked for. "Exactly once" is the whole promise
// of the deferred reset, and a count is the only way to read it instead of inferring it.
class CountingBackend final : public Backend {
public:
  int reset_calls = 0;
  Binding last_reset{};
  bool bind(Binding) override { return true; }
  bool reset(Binding binding) override {
    ++reset_calls;
    last_reset = binding;
    return true;
  }
  void unbind(Binding) override {}
  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return 42; }
  BackendBatch send(const BackendStimulus &input) override {
    BackendBatch batch;
    batch.disposition = BackendBatch::Disposition::Consumed;
    batch.backend_session_id = 42;
    batch.binding = input.binding;
    batch.stimulus_id = input.stimulus_id;
    batch.operations = {{BackendOperation::Type::SetPreedit, "n"}};
    return batch;
  }
};

// A composition ended by the application rather than by a key. The shell may not reset the
// Host inside the TSF callback that reported it — `Backend::reset` is a blocking round trip
// and that callback holds an edit cookie — so the callback only arms the intent, and the
// first boundary outside the session runs it. Both orders are checked, because the shell has
// no way to know which one a real application will produce.
void reset_after_external_termination() {
  {
    // The order the owner hit: focus left the document and this context never received another
    // key. Nothing but a boundary can reset it, and only one boundary may.
    CountingBackend backend;
    SemanticController controller(1201, 64, backend);
    Document doc;
    type(controller, doc, "n");
    require(backend.reset_calls == 0, "an ordinary key reset the Host context");
    controller.note_composition_terminated();
    require(controller.backend_reset_pending(), "the termination armed no reset intent");
    require(backend.reset_calls == 0, "the termination reset the Host from inside the callback");
    const auto flush = controller.flush_backend_reset(ResetSource::Terminated);
    require(flush.issued && flush.ok, "the deferred reset did not run");
    require(backend.reset_calls == 1 && backend.last_reset.context == 1201,
            "the deferred reset was not exactly one reset of this context");
    require(!controller.backend_reset_pending(), "the reset left the intent armed");
    // Every later boundary for the same context sees an empty intent and issues nothing: focus
    // -out, pop, teardown and deactivation all run on the way out of the one event.
    const auto again = controller.flush_backend_reset(ResetSource::FocusOut);
    require(!again.issued && backend.reset_calls == 1, "a second boundary reset it again");
    controller.destroy();
    section("PASS R51 a composition terminated by the application resets the Host exactly once");
  }
  {
    // The other order: a key arrives before any boundary. The key path is a flush like any
    // other, so the boundary that follows must not add a second reset.
    CountingBackend backend;
    SemanticController controller(1202, 64, backend);
    Document doc;
    type(controller, doc, "n");
    controller.note_composition_terminated();
    require(controller.backend_reset_pending(), "the termination armed no reset intent");
    const auto decision = controller.begin_key(physical('h'), true);
    doc.apply(controller, decision);
    require(backend.reset_calls == 1 && backend.last_reset.context == 1202,
            "the key that arrived first did not run the armed reset");
    require(!controller.backend_reset_pending(), "the key path left the intent armed");
    const auto later = controller.flush_backend_reset(ResetSource::Pop);
    require(!later.issued && backend.reset_calls == 1, "the key and the boundary reset it twice");
    section("PASS R51 a key that arrives first runs the armed reset and the boundary adds none");
  }
}
int main() {
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  SectionCoverage coverage;
  try {
    fallback();
    reset_after_external_termination();
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
    section("PASS real nihao, -> 你好，; callback order and duplicate fencing");
    // A genuine physical Tab is returned naturally only after all prior effects.
    auto tab = translate_key(9, false, false, false);
    require(controller.routes_key(tab), "Tab excluded before real dispatch");
    auto forward = controller.begin_key(tab, true);
    require(forward.backend_session_id && forward.forward && !forward.eaten,
            "Fcitx did not determine genuine Tab Forward");
    doc.apply(controller, forward);
    require(controller.finish_pass(), "Forward did not settle prior effects");
    section("PASS genuine Tab Forward; no owned key or replay");
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
    section("PASS real Backspace/Return and hard-policy gate");
    // R37 control, the other half of the same rule, as R53 changed it: a configured Fcitx
    // global hotkey is handled by the engine with **nothing** composing, and the shell used to
    // keep claiming the key it was bound to. R53 rule 2 stops claiming the function row while
    // idle, because Explorer — which honours OnTestKeyDown alone — then loses F2 and F5, and
    // the shell cannot tell a bound F6 from an unbound one without asking Fcitx. What is
    // still asserted is the other half: F6 is installed as this isolated instance's own
    // "toggle input method" hotkey (Hotkey/TriggerKeys) and the engine really does act on it
    // when the key reaches it, so the stopgap costs a user who binds one of these keys that
    // binding in the applications that route the key from T, not the engine's ability to act.
    // The full key matrix measures what replaces the rule.
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
      require(!hotkey.engine_has_work_for(f6),
              "R53 rule 2: T no longer claims an idle function key, even one Fcitx has bound");
      const auto global = hotkey.begin_key(f6, true);
      std::cout << "idle F6 eaten=" << (global.eaten ? 1 : 0)
                << " forward=" << (global.forward ? 1 : 0)
                << " route=" << static_cast<int>(global.route)
                << " ops=" << global.frontend.size() << '\n';
      require(global.eaten && !global.forward && global.frontend.empty(),
              "the engine did not handle the configured global hotkey");
      require(hotkey.finish_pass(), "global hotkey barrier");
      // F6 turned the input method off. The same key has to turn it back on: a global key is
      // handled before any input method and does not need an active context, so the inactive
      // state must still dispatch character-less keys and let the engine decide. The letter in
      // between is claimed under the mode the F6 batch reported (on), the engine hands it back,
      // and the shell delivers it once — R53's first-key fact reached through a different
      // toggle, so it is asserted the same way.
      const auto committed_before_english = hotkey_doc.committed;
      auto english = hotkey.begin_key(physical('n'), true);
      std::cout << "F6 off letter forward=" << (english.forward ? 1 : 0)
                << " eaten=" << (english.eaten ? 1 : 0) << " ops=" << english.frontend.size()
                << '\n';
      require(!english.forward && english.eaten && english.frontend.size() == 1 &&
                  english.frontend.front().operation.type == BackendOperation::Type::CommitText &&
                  english.frontend.front().operation.text == "n",
              "the letter after the F6 toggle was not delivered exactly once");
      hotkey_doc.apply(hotkey, english);
      require(hotkey_doc.committed == committed_before_english + "n",
              "the letter after the F6 toggle did not appear exactly once");
      const auto reactivate = hotkey.begin_key(f6, true);
      std::cout << "inactive F6 eaten=" << (reactivate.eaten ? 1 : 0)
                << " forward=" << (reactivate.forward ? 1 : 0)
                << " route=" << static_cast<int>(reactivate.route) << '\n';
      require(reactivate.eaten && !reactivate.forward,
              "the configured hotkey did not reach the engine while inactive");
      auto resumed = hotkey.begin_key(physical('n'), true);
      require(!resumed.frontend.empty(), "the hotkey did not turn the input method back on");
      hotkey.destroy();
      section("PASS configured global hotkey reached the engine with nothing composing");
      section("PASS the same global hotkey turns the input method back on");
    }

    // R39: a lone left Shift is the engine's own "temporarily switch input method"
    // binding (Hotkey/AltTriggerKeys, Shift_L by default). The shell relays the press and
    // its release with the key's own identity and keeps no state; what is asserted here is
    // the engine's answer, not a re-expression of the implementation.
    {
      const auto left_shift = translate_key(kVkLShift, false, false, false);
      require(left_shift.keysym == kKeysymShiftL && !left_shift.modified, "left Shift identity");
      require(translate_key(0x10, false, false, false, 0, false, false, kScanShiftRight).keysym ==
                  kKeysymShiftR,
              "the generic VK_SHIFT is told apart by its scan code");
      require(!translate_key(0x11 /*VK_CONTROL*/, false, false, false).keysym,
              "an unrelated modifier keeps no keysym");
      // A bare Shift is admitted by T, because an application that consults T skips K after
      // a FALSE, and K is where the press reaches the engine.
      require(SemanticController(901, 64, *make_fcitx_backend(FcitxBackendOptions{}))
                  .engine_has_work_for(left_shift),
              "the shell would not deliver a Shift press to the engine");

      const auto compose = [](SemanticController &c, Document &d) {
        for (const char *ch = "nihao"; *ch; ++ch)
          d.apply(c, c.begin_key(physical(*ch), true));
        return d.preedit;
      };
      const auto press_shift = [](SemanticController &c, std::uint32_t vk, std::uint32_t scan) {
        auto action = translate_key(vk, false, false, false, 0, false, false, scan);
        const auto down = c.begin_key(action, true);
        require(down.forward && down.frontend.empty(),
                "the engine did not hand the Shift press back to the application");
        action.release = true;
        return action;
      };

      const auto run_shape = [&](const char *label, std::vector<std::string> methods,
                                 const char *def, const char *scheme, std::uint64_t context) {
        FcitxBackendOptions options;
        options.fixture_input_methods = std::move(methods);
        options.fixture_default_method = def;
        auto shift_backend = make_fcitx_backend(options);
        SemanticController c(context, 64, *shift_backend);
        Document d;
        require(compose(c, d) == scheme, label);
        section(std::string(label) + " chinese=[" + d.preedit + "]");

        // One lone press: the engine settles its own binding on the release, which ends
        // the composition, and the next key is the application's with nothing composed.
        auto release = press_shift(c, kVkLShift, kScanShiftLeft);
        require(c.routes_release(release), "a lone Shift release is relayed");
        const auto toggled = c.release_key(release);
        section(std::string(label) + " toggle ops=" + std::to_string(toggled.frontend.size()) +
                " eaten=" + std::to_string(toggled.eaten ? 1 : 0));
        d.apply(c, toggled);
        require(d.preedit.empty(), "the engine did not end the composition on the toggle");
        auto english = c.begin_key(physical('n'), true);
        section(std::string(label) + " english route=" +
                std::to_string(static_cast<int>(english.route)) +
                " forward=" + std::to_string(english.forward ? 1 : 0) +
                " ops=" + std::to_string(english.frontend.size()));
        // R53 stopgap: the first key after the toggle is claimed under the mode the toggle's
        // own batch reported (on), and the engine — now off — hands it back, so the shell
        // delivers it once instead of dropping it in the applications that route the key from T.
        const auto committed_before_english = d.committed;
        require(!english.forward && english.eaten && english.frontend.size() == 1 &&
                    english.frontend.front().operation.type ==
                        BackendOperation::Type::CommitText &&
                    english.frontend.front().operation.text == "n",
                "the first English key after the toggle was not delivered exactly once");
        d.apply(c, english);
        require(d.committed == committed_before_english + "n",
                "the first English key after the toggle did not appear exactly once");
        // While inactive the context resolves Items/0 — the other Pinyin scheme — and a
        // character-less key is still dispatched, because that is how a configured global
        // hotkey turns the input method back on. With nothing composing the off slot hands
        // the key back unchanged, so it composes nothing.
        const auto arrow = c.begin_key(translate_key(0x25 /*VK_LEFT*/, false, false, false), true);
        require(arrow.forward && arrow.frontend.empty(),
                "an inactive character-less key was composed through the off slot");
        require(!c.routes_release(release), "a release with no relayed press is not ours");

        // And back: the same press returns to the same input method, so the same keys
        // compose to the same scheme.
        auto release_back = press_shift(c, kVkLShift, kScanShiftLeft);
        require(c.routes_release(release_back), "the returning Shift release is relayed");
        d.apply(c, c.release_key(release_back));
        require(compose(c, d) == scheme, "the toggle did not return to the same scheme");
        section(std::string(label) + " back=[" + d.preedit + "]");
        c.destroy();
      };

      // The first-run shape (Pinyin in use) and a saved single-method profile (Shuangpin
      // only), which is the shape the owner's own profile has.
      run_shape("PASS lone Shift: pinyin", {}, "", "ni hao", 960);
      run_shape("PASS lone Shift: shuangpin only", {"shuangpin"}, "shuangpin", "ni ha o", 961);

      // Shift+letter is a combination, not a lone press: the letter is the application's
      // own key (a modified key is never routed), and it ends the press, so the release
      // must not settle a modifier-only binding.
      {
        auto shift_backend = make_fcitx_backend(FcitxBackendOptions{});
        SemanticController c(962, 64, *shift_backend);
        Document d;
        compose(c, d);
        c.begin_key(left_shift, true);
        // Shift+letter is ordinary text input: the letter is engine text, and its
        // key-down is the fact that ends the lone-press window.
        const auto letter = translate_key('a', false, false, false, 'A', true, false);
        require(c.routes_key(letter), "Shift+letter is engine text");
        c.begin_key(letter, true);
        auto release = left_shift;
        release.release = true;
        require(!c.routes_release(release), "a Shift used with another key is not a lone press");
        require(c.release_key(release).frontend.empty(), "the combination produced callbacks");
        auto next = c.begin_key(physical('n'), true);
        require(!next.frontend.empty(), "the combination toggled instead of typing");
        section("PASS Shift+letter does not toggle");
        c.destroy();
      }

      // The right Shift is a different key and AltTriggerKeys names Shift_L: it must not
      // toggle, and it must never be presented as the left one.
      {
        auto shift_backend = make_fcitx_backend(FcitxBackendOptions{});
        SemanticController c(963, 64, *shift_backend);
        Document d;
        compose(c, d);
        auto release = press_shift(c, kVkRShift, kScanShiftRight);
        require(release.keysym == kKeysymShiftR, "right Shift identity");
        require(c.routes_release(release), "the right Shift release is relayed");
        d.apply(c, c.release_key(release));
        require(!compose(c, d).empty(), "the right Shift toggled a binding it does not name");
        section("PASS right Shift leaves the binding alone");
        c.destroy();
      }

      // A held Shift repeats key-downs; that is still one press.
      {
        auto shift_backend = make_fcitx_backend(FcitxBackendOptions{});
        SemanticController c(964, 64, *shift_backend);
        Document d;
        compose(c, d);
        for (int i = 0; i < 3; ++i)
          c.begin_key(left_shift, true);
        auto release = left_shift;
        release.release = true;
        require(c.routes_release(release), "a repeated press still settles");
        d.apply(c, c.release_key(release));
        require(!c.routes_release(release), "a repeated release is not relayed twice");
        auto english = c.begin_key(physical('n'), true);
        // One toggle means the engine is off, so the first key is handed back and delivered
        // once; three toggles would leave it on and compose instead. The shape below is what
        // separates those two, not the old "forwarded with nothing delivered".
        require(!english.forward && english.eaten && english.frontend.size() == 1 &&
                    english.frontend.front().operation.type ==
                        BackendOperation::Type::CommitText &&
                    english.frontend.front().operation.text == "n",
                "repeated Shift presses toggled more than once");
        section("PASS repeated Shift press toggles once");
        c.destroy();
      }

      // A release settles only a press the engine actually received. A T-only observation
      // (the application consulted T and never delivered K), a press the hard policy
      // refused, and a second release of the same press must all leave the binding alone.
      {
        auto shift_backend = make_fcitx_backend(FcitxBackendOptions{});
        SemanticController c(970, 64, *shift_backend);
        Document d;
        compose(c, d);
        auto release = left_shift;
        release.release = true;
        c.observe_key_down(left_shift); // observed, never forwarded
        require(!c.routes_release(release), "an observed-only press armed a release");
        require(c.release_key(release).frontend.empty(), "an observed-only press settled");

        const auto refused = c.begin_key(left_shift, false); // hard policy says no
        require(refused.route == SemanticDecision::Route::FailOpen, "a refused press dispatched");
        require(!c.routes_release(release), "a policy-refused press armed a release");

        require(c.begin_key(left_shift, true).forward, "the admitted press is not the engine's");
        require(c.routes_release(release), "an admitted press did not arm its release");
        require(c.release_admissible(release), "an admitted release was refused admission");
        const auto first = c.release_key(release);
        require(!first.frontend.empty(), "the admitted press did not settle");
        require(!c.routes_release(release), "one press produced two releases");
        require(c.release_key(release).frontend.empty(), "a repeated release settled again");

        // The same release refused by the *admission* path rather than by routing: a pass
        // barrier is still up, so the reply could not be settled in order. `routes_release`
        // is true and `release_admissible` is false — the exact disagreement T and K used to
        // have, where T claimed a key-up K then dropped and the application never saw its own
        // modifier release. `release_admissible` is what OnTestKeyUp returns, so this is T's
        // answer, and `release_key` agrees with it.
        {
          auto barrier_backend = make_fcitx_backend(FcitxBackendOptions{});
          SemanticController gated(971, 64, *barrier_backend);
          Document gated_doc;
          compose(gated, gated_doc);
          // An unsettled EFFECT is what puts the pass barrier up (Core::probe_pass answers
          // DrainRequired while one is pending), and it is a state a release really arrives
          // in: the engine's commit is still being written when the key-up comes.
          const auto gated_commit =
              gated.begin_key(translate_key(kVkSpace, false, false, false), true);
          require(!gated_commit.frontend.empty(), "the gated commit produced no work");
          require(gated.pass_probe() != Pass::Clear, "the gated commit left no barrier");
          require(gated.begin_key(left_shift, true).forward, "the gated press was not forwarded");
          auto held = left_shift;
          held.release = true;
          require(gated.routes_release(held), "the gated press did not arm a release");
          require(!gated.release_admissible(held),
                  "a release was admitted while the pass barrier was up");
          require(gated.release_key(held).frontend.empty(), "a deferred release still settled");
          // The same release once that work is settled is admitted, so the check above turns
          // on the barrier and not on something else.
          gated_doc.apply(gated, gated_commit);
          require(gated.pass_probe() == Pass::Clear, "the barrier did not clear");
          require(gated.release_admissible(held), "a release was refused with a clear barrier");
          gated.destroy();
        }
        section("PASS only a forwarded press arms its release");
      }


      // The configured key decides: with AltTriggerKeys moved to Shift_R the left Shift
      // does nothing and the right one toggles.
      {
        FcitxBackendOptions options;
        options.fixture_alt_trigger_key = "Shift_R";
        auto shift_backend = make_fcitx_backend(options);
        SemanticController c(965, 64, *shift_backend);
        Document d;
        compose(c, d);
        auto left_release = press_shift(c, kVkLShift, kScanShiftLeft);
        d.apply(c, c.release_key(left_release));
        require(!compose(c, d).empty(), "the shell ignored the configured binding");
        auto right_release = press_shift(c, kVkRShift, kScanShiftRight);
        d.apply(c, c.release_key(right_release));
        auto english = c.begin_key(physical('n'), true);
        require(!english.forward && english.eaten && english.frontend.size() == 1 &&
                    english.frontend.front().operation.type ==
                        BackendOperation::Type::CommitText &&
                    english.frontend.front().operation.text == "n",
                "the configured Shift_R binding did not toggle");
        section("PASS the configured modifier-only key decides");
        c.destroy();
      }
    }

    // R38 correction: the inactive passthrough must be decided per context, not from
    // Instance::state() / mostRecentInputContext(). Two bound contexts on one backend: A
    // toggles to English, then B is bound and typed into (bind() calls Instance::activate()
    // which flips the whole instance's state and makes B the mostRecent). A's character
    // key must still be forwarded because A's own per-context InputState is still inactive
    // and Fcitx's per-context Instance::inputMethod(A) still resolves to Items/0. On the
    // pre-fix code the predicate saw mostRecent != A and returned false, so A's key
    // reached keyEvent and composed with Items/0 — this require is the discriminator.
    // Every earlier section here used a single context, which is how this escaped.
    {
      auto two_backend = make_fcitx_backend(FcitxBackendOptions{});
      require(two_backend != nullptr, "two-context backend");
      SemanticController a(980, 64, *two_backend);
      Document da;
      type(a, da, "nihao");
      require(!da.preedit.empty(), "A did not compose in Chinese before the toggle");
      const auto a_release = [&] {
        auto action = translate_key(kVkLShift, false, false, false, 0, false, false, kScanShiftLeft);
        (void)a.begin_key(action, true);
        action.release = true;
        return action;
      }();
      require(a.routes_release(a_release), "A's Shift release is relayed");
      da.apply(a, a.release_key(a_release));
      require(da.preedit.empty(), "the toggle did not end A's composition");
      const auto a_english = a.begin_key(physical('n'), true);
      require(!a_english.forward && a_english.eaten && a_english.frontend.size() == 1 &&
                  a_english.frontend.front().operation.text == "n",
              "A did not pass through into English before B was bound");
      // Bind B and type into it. Its InputState is active; the instance's mostRecent is B.
      SemanticController b(981, 64, *two_backend);
      Document db;
      type(b, db, "nihao");
      require(!db.preedit.empty(), "B never composed after being bound over an inactive A");
      // Discriminating case: back to A. MostRecent is B, but A must still be English.
      const auto a_again = a.begin_key(physical('n'), true);
      require(a_again.forward && a_again.frontend.empty(),
              "A's character key composed after B became most recent — the predicate must "
              "read the dispatching context's own state, not mostRecentInputContext()");
      // B is unaffected throughout: it stays on its own scheme.
      const auto b_again = b.begin_key(physical('i'), true);
      require(!b_again.frontend.empty(), "B lost its active composition when A was typed into");
      // A's own Shift press returns it to its original scheme; the toggle is still per-context.
      const auto a_back_release = [&] {
        auto action = translate_key(kVkLShift, false, false, false, 0, false, false, kScanShiftLeft);
        (void)a.begin_key(action, true);
        action.release = true;
        return action;
      }();
      require(a.routes_release(a_back_release), "A's returning Shift release is relayed");
      da.apply(a, a.release_key(a_back_release));
      const auto a_reactivated = a.begin_key(physical('n'), true);
      require(!a_reactivated.frontend.empty(), "the returning Shift did not reactivate A");
      b.destroy();
      a.destroy();
      section("PASS the inactive passthrough follows the dispatching context, not mostRecent");
    }
    // The same discriminator with a configured Hotkey/TriggerKeys (F6) fixture: A toggled
    // off through F6, B bound and used, then F6 on A returns A to active. Proves the fix
    // does not affect the global-hotkey path.
    {
      FcitxBackendOptions options;
      options.fixture_global_hotkey = "F6";
      auto f6_backend = make_fcitx_backend(std::move(options));
      require(f6_backend != nullptr, "F6 two-context backend");
      SemanticController a(990, 64, *f6_backend);
      Document da;
      type(a, da, "nihao");
      // Commit first, so F6 arrives with nothing composing, exactly as the single-context
      // global-hotkey section proves the engine handles it.
      auto f6_commit = a.begin_key(translate_key(kVkSpace, false, false, false), true);
      require(f6_commit.eaten && !f6_commit.frontend.empty(), "F6 fixture commit");
      da.apply(a, f6_commit);
      require(a.finish_pass(), "F6 fixture commit barrier");
      const auto f6 = translate_key(0x75, false, false, false); // VK_F6 -> 0xffc3
      const auto off = a.begin_key(f6, true);
      require(off.eaten && !off.forward && off.frontend.empty(), "the F6 fixture did not turn A off");
      require(a.finish_pass(), "global hotkey barrier");
      const auto a_english = a.begin_key(physical('n'), true);
      require(!a_english.forward && a_english.eaten && a_english.frontend.size() == 1 &&
                  a_english.frontend.front().operation.text == "n",
              "A stayed composing instead of passing through after F6");
      SemanticController b(991, 64, *f6_backend);
      Document db;
      type(b, db, "nihao");
      // Discriminator: back to A, a character key must still be forwarded. On the pre-fix
      // code this composed because mostRecent was B and state() reflected B's active state.
      const auto a_again = a.begin_key(physical('n'), true);
      require(a_again.forward && a_again.frontend.empty(),
              "after B was bound A's character key reached the engine on the F6 fixture");
      // F6 still re-activates A with B having been typed into.
      const auto a_reactivate = a.begin_key(f6, true);
      require(a_reactivate.eaten && !a_reactivate.forward,
              "the configured global hotkey did not reach the engine while A was inactive");
      const auto a_resumed = a.begin_key(physical('n'), true);
      require(!a_resumed.frontend.empty(), "the F6 fixture did not turn A back on after B was used");
      b.destroy();
      a.destroy();
      section("PASS the configured global hotkey reactivates A after B became most recent");
    }

    // R44: the number keys. Measured on the pinned engine: with nothing composing a digit
    // comes back as ForwardKey — there is nothing to select — so the shell must stop
    // claiming it, because an application that honours OnTestKeyDown alone drops a key the
    // claim cannot retract (the shape R36 and R37 fixed for Return and the editing keys).
    // With a page on screen the same key selects, on the top row and on the keypad alike.
    // Its own engine instance, so the selections this section makes cannot move the
    // candidates the sections above assert.
    {
      auto digits_backend = make_fcitx_backend();
      SemanticController digits(953, 64, *digits_backend);
      Document digits_doc;
      const auto number = [](std::uint32_t vk, char ch) {
        return translate_key(vk, false, false, false, ch);
      };
      type(digits, digits_doc, "nihao");
      auto number_commit = digits.begin_key(translate_key(kVkSpace, false, false, false), true);
      require(number_commit.eaten && !number_commit.frontend.empty(), "number-key section commit");
      digits_doc.apply(digits, number_commit);
      require(digits.finish_pass(), "number-key commit barrier");
      for (const auto &[vk, name] :
           {std::pair<std::uint32_t, const char *>{'3', "top-row 3"},
            std::pair<std::uint32_t, const char *>{0x63, "keypad 3"}}) {
        const auto idle = number(vk, '3');
        require(digits.routes_key(idle), "a digit is still a T-routable key class");
        require(!digits.engine_has_work_for(idle),
                "an idle digit is not engine work, so T must not claim it");
        const auto handed_back = digits.begin_key(idle, true);
        std::cout << "idle " << name << " forward=" << (handed_back.forward ? 1 : 0)
                  << " route=" << static_cast<int>(handed_back.route) << '\n';
        require(handed_back.forward && !handed_back.eaten && handed_back.frontend.empty(),
                "the engine did not hand an idle digit back to the application");
        require(digits.finish_pass(), "idle digit barrier");
      }
      // The gate reads the translated keysym, so the punctuation Shift puts on the same
      // physical keys is not a candidate digit: with nothing composing it keeps the routing
      // it had before R44 and still reaches the engine.
      const auto check_punctuation = [&](std::uint32_t vk, char ch, const char *name) {
        const auto shifted = translate_key(vk, false, false, false, ch, true);
        const bool engine_work = digits.engine_has_work_for(shifted);
        std::cout << name << " keysym=" << static_cast<char>(shifted.keysym)
                  << " engine_work=" << (engine_work ? 1 : 0) << '\n';
        require(engine_work, "shifted punctuation was gated as if it were a number key");
        const auto handed = digits.begin_key(shifted, true);
        require(handed.eaten && !handed.forward && !handed.frontend.empty(),
                "the engine did not take the punctuation Shift produced");
        digits_doc.apply(digits, handed);
        require(digits.finish_pass(), "punctuation barrier");
      };
      check_punctuation('1', '!', "Shift+1");
      check_punctuation('9', '(', "Shift+9");
      check_punctuation('0', ')', "Shift+0");
      type(digits, digits_doc, "nihao");
      require(digits.displayed_page().has_value() &&
                  digits.displayed_page()->entries.size() > 3,
              "the composing page a number key selects from");
      const auto keypad = number(0x63, '3');
      const auto top_row = number('3', '3');
      require(digits.engine_has_work_for(top_row) && digits.engine_has_work_for(keypad),
              "a number key is engine work while a page is on screen, top row and keypad");
      const auto keypad_selected = digits.begin_key(keypad, true);
      std::cout << "composing keypad 3 eaten=" << (keypad_selected.eaten ? 1 : 0)
                << " ops=" << keypad_selected.frontend.size() << '\n';
      require(keypad_selected.eaten && !keypad_selected.forward &&
                  !keypad_selected.frontend.empty(),
              "a keypad number no longer selects the candidate the page shows");
      digits_doc.apply(digits, keypad_selected);
      (void)digits.finish_pass();
      type(digits, digits_doc, "nihao");
      const auto top_row_selected = digits.begin_key(top_row, true);
      std::cout << "composing 3 eaten=" << (top_row_selected.eaten ? 1 : 0)
                << " ops=" << top_row_selected.frontend.size() << '\n';
      require(top_row_selected.eaten && !top_row_selected.forward &&
                  !top_row_selected.frontend.empty(),
              "a number key no longer selects the candidate the page shows");
      digits_doc.apply(digits, top_row_selected);
      (void)digits.finish_pass();
      (void)digits.begin_key(translate_key(kVkEscape, false, false, false), true);
      (void)digits.finish_pass();
      digits.destroy();
      section("PASS number keys: an idle digit is the application's, a page's own number "
              "still selects on the top row and on the keypad");
    }
    // R47: the printable symbols. The digit gate above is a prediction about what the engine
    // will do with a key, and for punctuation that prediction cannot be made in the shell:
    // whether "," becomes "，" is the engine's punctuation configuration, not a property of
    // the key. So the shell claims the key and, when the engine declines it, delivers the
    // character the engine declined — which is what the Linux frontend's Forward amounts to
    // on a platform where a T claim cannot be retracted (R36/R37/R44).
    {
      const HKL us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
      require(us != nullptr, "US keyboard layout for the symbol table");
      const char printable[] =
          " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
      const auto symbol_key = [&](char ch) {
        const SHORT scan = VkKeyScanExW(static_cast<WCHAR>(ch), us);
        require(scan != -1, "the layout has no key for a printable ASCII character");
        return translate_key(static_cast<std::uint32_t>(scan & 0xff), false, false, false,
                             static_cast<char32_t>(ch), ((scan >> 8) & 1) != 0);
      };
      // Every case below is the FIRST key of a freshly bound context: a bound context is
      // already active (the Host activates it when it binds), so nothing is warmed up and no
      // letter is typed first — the case the owner hit when a field had just been focused.

      // (a) Translation, for every printable ASCII character: the layout's own VK and shift,
      // the character Windows produces for them, and the keysym the shell derives. The live
      // path makes the same ToUnicodeEx call with the live keyboard state; pinning the
      // layout here keeps the table identical on every machine.
      for (const char *c = printable; *c; ++c) {
        const SHORT scan = VkKeyScanExW(static_cast<WCHAR>(*c), us);
        require(scan != -1, "layout mapping for a printable ASCII character");
        const auto vk = static_cast<std::uint32_t>(scan & 0xff);
        BYTE state[256]{};
        if ((scan >> 8) & 1)
          state[VK_SHIFT] = 0x80;
        wchar_t text[8]{};
        const int count = ToUnicodeEx(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, us), state, text, 8,
                                      0, us);
        require(count == 1 && text[0] == static_cast<WCHAR>(*c),
                "the layout produced a different character than the table expects");
        require(symbol_key(*c).keysym == static_cast<unsigned char>(*c),
                "translate_key lost the character the layout produced");
      }
      // (b) Nothing printable is lost. Every printable ASCII symbol, idle: the key is claimed
      // in T, so either the engine handles it or the shell delivers it; the application is
      // never left with a key that has been taken away from it.
      for (const char *c = printable; *c; ++c) {
        if (*c >= '0' && *c <= '9')
          continue; // the digit gate leaves these to the application (R44)
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(970, 64, *symbol_backend);
        Document symbol_doc;
        const auto action = symbol_key(*c);
        require(symbols.routes_key(action), "a printable symbol is a T-routable key class");
        auto d = symbols.begin_key(action, true);
        if (d.forward) {
          std::cout << "lost '" << *c << "'\n";
          require(false, "a printable symbol was neither handled nor delivered");
        }
        symbol_doc.apply(symbols, d);
        (void)symbols.finish_pass();
        symbols.destroy();
      }
      // (c) The symbols the owner reported, delivered as themselves, from the top row and
      // from the keypad alike. These are the keys the engine declines with nothing
      // composing: it has no punctuation entry for them.
      const struct {
        const char *name;
        KeyAction action;
        const char *expected;
      } delivered[] = {
          {"/", symbol_key('/'), "/"},         {"-", symbol_key('-'), "-"},
          {"+", symbol_key('+'), "+"},         {"=", symbol_key('='), "="},
          {"&", symbol_key('&'), "&"},         {"*", symbol_key('*'), "*"},
          {"|", symbol_key('|'), "|"},         {"@", symbol_key('@'), "@"},
          {"keypad /", translate_key(0x6f, false, false, false, U'/'), "/"},
          {"keypad *", translate_key(0x6a, false, false, false, U'*'), "*"},
          {"keypad -", translate_key(0x6d, false, false, false, U'-'), "-"},
          {"keypad +", translate_key(0x6b, false, false, false, U'+'), "+"},
      };
      for (const auto &row : delivered) {
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(971, 64, *symbol_backend);
        Document symbol_doc;
        auto d = symbols.begin_key(row.action, true);
        symbol_doc.apply(symbols, d);
        (void)symbols.finish_pass();
        std::cout << "delivered " << row.name << "=[" << symbol_doc.committed << "]\n";
        require(d.eaten && !d.forward && symbol_doc.committed == row.expected,
                "a declined symbol was not delivered as itself");
        symbols.destroy();
      }
      // (d) The engine still owns the punctuation it maps: the same key produces the
      // official full-width character, and the shell adds nothing to it.
      const struct {
        char key;
        const char *expected;
      } punctuated[] = {{',', "，"}, {'.', "。"}, {'?', "？"}, {':', "："}, {'(', "（"}};
      for (const auto &row : punctuated) {
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(972, 64, *symbol_backend);
        Document symbol_doc;
        auto d = symbols.begin_key(symbol_key(row.key), true);
        symbol_doc.apply(symbols, d);
        (void)symbols.finish_pass();
        std::cout << "punctuated " << row.key << "=[" << symbol_doc.committed << "]\n";
        require(d.eaten && symbol_doc.committed == row.expected,
                "the engine no longer converts punctuation the way it did");
        symbols.destroy();
      }
      // (e) Composing: a declined symbol commits the candidate first and then delivers
      // itself, in that order — the engine's own commit is not disturbed.
      {
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(973, 64, *symbol_backend);
        Document symbol_doc;
        type(symbols, symbol_doc, "nihao");
        auto d = symbols.begin_key(symbol_key('-'), true);
        std::cout << "composing '-' ops=" << d.frontend.size() << " committed=[" << symbol_doc.committed
                  << "]\n";
        symbol_doc.apply(symbols, d);
        (void)symbols.finish_pass();
        require(d.eaten && symbol_doc.committed == "你好-",
                "a declined symbol broke the composition it followed");
        require(symbols.raw_buffer().empty(), "a declined symbol left a projection");
        symbols.destroy();
      }
      // (f) 中文标点 switched off. The switch is the punctuation module's own toggle key
      // (Ctrl+. by default) — the user's control, not a shell rule — and what it changes is
      // the engine's answer for the same key: a comma becomes the engine's Forward, which is
      // exactly the case (c) proves the shell delivers. The engine is asked directly here
      // because the shell never dispatches a modified key (routes_key refuses them).
      {
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(975, 64, *symbol_backend);
        Document symbol_doc;
        (void)symbols.begin_key(physical('n'), true); // binds the context, starts composing
        (void)symbols.finish_pass();
        (void)symbols.begin_key(translate_key(kVkEscape, false, false, false), true);
        (void)symbols.finish_pass();
        const Binding binding{975, 1, 1};
        const auto ask = [&](std::uint64_t id, std::uint32_t keysym, std::uint32_t modifiers) {
          BackendStimulus stimulus{binding, id};
          stimulus.type = BackendStimulus::Type::Character;
          stimulus.keysym = keysym;
          stimulus.modifiers = modifiers;
          return symbol_backend->send(stimulus);
        };
        const auto toggled = ask(3, '.', 4); // fcitx KeyState::Ctrl is bit 2
        require(toggled.disposition != BackendBatch::Disposition::Reject,
                "the punctuation toggle key was refused");
        const auto plain_comma = ask(4, ',', 0);
        std::cout << "punctuation off: comma consumed="
                  << (plain_comma.disposition == BackendBatch::Disposition::Consumed ? 1 : 0)
                  << '\n';
        require(plain_comma.disposition == BackendBatch::Disposition::Forward,
                "with punctuation off the engine still converted a comma");
        symbols.destroy();
      }
      // (g) Latin-1: translate_key encodes every character below 0x100 as the keysym itself, so
      // these keys carry 0xe9 / 0xa3 rather than an ASCII byte. The digit-row case is the one
      // that used to go wrong: its raw-input character is the unshifted "2", and falling back
      // to it delivered "2" instead of "é".
      const struct {
        const char *name;
        KeyAction action;
        const char *expected;
      } latin1[] = {
          // A digit-row key whose translated character is é: AppendChar keeps ch = '2'.
          {"digit row é", translate_key('2', false, false, false, U'\u00e9', true), "\xc3\xa9"},
          // A symbol key with no raw-input character at all: Pass type, ch = 0, keysym 0xa3.
          {"symbol £", translate_key(0xbf, false, false, false, U'\u00a3'), "\xc2\xa3"},
      };
      for (const auto &row : latin1) {
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(977, 64, *symbol_backend);
        Document symbol_doc;
        auto d = symbols.begin_key(row.action, true);
        symbol_doc.apply(symbols, d);
        (void)symbols.finish_pass();
        std::cout << "latin1 " << row.name << " keysym=0x" << std::hex << row.action.keysym
                  << std::dec << "=[" << symbol_doc.committed << "]\n";
        require(d.eaten && symbol_doc.committed == row.expected,
                "a forwarded Latin-1 character was not delivered as itself");
        symbols.destroy();
      }
      // (h) The first key of a freshly bound context. A bound context is already active — the
      // Host activates it when it binds — so a symbol pressed into a field that was just
      // focused must be delivered like any other. Nothing is typed first here on purpose.
      for (const char first : {'/', '@'}) {
        auto symbol_backend = make_fcitx_backend();
        SemanticController symbols(978, 64, *symbol_backend);
        Document symbol_doc;
        auto d = symbols.begin_key(symbol_key(first), true);
        symbol_doc.apply(symbols, d);
        (void)symbols.finish_pass();
        std::cout << "first key '" << first << "' eaten=" << (d.eaten ? 1 : 0)
                  << " forward=" << (d.forward ? 1 : 0) << "=[" << symbol_doc.committed << "]\n";
        require(d.eaten && !d.forward && symbol_doc.committed == std::string(1, first),
                "the first key of a new context was not delivered");
        symbols.destroy();
      }
      section("PASS symbols: every printable ASCII key reaches the document, the engine keeps "
              "the punctuation it maps, and a declined symbol is delivered by the shell");
    }

    // R53 rule 2: the character-less keys the owner reported lost in Explorer — F2 renames
    // nothing, F5 refreshes nothing — measured here against the pinned engine with nothing
    // composing, in its default configuration, reached by a real composition and a real
    // commit so that "idle" is the engine's own state and not the absence of any state. Each
    // line prints the engine's answer for the key and then the shell's new T answer for it.
    {
      auto idle_backend = make_fcitx_backend();
      SemanticController idle_keys(980, 64, *idle_backend);
      Document idle_doc;
      type(idle_keys, idle_doc, "nihao");
      const auto idle_commit =
          idle_keys.begin_key(translate_key(kVkSpace, false, false, false), true);
      require(idle_commit.eaten && !idle_commit.frontend.empty(), "R53 idle fixture commit");
      idle_doc.apply(idle_keys, idle_commit);
      require(idle_keys.finish_pass(), "R53 idle fixture commit barrier");
      require(idle_keys.raw_buffer().empty() && !idle_keys.displayed_page().has_value() &&
                  idle_keys.pass_probe() == fusion::Pass::Clear,
              "the R53 idle case is not actually idle");
      const std::pair<std::uint32_t, const char *> function_row[] = {
          {0x70, "F1"}, {0x71, "F2"}, {0x74, "F5"}, {0x7b, "F12"}, {0x87, "F24"}};
      for (const auto &[vk, name] : function_row) {
        const auto action = translate_key(vk, false, false, false);
        const bool claimed = idle_keys.engine_has_work_for(action);
        std::cout << "idle " << name << " keysym=0x" << std::hex << action.keysym << std::dec
                  << " eligible=" << (claimed ? 1 : 0) << '\n';
        require(!claimed, "T still claims an idle function key the engine hands back");
        const auto handed = idle_keys.begin_key(action, true);
        std::cout << "idle " << name << " forward=" << (handed.forward ? 1 : 0)
                  << " eaten=" << (handed.eaten ? 1 : 0)
                  << " route=" << static_cast<int>(handed.route)
                  << " ops=" << handed.frontend.size() << '\n';
        require(handed.forward && !handed.eaten && handed.frontend.empty() &&
                    handed.route == SemanticDecision::Route::Pass,
                "the pinned engine did not hand this idle key back to the application");
        require(idle_keys.finish_pass(), "idle R53 key barrier");
      }
      // The rest of rule 2's set carries no keysym in this shell's table, so the shell has
      // never presented one of them to the engine: T refused them before the rule and refusing
      // them again changes nothing on the product path. Measured, not assumed.
      const std::pair<std::uint32_t, const char *> never_presented[] = {
          {0x5d, "Apps"},        {0x2c, "PrintScreen"}, {0x13, "Pause"},
          {0xa6, "BrowserBack"}, {0xad, "VolumeDown"},  {0xb3, "MediaPlayPause"},
          {0xb7, "LaunchApp2"}};
      for (const auto &[vk, name] : never_presented) {
        const auto action = translate_key(vk, false, false, false);
        std::cout << "idle " << name << " keysym=0x" << std::hex << action.keysym << std::dec
                  << " presented=" << (idle_keys.routes_key(action) ? 1 : 0)
                  << " eligible=" << (idle_keys.engine_has_work_for(action) ? 1 : 0) << '\n';
        require(!idle_keys.routes_key(action),
                "this shell presents an idle character-less key to the engine");
        require(!idle_keys.engine_has_work_for(action),
                "T claims an idle character-less key it never sends");
      }
      // Asked of the engine directly with their own X11 keysyms — the only way these keys reach
      // it, and the shape the shell would send them in — the idle engine hands each one back,
      // so its default configuration binds none of them. This is the evidence rule 2 rests on
      // for keys the shell cannot otherwise present. A context of its own, because a dispatch
      // that bypasses the controller advances the engine's stimulus ordering and no later key
      // of this controller would be accepted.
      {
        auto ask_backend = make_fcitx_backend();
        SemanticController asked_keys(982, 64, *ask_backend);
        // One forwarded key binds the context, and binding is what activates it: after this
        // line the engine is in its on state with nothing composing, which is the state rule 2
        // is about.
        const auto bind_key = asked_keys.begin_key(translate_key(0x70, false, false, false), true);
        (void)asked_keys.finish_pass();
        std::cout << "ask context bound raw_empty=" << (asked_keys.raw_buffer().empty() ? 1 : 0)
                  << " page=" << (asked_keys.displayed_page().has_value() ? 1 : 0)
                  << " forward=" << (bind_key.forward ? 1 : 0) << '\n';
        require(asked_keys.raw_buffer().empty() && !asked_keys.displayed_page().has_value(),
                "the direct-probe fixture is not idle");
        const std::pair<std::uint32_t, const char *> engine_asked[] = {
            {0xff13, "Pause"}, {0xff14, "ScrollLock"}, {0xff61, "Print"}, {0xff67, "Menu"}};
        const Binding asked_binding{982, 1, 1};
        std::uint64_t asked = 1000;
        for (const auto &[keysym, name] : engine_asked) {
          BackendStimulus probe{asked_binding, ++asked};
          probe.type = BackendStimulus::Type::Return;
          probe.keysym = keysym;
          const auto answer = ask_backend->send(probe);
          std::cout << "idle engine " << name << " handed_back="
                    << (answer.disposition == BackendBatch::Disposition::Forward ? 1 : 0) << '\n';
          require(answer.disposition == BackendBatch::Disposition::Forward,
                  "the idle engine took this character-less key for itself");
        }
        asked_keys.destroy();
      }
      // ScrollLock is the key this rule got wrong the first time, and the correction is the case
      // worth keeping. `VK_SCROLL` is **0x91** in winuser.h; 0x45 is the letter E. The earlier
      // note read the two as one key, so rule 2 skipped ScrollLock for a collision that does not
      // exist. The engine's own answer for it is in the probe above — `{0xff14, "ScrollLock"}`
      // hands the key back — so the rule reaches it, and the letter E keeps the admission it
      // always had: a stopgap does not trade a key the owner types for one nobody presses.
      const auto scroll_lock = translate_key(kVkScroll, false, false, false);
      const auto letter_e = translate_key(0x45, false, false, false, U'e');
      std::cout << "idle scroll-lock vk=0x" << std::hex << scroll_lock.virtual_key << std::dec
                << " type=" << static_cast<int>(scroll_lock.type)
                << " eligible=" << (idle_keys.engine_has_work_for(scroll_lock) ? 1 : 0)
                << " letter-e-eligible=" << (idle_keys.engine_has_work_for(letter_e) ? 1 : 0)
                << '\n';
      require(scroll_lock.virtual_key == 0x91 && scroll_lock.type == KeyAction::Type::Pass,
              "VK_SCROLL is not 0x91, or it no longer classifies as a character-less key");
      require(!idle_keys.engine_has_work_for(scroll_lock),
              "rule 2 still claims ScrollLock while the engine is idle");
      require(idle_keys.engine_has_work_for(letter_e), "rule 2 refused the letter E");
      type(idle_keys, idle_doc, "e");
      require(!idle_doc.preedit.empty(), "the letter E no longer starts a composition");
      std::cout << "idle E preedit=[" << idle_doc.preedit << "]\n";
      // The ordered records this composition produced have to be settled before the next one:
      // the Cancel is a decision like any other, and its callbacks are the shell's to apply.
      const auto cancel_e =
          idle_keys.begin_key(translate_key(kVkEscape, false, false, false), true);
      idle_doc.apply(idle_keys, cancel_e);
      require(idle_keys.finish_pass(), "idle E barrier");
      // Composing: the same keys keep exactly the answer they had before the rule, because a
      // composition may have bound one of them and that is the engine's decision, not this
      // shell's.
      type(idle_keys, idle_doc, "nihao");
      require(idle_keys.displayed_page().has_value(), "no page to compose with");
      for (const auto &[vk, name] : {std::pair<std::uint32_t, const char *>{0x71, "F2"},
                                     std::pair<std::uint32_t, const char *>{0x74, "F5"}}) {
        const auto composing = translate_key(vk, false, false, false);
        std::cout << "composing " << name
                  << " eligible=" << (idle_keys.engine_has_work_for(composing) ? 1 : 0) << '\n';
        require(idle_keys.engine_has_work_for(composing),
                "a function key was gated while a composition was live");
      }
      const auto cancel_page =
          idle_keys.begin_key(translate_key(kVkEscape, false, false, false), true);
      idle_doc.apply(idle_keys, cancel_page);
      (void)idle_keys.finish_pass();
      idle_keys.destroy();
      section("PASS R53 rule 2: the pinned engine hands every idle function and media key "
              "back, and T stops claiming them while a live composition keeps its answer");
    }

    // R53 rule 1: with the engine in its off state T claims nothing but the bare Shift. The
    // state is learned from the batch the engine itself answered, so the case is reached the
    // way the owner reached it — type 你好, one lone Shift, type again — and the letters that
    // follow are the Notepad++ case: they reach the engine, the engine is not composing, and
    // an application that honours T alone never gets them back.
    {
      auto english_backend = make_fcitx_backend();
      SemanticController english(981, 64, *english_backend);
      Document english_doc;
      const auto shift =
          translate_key(kVkLShift, false, false, false, 0, false, false, kScanShiftLeft);
      const auto letter = physical('n');
      // Before any batch has answered, the shell keeps the admission it had: a state the
      // engine never reported cannot refuse a key the engine may want.
      require(english.engine_has_work_for(letter),
              "an unreported engine state already refused a letter");
      require(english.engine_has_work_for(shift), "the bare Shift is no longer admitted");
      type(english, english_doc, "nihao");
      auto release = shift;
      const auto press = english.begin_key(release, true);
      require(press.forward && press.frontend.empty(), "the engine did not hand the Shift back");
      release.release = true;
      require(english.routes_release(release), "a lone Shift release is relayed");
      english_doc.apply(english, english.release_key(release));
      require(english.raw_buffer().empty(), "the toggle left a projection");
      // The first letter after the toggle is the one that carries the answer: the engine is now
      // in its off state and hands the character back, while the cached mode still says the key
      // was claimed under an active engine. That is R44's fact one batch later, so the shell
      // delivers the character exactly once instead of dropping it (R53 stopgap).
      const auto committed_before_first = english_doc.committed;
      const auto first = english.begin_key(letter, true);
      std::cout << "english first letter forward=" << (first.forward ? 1 : 0)
                << " eaten=" << (first.eaten ? 1 : 0) << " route=" << static_cast<int>(first.route)
                << " ops=" << first.frontend.size() << '\n';
      require(!first.forward && first.eaten && first.frontend.size() == 1 &&
                  first.frontend.front().operation.type == BackendOperation::Type::CommitText &&
                  first.frontend.front().operation.text == "n",
              "the first letter after the toggle was not delivered exactly once");
      english_doc.apply(english, first);
      require(english_doc.committed == committed_before_first + "n",
              "the first letter after the toggle did not appear exactly once");
      require(english.finish_pass(), "English letter barrier");
      require(english.raw_buffer().empty() && !english.displayed_page().has_value(),
              "the Host kept a preedit in the engine's off state");
      std::cout << "english committed=[" << english_doc.committed << "]\n";
      // The state has now been reported, and every key class the rule is about is refused —
      // except the one key that can report the state back the other way.
      const std::pair<KeyAction, const char *> refused[] = {
          {physical('n'), "letter"},
          {physical('5'), "digit"},
          {physical(','), "punctuation"},
          {translate_key(0x74, false, false, false), "F5"},
          {translate_key(kVkReturn, false, false, false), "Return"},
          {translate_key(kVkEscape, false, false, false), "Escape"},
      };
      for (const auto &[action, name] : refused) {
        const bool claimed = english.engine_has_work_for(action);
        std::cout << "inactive " << name << " keysym=0x" << std::hex << action.keysym << std::dec
                  << " eligible=" << (claimed ? 1 : 0) << '\n';
        require(!claimed, "T still claims a key while the engine reports its off state");
      }
      require(english.engine_has_work_for(shift),
              "the bare Shift was refused, so the toggle back would be unreachable");
      // And back: the press of the same key is dispatched while the engine is off, and its
      // batch reports the engine active again, so the letters compose once more.
      auto back_release = shift;
      const auto press_back = english.begin_key(back_release, true);
      require(press_back.frontend.empty(), "the returning Shift press produced callbacks");
      back_release.release = true;
      english_doc.apply(english, english.release_key(back_release));
      require(english.engine_has_work_for(letter),
              "the engine's on state was not learned back from its own batch");
      const auto before = english_doc.committed.size();
      type(english, english_doc, "nihao");
      require(english_doc.committed.size() > before || !english_doc.preedit.empty(),
              "the toggle back did not return to composing");
      std::cout << "english back preedit=[" << english_doc.preedit << "]\n";
      english.destroy();
      section("PASS R53 rule 1: an engine that reports its off state leaves T every key but "
              "the bare Shift, and the same Shift learns the on state back");
    }
    // R53 stopgap, the rest of the first-key case. A digit and punctuation are the same fact as
    // a letter, but they reach the application by whichever of the two paths T leaves open: a
    // digit is not claimed with no candidate page (so it never left), punctuation is claimed and
    // the engine hands it back (so the fallback delivers it). Either way it appears exactly once,
    // which is what the owner's "nothing lost, nothing doubled" means.
    {
      const std::pair<KeyAction, std::string> first_keys[] = {{physical('5'), "5"},
                                                              {physical(','), ","}};
      int context_id = 984;
      for (const auto &[first_key, expected] : first_keys) {
        auto first_backend = make_fcitx_backend();
        SemanticController c(context_id++, 64, *first_backend);
        Document d;
        type(c, d, "nihao");
        const auto shift =
            translate_key(kVkLShift, false, false, false, 0, false, false, kScanShiftLeft);
        const auto press = c.begin_key(shift, true);
        require(press.forward && press.frontend.empty(), "the engine did not hand the Shift back");
        auto release = shift;
        release.release = true;
        require(c.routes_release(release), "a lone Shift release is relayed");
        d.apply(c, c.release_key(release));
        const auto claimed = c.engine_has_work_for(first_key);
        const auto committed_before = d.committed;
        std::cout << "first key [" << expected << "] claimed=" << (claimed ? 1 : 0) << '\n';
        if (!claimed) {
          // The application's key: nothing was dispatched, so it is delivered once, natively.
          require(d.committed == committed_before && d.preedit.empty(),
                  "a key T left alone still reached the document through the shell");
        } else {
          const auto answer = c.begin_key(first_key, true);
          std::cout << "first key [" << expected << "] forward=" << (answer.forward ? 1 : 0)
                    << " eaten=" << (answer.eaten ? 1 : 0) << " ops=" << answer.frontend.size()
                    << '\n';
          require(!answer.forward && answer.eaten && answer.frontend.size() == 1 &&
                      answer.frontend.front().operation.type ==
                          BackendOperation::Type::CommitText &&
                      answer.frontend.front().operation.text == expected,
                  "a first key after the toggle was not delivered exactly once");
          d.apply(c, answer);
          require(d.committed == committed_before + expected,
                  "a first key after the toggle did not appear exactly once");
          // The batch that delivered it also taught the shell the engine is off, so the keys
          // after it are the application's. When T never claimed the key there was no batch to
          // learn from, and the cached mode stays as it was — which the fallback then covers
          // on the next key that is claimed.
          require(!c.engine_has_work_for(physical('n')),
                  "the key after the first one was still claimed in T");
        }
        c.destroy();
      }
      // Only a printable key is delivered this way. A key with no character keeps the route it
      // had: there is nothing to deliver, and owning it would swallow it (R36's Return, R37's
      // editing keys), so the fallback must leave it alone.
      const std::pair<KeyAction, const char *> characterless[] = {
          {translate_key(kVkReturn, false, false, false), "Return"},
          {translate_key(kVkEscape, false, false, false), "Escape"},
          {translate_key(0x09, false, false, false), "Tab"},
          {translate_key(0x74, false, false, false), "F5"},
      };
      int characterless_id = 994;
      for (const auto &[first_key, name] : characterless) {
        auto cl_backend = make_fcitx_backend();
        SemanticController c(characterless_id++, 64, *cl_backend);
        Document d;
        type(c, d, "nihao");
        const auto shift =
            translate_key(kVkLShift, false, false, false, 0, false, false, kScanShiftLeft);
        (void)c.begin_key(shift, true);
        auto release = shift;
        release.release = true;
        require(c.routes_release(release), "a lone Shift release is relayed");
        d.apply(c, c.release_key(release));
        const auto committed_before = d.committed;
        const auto answer = c.begin_key(first_key, true);
        std::cout << "first key " << name << " forward=" << (answer.forward ? 1 : 0)
                  << " eaten=" << (answer.eaten ? 1 : 0) << " ops=" << answer.frontend.size()
                  << '\n';
        require(answer.forward && answer.frontend.empty() && d.committed == committed_before,
                "a character-less first key was delivered instead of handed back");
        c.destroy();
      }
      section("PASS R53 first key after the toggle: a digit and punctuation appear exactly once");
    }

    // R53 stopgap, Codex P1: a candidate action must not teach rule 1 the engine's mode. The
    // Host's is_candidate_action branch has no key event to report from, so its batch carries
    // `context_was_active` false whatever the engine is doing; recording that would make the
    // next letter the application's and the owner would type English until pressing Shift.
    {
      // Compose, act on the list the way the popup does, then keep typing.
      auto after = [](int context_id, const char *name, int rows, FcitxBackendOptions options,
                      auto &&act) {
        auto list_backend = make_fcitx_backend(std::move(options));
        SemanticController c(context_id, 64, *list_backend);
        Document d;
        if (rows)
          c.set_window_rows(rows);
        type(c, d, "nihao");
        require(c.displayed_page().has_value(), "no page to act on");
        act(c, d);
        require(c.finish_pass(), "candidate action barrier");
        const auto letter = physical('n');
        const bool claimed = c.engine_has_work_for(letter);
        std::cout << "after " << name << " letter claimed=" << (claimed ? 1 : 0) << '\n';
        require(claimed, "a candidate action made T refuse the next letter");
        auto next = c.begin_key(letter, true);
        require(next.eaten && !next.frontend.empty() &&
                    next.route != SemanticDecision::Route::CancelledRecovery,
                "the letter after a candidate action did not reach the engine");
        d.apply(c, next);
        require(!d.preedit.empty(), "the letter after a candidate action did not compose");
        std::cout << "after " << name << " preedit=[" << d.preedit << "]\n";
        c.destroy();
      };
      after(987, "a mouse selection", 0, {}, [](SemanticController &c, Document &d) {
        const auto decision = c.begin_candidate_action(CandidateAction::Select, 0);
        d.apply(c, decision);
      });
      after(988, "a page turn", 0, {}, [](SemanticController &c, Document &d) {
        const auto decision = c.begin_candidate_action(CandidateAction::PageNext, 0);
        d.apply(c, decision);
      });
      // The cursor move exists only in the scrolling arrangement, so this case asks for it.
      FcitxBackendOptions scrolling;
      scrolling.fixture_candidate_layout = "AutoScroll";
      after(989, "a cursor move", 3, scrolling, [](SemanticController &c, Document &d) {
        SemanticDecision decision;
        require(c.begin_candidate_cursor(1, decision), "the scroll window refused a cursor move");
        d.apply(c, decision);
      });
      section("PASS R53 P1: a candidate action leaves rule 1's cached mode alone");
    }
    return 0;
  } catch (const std::exception &e) {
    failure_reported = true;
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
