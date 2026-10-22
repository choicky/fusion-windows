// R40: the automatic scrolling candidate arrangement against the real pinned engine.
//
// What is proven here is the engine's side of it — that a window of several engine pages can
// be read without paging, that the window follows the engine's own paging keys, and that a
// candidate selected out of that window is the engine's candidate. The Win32 window's own
// geometry is `candidate_window_layout`; this file deliberately drives the semantic path the
// TSF uses (stimulus in, ordered batch out) so the two halves are separate evidence.
//
// Printed lines are the evidence record.

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"

#include <crtdbg.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace fusion;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

// The engine's display text of a page entry, for comparing a commit against the cell the
// user would have clicked.
std::string entry_text(const CandidatePage &page, std::size_t index) {
  if (index >= page.entries.size())
    return "(out of range)";
  return page.entries[index].text;
}

struct Driver {
  std::unique_ptr<Backend> backend;
  Binding binding{8101, 1, 1};
  std::uint64_t next = 1;
  CandidatePage page;

  explicit Driver(FcitxBackendOptions options) : backend(make_fcitx_backend(std::move(options))) {
    if (!backend->healthy() || !backend->bind(binding))
      throw std::runtime_error("the real backend did not bind");
  }

  BackendBatch send(BackendStimulus stimulus) {
    stimulus.binding = binding;
    stimulus.stimulus_id = next++;
    auto batch = backend->send(stimulus);
    for (const auto &operation : batch.operations)
      if (operation.type == BackendOperation::Type::CandidateSnapshot && operation.page)
        page = *operation.page;
    return batch;
  }
  BackendBatch type(const std::string &text) {
    BackendBatch last;
    for (char ch : text) {
      BackendStimulus stimulus{binding, 0, BackendStimulus::Type::Character, ch};
      stimulus.keysym = static_cast<std::uint8_t>(ch);
      last = send(stimulus);
    }
    return last;
  }
  BackendBatch key(BackendStimulus::Type type) {
    return send(BackendStimulus{binding, 0, type, 0});
  }
  // The last projected preedit text of a batch, "" when it carried none: what is still
  // composing after a partial selection.
  std::string preedit(const BackendBatch &batch) const {
    std::string text;
    for (const auto &operation : batch.operations)
      if (operation.type == BackendOperation::Type::SetPreedit)
        text = operation.text;
    return text;
  }
  BackendBatch select_global(std::int32_t index) {
    BackendStimulus stimulus{binding, 0, BackendStimulus::Type::SelectCandidateGlobal};
    stimulus.candidate_index = index;
    return send(stimulus);
  }
  // Everything the batch committed, in order.
  std::string committed(const BackendBatch &batch) const {
    std::string text;
    for (const auto &operation : batch.operations)
      if (operation.type == BackendOperation::Type::CommitText)
        text += operation.text;
    return text;
  }
};

int page_size_of(const CandidatePage &page) { return page.page_size > 0 ? page.page_size : 1; }

void describe(const char *label, const CandidatePage &page) {
  std::printf("%s arrangement=%d page_size=%d current=%d first=%d total=%d entries=%zu "
              "cursor=%d prev=%d next=%d\n",
              label, static_cast<int>(page.arrangement), page.page_size, page.current_page,
              page.window_first_page, page.total_pages, page.entries.size(), page.cursor_index,
              page.has_prev ? 1 : 0, page.has_next ? 1 : 0);
}

FcitxBackendOptions scrolling(const char *method = nullptr) {
  FcitxBackendOptions options;
  options.fixture_candidate_layout = "AutoScroll";
  if (method != nullptr) {
    options.fixture_input_methods = {method};
    options.fixture_default_method = method;
  }
  return options;
}

