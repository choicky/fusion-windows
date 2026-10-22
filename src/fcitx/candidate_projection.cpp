#include "candidate_projection.hpp"

#include <fcitx-utils/utf8.h>
#include <fcitx/inputpanel.h>

#include "fusion/text_utf16.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fusion {
namespace {

// Reads one candidate of the window at engine-global index `global`. The candidate on the
// engine's own page is read through the list the way every other projection does; the ones
// on the pages around it come from the list's bulk view, which is the interface the engine
// offers precisely for "a candidate outside the current page" (fcitx::BulkCandidateList,
// candidatelist.h). Nothing is paged to reach them and nothing is cached: the window is
// rebuilt from the live list on every projection.
//
// `nullptr` means the engine refused or does not offer that index — a partial window is
// reported as truncated rather than silently shifting the remaining candidates into
// positions they do not have, because a shifted position would name a different candidate
// than the one the engine would commit.
const fcitx::CandidateWord *window_candidate(const std::shared_ptr<fcitx::CandidateList> &list,
                                             int global, int page_start) {
  const int local = global - page_start;
  if (local >= 0 && local < list->size())
    return &list->candidate(local);
  auto *bulk = list->toBulk();
  if (bulk == nullptr)
    return nullptr;
  const int total = bulk->totalSize();
  if (total >= 0 && global >= total)
    return nullptr;
  try {
    return &bulk->candidateFromAll(global);
  } catch (const std::exception &) {
    // A lazily generated list may raise for an index it cannot produce yet; the window
    // stops there instead of presenting an index the engine cannot address.
    return nullptr;
  }
}

} // namespace

CandidateScrollWindow plan_scroll_window(int page_size, int current_page, int total_pages,
                                         bool used_next_before, int max_rows, int entry_bound) {
  CandidateScrollWindow window;
  if (page_size <= 0 || current_page < 0 || total_pages <= 0 || max_rows <= 0 ||
      current_page >= total_pages)
    return window;
  // A single page must fit the transport on its own, or there is no window to speak of and
  // the caller keeps the ordinary page shape.
  const int rows_that_fit =
      static_cast<int>(entry_bound) / page_size > 0 ? static_cast<int>(entry_bound) / page_size : 0;
  if (rows_that_fit < 1)
    return window;

  int first = current_page;
  int rows = 1;
  if (used_next_before && current_page > 0) {
    // The session's first line (page 0) was the one-row view before anything was turned, so
    // the scroll page starts after it: rows are grouped as 1..max_rows, max_rows+1.., and
    // the page already passed is not shown again.
    first = 1 + ((current_page - 1) / max_rows) * max_rows;
    rows = (std::min)(max_rows, total_pages - first);
  }
  if (rows < 1 || first >= total_pages || current_page < first ||
      current_page >= first + rows) {
    // Defensive: an engine whose page arithmetic does not match the grouping still gets a
    // window that contains the row it is on, rather than a wrong one.
    first = current_page;
    rows = 1;
  }
  rows = (std::min)(rows, rows_that_fit);
  if (current_page >= first + rows) {
    first = current_page;
    rows = 1;
  }

  window.active = true;
  window.page_size = page_size;
  window.current_page = current_page;
  window.first_page = first;
  window.rows = rows;
  window.total_pages = total_pages;
  return window;
}

