#include "desktop_groups_color_picker.h"
#include "language.h"
#include "theme.h"
#include "window_position.h"
#include "desktop_groups_render.h"
#include <dwmapi.h>
#include <array>
#include <string_view>

namespace desktop_groups {
namespace {
constexpr int editId = 100, firstSwatch = 200;
constexpr int width = 356, height = 276;
constexpr std::array<COLORREF, 8> presets{
    RGB(159, 70, 85), RGB(171, 100, 69), RGB(170, 128, 64), RGB(72, 120, 102),
    RGB(62, 120, 135), RGB(72, 111, 173), RGB(121, 95, 159), RGB(96, 99, 110)};

std::wstring hexColor(COLORREF color) {
  constexpr wchar_t digits[] = L"0123456789ABCDEF";
  std::wstring value = L"#000000";
  const BYTE channels[]{GetRValue(color), GetGValue(color), GetBValue(color)};
  for (size_t i = 0; i < std::size(channels); ++i) {
    value[1 + 2 * i] = digits[channels[i] >> 4];
    value[2 + 2 * i] = digits[channels[i] & 15];
  }
  return value;
}

std::optional<COLORREF> parseColor(std::wstring_view value) {
  if (value.starts_with(L'#')) value.remove_prefix(1);
  if (value.size() != 6) return std::nullopt;
  DWORD rgb = 0;
  for (const auto digit : value) {
    unsigned nibble = 0;
    if (digit >= L'0' && digit <= L'9') nibble = digit - L'0';
    else if (digit >= L'A' && digit <= L'F') nibble = digit - L'A' + 10;
    else if (digit >= L'a' && digit <= L'f') nibble = digit - L'a' + 10;
    else return std::nullopt;
    rgb = (rgb << 4) | nibble;
  }
  return RGB(rgb >> 16, rgb >> 8, rgb);
}

struct Picker {
  HINSTANCE instance;
  COLORREF color;
  const std::function<void(COLORREF)> &preview;
  ui::Theme theme;
  HFONT font = nullptr;
  HFONT heading = nullptr;
  HWND edit = nullptr;
  HWND hint = nullptr;

  ~Picker() {
    if (font) DeleteObject(font);
    if (heading) DeleteObject(heading);
  }
};

int px(HWND dialog, int value) { return MulDiv(value, GetDpiForWindow(dialog), 96); }

RECT scale(HWND dialog, RECT area) {
  return {px(dialog, area.left), px(dialog, area.top), px(dialog, area.right), px(dialog, area.bottom)};
}

std::optional<COLORREF> readColor(HWND edit) {
  wchar_t input[8]{};
  GetWindowTextW(edit, input, ARRAYSIZE(input));
  return parseColor(input);
}

void applyColor(HWND dialog, Picker &picker, COLORREF color) {
  if (picker.color == color) return;
  picker.color = color;
  const auto previewBounds = scale(dialog, {24, 86, 76, 124});
  InvalidateRect(dialog, &previewBounds, FALSE);
  for (size_t i = 0; i < presets.size(); ++i)
    InvalidateRect(GetDlgItem(dialog, firstSwatch + static_cast<int>(i)), nullptr, FALSE);
  if (picker.preview) picker.preview(color);
}

bool initPicker(HWND dialog, Picker &picker) {
  const auto work = ui::mouseWorkArea();
  ui::centerLogical(dialog, work, width, height);
  ui::centerLogical(dialog, work, width, height);
  const auto dpi = GetDpiForWindow(dialog);
  picker.font = ui::makeFont(dpi);
  picker.heading = ui::makeFont(dpi, 21, FW_SEMIBOLD);
  const auto control = [&](const wchar_t *cls, const wchar_t *caption, DWORD style, RECT area, int id) {
    const auto bounds = scale(dialog, area);
    const auto child = CreateWindowExW(0, cls, caption, WS_CHILD | WS_VISIBLE | style,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), picker.instance, nullptr);
    if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(picker.font), FALSE);
    return child;
  };

  control(L"STATIC", lang::text(L"颜色值"), SS_NOPREFIX, {24, 62, 332, 82}, -1);
  picker.edit = control(L"EDIT", hexColor(picker.color).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, {94, 94, 320, 117}, editId);
  if (!picker.edit) return false;
  SendMessageW(picker.edit, EM_SETLIMITTEXT, 7, 0);
  picker.hint = control(L"STATIC", L"", SS_NOPREFIX, {82, 130, 332, 150}, -1);
  if (picker.hint) ui::muted(picker.hint);

  control(L"STATIC", lang::text(L"预设颜色"), SS_NOPREFIX, {24, 154, 332, 174}, -1);
  for (size_t i = 0; i < presets.size(); ++i) {
    const int left = 24 + static_cast<int>(i) * 39;
    if (!control(L"BUTTON", hexColor(presets[i]).c_str(), BS_OWNERDRAW | WS_TABSTOP,
                 {left, 176, left + 35, 212}, firstSwatch + static_cast<int>(i))) return false;
  }
  if (!control(L"BUTTON", lang::text(L"取消"), WS_TABSTOP, {140, 224, 232, 256}, IDCANCEL) ||
      !control(L"BUTTON", lang::text(L"确定"), BS_DEFPUSHBUTTON | WS_TABSTOP, {240, 224, 332, 256}, IDOK)) return false;
  SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
  picker.theme.apply(dialog);
  const DWMNCRENDERINGPOLICY frame = DWMNCRP_DISABLED;
  DwmSetWindowAttribute(dialog, DWMWA_NCRENDERING_POLICY, &frame, sizeof(frame));
  RECT client{};
  GetClientRect(dialog, &client);
  if (const auto region = createBoxRegion(client.right, client.bottom, px(dialog, 24)))
    if (!SetWindowRgn(dialog, region, FALSE)) DeleteObject(region);
  SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
  SetFocus(picker.edit);
  SendMessageW(picker.edit, EM_SETSEL, 0, -1);
  return true;
}

