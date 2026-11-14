#pragma once
#include <windows.h>
#include <memory>

namespace desktop_groups {
class GlassBackground {
public:
  GlassBackground();
  ~GlassBackground();
  void reset();
  void paint(HWND window, HDC dc, RECT client, COLORREF tint, BYTE opacity, bool glass, POINT offset = {});

private:
  struct State;
  std::unique_ptr<State> state;
};
}
