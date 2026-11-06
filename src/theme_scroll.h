#pragma once
#include "theme.h"
#include <commctrl.h>
#include <functional>

namespace ui {
// A native scrollbar control with a slim, theme-coloured thumb. Range and
// position remain available through the normal scrollbar accessibility APIs.
class ScrollBar {
  HWND window_ = nullptr;
  Theme *theme_ = nullptr;
  std::function<void(int)> changed_;
  UINT dpi_ = 96;
  int total_ = 0, page_ = 1, pos_ = 0, line_ = 1, wheel_ = 0;
  int grab_ = 0, repeat_ = 0;
  bool hot_ = false, dragging_ = false, surface_ = false;
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  void paint(HDC);
  void stop();
  void repeatPage();
public:
  ScrollBar() = default;
  ~ScrollBar();
  ScrollBar(const ScrollBar &) = delete;
  HWND window() const { return window_; }
  int position() const { return pos_; }
  int maximum() const { return total_ > page_ ? total_ - page_ : 0; }
  RECT thumb() const;
  void create(HWND parent, int id, Theme &, std::function<void(int)>, bool surface = false);
  void configure(int total, int page, int position, UINT dpi, int line = 1);
  void move(int x, int y, int width, int height);
  void scrollTo(int position);
  void command(WPARAM); // WM_VSCROLL from accessibility/native callers.
  void wheel(int delta);
};
}
