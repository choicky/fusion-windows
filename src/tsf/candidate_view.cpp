#include "tsf/candidate_view.hpp"

#include "tsf/diag.hpp"
#include "tsf/semantic.hpp"
#include "tsf/service.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

namespace fusion::tsf {
namespace {

const wchar_t *const kWindowClassName = L"FusionCandidateWindow";
const wchar_t *const kTipClassName = L"FusionCandidateTip";

// What a page that carries no font is drawn with (Mock and unit-test pages). These are
// the candidate-presentation schema's own defaults; the popup names them again because a
// page without a font has to be drawable, not because it keeps a second preference.
constexpr char kDefaultFamily[] = "Microsoft YaHei UI";
constexpr int kDefaultSizePt = 11;
// The schema's bounds. The wire rejects anything further out; these keep the popup's own
// arithmetic sane when it is handed a page by something other than this Host.
constexpr int kMinSizePt = 6;
constexpr int kMaxSizePt = 48;

// How long the full-text box survives the pointer leaving one of the two windows. Long
// enough to cross the gap between them at any ordinary pointer speed, short enough that a
// pointer that really went elsewhere does not leave a stale box behind.
constexpr UINT kTipCloseDelayMs = 150;
// While the pointer stays *between* the two windows the delay is re-armed at most this many
// times, so the box can be crossed slowly but cannot be parked on screen forever.
constexpr int kTipCloseMaxRearms = 6;

std::wstring to_wide(const std::string &text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          result.data(), count) != count)
    return {};
  return result;
}

// One content-free structured line, formatted here because `diag::event` takes the finished
// string.
void diagf(const char *name, const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  diag::event(name, buffer);
}

RECT to_rect(const LayoutRect &rect) { return RECT{rect.left, rect.top, rect.right, rect.bottom}; }

// The width the entry text is wrapped and measured at, with the same call the painting code
// makes: one width, one wrapping rule, one font. `DT_CALCRECT` with `DT_WORDBREAK` returns
// exactly the height `DrawTextW` would occupy for the same string in the same width.
int wrapped_height(HDC dc, const std::wstring &text, int width) {
  RECT cell{0, 0, (std::max)(1, width), 0};
  DrawTextW(dc, text.c_str(), -1, &cell, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  return cell.bottom - cell.top;
}

int scaled(int value, int dpi) { return MulDiv(value, dpi, 96); }

int system_dpi() {
  HDC dc = GetDC(nullptr);
  const int dpi = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSY) : 0;
  if (dc != nullptr)
    ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi : 96;
}

// The DPI of the monitor the popup is being placed on. GetDpiForWindow would describe the
// monitor the window is on *now*, which is the old one while it is being moved to another
// display, so the monitor is resolved from the anchor first. A process that is not
// per-monitor aware gets its system DPI here, which is exactly what it draws at.
UINT dpi_for_monitor(HMONITOR monitor) {
  using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
  static const GetDpiForMonitorFn get_dpi_for_monitor = [] {
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    return shcore != nullptr ? reinterpret_cast<GetDpiForMonitorFn>(
                                   GetProcAddress(shcore, "GetDpiForMonitor"))
                             : nullptr;
  }();
  if (get_dpi_for_monitor != nullptr && monitor != nullptr) {
    UINT x = 96, y = 96;
    // MDT_EFFECTIVE_DPI = 0: the DPI the monitor actually runs at, which is what the
    // window has to be drawn for.
    if (SUCCEEDED(get_dpi_for_monitor(monitor, 0, &x, &y)) && x >= 48 && x <= 960)
      return x;
  }
  return static_cast<UINT>(system_dpi());
}

// The DPI the window itself has. For a per-monitor-aware process this is the monitor the
// window is on, which is not necessarily the monitor the caret was measured on; the two are
// reconciled once per show instead of being assumed equal.
UINT dpi_for_window(HWND hwnd) {
  using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
  static const auto get_dpi_for_window = [] {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 != nullptr ? reinterpret_cast<GetDpiForWindowFn>(
                                   GetProcAddress(user32, "GetDpiForWindow"))
                             : nullptr;
  }();
  if (get_dpi_for_window != nullptr && hwnd != nullptr)
    return get_dpi_for_window(hwnd);
  return static_cast<UINT>(system_dpi());
}

} // namespace

bool CandidateView::register_window_class() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = CandidateView::window_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWindowClassName;
  return RegisterClassExW(&wc) != 0;
}

bool CandidateView::register_tip_class() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = CandidateView::tip_window_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kTipClassName;
  return RegisterClassExW(&wc) != 0;
}

CandidateView::~CandidateView() {
  destroy();
  if (font_ != nullptr && font_owned_) {
    DeleteObject(font_);
  }
  font_ = nullptr;
  font_owned_ = false;
}

