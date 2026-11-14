#include "theme_scroll.h"
#include <windowsx.h>
#include <algorithm>
#include <stdexcept>

namespace ui {
namespace { constexpr UINT_PTR repeatTimer = 0x5343; }
ScrollBar::~ScrollBar() { if (window_) DestroyWindow(window_); }
void ScrollBar::create(HWND parent, int id, Theme &theme, std::function<void(int)> changed, bool surface, bool horizontal) {
  if (window_) DestroyWindow(window_);
  theme_ = &theme; changed_ = std::move(changed); surface_ = surface;
  horizontal_ = horizontal;
  window_ = CreateWindowExW(0, L"SCROLLBAR", horizontal ? L"水平滚动" : L"垂直滚动", WS_CHILD | (horizontal ? SBS_HORZ : SBS_VERT),
      0, 0, 1, 1, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  if (!window_) throw std::runtime_error("create scrollbar");
  SetWindowSubclass(window_, proc, 1, reinterpret_cast<DWORD_PTR>(this));
}
void ScrollBar::configure(int total, int page, int position, UINT dpi, int line) {
  total = std::max(0, total);
  page = std::max(1, page);
  line = std::max(1, line);
  position = std::clamp(position, 0, std::max(0, total - page));
  if (total == total_ && page == page_ && position == pos_ && dpi == dpi_ && line == line_) return;
  total_ = std::max(0, total); page_ = std::max(1, page); dpi_ = dpi; line_ = std::max(1, line);
  pos_ = std::clamp(position, 0, maximum());
  SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
  info.nMax = std::max(0, total_ - 1); info.nPage = page_; info.nPos = pos_;
  SetScrollInfo(window_, SB_CTL, &info, FALSE);
  if (!maximum()) stop();
  if (((GetWindowLongPtrW(window_, GWL_STYLE) & WS_VISIBLE) != 0) != (maximum() != 0))
    ShowWindow(window_, maximum() ? SW_SHOWNOACTIVATE : SW_HIDE);
  InvalidateRect(window_, nullptr, FALSE);
}
void ScrollBar::setTrackColor(COLORREF color) {
  trackColor_ = color;
  if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void ScrollBar::move(int x, int y, int width, int height) { MoveWindow(window_, x, y, width, height, TRUE); }
RECT ScrollBar::thumb() const {
  RECT rect{}; GetClientRect(window_, &rect);
  if (horizontal_) std::swap(rect.right, rect.bottom);
  int inset = std::max(1, MulDiv(2, dpi_, 96));
  int track = std::max(0, int(rect.bottom) - inset * 2);
  int height = total_ ? MulDiv(track, page_, total_) : track;
  height = std::clamp(height, std::min(track, MulDiv(24, dpi_, 96)), track);
  int top = inset + (maximum() ? MulDiv(track - height, pos_, maximum()) : 0);
  int width = std::min<int>(rect.right, MulDiv(hot_ || dragging_ || theme_->colors.highContrast ? 8 : 5, dpi_, 96));
  int left = (rect.right - width) / 2;
  return horizontal_ ? RECT{top, left, top + height, left + width} : RECT{left, top, left + width, top + height};
}
void ScrollBar::scrollTo(int position) {
  int next = std::clamp(position, 0, maximum());
  if (next == pos_) return;
  pos_ = next;
  SCROLLINFO info{sizeof(info), SIF_POS}; info.nPos = pos_; SetScrollInfo(window_, SB_CTL, &info, FALSE);
  InvalidateRect(window_, nullptr, FALSE);
  if (changed_) changed_(pos_);
}
void ScrollBar::command(WPARAM value) {
  switch (LOWORD(value)) {
  case SB_TOP: scrollTo(0); break;
  case SB_BOTTOM: scrollTo(maximum()); break;
  case SB_LINEUP: scrollTo(pos_ - line_); break;
  case SB_LINEDOWN: scrollTo(pos_ + line_); break;
  case SB_PAGEUP: scrollTo(pos_ - page_); break;
  case SB_PAGEDOWN: scrollTo(pos_ + page_); break;
  case SB_THUMBTRACK: case SB_THUMBPOSITION: scrollTo(HIWORD(value)); break;
  }
}
void ScrollBar::wheel(int delta) {
  wheel_ += delta; int ticks = wheel_ / WHEEL_DELTA; wheel_ %= WHEEL_DELTA;
  UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  int step = lines == WHEEL_PAGESCROLL ? page_ : std::min<UINT>(lines, 100) * line_;
  if (ticks) scrollTo(pos_ - ticks * step);
}
void ScrollBar::stop() {
  dragging_ = false; repeat_ = 0; KillTimer(window_, repeatTimer);
  if (GetCapture() == window_) ReleaseCapture();
  InvalidateRect(window_, nullptr, FALSE);
}
void ScrollBar::repeatPage() {
  POINT point{}; GetCursorPos(&point); ScreenToClient(window_, &point);
  auto bounds = thumb();
  const int coordinate = horizontal_ ? point.x : point.y;
  if ((repeat_ < 0 && coordinate < (horizontal_ ? bounds.left : bounds.top)) ||
      (repeat_ > 0 && coordinate >= (horizontal_ ? bounds.right : bounds.bottom))) scrollTo(pos_ + repeat_ * page_);
}
void ScrollBar::paint(HDC dc) {
  RECT rect{}; GetClientRect(window_, &rect);
  if (background_) background_(window_, dc);
  else if (trackColor_) {
    const auto previous = SetDCBrushColor(dc, *trackColor_);
    FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, previous);
  } else FillRect(dc, &rect, surface_ ? theme_->field : theme_->background);
  if (maximum()) {
    auto bounds = thumb(); const auto &p = theme_->colors;
    auto color = dragging_ ? p.accent : hot_ ? p.text : p.muted;
    rounded(dc, bounds, color, color, std::min(bounds.right - bounds.left, bounds.bottom - bounds.top) / 2);
  }
}
LRESULT CALLBACK ScrollBar::proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto &bar = *reinterpret_cast<ScrollBar *>(data);
  switch (message) {
  case WM_ERASEBKGND: return 1;
  case WM_PAINT: { PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint); bar.paint(dc); EndPaint(window, &paint); return 0; }
  case WM_PRINTCLIENT: bar.paint(reinterpret_cast<HDC>(w)); return 0;
  case WM_MOUSEMOVE: {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
    if (!bar.hot_) { bar.hot_ = true; InvalidateRect(window, nullptr, FALSE); }
    if (bar.dragging_) {
      RECT rect{}; GetClientRect(window, &rect); auto thumb = bar.thumb();
      int inset = std::max(1, MulDiv(2, bar.dpi_, 96));
      const bool horizontal = bar.horizontal_;
      int travel = (horizontal ? rect.right - (thumb.right - thumb.left) : rect.bottom - (thumb.bottom - thumb.top)) - inset * 2;
      if (travel > 0) bar.scrollTo(MulDiv((horizontal ? GET_X_LPARAM(l) : GET_Y_LPARAM(l)) - bar.grab_ - inset, bar.maximum(), travel));
    }
    return 0;
  }
  case WM_MOUSELEAVE: bar.hot_ = false; InvalidateRect(window, nullptr, FALSE); return 0;
  case WM_LBUTTONDOWN: {
    if (!bar.maximum()) return 0;
    auto thumb = bar.thumb();
    const int y = bar.horizontal_ ? GET_X_LPARAM(l) : GET_Y_LPARAM(l);
    const int start = bar.horizontal_ ? thumb.left : thumb.top, end = bar.horizontal_ ? thumb.right : thumb.bottom;
    SetCapture(window); bar.hot_ = true;
    if (y >= start && y < end) { bar.dragging_ = true; bar.grab_ = y - start; }
    else {
      bar.repeat_ = y < start ? -1 : 1;
      bar.scrollTo(bar.pos_ + bar.repeat_ * bar.page_); SetTimer(window, repeatTimer, 350, nullptr);
    }
    InvalidateRect(window, nullptr, FALSE); return 0;
  }
  case WM_LBUTTONUP: case WM_CANCELMODE: bar.stop(); return 0;
  case WM_CAPTURECHANGED:
    if (reinterpret_cast<HWND>(l) != window) { bar.dragging_ = false; bar.repeat_ = 0; KillTimer(window, repeatTimer); InvalidateRect(window, nullptr, FALSE); }
    return 0;
  case WM_TIMER:
    if (w == repeatTimer) { bar.repeatPage(); if (bar.repeat_) SetTimer(window, repeatTimer, 65, nullptr); return 0; }
    break;
  case WM_MOUSEWHEEL: bar.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0;
  case WM_KEYDOWN:
    switch (w) {
    case VK_LEFT: case VK_UP: bar.command(SB_LINEUP); return 0;
    case VK_RIGHT: case VK_DOWN: bar.command(SB_LINEDOWN); return 0;
    case VK_PRIOR: bar.command(SB_PAGEUP); return 0;
    case VK_NEXT: bar.command(SB_PAGEDOWN); return 0;
    case VK_HOME: bar.command(SB_TOP); return 0;
    case VK_END: bar.command(SB_BOTTOM); return 0;
    }
    break;
  case SBM_GETSCROLLBARINFO: {
    auto *info = reinterpret_cast<SCROLLBARINFO *>(l);
    if (!info || info->cbSize != sizeof(*info)) return FALSE;
    *info = {sizeof(*info)}; GetWindowRect(window, &info->rcScrollBar);
    auto bounds = bar.thumb();
    info->xyThumbTop = bar.horizontal_ ? bounds.left : bounds.top;
    info->xyThumbBottom = bar.horizontal_ ? bounds.right : bounds.bottom;
    info->rgstate[1] = info->rgstate[5] = STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE;
    if (!bar.maximum()) info->rgstate[0] = STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE;
    if (bar.dragging_) info->rgstate[3] = STATE_SYSTEM_PRESSED;
    return TRUE;
  }
  case WM_NCDESTROY:
    bar.stop(); RemoveWindowSubclass(window, proc, id); bar.window_ = nullptr; break;
  }
  auto result = DefSubclassProc(window, message, w, l);
  if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SIZE || message == WM_THEMECHANGED)
    InvalidateRect(window, nullptr, FALSE);
  return result;
}
}
