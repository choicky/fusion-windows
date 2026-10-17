// Focused regression test for the two MSVC empty-range portability guards added
// to cmake/patches/fcitx5-msvc.patch (src/lib/fcitx-utils/utf8.h).
//
// Under an MSVC Debug build with checked iterators, both paths below previously
// dereferenced an end iterator and aborted the whole process (exit 0xC0000409)
// before Fcitx produced any result:
//
//   A. PinyinEngine::handlePunc -> fcitx::utf8::lengthValidated(empty range).
//      The frontend deliberately does not declare CommitStringWithCursor, so the
//      engine takes the branch that validates the empty puncAfter.
//   B. PinyinEngine::keyEvent -> QuickPhrase -> QuickPhraseTempMode ->
//      InputBuffer::type -> fcitx::utf8::UTF8StringViewIter::update(empty range),
//      for the shipped QuickPhraseTriggerRegex defaults (".(/|@)$",
//      "^(www|bbs|forum|mail)\.", "^(http|https|ftp|telnet|mailto):").
//
// Both paths are driven through the real pinned Fcitx backend, so this is a
// dependency portability regression rather than a Windows-frontend behaviour
// test: the productive TSF key admission is intentionally not involved. A
// missing guard kills this process instead of failing an assertion; that is the
// nature of the diagnosed defect.
//
// The same run pins the unchanged non-empty behaviour of the affected helpers
// (Pinyin preedit projection, Space commit, Backspace projection, and genuine
// forwarding of keys the engine does not consume).

#include "fusion/backend.hpp"

#include <crtdbg.h>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace fusion;

namespace {

void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

std::size_t commit_count(const BackendBatch &batch) {
  std::size_t count = 0;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CommitText) {
      ++count;
    }
  }
  return count;
}

std::string last_preedit(const BackendBatch &batch) {
  std::string text;
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::SetPreedit) {
      text = operation.text;
    }
  }
  return text;
}

std::string committed_text(const BackendBatch &batch) {
  for (const auto &operation : batch.operations) {
    if (operation.type == BackendOperation::Type::CommitText) {
      return operation.text;
    }
  }
  return {};
}

bool consumed(const BackendBatch &batch) {
  return batch.disposition == BackendBatch::Disposition::Consumed;
}

// A genuine forward: Forward disposition and no document callback. Since Stage 5 every
// dispatch also carries the CandidatePage STATE snapshot, and for an idle key that snapshot
// must be the empty-panel hide signal (no candidate, no panel text) — nothing else may ride
// along with the original key.
bool forwarded(const BackendBatch &batch) {
  if (batch.disposition != BackendBatch::Disposition::Forward)
    return false;
  for (const auto &operation : batch.operations) {
    if (operation.type != BackendOperation::Type::CandidateSnapshot || !operation.page ||
        !operation.page->panel_empty())
      return false;
  }
  return true;
}

// Every scenario uses its own logical context so engine state cannot leak.
class Scenario {
public:
  Scenario(Backend &backend, std::uint64_t context) : backend_(&backend), binding_{context, 1, 1} {
    require(backend.bind(binding_), "fixture bind failed");
  }
  ~Scenario() { backend_->unbind(binding_); }
  Scenario(const Scenario &) = delete;
  Scenario &operator=(const Scenario &) = delete;

  BackendBatch type(const std::string &text) {
    BackendBatch batch{};
    for (char character : text) {
      batch = send(BackendStimulus::Type::Character, character);
    }
    return batch;
  }
  BackendBatch key(BackendStimulus::Type type) { return send(type, '\0'); }
  BackendBatch send(BackendStimulus::Type type, char character) {
    BackendStimulus stimulus{binding_, next_stimulus_++, type, character};
    const BackendBatch batch = backend_->send(stimulus);
    require(batch.backend_session_id == backend_->session_id(), "batch session mismatch");
    require(batch.binding == binding_, "batch binding mismatch");
    require(batch.stimulus_id == stimulus.stimulus_id, "batch stimulus mismatch");
    return batch;
  }

private:
  Backend *backend_;
  Binding binding_;
  std::uint64_t next_stimulus_ = 1;
};

