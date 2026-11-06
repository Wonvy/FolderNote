#pragma once
#include <windows.h>
#include <string>

namespace ui {
enum class ThemeMode : DWORD { System = 0, Light = 1, Dark = 2 };
ThemeMode configuredTheme();
UINT themeChangedMessage();
void notifyThemeChanged();
struct Palette {
  COLORREF background, surface, field, hover, pressed, border, text, muted, accent, onAccent;
  bool dark = false, highContrast = false;
  bool operator==(const Palette &) const = default;
};
Palette palette(ThemeMode mode);
HFONT makeFont(UINT dpi, int size = 14, int weight = FW_NORMAL);
HFONT makeSymbolFont(UINT dpi, int size = 16);
void rounded(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius);
void text(HDC dc, const std::wstring &value, RECT rect, COLORREF color,
          UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
void surface(HWND control, bool value = true);
void muted(HWND control);

// Native controls retain their keyboard, accessibility and control semantics.
// Painting is shared by the settings, note editor and folder manager.
class Theme {
public:
  ThemeMode mode = ThemeMode::System;
  Palette colors{};
  HBRUSH background = nullptr, card = nullptr, field = nullptr;
  Theme();
  ~Theme();
  Theme(const Theme &) = delete;
  Theme &operator=(const Theme &) = delete;
  void reset(ThemeMode value);
  void apply(HWND window);
  bool handle(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result);
};
}