void drawPicker(HWND dialog, const Picker &picker) {
  PAINTSTRUCT paint{};
  const auto dc = BeginPaint(dialog, &paint);
  RECT client{};
  GetClientRect(dialog, &client);
  const auto &colors = picker.theme.colors;
  FillRect(dc, &client, picker.theme.background);
  if (const auto region = createBoxRegion(client.right, client.bottom, px(dialog, 24))) {
    const auto oldColor = SetDCBrushColor(dc, colors.border);
    FrameRgn(dc, region, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)), 1, 1);
    SetDCBrushColor(dc, oldColor);
    DeleteObject(region);
  }
  const auto previous = SelectObject(dc, picker.heading);
  ui::text(dc, lang::text(L"盒子颜色"), scale(dialog, {24, 18, 332, 50}), colors.text);
  SelectObject(dc, previous);
  ui::rounded(dc, scale(dialog, {24, 86, 76, 124}), picker.color, colors.border, px(dialog, 8));
  ui::rounded(dc, scale(dialog, {82, 86, 332, 124}), colors.field,
              GetFocus() == picker.edit ? colors.accent : colors.border, px(dialog, 8));
  EndPaint(dialog, &paint);
}

void drawSwatch(HWND dialog, const Picker &picker, const DRAWITEMSTRUCT &item) {
  auto bounds = item.rcItem;
  const auto color = presets[item.CtlID - firstSwatch];
  const auto &colors = picker.theme.colors;
  const bool selected = picker.color == color;
  const bool focused = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
  FillRect(item.hDC, &bounds, picker.theme.background);
  if (selected || focused) ui::rounded(item.hDC, bounds, colors.background, colors.accent, px(dialog, 9));
  InflateRect(&bounds, -px(dialog, 3), -px(dialog, 3));
  ui::rounded(item.hDC, bounds, color, colors.border, px(dialog, 7));
  if (selected) {
    const auto old = SelectObject(item.hDC, picker.font);
    ui::text(item.hDC, L"✓", bounds, RGB(255, 255, 255),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, old);
  }
}

INT_PTR CALLBACK pickerProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
  auto *picker = reinterpret_cast<Picker *>(GetWindowLongPtrW(dialog, DWLP_USER));
  try {
    switch (message) {
    case WM_INITDIALOG:
      picker = reinterpret_cast<Picker *>(l);
      SetWindowLongPtrW(dialog, DWLP_USER, l);
      if (initPicker(dialog, *picker)) return FALSE;
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    case WM_PAINT:
      if (picker) {
        drawPicker(dialog, *picker);
        return TRUE;
      }
      break;
    case WM_DRAWITEM:
      if (picker && w >= firstSwatch && w < firstSwatch + presets.size()) {
        drawSwatch(dialog, *picker, *reinterpret_cast<const DRAWITEMSTRUCT *>(l));
        return TRUE;
      }
      break;
    case WM_COMMAND: {
      if (!picker || !picker->edit) break;
      const auto id = LOWORD(w);
      if (id == editId) {
        if (HIWORD(w) == EN_CHANGE) {
          const auto color = readColor(picker->edit);
          SetWindowTextW(picker->hint, color ? L"" : lang::text(L"输入 #RRGGBB 格式的颜色值"));
          EnableWindow(GetDlgItem(dialog, IDOK), color.has_value());
          if (color) applyColor(dialog, *picker, *color);
        } else if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) {
          const auto bounds = scale(dialog, {82, 86, 332, 124});
          InvalidateRect(dialog, &bounds, FALSE);
        }
        return TRUE;
      }
      if (id >= firstSwatch && id < firstSwatch + presets.size() && HIWORD(w) == BN_CLICKED) {
        const auto selected = presets[id - firstSwatch];
        SetWindowTextW(picker->edit, hexColor(selected).c_str());
        applyColor(dialog, *picker, selected);
        return TRUE;
      }
      if (id == IDOK) {
        if (const auto color = readColor(picker->edit)) {
          applyColor(dialog, *picker, *color);
          EndDialog(dialog, IDOK);
        }
        return TRUE;
      }
      if (id != IDCANCEL) break;
      [[fallthrough]];
    }
    case WM_CLOSE:
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    LRESULT result{};
    if (picker && picker->theme.handle(dialog, message, w, l, result))
      return message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC ? result : TRUE;
  } catch (...) {
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}
}

std::optional<COLORREF> pickBoxColor(HINSTANCE instance, HWND owner, COLORREF initial,
                                    const std::function<void(COLORREF)> &preview) {
  struct Template {
    DLGTEMPLATE dialog;
    WORD menu = 0, cls = 0, title[64]{};
  };
  alignas(DWORD) Template layout{};
  layout.dialog.style = WS_POPUP | DS_CENTER;
  layout.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW;
  layout.dialog.cx = 200;
  layout.dialog.cy = 160;
  wcsncpy_s(reinterpret_cast<wchar_t *>(layout.title), ARRAYSIZE(layout.title),
            lang::text(L"盒子颜色"), _TRUNCATE);
  Picker picker{instance, initial, preview};
  const auto result = DialogBoxIndirectParamW(instance, &layout.dialog, owner, pickerProc, reinterpret_cast<LPARAM>(&picker));
  if (result != IDOK) return std::nullopt;
  return picker.color;
}
}
