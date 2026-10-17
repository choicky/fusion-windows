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
constexpr int kRowHeight = 24;
constexpr int kSideMargin = 6;
constexpr int kItemGap = 12;
constexpr int kPagerWidth = 20;

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

std::wstring display_text(const CandidateEntry &entry) {
  std::wstring value = to_wide(entry.label);
  if (!value.empty())
    value += L" ";
  value += to_wide(entry.text);
  return value;
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
  if (page.empty()) {
    hide();
    return;
  }
  if (hwnd_ == nullptr)
    return;
  page_ = page;
  has_prev_button_ = page_.has_prev;
  has_next_button_ = page_.has_next;
  slot_rects_.assign(page_.entries.size(), RECT{});
  const bool horizontal = page_.layout == CandidateLayout::Horizontal;

  HDC dc = GetDC(hwnd_);
  HGDIOBJ previous_font = nullptr;
  auto measure = [&](const std::wstring &text) {
    if (dc == nullptr)
      return 12 * static_cast<int>(text.size());
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    return static_cast<int>(extent.cx);
  };
  if (dc != nullptr)
    previous_font = SelectObject(dc, font());

  int width = 0;
  int height = 0;
  prev_rect_ = RECT{};
  next_rect_ = RECT{};
  if (horizontal) {
    int x = kSideMargin;
    if (has_prev_button_) {
      prev_rect_ = RECT{x, kSideMargin, x + kPagerWidth, kRowHeight - kSideMargin};
      x += kPagerWidth + kItemGap;
    }
    for (std::size_t index = 0; index < page_.entries.size(); ++index) {
      const std::wstring text = display_text(page_.entries[index]);
      // A placeholder takes an index but no displayed slot: measure nothing and
      // leave an empty rect so the hit index stays aligned with candidate(index).
      const int advance = page_.entries[index].placeholder ? 0 : measure(text) + kItemGap;
      if (!page_.entries[index].placeholder)
        slot_rects_[index] = RECT{x, kSideMargin, x + advance - kItemGap, kRowHeight - kSideMargin};
      x += advance;
    }
    if (has_next_button_) {
      next_rect_ = RECT{x, kSideMargin, x + kPagerWidth, kRowHeight - kSideMargin};
      x += kPagerWidth;
    }
    width = x + kSideMargin;
    height = kRowHeight;
  } else {
    const int band = (has_prev_button_ || has_next_button_) ? kRowHeight : 0;
    width = 180;
    for (const auto &entry : page_.entries)
      if (!entry.placeholder)
        width = (std::max)(width, measure(display_text(entry)) + 2 * kSideMargin + kPagerWidth);
    int rows = 0;
    for (const auto &entry : page_.entries)
      if (!entry.placeholder)
        ++rows;
    height = band + rows * kRowHeight + kSideMargin;
    int y = band;
    for (std::size_t index = 0; index < page_.entries.size(); ++index) {
      if (page_.entries[index].placeholder)
        continue;
      slot_rects_[index] = RECT{kSideMargin, y, width - kSideMargin, y + kRowHeight};
      y += kRowHeight;
    }
    if (has_prev_button_)
      prev_rect_ = RECT{kSideMargin, 0, kSideMargin + kPagerWidth, band};
    if (has_next_button_)
      next_rect_ = RECT{width - kSideMargin - kPagerWidth, 0, width - kSideMargin, band};
  }

  if (previous_font != nullptr)
    SelectObject(dc, previous_font);
  if (dc != nullptr)
    ReleaseDC(hwnd_, dc);
  if (width < 40)
    width = 40;

  SetWindowPos(hwnd_, HWND_TOP, anchor.x, anchor.y + 2, width, height,
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

  auto draw_box = [&](const RECT &rect, const wchar_t *glyph, bool enabled) {
    if (rect.right <= rect.left)
      return;
    SetTextColor(dc, enabled ? RGB(20, 20, 20) : RGB(160, 160, 160));
    RECT cell = rect;
    DrawTextW(dc, glyph, -1, &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };
  draw_box(prev_rect_, L"<", has_prev_button_);
  draw_box(next_rect_, L">", has_next_button_);

  for (std::size_t index = 0; index < page_.entries.size(); ++index) {
    const RECT &rect = slot_rects_[index];
    if (rect.right <= rect.left)
      continue; // placeholder: index preserved, nothing drawn
    const bool selected = static_cast<std::int32_t>(index) == page_.cursor_index;
    if (selected) {
      RECT fill = rect;
      FillRect(dc, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    SetTextColor(dc, selected ? RGB(255, 255, 255) : RGB(0, 0, 0));
    RECT cell = rect;
    cell.left += 2;
    DrawTextW(dc, display_text(page_.entries[index]).c_str(), -1, &cell,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
  if (has_prev_button_ && PtInRect(&prev_rect_, client)) {
    action = static_cast<int>(CandidateAction::PagePrev);
    index = -1;
    return true;
  }
  if (has_next_button_ && PtInRect(&next_rect_, client)) {
    action = static_cast<int>(CandidateAction::PageNext);
    index = -1;
    return true;
  }
  for (std::size_t slot = 0; slot < slot_rects_.size(); ++slot) {
    if (slot_rects_[slot].left < slot_rects_[slot].right && PtInRect(&slot_rects_[slot], client)) {
      action = static_cast<int>(CandidateAction::Select);
      index = static_cast<std::int32_t>(slot);
      return true;
    }
  }
  return false;
}

void CandidateView::relayout_and_paint() {
  if (hwnd_ != nullptr)
    InvalidateRect(hwnd_, nullptr, TRUE);
}

} // namespace fusion::tsf