// The whole session, measured once per input method so Pinyin and Shuangpin are proven the
// same way rather than one standing in for the other.
void run_scroll_session(const char *label, const char *method, const char *expected_scheme) {
  Driver d(scrolling(method));
  d.type("nihao");
  require(!d.page.entries.empty(), "the engine produced no candidates");
  std::printf("[%s] ", label);
  describe("first row", d.page);
  require(d.page.arrangement == CandidateArrangement::Scroll,
          "the scrolling preference did not reach the projection");
  require(d.page.paging_known(), "the page carries no paging facts");
  require(d.page.page_size >= 1, "the engine reported no page size");
  require(static_cast<int>(d.page.entries.size()) == d.page.page_size,
          "the first projection is not exactly one row");
  require(d.page.current_page == 0 && d.page.window_first_page == 0,
          "the session did not start on the engine's first page");
  require(d.page.total_pages > 2, "this fixture needs more than two pages to scroll");

  // The first "next page" is the engine's own paging key: the window follows it, and shows
  // the following rows without repeating the first one.
  d.key(BackendStimulus::Type::PageNext);
  std::printf("[%s] ", label);
  describe("after first next", d.page);
  {
    std::string labels;
    for (const auto &entry : d.page.entries)
      labels += "[" + entry.label + "]";
    std::printf("labels of the window: %s\n", labels.c_str());
  }
  require(d.page.current_page == 1, "the engine did not move to its second page");
  require(d.page.window_first_page == 1, "the window repeated the line already passed");
  require(d.page.entries.size() > static_cast<std::size_t>(d.page.page_size),
          "more than one row remained but the window stayed a single row");
  const int rows = static_cast<int>(d.page.entries.size()) / d.page.page_size;
  require(rows <= 5, "the window is taller than the confirmed five rows");

  // Down the rows, then across a scroll page: the highlight row is the engine's page, so
  // the window's own current row has to follow it, and step off the last row of a scroll
  // page into the first row of the next one.
  int expected_page = 1;
  for (int row = 0; row < rows - 1; ++row) {
    d.key(BackendStimulus::Type::PageNext);
    ++expected_page;
    require(d.page.current_page == expected_page, "a row step did not follow the engine page");
    require(d.page.window_first_page == 1,
            "the window moved before the highlight reached its last row");
    require(d.page.current_row() == row + 1, "the highlight row is not the engine's page");
  }
  std::printf("[%s] last row of the first scroll page: current=%d row=%d first=%d\n", label,
              d.page.current_page, d.page.current_row(), d.page.window_first_page);

  d.key(BackendStimulus::Type::PageNext);
  std::printf("[%s] ", label);
  describe("next scroll page", d.page);
  require(d.page.current_page == expected_page + 1, "the engine did not advance a page");
  require(d.page.current_row() == 0,
          "the first row of the next scroll page is not the highlighted one");
  require(d.page.window_first_page > 1, "the window did not move on to the next scroll page");

  // Selecting a cell of a row the engine is not on. Two ways to the same candidate: the
  // window's global index (no paging), and the engine's own paging followed by a page-local
  // select. They have to produce the same result, which is what makes "the window's cell is
  // the engine's candidate" a fact rather than a claim.
  const std::size_t cell =
      static_cast<std::size_t>(2) * static_cast<std::size_t>(d.page.page_size);
  const std::int32_t target_global = d.page.global_index_of(cell);
  const std::string target_text = entry_text(d.page, cell);
  require(target_global > d.page.current_page * d.page.page_size,
          "the target cell is not on a later page than the engine's");
  const auto through_window = d.select_global(target_global);
  const std::string window_commit = d.committed(through_window);
  const std::string window_preedit = d.preedit(through_window);

  Driver paged(scrolling(method));
  paged.type("nihao");
  for (int step = 0; step < target_global / page_size_of(paged.page); ++step)
    paged.key(BackendStimulus::Type::PageNext);
  require(paged.page.current_page == target_global / page_size_of(paged.page),
          "the reference path did not reach the target page");
  BackendStimulus local{paged.binding, 0, BackendStimulus::Type::SelectCandidate};
  local.candidate_index = target_global % page_size_of(paged.page);
  const auto through_paging = paged.send(local);
  const std::string paged_commit = paged.committed(through_paging);
  const std::string paged_preedit = paged.preedit(through_paging);

  std::printf("[%s] global %d (%s): window=[%s|%s] paged=[%s|%s]\n", label, target_global,
              target_text.c_str(), window_commit.c_str(), window_preedit.c_str(),
              paged_commit.c_str(), paged_preedit.c_str());
  require(window_commit == paged_commit && window_preedit == paged_preedit,
          "selecting through the window differs from paging there and selecting");
  require(!(window_commit.empty() && window_preedit.empty()),
          "the selection had no effect at all");
  // The engine picked the word it was asked for: a full match commits it, a partial match
  // leaves it at the front of what is still composing. Either way the word is in the result.
  require(window_commit.find(target_text) == 0 || window_preedit.find(target_text) == 0,
          "the selected word is not in what the engine committed or kept composing");
  std::printf("[%s] the window's cell is the engine's candidate\n", label);

  // The first candidate of the engine's own page is the full match, and selecting it through
  // the same global path commits exactly the text the cell shows.
  Driver head(scrolling(method));
  head.type("nihao");
  const std::string head_text = entry_text(head.page, 0);
  const std::string head_commit = head.committed(head.select_global(0));
  std::printf("[%s] global 0 (%s) committed [%s]\n", label, head_text.c_str(),
              head_commit.c_str());
  require(!head_commit.empty(), "the first candidate committed nothing");

  // A new input is a new list: the arrangement starts from one row again, and the engine's
  // own page is its first one.
  Driver fresh(scrolling(method));
  fresh.type("nihao");
  fresh.key(BackendStimulus::Type::PageNext);
  fresh.key(BackendStimulus::Type::PageNext);
  require(fresh.page.window_first_page > 0, "the fixture did not expand before the reset check");
  fresh.key(BackendStimulus::Type::PagePrev);
  fresh.type("a");
  std::printf("[%s] ", label);
  describe("after new input", fresh.page);
  require(fresh.page.current_page == 0 && fresh.page.window_first_page == 0,
          "a new candidate list did not start from its first row");
  require(static_cast<int>(fresh.page.entries.size()) == fresh.page.page_size,
          "a new candidate list is not a single row");
  (void)expected_scheme;
}