std::optional<CandidatePage>
project_candidate_page(fcitx::Instance &instance, fcitx::InputContext *context,
                       const std::shared_ptr<fcitx::CandidateList> &list,
                       const CandidateProjectionOptions &options, std::size_t entry_bound,
                       std::size_t byte_bound, std::size_t field_bound) {
  CandidatePage page;
  page.cursor_index = list->cursorIndex();
  // The page carries the *effective* layout: the engine's hint when it names one, the
  // user's stored preference when it does not. The renderer therefore never has to know
  // about the preference, and no stored value can be silently overwritten by a hint.
  const auto hint = static_cast<CandidateLayout>(static_cast<std::uint8_t>(list->layoutHint()));
  page.layout = resolve_candidate_layout(hint, options.layout);
  if (auto *pager = list->toPageable()) {
    page.has_prev = pager->hasPrev();
    page.has_next = pager->hasNext();
  }

  // A window of several engine pages is planned only when the user asked for it and the
  // engine left the arrangement to us. A hint keeps its precedence over every preference
  // (resolve_candidate_layout), so a list that asks for a column is not turned into a grid.
  CandidateScrollWindow window;
  if (options.auto_scroll && hint == CandidateLayout::NotSet) {
    if (auto *common = dynamic_cast<fcitx::CommonCandidateList *>(list.get())) {
      // The rows a scroll page holds are what the window can actually show, not a constant:
      // a short screen that shows three rows turns pages of three, so stepping off the last
      // visible row really is the next scroll page's first row.
      const int rows = options.max_scroll_rows > 0 ? options.max_scroll_rows : 1;
      window = plan_scroll_window(common->pageSize(), common->currentPage(),
                                  common->totalPages(), common->usedNextBefore(), rows,
                                  static_cast<int>(entry_bound));
      if (window.active) {
        // Pinyin resets its cursor to the page's first column on every page turn, which is
        // the classic UI's convention and not this one: the confirmed behaviour for the
        // scrolling arrangement is to keep the column and clamp on a short row. The engine
        // still owns the cursor — this only tells it which of its own policies to apply, and
        // `SameAsLast` is exactly "same index, or the last candidate when the page is
        // shorter" (candidatelist.cpp, fixCursorAfterPaging). A list that is not paged has no
        // column to keep, and every other arrangement keeps the engine's own choice.
        common->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::SameAsLast);
      }
    }
  }

  std::size_t used = 0;
  enum class Appended { Ok, OverByteBound, Invalid };
  const auto append = [&](const fcitx::CandidateWord &word, int label_index) -> Appended {
    CandidateEntry entry;
    entry.placeholder = word.isPlaceHolder();
    // The engine's own display filtering, the same per-field call the mature
    // frontend makes; it must not be applied twice, and it must not be skipped.
    entry.text = instance.outputFilter(context, word.text()).toString();
    entry.comment = instance.outputFilter(context, word.comment()).toString();
    // Labels are page-local and only padded to ten; an engine with a wider
    // page has no label for the extra slots rather than a different candidate. A
    // candidate of another page has no label at all: its number is an engine number for
    // the page the paging keys are on, and drawing it here would claim a key that does
    // not select it.
    if (label_index >= 0 && label_index < 10)
      entry.label = instance.outputFilter(context, list->label(label_index)).toString();
    used += entry.label.size() + entry.text.size() + entry.comment.size();
    if (used > byte_bound)
      return Appended::OverByteBound;
    for (const auto &field : {entry.label, entry.text, entry.comment}) {
      if (!field.empty() && !fcitx::utf8::validate(field))
        return Appended::Invalid;
      if (field.size() > field_bound)
        return Appended::Invalid;
    }
    page.entries.push_back(std::move(entry));
    return Appended::Ok;
  };
  const auto stop_if_bounded = [&](Appended result) {
    if (result == Appended::Invalid)
      return false;
    if (result == Appended::OverByteBound)
      page.truncated = true;
    return result == Appended::Ok;
  };

  if (window.active) {
    // A window is published only when it is complete enough to be *the* page: contiguous from
    // its first page, and containing every candidate of the engine's own page. Half a window
    // is worse than no window — the numbers on the engine's row would select candidates the
    // user cannot see — so a window that cannot be built inside the transport's bounds is
    // abandoned and the ordinary page projection takes over.
    auto *bulk = list->toBulk();
    const int total = bulk != nullptr ? bulk->totalSize() : -1;
    const int page_start = window.current_page * window.page_size;
    const int last_page = (std::min)(window.total_pages - 1, window.first_page + window.rows - 1);
    int window_end = (last_page + 1) * window.page_size;
    if (total >= 0)
      window_end = (std::min)(window_end, total); // a short last page is not a failure
    const int current_end = page_start + (std::min)(list->size(), window.page_size);

    int built_end = window.first_page * window.page_size;
    bool failed = false;
    for (int global = window.first_page * window.page_size; global < window_end; ++global) {
      if (page.entries.size() >= entry_bound) {
        failed = true;
        break;
      }
      const auto *word = window_candidate(list, global, page_start);
      if (word == nullptr) {
        failed = true;
        break;
      }
      const int local = global - page_start;
      const auto result = append(*word, local >= 0 && local < list->size() ? local : -1);
      if (result == Appended::Invalid)
        return std::nullopt;
      if (result != Appended::Ok) {
        failed = true; // a byte budget that cannot even hold the current page
        break;
      }
      built_end = global + 1;
    }
    if (failed || built_end < current_end || built_end <= window.first_page * window.page_size) {
      // Not a usable window. Fall through to the page shape this product has always sent,
      // which has its own (and older) truncation semantics.
      page.entries.clear();
      page.truncated = false;
      used = 0; // the page projection starts its own budget, not a spent one
      window = CandidateScrollWindow{};
      page.cursor_index = list->cursorIndex();
    } else {
      page.arrangement = CandidateArrangement::Scroll;
      page.page_size = window.page_size;
      page.current_page = window.current_page;
      page.window_first_page = window.first_page;
      page.total_pages = window.total_pages;
    }
  }
  if (!window.active) {
    const int visible = list->size();
    for (int index = 0; index < visible; ++index) {
      if (page.entries.size() >= entry_bound) {
        page.truncated = true;
        break;
      }
      if (!stop_if_bounded(append(list->candidate(index), index))) {
        if (page.truncated)
          break;
        return std::nullopt;
      }
    }
  }
  // The engine cursor is page-local; in a window it moves to where that page's slot is.
  if (page.arrangement == CandidateArrangement::Scroll && page.cursor_index >= 0) {
    const long long moved = static_cast<long long>(window.current_page - window.first_page) *
                                window.page_size +
                            page.cursor_index;
    page.cursor_index = moved >= 0 && moved < static_cast<long long>(page.entries.size())
                            ? static_cast<std::int32_t>(moved)
                            : -1;
  }
  // A transport bound may have dropped the entry the engine cursor referred
  // to. Normalize the projection to the retained prefix before the page goes
  // on the wire (presentation only; the engine keeps its own cursor).
  page.normalize_cursor_to_retained_prefix();
  return page;
}

