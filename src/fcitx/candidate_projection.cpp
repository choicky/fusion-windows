#include "candidate_projection.hpp"

#include <fcitx-utils/utf8.h>
#include <fcitx/inputpanel.h>

#include "fusion/text_utf16.hpp"
#include <utility>

namespace fusion {

std::optional<CandidatePage>
project_candidate_page(fcitx::Instance &instance, fcitx::InputContext *context,
                       const std::shared_ptr<fcitx::CandidateList> &list,
                       CandidateLayout preferred_layout, std::size_t entry_bound,
                       std::size_t byte_bound, std::size_t field_bound) {
  CandidatePage page;
  page.cursor_index = list->cursorIndex();
  // The page carries the *effective* layout: the engine's hint when it names one, the
  // user's stored preference when it does not. The renderer therefore never has to know
  // about the preference, and no stored value can be silently overwritten by a hint.
  page.layout = resolve_candidate_layout(
      static_cast<CandidateLayout>(static_cast<std::uint8_t>(list->layoutHint())),
      preferred_layout);
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
