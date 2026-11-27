#include "theme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>

namespace ui {
namespace {
constexpr wchar_t surfaceKey[] = L"FolderNote.Theme.Surface";
constexpr wchar_t mutedKey[] = L"FolderNote.Theme.Muted";
constexpr wchar_t switchKey[] = L"FolderNote.Theme.Switch";
constexpr wchar_t hoverKey[] = L"FolderNote.Theme.Hover";
int px(HWND window, int n) { return MulDiv(n, GetDpiForWindow(window), 96); }
COLORREF mixColor(COLORREF a, COLORREF b, int weight) {
  const auto channel = [weight](int from, int to) { return (from * (100 - weight) + to * weight) / 100; };
  return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}
void smoothPill(HDC dc, RECT bounds, COLORREF fill, COLORREF edge, COLORREF background) {
  const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  const auto inside = [=](int x, int y, int inset) {
    const int radius = height * 4 - inset;
    const int dx = x - std::clamp(x, height * 4, width * 8 - height * 4);
    const int dy = y - height * 4;
    return dx * dx + dy * dy <= radius * radius;
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int outer = 0, inner = 0;
      for (int sy : {1, 3, 5, 7}) for (int sx : {1, 3, 5, 7}) {
        outer += inside(x * 8 + sx, y * 8 + sy, 0);
        inner += inside(x * 8 + sx, y * 8 + sy, 8);
      }
      if (!outer) continue;
      const auto mix = [&](int fg, int border, int bg) {
        return (fg * inner + border * (outer - inner) + bg * (16 - outer) + 8) / 16;
      };
      SetPixelV(dc, bounds.left + x, bounds.top + y,
          RGB(mix(GetRValue(fill), GetRValue(edge), GetRValue(background)),
              mix(GetGValue(fill), GetGValue(edge), GetGValue(background)),
              mix(GetBValue(fill), GetBValue(edge), GetBValue(background))));
    }
  }
}
std::wstring caption(HWND window) {
  std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
  value.resize(GetWindowTextW(window, value.data(), static_cast<int>(value.size())));
  return value;
}
LRESULT CALLBACK controlProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto &theme = *reinterpret_cast<Theme *>(data);
  wchar_t name[32]{}; GetClassNameW(window, name, ARRAYSIZE(name));
  const bool button = !wcscmp(name, L"Button"), combo = !wcscmp(name, L"ComboBox");
  const bool customPaint = combo || (button && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, controlProc, id);
    RemovePropW(window, surfaceKey); RemovePropW(window, mutedKey);
    RemovePropW(window, switchKey); RemovePropW(window, hoverKey);
    return DefSubclassProc(window, message, w, l);
  }
  if (customPaint && message == WM_ERASEBKGND) return 1;
  if (customPaint && message == WM_MOUSEMOVE) {
    if (!GetPropW(window, hoverKey)) {
      SetPropW(window, hoverKey, reinterpret_cast<HANDLE>(1));
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
      InvalidateRect(window, nullptr, FALSE);
    }
    if (button && GetCapture() != window) return 0;
  }
  if (customPaint && message == WM_MOUSELEAVE) {
    if (GetPropW(window, hoverKey)) {
      RemovePropW(window, hoverKey);
      InvalidateRect(window, nullptr, FALSE);
    }
    if (button && GetCapture() != window) return 0;
  }
  if (message == WM_PAINT && customPaint) {
    PAINTSTRUCT paint{}; HDC paintDc = BeginPaint(window, &paint);
    RECT rect{}; GetClientRect(window, &rect);
    const int bufW = rect.right - rect.left, bufH = rect.bottom - rect.top;
    HDC bufferDc = bufW > 0 && bufH > 0 ? CreateCompatibleDC(paintDc) : nullptr;
    HBITMAP bufferBmp = bufferDc ? CreateCompatibleBitmap(paintDc, bufW, bufH) : nullptr;
    HGDIOBJ oldBmp = bufferBmp ? SelectObject(bufferDc, bufferBmp) : nullptr;
    if (!oldBmp && bufferDc) {
      DeleteDC(bufferDc); bufferDc = nullptr;
      if (bufferBmp) { DeleteObject(bufferBmp); bufferBmp = nullptr; }
    }
    HDC dc = bufferDc ? bufferDc : paintDc;
    auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    const auto &p = theme.colors;
    const bool enabled = IsWindowEnabled(window) != FALSE, focus = GetFocus() == window;
    POINT mouse{}; GetCursorPos(&mouse); ScreenToClient(window, &mouse);
    bool hot = enabled && PtInRect(&rect, mouse);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    auto color = enabled ? p.text : p.muted;
    FillRect(dc, &rect, GetPropW(window, surfaceKey) ? theme.card : theme.background);
    auto title = caption(window);
    if (button && GetPropW(window, switchKey)) {
      const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
      const bool keyboardFocus = focus && GetKeyState(VK_LBUTTON) >= 0 && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
      const int height = px(window, 18), width = px(window, 34);
      const int top = (rect.bottom - height) / 2;
      RECT track{rect.right - width - 1, top, rect.right - 1, top + height};
      const COLORREF bg = GetPropW(window, surfaceKey) ? p.surface : p.background;
      COLORREF fill = p.background, edge = p.border, knobColor = p.muted;
      if (p.highContrast) {
        fill = checked && enabled ? p.accent : hot ? p.hover : p.background;
        edge = keyboardFocus || (checked && enabled) ? p.accent : enabled ? p.muted : p.border;
        knobColor = checked && enabled ? p.onAccent : enabled ? p.text : p.muted;
      } else if (checked && enabled) {
        fill = hot ? mixColor(p.accent, p.onAccent, 10) : p.accent;
        edge = fill;
        knobColor = p.onAccent;
      } else {
        fill = mixColor(bg, p.muted, p.dark ? (hot ? 46 : 34) : (hot ? 38 : 26));
        edge = keyboardFocus ? p.accent : fill;
        knobColor = enabled ? (p.dark ? RGB(236, 239, 244) : RGB(255, 255, 255)) : p.muted;
      }
      smoothPill(dc, track, fill, edge, bg);
      const int inset = px(window, 3), knobSize = height - inset * 2;
      const int x = checked ? track.right - inset - knobSize : track.left + inset;
      RECT knob{x, top + inset, x + knobSize, top + inset + knobSize};
      smoothPill(dc, knob, knobColor, knobColor, fill);
      rect.right = track.left - px(window, 10);
      text(dc, title, rect, color);
    } else if (button && ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX || (style & BS_TYPEMASK) == BS_CHECKBOX)) {
      int size = px(window, 18), top = (rect.bottom - size) / 2;
      RECT box{1, top, size + 1, top + size};
      bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
      const COLORREF uncheckedBorder = p.highContrast ? p.border
          : (hot ? mixColor(p.border, p.muted, 30) : mixColor(p.field, p.border, p.dark ? 70 : 78));
      rounded(dc, box, checked ? p.accent : (hot ? p.hover : p.field), checked ? p.accent : uncheckedBorder, px(window, 4));
      if (checked) {
        auto pen = CreatePen(PS_SOLID, std::max(1, px(window, 2)), p.onAccent);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, px(window, 5), top + size / 2, nullptr);
        LineTo(dc, px(window, 8), top + size * 3 / 4); LineTo(dc, px(window, 14), top + size / 4);
        SelectObject(dc, old); DeleteObject(pen);
      }
      rect.left = px(window, 28); text(dc, title, rect, color);
    } else if (button && ((style & BS_TYPEMASK) == BS_AUTORADIOBUTTON || (style & BS_TYPEMASK) == BS_RADIOBUTTON)) {
      const int size = px(window, 18), top = (rect.bottom - size) / 2;
      RECT circle{1, top, size + 1, top + size};
      const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
      const COLORREF bg = GetPropW(window, surfaceKey) ? p.surface : p.background;
      if (checked && enabled) {
        smoothPill(dc, circle, p.accent, p.accent, bg);
        const int inset = px(window, 5);
        RECT dot{circle.left + inset, circle.top + inset, circle.right - inset, circle.bottom - inset};
        smoothPill(dc, dot, p.onAccent, p.onAccent, p.accent);
      } else {
        const COLORREF fill = checked ? p.muted : (hot && enabled ? p.hover : p.field);
        const COLORREF edge = p.highContrast ? p.border
            : (hot && enabled ? mixColor(p.border, p.muted, 30) : mixColor(p.field, p.border, p.dark ? 70 : 78));
        smoothPill(dc, circle, fill, edge, bg);
        if (checked) {
          const int inset = px(window, 5);
          RECT dot{circle.left + inset, circle.top + inset, circle.right - inset, circle.bottom - inset};
          const COLORREF dotColor = GetPropW(window, surfaceKey) ? p.surface : p.background;
          smoothPill(dc, dot, dotColor, dotColor, fill);
        }
      }
      rect.left = px(window, 28);
      text(dc, title, rect, color);
    } else {
      bool primary = button && (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
      bool down = button && (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED);
      const bool keyboardFocus = focus && !down && GetKeyState(VK_LBUTTON) >= 0 && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
      const COLORREF ctrlFill = p.highContrast
          ? (primary && enabled ? p.accent : down ? p.pressed : hot ? p.hover : p.field)
          : (primary && enabled ? p.accent : down ? p.pressed : hot ? mixColor(p.field, p.hover, p.dark ? 75 : 55) : p.field);
      const COLORREF ctrlBorder = p.highContrast
          ? (keyboardFocus ? p.accent : p.border)
          : (keyboardFocus ? p.accent : (hot && enabled ? mixColor(p.border, p.muted, p.dark ? 32 : 28) : mixColor(p.field, p.border, p.dark ? 70 : 78)));
      rounded(dc, rect, ctrlFill, ctrlBorder, px(window, 6));
      rect.left += px(window, 12); rect.right -= px(window, combo ? 32 : 12);
      text(dc, title, rect, primary && enabled ? p.onAccent : color,
           DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (combo ? DT_LEFT : DT_CENTER));
      if (combo) {
        int x = rect.right + px(window, 14), y = rect.bottom / 2;
        auto pen = CreatePen(PS_SOLID, std::max(1, px(window, 1)), color); auto old = SelectObject(dc, pen);
        MoveToEx(dc, x - px(window, 4), y - px(window, 2), nullptr);
        LineTo(dc, x, y + px(window, 2)); LineTo(dc, x + px(window, 4), y - px(window, 2));
        SelectObject(dc, old); DeleteObject(pen);
      }
    }
    SelectObject(dc, oldFont);
    if (bufferDc) {
      BitBlt(paintDc, 0, 0, bufW, bufH, bufferDc, 0, 0, SRCCOPY);
      SelectObject(bufferDc, oldBmp);
      DeleteObject(bufferBmp);
      DeleteDC(bufferDc);
    }
    EndPaint(window, &paint); return 0;
  }
  auto result = DefSubclassProc(window, message, w, l);
  if (customPaint) {
    switch (message) {
    case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
    case BM_SETCHECK: case BM_SETSTATE: case CB_SETCURSEL: case WM_SETTEXT:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_KEYDOWN: case WM_KEYUP:
    case WM_UPDATEUISTATE: InvalidateRect(window, nullptr, FALSE); break;
    }
  }
  return result;
}
BOOL CALLBACK styleControl(HWND child, LPARAM data) {
  wchar_t name[32]{}; GetClassNameW(child, name, ARRAYSIZE(name));
  const bool button = !wcscmp(name, L"Button"), combo = !wcscmp(name, L"ComboBox");
  const bool customPaint = combo || (button && (GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW);
  if (customPaint) {
    SetWindowSubclass(child, controlProc, 0x464e, static_cast<DWORD_PTR>(data));
    InvalidateRect(child, nullptr, FALSE);
  }
  return TRUE;
}
}
ThemeMode configuredTheme() {
  DWORD value = 0, size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"Theme", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS && value <= 2 ? ThemeMode(value) : ThemeMode::System;
}
UINT themeChangedMessage() {
  static const UINT value = RegisterWindowMessageW(L"FolderNote.ThemeChanged.v1"); return value;
}
void notifyThemeChanged() {
  EnumWindows([](HWND window, LPARAM message) -> BOOL {
    wchar_t name[128]{}; GetClassNameW(window, name, ARRAYSIZE(name));
    if (!wcsncmp(name, L"FolderNote", 10)) PostMessageW(window, static_cast<UINT>(message), 0, 0);
    return TRUE;
  }, themeChangedMessage());
}
Palette palette(ThemeMode mode) {
  HIGHCONTRASTW contrast{sizeof(contrast)};
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON))
    return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW),
            GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_WINDOWTEXT),
            GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_GRAYTEXT), GetSysColor(COLOR_HIGHLIGHT),
            GetSysColor(COLOR_HIGHLIGHTTEXT), false, true};
  DWORD light = 1, size = sizeof(light);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && light == 0);
  // Explorer uses neutral surfaces rather than an accent-tinted application
  // background. Legacy GetSysColor has no dark palette, so use Explorer's
  // neutral dark surfaces while retaining the system selection/accent colors.
  if (dark) return {RGB(25,25,25), RGB(32,32,32), RGB(32,32,32), RGB(53,53,53), RGB(64,64,64),
                   RGB(64,64,64), RGB(240,240,240), RGB(166,166,166), GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT), true};
  return {GetSysColor(COLOR_BTNFACE), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), RGB(229,229,229), RGB(214,214,214),
          RGB(214,214,214), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_GRAYTEXT), GetSysColor(COLOR_HIGHLIGHT), GetSysColor(COLOR_HIGHLIGHTTEXT), false};
}
HFONT makeFont(UINT dpi, int size, int weight) {
  return CreateFontW(-MulDiv(size, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}
HFONT makeSymbolFont(UINT dpi, int size) {
  auto create = [&](const wchar_t *face) {
    return CreateFontW(-MulDiv(size, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        0, 0, CLEARTYPE_QUALITY, 0, face);
  };
  HFONT font = create(L"Segoe Fluent Icons");
  HDC dc = CreateCompatibleDC(nullptr); auto old = SelectObject(dc, font);
  wchar_t face[LF_FACESIZE]{}; GetTextFaceW(dc, LF_FACESIZE, face);
  SelectObject(dc, old); DeleteDC(dc);
  if (_wcsicmp(face, L"Segoe Fluent Icons")) { DeleteObject(font); font = create(L"Segoe MDL2 Assets"); }
  return font;
}
void rounded(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
  auto brush = CreateSolidBrush(fill); auto pen = CreatePen(PS_SOLID, 1, border);
  auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
  SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}
void text(HDC dc, const std::wstring &value, RECT rect, COLORREF color, UINT format) {
  SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
  DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, format);
}
void surface(HWND control, bool value) { if (value) SetPropW(control, surfaceKey, reinterpret_cast<HANDLE>(1)); else RemovePropW(control, surfaceKey); }
void muted(HWND control) { SetPropW(control, mutedKey, reinterpret_cast<HANDLE>(1)); }
void toggleSwitch(HWND control) { SetPropW(control, switchKey, reinterpret_cast<HANDLE>(1)); }
Theme::Theme() { reset(configuredTheme()); }
Theme::~Theme() { DeleteObject(background); DeleteObject(card); DeleteObject(field); }
void Theme::reset(ThemeMode value) {
  mode = value; colors = palette(value);
  DeleteObject(background); DeleteObject(card); DeleteObject(field);
  background = CreateSolidBrush(colors.background); card = CreateSolidBrush(colors.surface); field = CreateSolidBrush(colors.field);
}
void Theme::apply(HWND window) {
  const bool topLevel = (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) == 0;
  if (topLevel) {
    BOOL dark = colors.dark;
    if (FAILED(DwmSetWindowAttribute(window, 20, &dark, sizeof(dark))))
      DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
  }
  EnumChildWindows(window, styleControl, reinterpret_cast<LPARAM>(this));
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | (topLevel ? RDW_FRAME : 0));
}
bool Theme::handle(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
  if (message == WM_ERASEBKGND) {
    RECT rect{}; GetClientRect(window, &rect); FillRect(reinterpret_cast<HDC>(w), &rect, background); result = 1; return true;
  }
  if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC) {
    HDC dc = reinterpret_cast<HDC>(w); HWND control = reinterpret_cast<HWND>(l);
    bool onCard = GetPropW(control, surfaceKey) != nullptr;
    bool input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
    SetTextColor(dc, !IsWindowEnabled(control) || GetPropW(control, mutedKey) ? colors.muted : colors.text);
    SetBkColor(dc, input ? colors.field : onCard ? colors.surface : colors.background);
    result = reinterpret_cast<LRESULT>(input ? field : onCard ? card : background); return true;
  }
  if (message == WM_DRAWITEM) {
    auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
    if (item->CtlType != ODT_COMBOBOX || item->itemID == UINT(-1)) return false;
    auto brush = CreateSolidBrush((item->itemState & ODS_SELECTED) ? colors.hover : colors.field);
    FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
    int length = static_cast<int>(SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0));
    if (length >= 0) {
      std::wstring value(size_t(length) + 1, 0);
      SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(value.data()));
      value.resize(length); RECT rect = item->rcItem; rect.left += px(window, 10);
      text(item->hDC, value, rect, colors.text);
    }
    result = TRUE; return true;
  }
  return false;
}
}