bool CandidateView::ensure_created(TextService *service, void *entry, std::uint64_t generation,
                                   HWND owner, bool owner_from_view) {
  // A different owner, or an owner that is gone, means this window belongs to a relationship
  // that no longer exists. Recreate it under the owner the entry has now: an owned popup
  // follows its owner's lifetime, so reusing the old window would either tie the popup to a
  // closed window or leave it floating unowned again.
  if (hwnd_ != nullptr && (owner_ != owner || !IsWindow(hwnd_)))
    destroy();
  service_ = service;
  entry_ = entry;
  generation_ = generation;
  if (hwnd_ != nullptr)
    return true;
  // Registered exactly once per process: C++11 magic statics are thread-safe and
  // RegisterClassExW would fail on a duplicate name, so a second call is never made.
  static const bool registered = register_window_class();
  if (!registered)
    return false;
  // WS_EX_TOPMOST, and `owner` in the parent slot: for a WS_POPUP window that slot is the
  // owner, which is what makes the popup rise with the application window it describes
  // instead of sitting wherever the desktop put it (R49 step 4: the anchor resolved, the
  // window was placed, and the taskbar flyout still covered it).
  const HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                    kWindowClassName, L"Fusion", WS_POPUP, 0, 0, 1, 1, owner,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
  if (hwnd == nullptr)
    return false;
  // GWLP_USERDATA carries the CandidateView for the static window procedure.
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  hwnd_ = hwnd;
  owner_ = owner;
  owner_from_view_ = owner != nullptr && owner_from_view;
  return true;
}

HFONT CandidateView::font_for(const CandidateFont &requested, int dpi) {
  const std::string family = requested.family.empty() ? kDefaultFamily : requested.family;
  const int size_pt = (std::min)(kMaxSizePt,
                                 (std::max)(kMinSizePt,
                                            requested.size_pt > 0 ? requested.size_pt
                                                                  : kDefaultSizePt));
  if (font_ != nullptr && family == font_family_ && size_pt == font_size_pt_ &&
      dpi == font_dpi_) {
    return font_;
  }
  if (font_ != nullptr && font_owned_) {
    DeleteObject(font_);
  }
  font_ = nullptr;
  font_owned_ = false;
  // Points, not pixels: the same stored value has to mean the same physical size on any
  // monitor, so the conversion happens here against the DPI the popup is drawing at.
  const int height = -MulDiv(size_pt, dpi, 72);
  const std::wstring wide_family = to_wide(family);
  font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE,
                      wide_family.c_str());
  if (font_ != nullptr) {
    font_owned_ = true;
  } else {
    // GDI substitutes a missing family and font-links missing glyphs; this is the last
    // resort for a handle that could not be created at all.
    font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    font_owned_ = false;
  }
  font_family_ = family;
  font_size_pt_ = size_pt;
  font_dpi_ = dpi;
  return font_;
}

void CandidateView::resolve_target(POINT anchor) {
  const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) {
    work_area_ = info.rcWork;
  } else {
    work_area_ = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  }
  dpi_ = static_cast<int>(dpi_for_monitor(monitor));
}

void CandidateView::show(const CandidatePage &page, POINT anchor) {
  if (page.panel_empty()) {
    hide();
    return;
  }
  if (hwnd_ == nullptr)
    return;
  const bool was_visible = visible();
  cancel_tip_close();
  page_ = page;
  anchor_ = anchor;
  // Geometry follows the display the caret is on, every time: a page that reopens on
  // another monitor must be measured again, not scaled from the previous one.
  resolve_target(anchor);
  reflow();
  tracking_leave_ = false;
  place_and_paint();

  // The window's own DPI is the authority for how big a point is here. On a mixed-DPI
  // desktop the caret can be reported in one monitor's coordinates while the popup lands
  // on another, and drawing with the wrong factor is exactly the "text too big / too
  // small on the second screen" defect. One bounded correction, not a loop.
  const int actual = static_cast<int>(dpi_for_window(hwnd_));
  if (actual > 0 && actual != dpi_ && !in_dpi_change_) {
    in_dpi_change_ = true;
    dpi_ = actual;
    reflow();
    place_and_paint();
    in_dpi_change_ = false;
  }

  announce_shown(was_visible);
  note_shown();

  // The keyboard case: a highlighted candidate too long for its cell is offered in full
  // without the pointer having to find it. The pointer case replaces this on hover.
  update_tip(page_.cursor_index >= 0 && layout_.slot_clipped(static_cast<std::size_t>(
                                            page_.cursor_index))
                     ? page_.cursor_index
                     : -1);
}

