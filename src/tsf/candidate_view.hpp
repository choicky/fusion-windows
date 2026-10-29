#pragma once

// Stage 5 minimal candidate presentation surface (D072 §16 seam choice).
//
// This is a non-activating, owned popup that renders the SemanticController's
// projected CandidatePage and turns a pointer hit back into an explicit
// non-key candidate action. It is a pure Windows materialization/UI-integration
// object: it owns no semantic state, no engine and no second projection. The
// page it displays is display text handed to it; a click reports a page-local
// index (or a pager request) and nothing more.
//
// The font it draws with comes from the page (CandidateFont), which the Host projects
// from the one candidate-presentation configuration; this object keeps no copy of the
// setting and reads no configuration of its own. Everything geometric is recomputed per
// show from the target monitor's DPI, so moving between displays relayouts rather than
// scaling an old result.
//
// D072 §16 stays UNFROZEN on placement. This specialized in-process surface is
// the first seam only because it needs no anchor-over-IPC and keeps the Host
// headless; it is not a freeze of that boundary.

#include "fusion/backend.hpp"
#include "tsf/candidate_layout.hpp"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace fusion::tsf {

class TextService;

// One popup window per live context, created lazily on the owning UI thread.
// Mouse messages arrive through the application's own message loop (same thread,
// serialized with key handling), so no private pump is needed. The back-pointer
// to the service and the entry identity+generation are checked by the caller on
// every routed action, reusing the stale-entry fence the edit sessions use.
//
// R49: the popup is *owned* by the application window the context's view reports and is
// topmost. Created with no owner and raised with HWND_TOP it could sit behind the very
// surface it belongs to — measured in the taskbar search box, where every anchor probe
// succeeded and nothing was visible because the input flyout covered it. The owner is what
// makes it follow that window (activation, minimisation, destruction) instead of drifting
// on the desktop; topmost is what makes it appear above it. Same for every application.
class CandidateView {
public:
  CandidateView() = default;
  ~CandidateView();
  CandidateView(const CandidateView &) = delete;
  CandidateView &operator=(const CandidateView &) = delete;

  // Create the popup the first time a page must be shown, owned by `owner` (the
  // application window this context's text is in — for a WS_POPUP the CreateWindowExW
  // parent slot means owner, not parent). Ownership of the HWND stays with this object;
  // DestroyWindow happens in destroy()/the destructor.
  //
  // An owner that changed since this window was made, or that is gone, is not reused: the
  // window is recreated under the current owner. Windows destroys an owned popup together
  // with its owner, so a closed application window arrives here as this window's own
  // WM_NCDESTROY, and `hwnd_` goes with it — a dangling handle is never used.
  bool ensure_created(TextService *service, void *entry, std::uint64_t generation, HWND owner,
                      bool owner_from_view);

  // Replace the whole displayed panel (STATE projection): panel text rows and the
  // candidate page. A panel with nothing to display (panel_empty()) hides.
  void show(const CandidatePage &page, POINT anchor);
  void hide();
  void destroy();
  bool visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }

  // The popup's real window handle, null until ensure_created succeeds. Exposed so a
  // check can read the window's actual rectangle and extended style instead of
  // re-deriving the geometry it is supposed to be verifying; nothing in the product
  // path uses it.
  HWND window() const { return hwnd_; }

  // The owner this window was created under, and whether it came from the context's view
  // (rather than the thread focus). Read by the owner-lifetime check for the same reason
  // `window()` is; the product path never asks.
  HWND owner() const { return owner_; }
  bool owner_from_view() const { return owner_from_view_; }

  // The message a layout notification leaves for the popup: re-place yourself from the
  // anchor the last probe resolved. Posted, never sent, so the window call happens on the
  // application's message loop and not inside an edit session. Public for the same reason
  // `window()` is — a check can drive it without waiting for TSF to raise the change.
  static constexpr UINT reposition_message = WM_APP + 1;
  void request_reposition();

  // The measured text height the current font produces at the current DPI, and the
  // geometry that was laid out from it. Read by the geometry checks for the same reason
  // window() is: a test that re-measured with its own font would be checking itself.
  int line_height() const { return line_height_; }
  int dpi() const { return dpi_; }
  const CandidatePanelLayout &layout() const { return layout_; }

  // The full-text popup's window handle, null while no clipped slot is being shown.
  // Exposed for the same reason `window()` is; the product path never touches it.
  HWND tip_window() const { return tip_hwnd_; }
  // The timer identity used for the delayed close. A check that has no message pump drives
  // the delay by posting that WM_TIMER itself instead of waiting for it; the product path
  // only ever receives the real timer.
  static constexpr UINT_PTR tip_close_timer_id = 1;
  // The width the full text was wrapped and measured at, which is also the width it is drawn
  // at (client width minus padding). A check re-measures the same text with its own GDI
  // measurement at this width, so "the end is reachable" is not proven by comparing the
  // popup's own counters with each other.
  int tip_wrap_width() const { return tip_wrap_width_; }
  // How far the full-text popup is scrolled, and how far it can go. A test can therefore
  // check that the end of a long text is actually reachable instead of only that a window
  // exists; the product path reads neither.
  int tip_scroll() const { return tip_scroll_; }
  int tip_scroll_max() const { return tip_scroll_max_; }
  bool tip_scrollable() const { return tip_scroll_max_ > 0; }
  // The full text's wrapped height and the visible height, so a check can state "the end is
  // inside the box at maximum scroll" instead of asserting that a window merely exists.
  // How many complete candidate rows the panel can currently draw: what the Host needs to
  // group scroll pages the way this window draws them. 0 before the first layout.
  int row_capacity() const { return layout_.row_capacity; }
  int tip_content_height() const { return tip_content_height_; }
  int tip_view_height() const { return tip_view_height_; }