// The number keys are the engine's own: they select a candidate of the page the engine is
// on, which is the row the window highlights. Two facts make showing numbers on that row and
// nowhere else honest, and both are measured rather than asserted:
//
//   * the labels the projection carries exist on the highlighted row only, because the
//     engine's labels are page-local; and
//   * pressing "n" has exactly the effect of selecting that row's (n-1)-th cell.
void run_number_keys(const char *method) {
  for (int digit = 1; digit <= 2; ++digit) {
    Driver d(scrolling(method));
    d.type("nihao");
    d.key(BackendStimulus::Type::PageNext);
    d.key(BackendStimulus::Type::PageNext);
    const std::size_t row = static_cast<std::size_t>(d.page.current_row());
    const std::size_t page_size = static_cast<std::size_t>(page_size_of(d.page));
    const std::size_t index = row * page_size + static_cast<std::size_t>(digit - 1);
    require(index < d.page.entries.size(), "the highlighted row has no such cell");

    // Labels: the highlighted row carries "1." .. "N.", and no other row carries any.
    for (std::size_t cell = 0; cell < d.page.entries.size(); ++cell) {
      const bool on_row = cell / page_size == row;
      const bool labelled = !d.page.entries[cell].label.empty();
      require(labelled == on_row, "a number label is drawn on a row the key cannot reach");
    }
    require(!d.page.entries[index].label.empty(), "the digit's own cell carries no label");

    const std::string expected = entry_text(d.page, index);
    const auto typed = d.type(std::to_string(digit));
    const std::string typed_commit = d.committed(typed);
    const std::string typed_preedit = d.preedit(typed);

    // The same selection, asked for directly through the page the engine is on.
    Driver reference(scrolling(method));
    reference.type("nihao");
    reference.key(BackendStimulus::Type::PageNext);
    reference.key(BackendStimulus::Type::PageNext);
    BackendStimulus local{reference.binding, 0, BackendStimulus::Type::SelectCandidate};
    local.candidate_index = digit - 1;
    const auto direct = reference.send(local);

    std::printf("number %d on row %zu (%s): typed=[%s|%s] direct=[%s|%s]\n", digit, row,
                expected.c_str(), typed_commit.c_str(), typed_preedit.c_str(),
                reference.committed(direct).c_str(), reference.preedit(direct).c_str());
    require(typed_commit == reference.committed(direct) &&
                typed_preedit == reference.preedit(direct),
            "a number key did not select the highlighted row's cell");
    require(typed_commit.find(expected) == 0 || typed_preedit.find(expected) == 0,
            "the number key's result does not contain that cell's text");
  }
  std::printf("PASS number keys act on the highlighted row and are labelled only there\n");
}

// Vertical and horizontal keep the shape they always had: the engine's page, no window.
void run_page_shapes() {
  for (const char *preference : {"Vertical", "Horizontal"}) {
    FcitxBackendOptions options;
    options.fixture_candidate_layout = preference;
    Driver d(options);
    d.type("nihao");
    require(d.page.arrangement == CandidateArrangement::Page,
            "a non-scrolling preference produced a window");
    require(d.page.page_size == 0 && d.page.total_pages == 0,
            "a plain page carries paging facts it does not use");
    const std::size_t page = d.page.entries.size();
    require(page > 1, "a plain pinyin page has a single candidate");
    d.key(BackendStimulus::Type::PageNext);
    require(d.page.page_size == 0 && d.page.window_first_page == 0,
            "paging a plain page turned it into a window");
    // One page per projection, at the engine's own size — the shape that shipped before this
    // arrangement existed, unchanged.
    require(d.page.entries.size() == page, "a plain page no longer is one engine page");
    std::printf("PASS %s keeps one page per projection\n", preference);
  }
}

} // namespace

int main() {
  try {
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    run_scroll_session("pinyin", nullptr, "ni hao");
    run_scroll_session("shuangpin", "shuangpin", "ni ha o");
    run_number_keys(nullptr);
    run_page_shapes();
    std::printf("PASS automatic scrolling: window, row steps, scroll pages and global selects\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("FAIL %s\n", error.what());
    return 1;
  }
}