void CandidateView::reflow() {
  if (hwnd_ == nullptr)
    return;
  HDC dc = GetDC(hwnd_);
  HGDIOBJ previous_font = nullptr;
  if (dc != nullptr) {
    previous_font = SelectObject(dc, font_for(page_.font, dpi_));
    TEXTMETRICW metrics{};
    if (GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0)
      line_height_ = metrics.tmHeight;
  }
  // Measurement and drawing share this one font and these one metrics, so a slot is
  // sized for the text that will be drawn into it.
  const auto measure = [&](const std::string &utf8) {
    const std::wstring text = to_wide(utf8);
    if (dc == nullptr)
      return 12 * static_cast<int>(text.size());
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    return static_cast<int>(extent.cx);
  };
  // Both budgets come from the same work area, and the layout divides them before it
  // places anything: the panel is inside them by construction rather than by clamping.
  LayoutLimits limits;
  limits.max_width = static_cast<int>(work_area_.right - work_area_.left);
  limits.max_height = static_cast<int>(work_area_.bottom - work_area_.top);
  layout_ = layout_candidate_panel(page_, measure, layout_metrics_for(line_height_, dpi_), limits);
  if (previous_font != nullptr)
    SelectObject(dc, previous_font);
  if (dc != nullptr)
    ReleaseDC(hwnd_, dc);
}

void CandidateView::hide() {
  cancel_tip_close();
  const bool was_visible = visible();
  if (hwnd_ != nullptr && IsWindowVisible(hwnd_))
    ShowWindow(hwnd_, SW_HIDE);
  update_tip(-1);
  if (was_visible)
    announce_hidden();
}

void CandidateView::destroy() {
  cancel_tip_close();
  destroy_tip();
  if (hwnd_ != nullptr) {
    const HWND hwnd = hwnd_;
    hwnd_ = nullptr; // WM_NCDESTROY clears the pointer too; guard re-entry.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
  }
  owner_ = nullptr;
  owner_from_view_ = false;
  service_ = nullptr;
  entry_ = nullptr;
}

void CandidateView::request_reposition() {
  if (hwnd_ == nullptr)
    return;
  // Posted, not sent: the caller may be inside an edit session, and the window move is not
  // text work. It runs on this thread's message loop, after the session has returned.
  PostMessageW(hwnd_, reposition_message, 0, 0);
}

void CandidateView::announce_shown(bool was_visible) {
  if (hwnd_ == nullptr)
    return;
  // EVENT_OBJECT_IME_CHANGE is what an already-visible surface that just moved or resized
  // reports; SHOW is for the surface appearing. A client that tracks the candidate window by
  // accessibility events only learns about it from these.
  NotifyWinEvent(was_visible ? EVENT_OBJECT_IME_CHANGE : EVENT_OBJECT_IME_SHOW, hwnd_, OBJID_CLIENT,
                 INDEXID_OBJECT);
}

void CandidateView::announce_hidden() {
  if (hwnd_ == nullptr)
    return;
  NotifyWinEvent(EVENT_OBJECT_IME_HIDE, hwnd_, OBJID_CLIENT, INDEXID_OBJECT);
}

void CandidateView::note_shown() {
  if (hwnd_ == nullptr)
    return;
  RECT rect{};
  GetWindowRect(hwnd_, &rect);
  // Recorded, never decided on: no behaviour in this surface depends on immersive mode, and
  // the flag is here so a field report can say which mode the thread was in.
  int immersive = -1; // no service (an isolated check) means no thread manager to ask
  if (service_ != nullptr) {
    ITfThreadMgr *tim = service_->thread_mgr();
    ITfThreadMgrEx *tim_ex = nullptr;
    if (tim != nullptr &&
        SUCCEEDED(tim->QueryInterface(IID_ITfThreadMgrEx, reinterpret_cast<void **>(&tim_ex))) &&
        tim_ex != nullptr) {
      DWORD flags = 0;
      immersive =
          SUCCEEDED(tim_ex->GetActiveFlags(&flags)) && (flags & TF_TMF_IMMERSIVEMODE) != 0 ? 1 : 0;
      tim_ex->Release();
    }
  }
  diagf("CANDIDATE_VIEW",
        "shown owner=%p owner-source=%s rect=%ld,%ld,%ld,%ld topmost=1 immersive=%d",
        static_cast<void *>(owner_), owner_from_view_ ? "view" : "focus", rect.left, rect.top,
        rect.right, rect.bottom, immersive);
}