// A. Punctuation: PinyinEngine::handlePunc validates the empty puncAfter.
void punctuation_empty_range(Backend &backend) {
  {
    Scenario composing(backend, 801);
    (void)composing.type("nihao");
    const BackendBatch comma = composing.send(BackendStimulus::Type::Character, ',');
    require(consumed(comma), "punctuation during composition was not consumed");
    require(commit_count(comma) == std::size_t{2},
            "punctuation during composition must commit candidate and punctuation");
    require(committed_text(comma) == "你好", "candidate commit text changed");
    require(last_preedit(comma).empty(), "punctuation did not clear the preedit");
  }
  {
    Scenario idle(backend, 802);
    const BackendBatch comma = idle.send(BackendStimulus::Type::Character, ',');
    require(consumed(comma), "idle punctuation was not consumed");
    require(commit_count(comma) == std::size_t{1}, "idle punctuation commit count");
    require(committed_text(comma) == "，", "idle punctuation conversion changed");
  }
}

// B. QuickPhrase trigger keys: UTF8StringViewIter::update on an empty range.
void quickphrase_empty_range(Backend &backend) {
  struct Trigger {
    const char *prefix;
    char key;
    const char *expected;
  };
  const Trigger triggers[] = {{"nihao", '/', "nihao/"},
                              {"nihao", '@', "nihao@"},
                              {"www", '.', "www."},
                              {"mail", '.', "mail."},
                              {"http", ':', "http:"}};
  std::uint64_t context = 811;
  for (const Trigger &trigger : triggers) {
    Scenario scenario(backend, context++);
    (void)scenario.type(trigger.prefix);
    const BackendBatch batch = scenario.send(BackendStimulus::Type::Character, trigger.key);
    require(consumed(batch), "quickphrase trigger was not consumed");
    require(last_preedit(batch) == trigger.expected,
            "quickphrase temp-mode preedit changed (engine path not taken)");
  }
}

// C. Non-empty behaviour of the same helpers must be unchanged.
void non_empty_behaviour(Backend &backend) {
  {
    Scenario scenario(backend, 821);
    const BackendBatch preedit = scenario.type("nihao");
    require(consumed(preedit), "typing was not consumed");
    require(last_preedit(preedit) == "ni hao", "Pinyin preedit projection changed");
    const BackendBatch commit = scenario.key(BackendStimulus::Type::Space);
    require(consumed(commit), "Space commit was not consumed");
    require(commit_count(commit) == std::size_t{1}, "Space commit count");
    require(committed_text(commit) == "你好", "Space commit text changed");
    require(last_preedit(commit).empty(), "Space commit did not clear the preedit");
  }
  {
    Scenario scenario(backend, 822);
    (void)scenario.type("nihao");
    const BackendBatch backspace = scenario.key(BackendStimulus::Type::Backspace);
    require(consumed(backspace), "Backspace was not consumed");
    require(last_preedit(backspace) == "ni ha", "Backspace projection changed");
  }
  {
    Scenario scenario(backend, 823);
    require(forwarded(scenario.key(BackendStimulus::Type::Space)),
            "idle Space must stay a genuine forward");
    require(forwarded(scenario.send(BackendStimulus::Type::Character, '@')),
            "idle '@' must stay a genuine forward");
    require(forwarded(scenario.key(BackendStimulus::Type::Return)),
            "idle Return must stay a genuine forward");
  }
}

} // namespace

int main() {
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  try {
    const auto backend = make_fcitx_backend();
    require(backend->healthy(), "real backend not ready");
    punctuation_empty_range(*backend);
    std::cout << "PASS punctuation guard (utf8::lengthValidated, empty puncAfter)\n";
    quickphrase_empty_range(*backend);
    std::cout << "PASS quickphrase trigger guard (utf8::UTF8StringViewIter::update, empty range)\n";
    non_empty_behaviour(*backend);
    std::cout << "PASS non-empty utf8 helper behaviour unchanged\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
