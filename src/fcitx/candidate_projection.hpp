#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>

#include "fusion/backend.hpp"

namespace fusion {

// Flattens the engine's current candidate page into the transport CandidatePage
// exactly the way a mature Fcitx frontend prepares candidate display data.
// Classic UI passes label, candidate text and comment through
// Instance::outputFilter before rendering them (src/ui/classic/inputwindow.cpp
// at pinned revision a7f2528); that engine-side filtering belongs to this
// projection as well — the display data stays Fcitx's (D082), and Windows adds
// no filtering authority of its own. With no OutputFilter consumer connected
// the engine call is the identity, so an unfiltered engine page projects
// unchanged.
//
// Returns nullopt when a display field fails the transport validity checks;
// the caller then invalidates the batch rather than shipping a malformed page.
// entry_bound/byte_bound/field_bound are engineering transport bounds, not a
// semantic page size; cursor normalization keeps a prefix truncated by a bound
// self-consistent.
//
// What the user asked the Windows window to do, and — when it asks for scrolling — how
// many engine pages one window may hold. The preference is applied only when the engine
// set no hint; see resolve_candidate_layout.
struct CandidateProjectionOptions {
  CandidateLayout layout = CandidateLayout::Vertical;
  bool auto_scroll = false;
  // The window's tallest shape, in engine pages (one page = one row). Five is the
  // owner-confirmed maximum; the renderer shows fewer when the work area cannot hold them.
  int max_scroll_rows = 5;
};

std::optional<CandidatePage>
project_candidate_page(fcitx::Instance &instance, fcitx::InputContext *context,
                       const std::shared_ptr<fcitx::CandidateList> &list,
                       const CandidateProjectionOptions &options, std::size_t entry_bound,
                       std::size_t byte_bound, std::size_t field_bound);

// Which engine pages a scroll window shows, given the list's own paging state.
//
// This is a pure function of the engine's numbers — no session state on the Windows side,
// nothing remembered between pages — which is what keeps one authority: the highlighted
// row *is* the engine's current page, the rows above it are the pages already turned, and
// a new list starts from its own first page again (usedNextBefore false). The rules are
// the owner-confirmed ones:
//
//   * nothing turned yet, or back on the first page → one row (the session's first line);
//   * after that, the window is one scroll page of up to `max_rows` rows, starting after
//     that first line, so the line already passed is not shown again;
//   * a list whose remaining rows fit in one row stays one row.
struct CandidateScrollWindow {
  bool active = false; // false = the page keeps its ordinary one-row/one-column shape
  int page_size = 0;
  int current_page = 0;
  int first_page = 0;
  int rows = 0;
  int total_pages = 0;
};
CandidateScrollWindow plan_scroll_window(int page_size, int current_page, int total_pages,
                                         bool used_next_before, int max_rows, int entry_bound);

// The effective layout of one projected page: an engine hint that names a layout wins,
// and the stored preference applies only when the hint is NotSet.
//
// This is classicui's own rule, read from the pinned source rather than assumed
// (src/ui/classic/inputwindow.cpp:598-603 and again at 938-943, fcitx5 a7f2528):
//
//     bool vertical = config().verticalCandidateList.value();
//     if (layoutHint_ == CandidateLayoutHint::Vertical) vertical = true;
//     else if (layoutHint_ == CandidateLayoutHint::Horizontal) vertical = false;
//
// So the preference is a default and a semantic hint overrides it. Windows adopts the
// rule verbatim: it neither invents a precedence nor silently overrides a hint an engine
// set on purpose (the clipboard, unicode and imselector modes all request a vertical
// list that must keep working). Resolving here — the single place the hint is read —
// is also what makes "saves successfully but the renderer ignores it" impossible: the
// page carries the effective layout, not a value the renderer would overwrite.
constexpr CandidateLayout resolve_candidate_layout(CandidateLayout hint,
                                                   CandidateLayout preferred_layout) {
  return hint == CandidateLayout::NotSet ? preferred_layout : hint;
}

// The displayed client preedit, exactly the way the dbus client-side frontend
// prepares it (src/frontend/dbusfrontend/dbusfrontend.cpp updatePreeditImpl at
// the pinned revision): Instance::outputFilter applied to the InputPanel's own
// clientPreedit() Text (the engine identity-checks that reference, e.g. for
// password masking), normalized, then flattened into the text plus its
// segment/format list and byte cursor. A cursor outside the text or inside a
// code point is shown as hidden, the classic-ui display rule for an unusable
// cursor. nullopt when the text is not valid UTF-8 or exceeds a bound.
struct ProjectedPreedit {
  std::string text;
  FormattedPreedit format;
};
std::optional<ProjectedPreedit> project_client_preedit(fcitx::Instance &instance,
                                                       fcitx::InputContext *context,
                                                       std::size_t byte_bound,
                                                       std::size_t segment_bound);

// Fill the panel text rows of `page` the way classic-ui does
// (src/ui/classic/inputwindow.cpp InputWindow::update): auxUp, the server-side
// preedit() and auxDown, each through Instance::outputFilter, only when the
// InputPanel is not empty. false when a field is not valid UTF-8 or exceeds
// field_bound.
bool project_panel_text(fcitx::Instance &instance, fcitx::InputContext *context,
                        CandidatePage &page, std::size_t field_bound);

} // namespace fusion