void CandidateView::place_and_paint() {
  if (hwnd_ == nullptr)
    return;
  const int width = layout_.width;
  const int height = layout_.height;
  const int gap = scaled(2, dpi_);
  int x = anchor_.x;
  int y = anchor_.y + gap;
  if (work_area_.right > work_area_.left && work_area_.bottom > work_area_.top) {
    // Work-area bounds, not "wherever the caret is": a page placed near the right or
    // bottom edge has to stay reachable instead of hanging off the screen.
    if (x + width > work_area_.right)
      x = (std::max)(work_area_.left, work_area_.right - width);
    if (x < work_area_.left)
      x = work_area_.left;
    if (y + height > work_area_.bottom) {
      const int above = anchor_.y - height - gap;
      y = above >= work_area_.top ? above : (std::max)(work_area_.top, work_area_.bottom - height);
    }
    if (y < work_area_.top)
      y = work_area_.top;
  }
  // HWND_TOPMOST, not HWND_TOP: TOP only puts the popup above the windows of its own
  // thread, and the surface that covers a candidate list is usually somebody else's — the
  // taskbar flyout in R49 step 4. The window is created topmost for the same reason; this
  // is what keeps it there across a re-place.
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  relayout_and_paint();
}

void CandidateView::draw(HDC dc) {
  RECT bounds{};
  GetClientRect(hwnd_, &bounds);
  FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = SelectObject(dc, font_for(page_.font, dpi_));

  auto draw_text = [&](const LayoutRect &rect, const std::string &utf8, COLORREF color,
                       UINT format) {
    if (rect.empty() || utf8.empty())
      return;
    SetTextColor(dc, color);
    RECT cell = to_rect(rect);
    DrawTextW(dc, to_wide(utf8).c_str(), -1, &cell, format | DT_VCENTER | DT_SINGLELINE);
  };
  // Panel text rows (classic-ui upper: auxUp + panel preedit; lower: auxDown).
  draw_text(layout_.upper, panel_upper_text(page_), RGB(0, 0, 0), DT_LEFT | DT_END_ELLIPSIS);
  draw_text(layout_.lower, page_.aux_down, RGB(0, 0, 0), DT_LEFT | DT_END_ELLIPSIS);
  draw_text(layout_.prev, "<", page_.has_prev ? RGB(20, 20, 20) : RGB(160, 160, 160), DT_CENTER);
  draw_text(layout_.next, ">", page_.has_next ? RGB(20, 20, 20) : RGB(160, 160, 160), DT_CENTER);

  for (std::size_t index = 0; index < page_.entries.size() && index < layout_.slots.size();
       ++index) {
    const LayoutRect &slot = layout_.slots[index];
    if (slot.empty())
      continue; // placeholder: index preserved, nothing drawn
    const CandidateEntry &entry = page_.entries[index];
    const bool selected = static_cast<std::int32_t>(index) == page_.cursor_index;
    RECT fill = to_rect(slot);
    if (selected)
      FillRect(dc, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
    // "label text", then the engine comment after one space in a secondary color. Both
    // halves are single-line and end-ellipsized, but only inside the rectangle the layout
    // measured the text at — the layout already reserved the inset, so nothing is taken off
    // here. A candidate that fits its own measurement is therefore drawn whole: the ellipsis
    // means the text is really wider than the slot, which is exactly what `clipped` says and
    // exactly when the full text is offered beside the panel.
    const std::wstring main = to_wide(slot_main_text(entry));
    RECT cell = to_rect(layout_.text_rect(index));
    const int available = cell.right - cell.left;
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(0, 0, 0));
    DrawTextW(dc, main.c_str(), -1, &cell, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!entry.comment.empty()) {
      SIZE main_extent{};
      GetTextExtentPoint32W(dc, main.c_str(), static_cast<int>(main.size()), &main_extent);
      // A comment is drawn only beside a main text that was drawn whole: once the main text
      // itself is ellipsized, there is no honest place to put it, and the full-text box
      // already carries both.
      if (main_extent.cx > 0 && main_extent.cx <= available) {
        const std::wstring separator = main + L" ";
        SIZE offset{};
        GetTextExtentPoint32W(dc, separator.c_str(), static_cast<int>(separator.size()), &offset);
        RECT comment_cell = cell;
        comment_cell.left = to_rect(layout_.text_rect(index)).left + offset.cx;
        if (comment_cell.left < comment_cell.right) {
          SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(110, 110, 110));
          DrawTextW(dc, to_wide(entry.comment).c_str(), -1, &comment_cell,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
      }
    }
  }
  // A vertical column longer than the space it was given shows a window of its rows. A thin
  // bar inside the candidate area's own edge padding says which way the rest of the page
  // continues, so a hidden row is visibly hidden rather than silently missing; the bar is
  // decoration only and takes no pointer.
  if (layout_.total_rows > layout_.visible_rows) {
    const int bar = (std::max)(1, scaled(2, dpi_));
    const LayoutRect *first = nullptr;
    const LayoutRect *last = nullptr;
    for (const auto &slot : layout_.slots) {
      if (slot.empty())
        continue;
      if (first == nullptr || slot.top < first->top)
        first = &slot;
      if (last == nullptr || slot.bottom > last->bottom)
        last = &slot;
    }
    if (first != nullptr && last != nullptr) {
      const HBRUSH shade = GetSysColorBrush(COLOR_BTNSHADOW);
      if (layout_.rows_hidden_above()) {
        RECT more{first->left, first->top, first->right, first->top + bar};
        if (more.bottom > more.top)
          FillRect(dc, &more, shade);
      }
      if (layout_.rows_hidden_below()) {
        RECT more{last->left, last->bottom - bar, last->right, last->bottom};
        if (more.bottom > more.top)
          FillRect(dc, &more, shade);
      }
    }
  }

  if (previous_font != nullptr)
    SelectObject(dc, previous_font);
}

void CandidateView::update_tip(std::int32_t index) {
  cancel_tip_close();
  const bool wanted = index >= 0 && static_cast<std::size_t>(index) < page_.entries.size() &&
                      layout_.slot_clipped(static_cast<std::size_t>(index)) && hwnd_ != nullptr &&
                      IsWindowVisible(hwnd_);
  const std::wstring text =
      wanted
          ? to_wide(slot_full_text(page_.entries[static_cast<std::size_t>(index)]))
          : std::wstring();
  if (!wanted || text.empty()) {
    if (tip_hwnd_ != nullptr && IsWindowVisible(tip_hwnd_))
      ShowWindow(tip_hwnd_, SW_HIDE);
    tip_index_ = -1;
    tip_tracking_leave_ = false;
    tip_content_height_ = 0;
    tip_view_height_ = 0;
    tip_scroll_max_ = 0;
    tip_wrap_width_ = 0;
    return;
  }
  if (tip_hwnd_ == nullptr) {
    static const bool registered = register_tip_class();
    if (!registered)
      return;
    // Owned by the candidate popup and non-activating: it must never take focus, never
    // take activation, and never outlive the panel it belongs to. Topmost too, because it
    // describes a slot in that panel and is worthless behind it.
    const HWND tip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                     kTipClassName, L"Fusion", WS_POPUP, 0, 0, 1, 1, hwnd_, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (tip == nullptr)
      return;
    SetWindowLongPtrW(tip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    tip_hwnd_ = tip;
  }

  // A different slot starts at the top again; the same slot keeps the reader's position.
  const bool same_text = tip_index_ == index && tip_text_ == text;
  if (!same_text)
    tip_scroll_ = 0;
  tip_text_ = text;
  tip_index_ = index;

  RECT work = work_area_;
  if (work.right <= work.left || work.bottom <= work.top) {
    work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  }
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int margin = scaled(8, dpi_);
  const int padding = scaled(6, dpi_);
  const int max_width = (std::max)(scaled(160, dpi_), work_width / 2);

  HDC dc = GetDC(tip_hwnd_);
  if (dc == nullptr)
    return;
  HGDIOBJ previous_font = SelectObject(dc, font_for(page_.font, dpi_));
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  tip_line_height_ = (std::max)(1, static_cast<int>(metrics.tmHeight));

  // Width first: a short text keeps its natural width, a long one wraps at half the screen.
  SIZE natural{};
  GetTextExtentPoint32W(dc, tip_text_.c_str(), static_cast<int>(tip_text_.size()), &natural);
  int width = (std::min)(static_cast<int>(natural.cx) + 2 * padding, max_width);
  width = (std::max)(width, scaled(60, dpi_));
  const int max_height = (std::max)(tip_line_height_, work_height - 2 * margin);

  RECT panel_rect{};
  GetWindowRect(hwnd_, &panel_rect);
  const LayoutRect panel{static_cast<int>(panel_rect.left), static_cast<int>(panel_rect.top),
                         static_cast<int>(panel_rect.right), static_cast<int>(panel_rect.bottom)};
  const LayoutRect work_rect{static_cast<int>(work.left), static_cast<int>(work.top),
                             static_cast<int>(work.right), static_cast<int>(work.bottom)};

  // Place, then measure again at the width that was *actually* placed. Placement can narrow
  // the box (the free band beside the panel is narrower than the text wanted), and a
  // narrower box wraps into more lines: measuring the height against the pre-placement width
  // would set a scroll limit that stops short of the real end. The loop is bounded because
  // placement only ever narrows: the second pass measures the width that will be drawn.
  LayoutRect bounds;
  int content_height = 0;
  int requested_width = width;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const int wrap_width = (std::max)(1, requested_width - 2 * padding);
    content_height = wrapped_height(dc, tip_text_, wrap_width);
    const int wanted_height = (std::min)(content_height + 2 * padding, max_height);
    bounds = place_fulltext_tip(panel, work_rect, requested_width, wanted_height, scaled(2, dpi_));
    if (bounds.empty())
      break; // no free band: the caller hides the box rather than covering the candidates
    const int placed = bounds.right - bounds.left;
    if (placed >= requested_width)
      break; // converged: the measurement above used the width it will be drawn at
    requested_width = placed;
  }
  SelectObject(dc, previous_font);
  ReleaseDC(tip_hwnd_, dc);

  if (bounds.empty() || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    // The panel leaves no room for the text anywhere on this work area. Showing it over the
    // candidates would take their clicks, so it is not shown at all.
    if (IsWindowVisible(tip_hwnd_))
      ShowWindow(tip_hwnd_, SW_HIDE);
    tip_index_ = -1;
    tip_tracking_leave_ = false;
    tip_content_height_ = 0;
    tip_view_height_ = 0;
    tip_scroll_max_ = 0;
    tip_wrap_width_ = 0;
    return;
  }

  const int placed_width = bounds.right - bounds.left;
  const int placed_height = bounds.bottom - bounds.top;
  tip_padding_ = padding;
  tip_content_height_ = content_height;
  // Both the measurement and the painting use this one width (client width minus padding),
  // so a line is wrapped where it was counted.
  tip_wrap_width_ = (std::max)(1, placed_width - 2 * padding);

  // The scroll extent is a property of the box the reader actually gets: the visible height
  // comes from the placed height, and the maximum scroll is exactly the text that does not
  // fit inside it.
  tip_view_height_ = (std::max)(0, placed_height - 2 * padding);
  tip_scroll_max_ = (std::max)(0, content_height - tip_view_height_);
  tip_scroll_ = (std::min)(tip_scroll_, tip_scroll_max_);

  // A text that needs scrolling has to receive the wheel, so it stops being click-through
  // while it is scrollable; one that fits stays fully transparent, so nothing about it can
  // intercept the pointer. Neither form can be activated, and a click on it is ignored.
  LONG_PTR style = GetWindowLongPtrW(tip_hwnd_, GWL_EXSTYLE);
  style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  if (tip_scroll_max_ > 0)
    style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
  else
    style |= WS_EX_TRANSPARENT;
  SetWindowLongPtrW(tip_hwnd_, GWL_EXSTYLE, style);

  SetWindowPos(tip_hwnd_, HWND_TOPMOST, bounds.left, bounds.top, placed_width, placed_height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(tip_hwnd_, nullptr, TRUE);
}

void CandidateView::arm_tip_close() {
  if (hwnd_ == nullptr)
    return;
  tip_close_pending_ = true;
  SetTimer(hwnd_, tip_close_timer_id, kTipCloseDelayMs, nullptr);
}

void CandidateView::cancel_tip_close() {
  if (hwnd_ != nullptr && tip_close_pending_)
    KillTimer(hwnd_, tip_close_timer_id);
  tip_close_pending_ = false;
  tip_close_rearms_ = 0;
}

void CandidateView::on_tip_close_timer() {
  if (hwnd_ != nullptr)
    KillTimer(hwnd_, tip_close_timer_id);
  tip_close_pending_ = false;
  const bool tip_shown = tip_hwnd_ != nullptr && IsWindowVisible(tip_hwnd_);
  POINT cursor{};
  GetCursorPos(&cursor);
  RECT panel{};
  if (hwnd_ != nullptr)
    GetWindowRect(hwnd_, &panel);
  RECT tip{};
  if (tip_shown)
    GetWindowRect(tip_hwnd_, &tip);

  if (tip_shown && PtInRect(&tip, cursor)) {
    // The pointer reached the box: it owns the pointer now and will report its own leave.
    tip_close_rearms_ = 0;
    return;
  }
  if (PtInRect(&panel, cursor)) {
    // Back on the candidates. The panel's own WM_MOUSEMOVE has already decided what belongs
    // there, so the delay simply stops; re-deciding from the keyboard highlight here would
    // undo a hover that is still in progress.
    tip_close_rearms_ = 0;
    return;
  }
  if (tip_shown) {
    // Between the two windows, on its way from one to the other. Keep the box across the
    // gap, but only for a bounded number of delays: a pointer that stops there must not
    // leave a stale box on screen indefinitely.
    RECT span{};
    span.left = (std::min)(panel.left, tip.left);
    span.top = (std::min)(panel.top, tip.top);
    span.right = (std::max)(panel.right, tip.right);
    span.bottom = (std::max)(panel.bottom, tip.bottom);
    if (PtInRect(&span, cursor) && tip_close_rearms_ < kTipCloseMaxRearms) {
      ++tip_close_rearms_;
      arm_tip_close();
      return;
    }
  }
  tip_close_rearms_ = 0;
  update_tip(-1);
}

void CandidateView::destroy_tip() {
  if (tip_hwnd_ != nullptr) {
    const HWND tip = tip_hwnd_;
    tip_hwnd_ = nullptr;
    SetWindowLongPtrW(tip, GWLP_USERDATA, 0);
    DestroyWindow(tip);
  }
  tip_index_ = -1;
  tip_text_.clear();
  tip_tracking_leave_ = false;
}

LRESULT CALLBACK CandidateView::tip_window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                                LPARAM lparam) {
  auto *self = reinterpret_cast<CandidateView *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr)
    return DefWindowProcW(hwnd, message, wparam, lparam);
  return self->on_tip_message(hwnd, message, wparam, lparam);
}

