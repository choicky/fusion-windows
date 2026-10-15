#include "fusion/backend.hpp"
#include "mock_backend.hpp"
#include "tsf/semantic.hpp"
#include <crtdbg.h>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace fusion;
using namespace fusion::tsf;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
SemanticDecision state(SemanticController &controller, std::uint32_t key) {
  auto decision = controller.begin_key(classify_key(key, false, false, false), true);
  require(decision.route == SemanticDecision::Route::State && decision.key.has_value(),
          "real STATE route missing");
  require(decision.backend_session_id != 0 && decision.operation_count != 0,
          "real frontend observations missing");
  require(controller.settle_state(*decision.key, EditResult::AppliedReadback) == Result::Accepted,
          "STATE settlement failed");
  return decision;
}
int main() {
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  try {
    auto backend = make_fcitx_backend();
    require(backend->healthy(), "real backend not ready");
    SemanticController controller(501, 32, *backend);
    std::string preedit;
    for (auto key : std::string("NIHAO"))
      preedit = state(controller, static_cast<std::uint32_t>(key)).text;
    require(!preedit.empty(), "real Pinyin preedit empty");
    auto commit = controller.begin_key(classify_key(kVkSpace, false, false, false), true);
    require(commit.route == SemanticDecision::Route::Effect && commit.text == "你好",
            "real Pinyin Space commit mismatch");
    require(controller.propose_effect(*commit.key) == Result::Accepted,
            "commit watermark gate failed");
    auto ticket = controller.open_effect(*commit.key);
    require(ticket.has_value(), "commit ticket missing");
    require(controller.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
            "commit settlement failed");
    std::cout
        << "PASS real stimulus -> adapter -> Fcitx frontend preedit/commit -> Host/Core ticket\n";
    state(controller, 'N');
    auto before = state(controller, 'I').text;
    auto after = state(controller, kVkBack).text;
    require(before != after, "Backspace did not mutate real composition");
    state(controller, kVkBack);
    auto pass = controller.begin_key(classify_key(kVkReturn, false, false, false), true);
    require(pass.route == SemanticDecision::Route::Pass && !pass.eaten && controller.finish_pass(),
            "genuine unconsumed Return not PASS");
    std::cout << "PASS real Backspace and genuine unconsumed Return\n";
    const auto stale_binding = controller.core().binding();
    require(controller.replace_context(501) == Result::Accepted,
            "context generation replacement failed");
    auto stale = backend->send({stale_binding, 999, BackendStimulus::Type::Character, 'x'});
    require(stale.disposition == BackendBatch::Disposition::Reject && stale.operations.empty(),
            "stale generation produced observations");
    require(state(controller, 'N').text == "n", "stale generation changed newer composition");
    controller.note_composition_terminated();
    require(state(controller, 'I').text == "i",
            "external termination did not reset real semantic state");
    controller.destroy();
    std::cout << "PASS stale context generation fenced before Fcitx dispatch\n";
    const Binding duplicate_binding{601, 1, 1};
    require(backend->bind(duplicate_binding), "duplicate fixture bind failed");
    require(
        backend->send({duplicate_binding, 1, BackendStimulus::Type::Character, 'n'}).disposition ==
            BackendBatch::Disposition::Consumed,
        "direct real stimulus not consumed");
    require(
        backend->send({duplicate_binding, 1, BackendStimulus::Type::Character, 'i'}).disposition ==
            BackendBatch::Disposition::Reject,
        "duplicate stimulus accepted");
    auto back = backend->send({duplicate_binding, 2, BackendStimulus::Type::Backspace});
    bool empty = false;
    for (const auto &operation : back.operations)
      if (operation.type == BackendOperation::Type::SetPreedit)
        empty = operation.text.empty();
    require(empty, "duplicate stimulus mutated composition");
    backend->unbind(duplicate_binding);
    std::cout << "PASS duplicate stimulus rejected before semantic mutation\n";
    test::MockBackend mock;
    SemanticController mock_controller(701, 8, mock);
    auto mock_state = mock_controller.begin_key(classify_key('N', false, false, false), true);
    require(mock_state.route == SemanticDecision::Route::State && mock_state.text == "n" &&
                mock_state.backend_session_id == 0,
            "MockBackend regression");
    require(mock_controller.settle_state(*mock_state.key, EditResult::AppliedReadback) ==
                Result::Accepted,
            "MockBackend state regression");
    std::cout << "PASS MockBackend remains operational alongside optional backend\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