std::optional<ProjectedPreedit> project_client_preedit(fcitx::Instance &instance,
                                                       fcitx::InputContext *context,
                                                       std::size_t byte_bound,
                                                       std::size_t segment_bound) {
  // Pass the panel's own Text: outputFilter compares the reference with
  // clientPreedit() to decide engine-side masking.
  const fcitx::Text filtered =
      instance.outputFilter(context, context->inputPanel().clientPreedit()).normalize();
  ProjectedPreedit result;
  if (filtered.size() > segment_bound)
    return std::nullopt;
  for (int index = 0, count = static_cast<int>(filtered.size()); index < count; ++index) {
    const std::string &piece = filtered.stringAt(index);
    result.text += piece;
    result.format.segments.push_back(
        {static_cast<std::uint32_t>(piece.size()),
         static_cast<std::uint32_t>(filtered.formatAt(index).toInteger()) & text_format::known});
  }
  if (result.text.size() > byte_bound ||
      (!result.text.empty() && !fcitx::utf8::validate(result.text)))
    return std::nullopt;
  const int cursor = filtered.cursor();
  if (cursor >= 0 && utf16_offset_of_utf8_byte(result.text, static_cast<std::size_t>(cursor)))
    result.format.cursor = cursor;
  return result;
}

bool project_panel_text(fcitx::Instance &instance, fcitx::InputContext *context,
                        CandidatePage &page, std::size_t field_bound) {
  auto &panel = context->inputPanel();
  if (panel.empty())
    return true; // nothing displayed; the rows stay empty
  page.aux_up = instance.outputFilter(context, panel.auxUp()).toString();
  page.panel_preedit = instance.outputFilter(context, panel.preedit()).toString();
  page.aux_down = instance.outputFilter(context, panel.auxDown()).toString();
  for (const auto *field : {&page.aux_up, &page.panel_preedit, &page.aux_down}) {
    if (field->size() > field_bound || (!field->empty() && !fcitx::utf8::validate(*field)))
      return false;
  }
  return true;
}

} // namespace fusion
