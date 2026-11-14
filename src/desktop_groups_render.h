#pragma once
#include "theme.h"

namespace desktop_groups {
inline constexpr BYTE boxOutlineAlpha = 46;
COLORREF boxOutlineColor(COLORREF background, const ui::Palette &colors);
HRGN createBoxRegion(int width, int height, int diameter);
bool drawBoxShadow(DWORD *pixels, int width, int height, int spread, int offset, int diameter, BYTE opacity);
bool drawFenceItem(HDC dc, RECT row, RECT label, RECT icon, HICON image, const std::wstring &text,
                   const ui::Palette &colors, bool selected, bool hovered, bool listMode, UINT dpi);
}
