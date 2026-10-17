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
// D072 §16 stays UNFROZEN on placement. This specialized in-process surface is
// the first seam only because it needs no anchor-over-IPC and keeps the Host
// headless; it is not a freeze of that boundary.

#include "fusion/backend.hpp"
#include "tsf/candidate_layout.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace fusion::tsf {

class TextService;

// One popup window per live context, created lazily on the owning UI thread.
// Mouse messages arrive through the application's own message loop (same thread,
// serialized with key handling), so no private pump is needed. The back-pointer
// to the service and the entry identity+generation are checked by the caller on
// every routed action, reusing the stale-entry fence the edit sessions use.
class CandidateView {
public:
  CandidateView() = default;
  ~CandidateView();
  CandidateView(const CandidateView &) = delete;
  CandidateView &operator=(const CandidateView &) = delete;

  // Create the popup the first time a page must be shown. Ownership of the HWND
  // stays with this object; DestroyWindow happens in destroy()/the destructor.
  bool ensure_created(TextService *service, void *entry, std::uint64_t generation);

  // Replace the whole displayed panel (STATE projection): panel text rows and the
  // candidate page. A panel with nothing to display (panel_empty()) hides.
  void show(const CandidatePage &page, POINT anchor);
  void hide();
  void destroy();
  bool visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }

private:
  static bool register_window_class();
  static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT on_message(UINT message, WPARAM wparam, LPARAM lparam);
  void relayout_and_paint();
  void draw(HDC dc);
  HFONT font();

  // Map a client point to a page-local entry index (select), or a pager request.
  // Returns the action through the out parameters; false means no actionable hit.
  bool hit_test(POINT client, int &action, std::int32_t &index) const;

  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  TextService *service_ = nullptr;
  void *entry_ = nullptr; // ContextEntry identity; validated by the service
  std::uint64_t generation_ = 0;
  CandidatePage page_;
  // Geometry from layout_candidate_panel: slot rects aligned to page_.entries
  // indices (a placeholder keeps its index with an empty, non-hit rect, so
  // select() is never shifted off the engine's own list index), pager arrows and
  // the panel text rows.
  CandidatePanelLayout layout_;
};

} // namespace fusion::tsf
