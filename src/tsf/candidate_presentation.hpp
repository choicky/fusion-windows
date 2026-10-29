#pragma once

// The candidate popup's two presentation operations, kept apart from the TSF state that feeds
// them.
//
// A request to re-place the popup — the layout notification, the read-only session it admits,
// and the reposition message that session posts — is a claim about a page that was on screen,
// or deferred for want of an anchor, *when the request was made*. Time passes between those
// points, and a page can be hidden in the meantime for a reason that has nothing to do with
// layout: focus moved, the context was torn down, the composition ended, the projection
// emptied. Since R49 such a page reappears in front of the wrong application: the popup is
// owned and topmost, so a re-show nobody asked for lands above whatever the user is working in
// now, and it is clickable. The fence therefore reads the state at the moment of delivery, and
// both halves of it live here:
//
//   - every product-path hide clears the deferred flag, so a page that is off screen is not
//     left waiting for a layout that would bring it back — which is what service.hpp documents
//     as "cleared by any hide";
//   - a delivered request acts only when `popup_may_be_placed` says it may, and records one
//     line when it refuses, so a log can tell "nothing happened" apart from "something refused".
//
// Both take the popup and the flag rather than a `ContextEntry`, so the code the product runs
// is also the code the check drives: a real window, a real posted `reposition_message`, and the
// same decision on delivery. A check that re-implemented this in its own stub would only be
// testing itself.

#include "tsf/anchor_probe.hpp"
#include "tsf/candidate_view.hpp"
#include "tsf/diag.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace fusion::tsf {

// The one door for a product-path hide: the page is off screen, and nothing is left that would
// put it back. It comes back only when a key path presents it again through
// `sync_candidate_view`. `page_deferred` is the owning entry's flag, passed by reference
// because clearing it is half of what a hide means.
inline void hide_candidate_popup(CandidateView *popup, bool &page_deferred) {
  page_deferred = false;
  if (popup != nullptr)
    popup->hide();
}

// What one delivered re-place decided, and therefore what the caller's log says about it.
enum class RepositionFence {
  kPlaced,   // the page is now on screen at the anchor the probe resolved
  kHidden,   // hidden since the request was made: the fence line names it
  kNoAnchor, // this probe found no anchor, so a deferred page keeps waiting for one
  kNoPage,   // the projection has nothing displayed to place any more
};

// Deliver one re-place against the real popup. The visibility is read from the window itself —
// the request may only act on what it can see — together with the entry's own deferred flag,
// which is what a page waiting for its first anchor has instead of a window on screen. A page
// hidden for any other reason is not this request's business, and says so in one line.
inline RepositionFence place_candidate_popup(CandidateView &popup, bool &page_deferred,
                                             bool anchor_valid, const CandidatePage *page,
                                             POINT anchor, std::uint64_t context_id) {
  if (!popup_may_be_placed(popup.visible(), page_deferred)) {
    char details[96];
    std::snprintf(details, sizeof(details), "kind=reposition hidden cid=%" PRIu64, context_id);
    diag::event("FENCE", details);
    return RepositionFence::kHidden;
  }
  // An anchor that went missing between the re-probe and here is not a reason to move or remove
  // a visible page, and not a reason to stop waiting for one either: the next key path
  // re-decides it through sync_candidate_view.
  if (!anchor_valid)
    return RepositionFence::kNoAnchor;
  if (page == nullptr) {
    // The page went away between the notification and here (a commit, a fenced dispatch): there
    // is nothing left to place, and nothing left to wait for either.
    page_deferred = false;
    return RepositionFence::kNoPage;
  }
  popup.show(*page, anchor);
  // Whether this window was already on screen or is the deferred page being placed for the
  // first time, it is now showing the current page at a resolved anchor.
  page_deferred = false;
  return RepositionFence::kPlaced;
}

} // namespace fusion::tsf