LRESULT CandidateView::on_tip_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    HGDIOBJ previous_font = SelectObject(dc, font_for(page_.font, dpi_));
    // Scrolled by moving the origin, not by re-wrapping: the text is laid out once and the
    // reader moves over it, so the last line is reachable exactly like the first.
    SetViewportOrgEx(dc, tip_padding_, tip_padding_ - tip_scroll_, nullptr);
    RECT cell{0, 0, bounds.right - 2 * tip_padding_, tip_content_height_};
    DrawTextW(dc, tip_text_.c_str(), -1, &cell, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    if (tip_scroll_max_ > 0) {
      // A track, so a clipped text is visibly clipped and not just truncated: the reader can
      // see there is more and that the wheel moves it.
      const int track_top = tip_padding_;
      const int track_height = (std::max)(1, static_cast<int>(bounds.bottom) - 2 * tip_padding_);
      const int thumb_height = (std::max)(scaled(12, dpi_),
                                          track_height * tip_view_height_ /
                                              (std::max)(1, tip_content_height_));
      const int travel = (std::max)(0, track_height - thumb_height);
      const int thumb_top =
          track_top + (tip_scroll_max_ > 0 ? travel * tip_scroll_ / tip_scroll_max_ : 0);
      RECT track{bounds.right - scaled(4, dpi_), track_top,
                 bounds.right - scaled(2, dpi_), track_top + track_height};
      RECT thumb{track.left, thumb_top, track.right, thumb_top + thumb_height};
      FillRect(dc, &track, static_cast<HBRUSH>(GetSysColorBrush(COLOR_SCROLLBAR)));
      FillRect(dc, &thumb, static_cast<HBRUSH>(GetSysColorBrush(COLOR_BTNSHADOW)));
    }
    SelectObject(dc, previous_font);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_MOUSEMOVE: {
    // The box took the pointer, so it is now responsible for noticing when the pointer
    // leaves it. Arm a single leave notification; without this the box would linger after the
    // pointer has gone somewhere else entirely.
    cancel_tip_close();
    if (!tip_tracking_leave_) {
      TRACKMOUSEEVENT track{};
      track.cbSize = sizeof(track);
      track.dwFlags = TME_LEAVE;
      track.hwndTrack = hwnd;
      tip_tracking_leave_ = TrackMouseEvent(&track) != 0;
    }
    return 0;
  }
  case WM_MOUSELEAVE:
    // The pointer left this box, but it may be on its way back over the candidates (or into
    // the gap between them). The delayed close decides from where it actually is; the panel
    // cancels the delay the moment the pointer lands on it.
    tip_tracking_leave_ = false;
    arm_tip_close();
    return 0;
  case WM_MOUSEWHEEL: {
    if (tip_scroll_max_ > 0) {
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      const int step = (std::max)(1, tip_line_height_);
      const int lines = delta / WHEEL_DELTA;
      const int moved = tip_scroll_ - lines * 3 * step;
      const int clamped = (std::max)(0, (std::min)(tip_scroll_max_, moved));
      if (clamped != tip_scroll_) {
        tip_scroll_ = clamped;
        InvalidateRect(hwnd, nullptr, TRUE);
      }
    }
    return 0;
  }
  case WM_LBUTTONUP:
  case WM_LBUTTONDOWN:
    // A click on the tooltip never selects anything: the candidate list is behind it.
    return 0;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_POINTERACTIVATE:
    return PA_NOACTIVATE;
  case WM_NCHITTEST:
    // While the text scrolls the window takes the pointer (the wheel has to arrive
    // somewhere); a text that fits stays transparent so it cannot swallow anything.
    return tip_scroll_max_ > 0 ? HTCLIENT : HTTRANSPARENT;
  case WM_DESTROY:
    if (hwnd == tip_hwnd_)
      tip_hwnd_ = nullptr;
    return 0;
  default:
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

LRESULT CALLBACK CandidateView::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto *self = reinterpret_cast<CandidateView *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr)
    return DefWindowProcW(hwnd, message, wparam, lparam);
  return self->on_message(message, wparam, lparam);
}

