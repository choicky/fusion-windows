#include "tsf/candidate_view.hpp"

#include "tsf/diag.hpp"
#include "tsf/semantic.hpp"
#include "tsf/service.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
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

RECT to_rect(const LayoutRect &rect) { return RECT{rect.left, rect.top, rect.right, rect.bottom}; }

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

bool CandidateView::ensure_created(TextService *service, void *entry, std::uint64_t generation) {
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
  const HWND hwnd =
      CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClassName, L"Fusion", WS_POPUP, 0,
                      0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (hwnd == nullptr)
    return false;
  // GWLP_USERDATA carries the CandidateView for the static window procedure.
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  hwnd_ = hwnd;
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
  page_ = page;
  anchor_ = anchor;
  // Geometry follows the display the caret is on, every time: a page that reopens on
  // another monitor must be measured again, not scaled from the previous one.
  resolve_target(anchor);

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
  LayoutLimits limits;
  limits.max_width = static_cast<int>(work_area_.right - work_area_.left);
  layout_ = layout_candidate_panel(page_, measure, layout_metrics_for(line_height_, dpi_), limits);
  if (previous_font != nullptr)
    SelectObject(dc, previous_font);
  if (dc != nullptr)
    ReleaseDC(hwnd_, dc);

  tracking_leave_ = false;
  place_and_paint();
  // The keyboard case: a highlighted candidate too long for its cell is offered in full
  // without the pointer having to find it. The pointer case replaces this on hover.
  update_tip(page_.cursor_index >= 0 && layout_.slot_clipped(static_cast<std::size_t>(
                                            page_.cursor_index))
                     ? page_.cursor_index
                     : -1);
}

void CandidateView::hide() {
  if (hwnd_ != nullptr && IsWindowVisible(hwnd_))
    ShowWindow(hwnd_, SW_HIDE);
  update_tip(-1);
}

void CandidateView::destroy() {
  destroy_tip();
  if (hwnd_ != nullptr) {
    const HWND hwnd = hwnd_;
    hwnd_ = nullptr; // WM_DESTROY clears the pointer too; guard re-entry.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
  }
  service_ = nullptr;
  entry_ = nullptr;
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
  SetWindowPos(hwnd_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    // halves are single-line and end-ellipsized inside the slot: a long candidate is
    // shortened on screen, never the string the engine would commit.
    const std::wstring main = to_wide(slot_main_text(entry));
    RECT cell = fill;
    cell.left += 2;
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(0, 0, 0));
    DrawTextW(dc, main.c_str(), -1, &cell, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!entry.comment.empty()) {
      SIZE extent{};
      GetTextExtentPoint32W(dc, (main + L" ").c_str(), static_cast<int>(main.size() + 1), &extent);
      RECT comment_cell = cell;
      comment_cell.left += extent.cx;
      if (comment_cell.left < comment_cell.right) {
        SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(110, 110, 110));
        DrawTextW(dc, to_wide(entry.comment).c_str(), -1, &comment_cell,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
    }
  }
  if (previous_font != nullptr)
    SelectObject(dc, previous_font);
}

void CandidateView::update_tip(std::int32_t index) {
  const bool show = index >= 0 && static_cast<std::size_t>(index) < page_.entries.size() &&
                    layout_.slot_clipped(static_cast<std::size_t>(index)) && hwnd_ != nullptr;
  if (!show) {
    if (tip_hwnd_ != nullptr && IsWindowVisible(tip_hwnd_))
      ShowWindow(tip_hwnd_, SW_HIDE);
    tip_index_ = -1;
    return;
  }
  const std::wstring text = to_wide(slot_full_text(page_.entries[static_cast<std::size_t>(index)]));
  if (text.empty()) {
    if (tip_hwnd_ != nullptr && IsWindowVisible(tip_hwnd_))
      ShowWindow(tip_hwnd_, SW_HIDE);
    tip_index_ = -1;
    return;
  }
  if (tip_hwnd_ == nullptr) {
    static const bool registered = register_tip_class();
    if (!registered)
      return;
    // Owned by the candidate popup and click-through: it must never take focus, never
    // cover the pointer's own hit testing, and never outlive the panel it belongs to.
    const HWND tip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                     kTipClassName, L"Fusion", WS_POPUP, 0, 0, 1, 1, hwnd_,
                                     nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tip == nullptr)
      return;
    SetWindowLongPtrW(tip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    tip_hwnd_ = tip;
  }
  tip_text_ = text;
  tip_index_ = index;

  // Size the tip for wrapped text inside the work area: a very long candidate is shown in
  // as many lines as fit, and only then ellipsized. The popup never becomes screen-sized.
  RECT work = work_area_;
  if (work.right <= work.left || work.bottom <= work.top) {
    work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  }
  const int margin = scaled(8, dpi_);
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int max_width = (std::max)(scaled(160, dpi_), work_width / 2);
  HDC dc = GetDC(tip_hwnd_);
  int width = 0;
  int height = 0;
  if (dc != nullptr) {
    HGDIOBJ previous_font = SelectObject(dc, font_for(page_.font, dpi_));
    RECT measure{0, 0, max_width, 0};
    DrawTextW(dc, tip_text_.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    width = measure.right - measure.left;
    height = measure.bottom - measure.top;
    SelectObject(dc, previous_font);
    ReleaseDC(tip_hwnd_, dc);
  }
  if (width <= 0 || height <= 0) {
    ShowWindow(tip_hwnd_, SW_HIDE);
    tip_index_ = -1;
    return;
  }
  const int padding = scaled(6, dpi_);
  width = (std::min)(width + 2 * padding, work_width - 2 * margin);
  height = (std::min)(height + 2 * padding, work_height - 2 * margin);

  // Prefer directly under the slot; flip above when the work area ends first. The x is
  // the slot's own left edge, pulled back inside the work area.
  const LayoutRect slot = layout_.slots[static_cast<std::size_t>(index)];
  RECT popup{};
  GetWindowRect(hwnd_, &popup);
  int x = popup.left + slot.left;
  int y = popup.top + slot.bottom + scaled(2, dpi_);
  if (y + height > work.bottom)
    y = popup.top + slot.top - height - scaled(2, dpi_);
  if (y < work.top)
    y = work.top;
  if (y + height > work.bottom)
    y = (std::max)(static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
  if (x + width > work.right)
    x = (std::max)(work.left, work.right - width);
  if (x < work.left)
    x = work.left;
  SetWindowPos(tip_hwnd_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(tip_hwnd_, nullptr, TRUE);
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
    RECT cell = bounds;
    InflateRect(&cell, -scaled(6, dpi_), -scaled(6, dpi_));
    DrawTextW(dc, tip_text_.c_str(), -1, &cell,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, previous_font);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_POINTERACTIVATE:
    return PA_NOACTIVATE;
  case WM_NCHITTEST:
    return HTTRANSPARENT; // clicks and hovers belong to whatever is underneath
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
    // double-scale the margins and leave the text at the old size.
    if (!page_.panel_empty())
      show(page_, anchor_);
    return 0;
  }
  case WM_MOUSEMOVE: {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    int action = 0;
    std::int32_t index = -1;
    const bool over_slot = hit_test(point, action, index) &&
                           static_cast<CandidateAction>(action) == CandidateAction::Select;
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
    update_tip(page_.cursor_index);
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
  case WM_DESTROY:
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    hwnd_ = nullptr;
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