private:
  static bool register_window_class();
  static bool register_tip_class();
  static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK tip_window_proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT on_message(UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT on_tip_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void relayout_and_paint();
  // Measure the page with the current font and DPI and lay the panel out against the work
  // area. Separate from show() because a DPI correction has to redo exactly this.
  void reflow();
  void draw(HDC dc);
  // The handle for `requested` at `dpi`, recreated when the request differs from what the
  // current handle was made for. Falls back to the default GUI font if creation fails.
  HFONT font_for(const CandidateFont &requested, int dpi);
  // The monitor the popup is placed on: its work area and DPI.
  void resolve_target(POINT anchor);
  // Place the window at the anchor, inside the work area (above it when there is no room
  // below), and paint.
  void place_and_paint();
  // Show (or hide) the full text of a clipped slot. `index` is a page-local slot index or
  // -1 for "nothing to show".
  void update_tip(std::int32_t index);
  void destroy_tip();
  // The accessibility announcements of a candidate surface: shown, hidden, or moved while
  // shown. Screen readers and the focus-tracking machinery a modern application runs on
  // listen for these; a popup that never raises them is invisible to them even when it is
  // visible on screen.
  void announce_shown(bool was_visible);
  void announce_hidden();
  // The one content-free line per show: owner, how the owner was found, the placed rect,
  // and whether this thread is in TSF's immersive mode. The immersive flag is recorded and
  // nothing is decided from it.
  void note_shown();
  // The pointer leaving one of the two windows does not close the full text at once: it can
  // be on its way to the other one across the gap the placement leaves between them. A short
  // timer is armed instead, and the cursor's position decides what happens when it fires.
  void arm_tip_close();
  void cancel_tip_close();
  void on_tip_close_timer();

  // Map a client point to a page-local entry index (select), or a pager request.
  // Returns the action through the out parameters; false means no actionable hit.
  bool hit_test(POINT client, int &action, std::int32_t &index) const;

  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  bool font_owned_ = false; // a stock font must not be deleted
  // What the current handle was created for. A page whose font or DPI differs recreates
  // it instead of reusing a handle that no longer matches the setting.
  std::string font_family_;
  int font_size_pt_ = 0;
  int font_dpi_ = 0;
  int line_height_ = 20;
  int dpi_ = 96;
  RECT work_area_{};
  POINT anchor_{};
  TextService *service_ = nullptr;
  void *entry_ = nullptr; // ContextEntry identity; validated by the service
  std::uint64_t generation_ = 0;
  // The application window this popup belongs to, and whether the context's view reported
  // it or the thread focus stood in. Cleared with the window: an owner that dies takes this
  // popup with it, and the next show creates a window under the owner it has now.
  HWND owner_ = nullptr;
  bool owner_from_view_ = false;
  CandidatePage page_;
  // Geometry from layout_candidate_panel: slot rects aligned to page_.entries
  // indices (a placeholder keeps its index with an empty, non-hit rect, so
  // select() is never shifted off the engine's own list index), pager arrows and
  // the panel text rows.
  CandidatePanelLayout layout_;
  // The non-activating sibling that shows a clipped candidate's full text. Created on
  // first need; it never takes focus, never selects, and is click-through.
  HWND tip_hwnd_ = nullptr;
  std::int32_t tip_index_ = -1;
  std::wstring tip_text_;
  // The full-text popup is bounded by the work area; a text longer than that is scrolled
  // rather than ellipsized, so its end is reachable instead of merely declared present.
  int tip_scroll_ = 0;
  int tip_scroll_max_ = 0;
  int tip_content_height_ = 0;
  int tip_view_height_ = 0;
  int tip_padding_ = 0;
  int tip_line_height_ = 0;
  int tip_wrap_width_ = 0;
  // The delayed close: whether a WM_TIMER is outstanding, and how many times it has been
  // re-armed while the pointer stayed in the gap between the two windows. The re-arm count is
  // bounded so a pointer parked between them cannot keep a stale box on screen.
  bool tip_close_pending_ = false;
  int tip_close_rearms_ = 0;
  bool tracking_leave_ = false;
  // The full-text box, once it takes the pointer (a scrollable text), tracks its own leave so
  // it can hide when the pointer finally leaves it — not merely when it leaves the panel.
  bool tip_tracking_leave_ = false;
  // WM_DPICHANGED runs a full re-measure that calls SetWindowPos, which can raise another
  // one; the guard keeps a single re-layout from recursing into itself.
  bool in_dpi_change_ = false;
};

} // namespace fusion::tsf