LRESULT CandidateView::on_message(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    draw(dc);
    EndPaint(hwnd_, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1; // fully painted in WM_PAINT
  case WM_DPICHANGED: {
    // The popup was moved (or the display's scale changed) under it: measure again for the
    // new DPI from the same anchor instead of scaling the previous geometry, which would
    // double-scale the margins and leave the text at the old size. SetWindowPos inside this
    // path can raise another WM_DPICHANGED; the guard makes the correction happen once.
    if (!in_dpi_change_ && !page_.panel_empty())
      show(page_, anchor_);
    return 0;
  }
  case WM_MOUSEMOVE: {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    int action = 0;
    std::int32_t index = -1;
    const bool over_slot = hit_test(point, action, index) &&
                           static_cast<CandidateAction>(action) == CandidateAction::Select;
    // The pointer is on the candidates again: any pending close is moot, and the slot under
    // it decides what is shown.
    cancel_tip_close();
    update_tip(over_slot ? index : -1);
    if (!tracking_leave_) {
      TRACKMOUSEEVENT track{};
      track.cbSize = sizeof(track);
      track.dwFlags = TME_LEAVE;
      track.hwndTrack = hwnd_;
      tracking_leave_ = TrackMouseEvent(&track) != 0;
    }
    return 0;
  }
  case WM_MOUSELEAVE:
    tracking_leave_ = false;
    // Leaving the panel is not the same as leaving the reading surface. A full-text box is
    // normally placed in the free band beside the panel, with a gap, so the pointer that
    // goes to read it has to cross space that belongs to neither window. Closing here would
    // take the box away exactly when the pointer was on its way into it, which is why the
    // close is deferred: the cursor's position at the end of the delay decides.
    if (tip_hwnd_ != nullptr && IsWindowVisible(tip_hwnd_)) {
      arm_tip_close();
      return 0;
    }
    update_tip(page_.cursor_index);
    return 0;
  case WM_TIMER:
    if (wparam == tip_close_timer_id) {
      on_tip_close_timer();
      return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
  case reposition_message:
    // A layout notification resolved this context's anchor again and left the window move
    // for the message loop. The service re-validates the entry identity first, so a context
    // that went away between the probe and here is not moved.
    if (service_ != nullptr)
      service_->place_candidate_from_anchor(entry_, generation_);
    return 0;
  case WM_LBUTTONUP: {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    int action = 0;
    std::int32_t index = -1;
    if (service_ != nullptr && hit_test(point, action, index)) {
      if (index >= 0)
        diag::event("CANDIDATE_HIT", std::to_string(index).c_str());
      service_->on_candidate_hit(entry_, generation_, static_cast<CandidateAction>(action), index);
    }
    return 0;
  }
  // A click must never activate the popup. WS_EX_NOACTIVATE alone does not stop
  // DefWindowProc from answering MA_ACTIVATE / PA_ACTIVATE: the popup then takes
  // activation and focus from the application, which ends the composition and
  // hides the popup before the button-up that selects arrives (observed in
  // Notepad, a pointer-input process, for both physical and injected clicks:
  // the request came as WM_MOUSEACTIVATE). WM_POINTERACTIVATE is the same
  // request for pointer-input activation and gets the same answer.
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_POINTERACTIVATE:
    return PA_NOACTIVATE;
  case WM_NCHITTEST:
    return HTCLIENT; // whole popup clickable, no resize border
  case WM_NCDESTROY:
    // The window is gone, and so is the relationship it was built on. This is also how a
    // destroyed owner reaches the popup: Windows destroys an owned window with its owner,
    // so the entry's next show finds no window here and makes one under the owner it has
    // now. Clearing the user data only at the very end (WM_DESTROY would be soon enough to
    // route the rest of this destruction through DefWindowProc) is what lets this handler
    // run at all.
    {
      const HWND dying = hwnd_;
      hwnd_ = nullptr;
      owner_ = nullptr;
      owner_from_view_ = false;
      if (dying != nullptr)
        SetWindowLongPtrW(dying, GWLP_USERDATA, 0);
    }
    return 0;
  default:
    return DefWindowProcW(hwnd_, message, wparam, lparam);
  }
}

bool CandidateView::hit_test(POINT client, int &action, std::int32_t &index) const {
  const auto hit = hit_test_candidate_panel(layout_, client.x, client.y);
  if (!hit)
    return false;
  switch (hit->kind) {
  case PanelHit::Kind::PagePrev:
    action = static_cast<int>(CandidateAction::PagePrev);
    break;
  case PanelHit::Kind::PageNext:
    action = static_cast<int>(CandidateAction::PageNext);
    break;
  case PanelHit::Kind::Select:
    action = static_cast<int>(CandidateAction::Select);
    break;
  }
  index = hit->index;
  return true;
}

void CandidateView::relayout_and_paint() {
  if (hwnd_ != nullptr)
    InvalidateRect(hwnd_, nullptr, TRUE);
}

} // namespace fusion::tsf
