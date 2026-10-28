#pragma once

// R46: the anchor decision, kept apart from the TSF calls that produce it.
//
// A text store may answer GetTextExt with TS_E_NOLAYOUT for a range it has not laid out
// yet. Measured twice: in Firefox's contenteditable editor (2026-10-20, R25's caret
// fallback) and again in the taskbar search box (2026-10-28), where the probe failed on
// every keystroke while composition and commit worked. Hiding the page for that one
// answer is what made the candidate window vanish mid-word, so the rule is not "show it
// anywhere" but "keep it where this context's text last had a place". That is a decision
// about data the probe already holds, so it lives here; the TSF shell only feeds it.

#include <windows.h>

namespace fusion::tsf {

// The value TextStor.h documents as TS_E_NOLAYOUT: the text store has no layout
// information for this range right now. Spelled out rather than included because the TIP
// does not otherwise pull in the text-store API.
constexpr HRESULT kTsENoLayout = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0206);

// Why a probe ran. A layout notification re-probes for the same rectangle the key path
// asks for, and the log has to say which of the two produced it.
enum class ProbeTrigger { Key, LayoutChange };

// What one probe observed. `has_rect` and `hr` are mutually exclusive by construction:
// the shell fills in whichever the last GetTextExt gave it.
struct AnchorObservation {
  RECT rect{};
  bool has_rect = false;
  HRESULT hr = S_OK;
  BOOL clipped = FALSE;
  // The range the rectangle (or the failure) came from: composition, selection or
  // composition-caret. Reported in the log, so a line names its own source.
  const char *source = "selection";
};

// The last successful probe of this context, if there was one.
struct LastGoodAnchor {
  POINT point{};
  bool valid = false;
};

// What to do with the page.
struct AnchorDecision {
  bool placed = false;
  POINT anchor{};
  // The `source=` the log reports: the range for a fresh rectangle on a key probe,
  // "layout-change" for a fresh one a layout notification produced, "last-good" for the
  // retained anchor.
  const char *source = "";
  // The `range=` the log adds when it says something `source` no longer does — a
  // layout-change reprobe that resolved a rectangle still has to report which range it
  // came from. Null when the two would be the same word.
  const char *range = nullptr;
  // Whether the anchor is worth remembering as this context's last good one.
  bool becomes_last_good = false;
  // The page is being kept at a previous probe's place rather than a measured one. Reported
  // apart because the geometry this line can quote is a stored point, not a RECT.
  bool retained = false;
  // A probe that did not place the page but failed with TS_E_NOLAYOUT is the R46 case;
  // the shell logs the other failures exactly as it did before this rule existed.
  bool no_layout = false;
};

// The anchor is the bottom-right of the resolved rectangle: the popup opens below the
// text it belongs to, which is what every placement in this surface assumes.
inline POINT anchor_of(const RECT &rect) { return POINT{rect.right, rect.bottom}; }

// The one rule: a resolved rectangle wins and becomes this context's last-good anchor;
// TS_E_NOLAYOUT with a previous successful probe keeps the page at that anchor; anything
// else defers it, and the caller hides the page rather than placing it on a guess.
inline AnchorDecision decide_anchor(const AnchorObservation &obs, const LastGoodAnchor &last_good,
                                    ProbeTrigger trigger) {
  AnchorDecision decision;
  decision.no_layout = obs.hr == kTsENoLayout;
  if (obs.has_rect) {
    decision.placed = true;
    decision.anchor = anchor_of(obs.rect);
    decision.source = trigger == ProbeTrigger::LayoutChange ? "layout-change" : obs.source;
    decision.range = trigger == ProbeTrigger::LayoutChange ? obs.source : nullptr;
    decision.becomes_last_good = true;
    return decision;
  }
  if (decision.no_layout && last_good.valid) {
    decision.placed = true;
    decision.retained = true;
    decision.anchor = last_good.point;
    decision.source = "last-good";
  }
  return decision;
}

} // namespace fusion::tsf
