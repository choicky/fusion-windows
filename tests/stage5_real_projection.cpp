// Stage 5 closure against the real pinned Fcitx5 + chinese-addons pinyin engine
// (in-process FusionFcitxHost, the same backend FusionHost.exe hosts). It proves
// the producer side of closure blockers 1 and 2 with real engine semantics:
//   1. the displayed preedit carries the fcitx::Text contract — segments,
//      TextFormatFlag bits and the engine byte cursor — and the Windows-default
//      pinyin shape (HighLight up to the engine cursor, cursor fixed at 0) moves
//      with Left/Right; partial selection and continued composition map onto
//      TSF UTF-16 positions;
//   2. auxUp reaches the panel snapshot (pinyin stroke filter on `grave`,
//      QuickPhrase on `;`) and keeps the panel displayed without candidates,
//      and an emptied InputPanel is the hide signal.
// Printed lines are the evidence record.

#include "fusion/backend.hpp"
#include "fusion/text_utf16.hpp"
#include "tsf/preedit_presentation.hpp"

#include <crtdbg.h>

#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace fusion;
using namespace fusion::tsf;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

constexpr std::uint32_t kLeft = 0xff51;
constexpr std::uint32_t kRight = 0xff53;
constexpr std::uint32_t kEscape = 0xff1b;

struct Driver {
  std::unique_ptr<Backend> backend = make_fcitx_backend();
  Binding binding{7001, 1, 1};
  std::uint64_t next = 1;

  BackendBatch key(std::uint32_t keysym, char ch = 0) {
    BackendStimulus s{binding, next++, BackendStimulus::Type::Character, ch};
    s.keysym = keysym;
    return backend->send(s);
  }
  BackendBatch select(std::int32_t index) {
    BackendStimulus s{binding, next++, BackendStimulus::Type::SelectCandidate};
    s.candidate_index = index;
    return backend->send(s);
  }
  BackendBatch type(const std::string &text) {
    BackendBatch last;
    for (char c : text)
      last = key(static_cast<unsigned char>(c), c);
    return last;
  }
};

const BackendOperation *last_of(const BackendBatch &batch, BackendOperation::Type type) {
  const BackendOperation *found = nullptr;
  for (const auto &op : batch.operations)
    if (op.type == type)
      found = &op;
  return found;
}

std::string describe(const BackendOperation &op) {
  std::string out = "text=[" + op.text + "]";
  if (!op.preedit)
    return out + " (no projection)";
  out += " cursor=" + std::to_string(op.preedit->cursor) + " segments=";
  for (const auto &segment : op.preedit->segments)
    out += "(" + std::to_string(segment.bytes) + "B,fmt=" + std::to_string(segment.format) + ")";
  return out;
}

// The highlighted prefix length in bytes (Windows default pinyin: HighLight marks
// the text up to the engine cursor).
std::size_t highlighted_bytes(const FormattedPreedit &preedit) {
  std::size_t bytes = 0;
  for (const auto &segment : preedit.segments) {
    if (!(segment.format & text_format::highlight))
      break;
    bytes += segment.bytes;
  }
  return bytes;
}

