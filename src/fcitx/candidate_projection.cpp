#include "candidate_projection.hpp"

#include <fcitx-utils/utf8.h>
#include <utility>

namespace fusion {

std::optional<CandidatePage>
project_candidate_page(fcitx::Instance &instance, fcitx::InputContext *context,
                       const std::shared_ptr<fcitx::CandidateList> &list, std::size_t entry_bound,
                       std::size_t byte_bound, std::size_t field_bound) {
  CandidatePage page;
  page.cursor_index = list->cursorIndex();
  page.layout = static_cast<CandidateLayout>(static_cast<std::uint8_t>(list->layoutHint()));
  if (auto *pager = list->toPageable()) {
    page.has_prev = pager->hasPrev();
    page.has_next = pager->hasNext();
  }
  const int visible = list->size();
  std::size_t used = 0;
  for (int index = 0; index < visible; ++index) {
    if (page.entries.size() >= entry_bound) {
      page.truncated = true;
      break;
    }
    const auto &word = list->candidate(index);
    CandidateEntry entry;
    entry.placeholder = word.isPlaceHolder();
    // The engine's own display filtering, the same per-field call the mature
    // frontend makes; it must not be applied twice, and it must not be skipped.
    entry.text = instance.outputFilter(context, word.text()).toString();
    entry.comment = instance.outputFilter(context, word.comment()).toString();
    // Labels are page-local and only padded to ten; an engine with a wider
    // page has no label for the extra slots rather than a different candidate.
    if (index < 10) {
      entry.label = instance.outputFilter(context, list->label(index)).toString();
    }
    used += entry.label.size() + entry.text.size() + entry.comment.size();
    if (used > byte_bound) {
      page.truncated = true;
      break;
    }
    for (const auto &field : {entry.label, entry.text, entry.comment}) {
      if (!field.empty() && !fcitx::utf8::validate(field))
        return std::nullopt;
      if (field.size() > field_bound)
        return std::nullopt;
    }
    page.entries.push_back(std::move(entry));
  }
  // A transport bound may have dropped the entry the engine cursor referred
  // to. Normalize the projection to the retained prefix before the page goes
  // on the wire (presentation only; the engine keeps its own cursor).
  page.normalize_cursor_to_retained_prefix();
  return page;
}

} // namespace fusion
