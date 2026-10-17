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
std::optional<CandidatePage>
project_candidate_page(fcitx::Instance &instance, fcitx::InputContext *context,
                       const std::shared_ptr<fcitx::CandidateList> &list, std::size_t entry_bound,
                       std::size_t byte_bound, std::size_t field_bound);

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
