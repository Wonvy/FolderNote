#include "theme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>

namespace ui {
namespace {
constexpr wchar_t surfaceKey[] = L"FolderNote.Theme.Surface";
constexpr wchar_t mutedKey[] = L"FolderNote.Theme.Muted";
int px(HWND window, int n) { return MulDiv(n, GetDpiForWindow(window), 96); }
std::wstring caption(HWND window) {
  std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
  value.resize(GetWindowTextW(window, value.data(), static_cast<int>(value.size())));
  return value;
}
LRESULT CALLBACK controlProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto &theme = *reinterpret_cast<Theme *>(data);
  wchar_t name[32]{}; GetClassNameW(window, name, ARRAYSIZE(name));
  const bool button = !wcscmp(name, L"Button"), combo = !wcscmp(name, L"ComboBox");
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, controlProc, id);
    RemovePropW(window, surfaceKey); RemovePropW(window, mutedKey);
    return DefSubclassProc(window, message, w, l);
  }
  if (message == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
    InvalidateRect(window, nullptr, FALSE);
  }
  if (message == WM_PAINT && (combo || (button && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW))) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
    RECT rect{}; GetClientRect(window, &rect);
    auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    const auto &p = theme.colors;
    const bool enabled = IsWindowEnabled(window) != FALSE, focus = GetFocus() == window;
    POINT mouse{}; GetCursorPos(&mouse); ScreenToClient(window, &mouse);
    bool hot = enabled && PtInRect(&rect, mouse);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    auto color = enabled ? p.text : p.muted;
    FillRect(dc, &rect, GetPropW(window, surfaceKey) ? theme.card : theme.background);
    auto title = caption(window);
    if (button && ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX || (style & BS_TYPEMASK) == BS_CHECKBOX)) {
      int size = px(window, 18), top = (rect.bottom - size) / 2;
      RECT box{1, top, size + 1, top + size};
      bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
      rounded(dc, box, checked ? p.accent : (hot ? p.hover : p.field), checked ? p.accent : p.border, px(window, 4));
      if (checked) {
        auto pen = CreatePen(PS_SOLID, std::max(1, px(window, 2)), p.onAccent);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, px(window, 5), top + size / 2, nullptr);
        LineTo(dc, px(window, 8), top + size * 3 / 4); LineTo(dc, px(window, 14), top + size / 4);
        SelectObject(dc, old); DeleteObject(pen);
      }
      rect.left = px(window, 28); text(dc, title, rect, color);
    } else {
      bool primary = button && (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
      bool down = button && (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED);
      rounded(dc, rect, primary && enabled ? p.accent : down ? p.pressed : hot ? p.hover : p.field,
              focus ? p.accent : p.border, px(window, 6));
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
    if (focus && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
      RECT focusRect{}; GetClientRect(window, &focusRect); InflateRect(&focusRect, -px(window, 3), -px(window, 3));
      SetTextColor(dc, p.text); SetBkColor(dc, p.field); DrawFocusRect(dc, &focusRect);
    }
    SelectObject(dc, oldFont); EndPaint(window, &paint); return 0;
  }
  auto result = DefSubclassProc(window, message, w, l);
  switch (message) {
  case WM_MOUSELEAVE: case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
  case BM_SETCHECK: case BM_SETSTATE: case CB_SETCURSEL: case WM_SETTEXT:
  case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_KEYDOWN: case WM_KEYUP:
  case WM_UPDATEUISTATE: InvalidateRect(window, nullptr, FALSE); break;
  }
  return result;
}
BOOL CALLBACK styleControl(HWND child, LPARAM data) {
  SetWindowSubclass(child, controlProc, 0x464e, static_cast<DWORD_PTR>(data));
  InvalidateRect(child, nullptr, TRUE); return TRUE;
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
Theme::Theme() { reset(configuredTheme()); }
Theme::~Theme() { DeleteObject(background); DeleteObject(card); DeleteObject(field); }
void Theme::reset(ThemeMode value) {
  mode = value; colors = palette(value);
  DeleteObject(background); DeleteObject(card); DeleteObject(field);
  background = CreateSolidBrush(colors.background); card = CreateSolidBrush(colors.surface); field = CreateSolidBrush(colors.field);
}
void Theme::apply(HWND window) {
  BOOL dark = colors.dark;
  if (FAILED(DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
  DWORD corners = 2; DwmSetWindowAttribute(window, 33, &corners, sizeof(corners));
  DwmSetWindowAttribute(window, 35, &colors.background, sizeof(COLORREF));
  DwmSetWindowAttribute(window, 36, &colors.text, sizeof(COLORREF));
  DwmSetWindowAttribute(window, 34, &colors.border, sizeof(COLORREF));
  EnumChildWindows(window, styleControl, reinterpret_cast<LPARAM>(this));
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
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