void check_projection(const BackendOperation &op) {
  require(op.preedit.has_value(), "real SetPreedit carries the fcitx::Text projection");
  require(op.preedit->consistent_with(op.text), "segments tile the displayed preedit");
  require(layout_preedit(op.text, *op.preedit).has_value(),
          "preedit maps onto TSF UTF-16 positions");
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
    Driver d;
    require(d.backend->healthy(), "real backend not ready");
    require(d.backend->bind(d.binding), "bind");

    // --- Blocker 1: preedit contract -----------------------------------------
    auto batch = d.type("nihao");
    const auto *pre = last_of(batch, BackendOperation::Type::SetPreedit);
    require(pre != nullptr, "nihao produced a preedit");
    std::cout << "nihao preedit " << describe(*pre) << '\n';
    check_projection(*pre);
    require(pre->preedit->cursor == 0,
            "Windows-default pinyin fixes the embedded preedit cursor at the beginning");
    const std::size_t full = highlighted_bytes(*pre->preedit);
    require(full == pre->text.size(), "engine cursor at the end: whole preedit highlighted");
    const auto layout = layout_preedit(pre->text, *pre->preedit);
    require(layout->caret == std::size_t{0}, "TSF caret at UTF-16 offset 0");
    require(!layout->ranges.empty() && layout->ranges.front().style == PreeditStyle::Highlighted,
            "HighLight maps to the highlighted TSF attribute");

    batch = d.key(kLeft);
    pre = last_of(batch, BackendOperation::Type::SetPreedit);
    require(pre != nullptr, "Left re-projected the preedit");
    std::cout << "Left    preedit " << describe(*pre) << '\n';
    check_projection(*pre);
    const std::size_t after_left = highlighted_bytes(*pre->preedit);
    require(after_left < full, "Left moved the engine cursor: highlighted prefix shrank");
    const auto left_layout = layout_preedit(pre->text, *pre->preedit);
    require(left_layout->ranges.size() >= 2 &&
                left_layout->ranges[0].style == PreeditStyle::Highlighted &&
                left_layout->ranges[1].style == PreeditStyle::Input,
            "the engine cursor boundary is visible as Highlighted|Input ranges");

    batch = d.key(kRight);
    pre = last_of(batch, BackendOperation::Type::SetPreedit);
    require(pre != nullptr && highlighted_bytes(*pre->preedit) == full,
            "Right restored the engine cursor");

    // Partial selection: pick a one-character candidate covering "ni" if the page
    // offers one; the remaining pinyin stays composing.
    const auto *page_op = last_of(batch, BackendOperation::Type::CandidateSnapshot);
    require(page_op && page_op->page && !page_op->page->entries.empty(), "nihao page present");
    std::int32_t single = -1;
    for (std::size_t i = 0; i < page_op->page->entries.size(); ++i) {
      std::cout << "  candidate[" << i << "] label=[" << page_op->page->entries[i].label
                << "] text=[" << page_op->page->entries[i].text << "] comment=["
                << page_op->page->entries[i].comment << "]\n";
      if (single < 0 && page_op->page->entries[i].text == "\xE4\xBD\xA0") // 你
        single = static_cast<std::int32_t>(i);
    }
    if (single >= 0) {
      batch = d.select(single);
      pre = last_of(batch, BackendOperation::Type::SetPreedit);
      require(pre != nullptr, "partial selection re-projected the preedit");
      std::cout << "partial preedit " << describe(*pre) << '\n';
      check_projection(*pre);
      require(pre->text.rfind("\xE4\xBD\xA0", 0) == 0, "partial selection keeps 你 composing");
      require(last_of(batch, BackendOperation::Type::CommitText) == nullptr,
              "partial selection commits nothing yet");
      const auto *after = last_of(batch, BackendOperation::Type::CandidateSnapshot);
      require(after && after->page && !after->page->entries.empty(),
              "continued composition offers candidates for the rest");
      batch = d.key(' ', ' ');
      const auto *committed = last_of(batch, BackendOperation::Type::CommitText);
      require(committed && committed->text.rfind("\xE4\xBD\xA0", 0) == 0,
              "continued composition commits after the selected prefix");
      std::cout << "partial commit [" << committed->text << "]\n";
    } else {
      std::cout << "no single-character 你 on page 1; partial selection case skipped\n";
      d.key(kEscape);
    }
    batch = d.type("ni");
    batch = d.key(kEscape);
    pre = last_of(batch, BackendOperation::Type::SetPreedit);
    const auto *hidden = last_of(batch, BackendOperation::Type::CandidateSnapshot);
    require(pre && pre->text.empty(), "Escape clears the preedit");
    require(hidden && hidden->page && hidden->page->panel_empty(),
            "an emptied InputPanel is the hide signal");

    // --- Blocker 2: auxUp producers --------------------------------------------
    d.type("nihao");
    batch = d.key('`', '`');
    page_op = last_of(batch, BackendOperation::Type::CandidateSnapshot);
    require(page_op && page_op->page, "stroke filter snapshot");
    std::cout << "stroke filter auxUp=[" << page_op->page->aux_up << "]\n";
    require(!page_op->page->aux_up.empty(), "pinyin stroke filter shows auxUp");
    require(!page_op->page->panel_empty(), "stroke filter panel displayed");
    // A stroke key typed in this mode is not driven here: on the pinned MSVC
    // Debug runtime the engine's stroke lookup asserts (string_view end
    // dereference) on the unchanged base as well; it is recorded as a separate
    // pre-existing pinned-runtime defect, not a projection property.
    d.key(kEscape);
    batch = d.key(kEscape);

    batch = d.key(';', ';');
    page_op = last_of(batch, BackendOperation::Type::CandidateSnapshot);
    require(page_op && page_op->page, "quickphrase snapshot");
    std::cout << "quickphrase auxUp=[" << page_op->page->aux_up
              << "] entries=" << page_op->page->entries.size() << '\n';
    require(!page_op->page->aux_up.empty(), "QuickPhrase shows its prompt in auxUp");
    require(!page_op->page->panel_empty(), "auxUp alone keeps the panel displayed");
    d.key(kEscape);

    // --- Candidate presentation: the popup font --------------------------------
    // The font the popup draws with is projected from the Host's own presentation
    // configuration, per page. This isolated profile has no candidate-presentation.conf, so
    // the page carries the schema defaults; an empty projection here would mean every user
    // gets the popup's built-in default instead of their setting.
    batch = d.type("ni");
    page_op = last_of(batch, BackendOperation::Type::CandidateSnapshot);
    require(page_op && page_op->page, "presentation snapshot");
    std::cout << "font=[" << page_op->page->font.family << "] "
              << page_op->page->font.size_pt << "pt\n";
    require(page_op->page->font.family == "Microsoft YaHei UI",
            "the page carries the configured candidate font family");
    require(page_op->page->font.size_pt == 11, "the page carries the configured candidate font size");
    d.key(kEscape);

    d.backend->unbind(d.binding);
    std::cout << "PASS stage5 real projection (preedit contract, aux producers, hide signal)\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
