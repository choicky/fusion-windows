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

CandidateView::~CandidateView() {
  destroy();
  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
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

HFONT CandidateView::font() {
  if (font_ == nullptr) {
    font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE,
                        L"Microsoft YaHei UI");
    if (font_ == nullptr)
      font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  }
  return font_;
}

void CandidateView::show(const CandidatePage &page, POINT anchor) {
  if (page.panel_empty()) {
    hide();
    return;
  }
  if (hwnd_ == nullptr)
    return;
  page_ = page;

  HDC dc = GetDC(hwnd_);
  HGDIOBJ previous_font = nullptr;
  if (dc != nullptr)
    previous_font = SelectObject(dc, font());
  const auto measure = [&](const std::string &utf8) {
    const std::wstring text = to_wide(utf8);
    if (dc == nullptr)
      return 12 * static_cast<int>(text.size());
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    return static_cast<int>(extent.cx);
  };
  layout_ = layout_candidate_panel(page_, measure);
  if (previous_font != nullptr)
    SelectObject(dc, previous_font);
  if (dc != nullptr)
    ReleaseDC(hwnd_, dc);

  SetWindowPos(hwnd_, HWND_TOP, anchor.x, anchor.y + 2, layout_.width, layout_.height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  relayout_and_paint();
}

void CandidateView::hide() {
  if (hwnd_ != nullptr && IsWindowVisible(hwnd_))
    ShowWindow(hwnd_, SW_HIDE);
}

void CandidateView::destroy() {
  if (hwnd_ != nullptr) {
    const HWND hwnd = hwnd_;
    hwnd_ = nullptr; // WM_DESTROY clears the pointer too; guard re-entry.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
  }
  service_ = nullptr;
  entry_ = nullptr;
}

void CandidateView::draw(HDC dc) {
  RECT bounds{};
  GetClientRect(hwnd_, &bounds);
  FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = SelectObject(dc, font());

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
    // "label text", then the engine comment after one space in a secondary color.
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
  SelectObject(dc, previous_font);
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
