#include "quick_folders.h"
#include "language.h"
#include "core.h"
#include "window_position.h"
#include "quick_icons.h"
#include "theme_scroll.h"
#include "quick_find_controls.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <algorithm>
#include <shellapi.h>
namespace quick {
namespace {
constexpr UINT iconsReady = WM_APP + 81;
struct Manager {
  HWND pins = nullptr, history = nullptr, tips = nullptr;
  HFONT font = nullptr;
  ui::Theme theme;
  ui::ScrollBar pinsScroll, historyScroll;
  std::unique_ptr<icons::Cache> icons;
  icons::Image folder;
  UINT dpi = 96;
  int hotPin = -1, hotHistory = -1;
  std::vector<HWND> controls;
  State data;
  bool done = false;
  ~Manager() { DeleteObject(font); }
  int px(int n) const { return MulDiv(n, dpi, 96); }
  ui::ScrollBar &scroll(HWND list) { return list == pins ? pinsScroll : historyScroll; }
  void syncList(HWND list) {
    if (!scroll(list).window()) return;
    RECT rect{}; GetClientRect(list, &rect);
    int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
    scroll(list).configure(count, std::max(1, int(rect.bottom) / px(30)), std::max(0, top), dpi);
    scroll(list).move(rect.right - px(14), px(2), px(12), std::max(1, int(rect.bottom) - px(4)));
  }
  void style() {
    if (tips) {
      SendMessageW(tips, TTM_SETTIPBKCOLOR, theme.colors.surface, 0);
      SendMessageW(tips, TTM_SETTIPTEXTCOLOR, theme.colors.text, 0);
    }
    for (auto list : {pins, history}) { syncList(list); InvalidateRect(list, nullptr, TRUE); }
  }
  void layout(HWND window) {
    if (!pins || !history) return;
    RECT rect{}; GetClientRect(window, &rect);
    const int margin = px(20), gap = px(20), columns = std::max(px(120), (int(rect.right) - margin * 2 - gap) / 2);
    const int right = margin + columns + gap;
    const int buttonsY = std::max(px(108), int(rect.bottom) - px(48));
    const int listHeight = std::max(px(30), buttonsY - px(60));
    auto move = [&](int id, int x, int y, int width, int height) { MoveWindow(GetDlgItem(window, id), x, y, width, height, TRUE); };
    move(30, margin, px(12), columns, px(24)); move(31, right, px(12), columns, px(24));
    move(10, margin, px(48), columns, listHeight); move(11, right, px(48), columns, listHeight);
    const int buttonGap = px(8), buttonWidth = (columns - 3 * buttonGap) / 4;
    for (int index = 0; index < 4; ++index) move(20 + index, margin + index * (buttonWidth + buttonGap), buttonsY, buttonWidth, px(32));
    const int wide = std::min(px(110), (columns - buttonGap) / 2);
    move(24, right, buttonsY, wide, px(32)); move(25, right + wide + buttonGap, buttonsY, wide, px(32));
    style();
  }
  void scale(HWND window, UINT value) {
    if (!value || value == dpi) { layout(window); return; }
    dpi = value; auto old = font; font = ui::makeFont(dpi);
    icons = std::make_unique<icons::Cache>(window, iconsReady, px(16));
    folder = icons::read(icons::Source::system(SIID_FOLDER), px(16));
    for (auto control : controls) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    DeleteObject(old);
    for (auto list : {pins, history}) SendMessageW(list, LB_SETITEMHEIGHT, 0, px(30));
    SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, px(520));
    layout(window);
  }
  static LRESULT CALLBACK listProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR value) {
    auto &state = *reinterpret_cast<Manager *>(value);
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, listProc, id); return DefSubclassProc(window, message, w, l); }
    if (message == WM_MOUSEWHEEL) { state.scroll(window).wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0; }
    if (message == WM_VSCROLL && reinterpret_cast<HWND>(l) == state.scroll(window).window()) { state.scroll(window).command(w); return 0; }
    if (message == WM_MOUSEMOVE && state.tips) {
      auto hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, l);
      int row = HIWORD(hit) ? -1 : LOWORD(hit); auto &previous = window == state.pins ? state.hotPin : state.hotHistory;
      if (row != previous) { SendMessageW(state.tips, TTM_POP, 0, 0); previous = row; }
    }
    auto result = DefSubclassProc(window, message, w, l);
    switch (message) {
    case WM_KEYDOWN: case WM_LBUTTONDOWN: case LB_SETCURSEL: case LB_SETTOPINDEX:
    case LB_ADDSTRING: case LB_RESETCONTENT: case WM_SIZE: case WM_SETFONT: state.syncList(window); break;
    }
    return result;
  }
  void prepareList(HWND window, HWND parent) {
    quick_find::attach(window, [this, window](int row) {
      const auto &paths = window == pins ? data.pins : data.history;
      if (row < 0 || size_t(row) >= paths.size()) return;
      const auto attributes = GetFileAttributesW(paths[row].c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES && quick_find::settings().opens((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
        ShellExecuteW(window, L"open", paths[row].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    SendMessageW(window, LB_SETITEMHEIGHT, 0, px(30));
    SetWindowSubclass(window, listProc, 0x464e, reinterpret_cast<DWORD_PTR>(this));
    scroll(window).create(window, 1, theme, [this, window](int top) {
      if (tips) SendMessageW(tips, TTM_POP, 0, 0);
      SendMessageW(window, LB_SETTOPINDEX, top, 0);
    });
    TOOLINFOW tool{sizeof(tool)}; tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tool.hwnd = parent;
    tool.uId = reinterpret_cast<UINT_PTR>(window); tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  }
  void draw(const DRAWITEMSTRUCT &item) {
    if (item.CtlType != ODT_LISTBOX) return;
    FillRect(item.hDC, &item.rcItem, theme.background);
    if (item.itemID == UINT(-1)) return;
    const auto &paths = item.hwndItem == pins ? data.pins : data.history;
    if (item.itemID >= paths.size()) return;
    const auto &p = theme.colors; RECT row = item.rcItem; auto dc = item.hDC;
    row.right -= px(15); InflateRect(&row, -px(3), -px(2));
    bool selected = (item.itemState & ODS_SELECTED) != 0;
    if (selected) {
      ui::rounded(dc, row, p.hover, p.hover, px(4));
      RECT marker{row.left, row.top + px(5), row.left + px(3), row.bottom - px(5)};
      ui::rounded(dc, marker, p.accent, p.accent, px(1));
    }
    auto image = icons ? icons->get(icons::Source::file(paths[item.itemID])) : icons::Image{};
    HICON icon = image ? image->handle : folder ? folder->handle : nullptr;
    if (icon) DrawIconEx(dc, row.left + px(9), row.top + (row.bottom - row.top - px(16)) / 2,
        icon, px(16), px(16), 0, nullptr, DI_NORMAL);
    RECT label = row; label.left += px(34); label.right -= px(7);
    auto old = SelectObject(dc, font);
    ui::text(dc, paths[item.itemID], label, selected && p.highContrast ? p.onAccent : p.text,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
  }
  void refresh() {
    if (tips) SendMessageW(tips, TTM_POP, 0, 0);
    data = load();
    SendMessageW(pins, WM_SETREDRAW, FALSE, 0); SendMessageW(history, WM_SETREDRAW, FALSE, 0);
    SendMessageW(pins, LB_RESETCONTENT, 0, 0); SendMessageW(history, LB_RESETCONTENT, 0, 0);
    for (const auto &path : data.pins) SendMessageW(pins, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
    for (const auto &path : data.history) SendMessageW(history, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
    SendMessageW(pins, WM_SETREDRAW, TRUE, 0); SendMessageW(history, WM_SETREDRAW, TRUE, 0);
    style();
  }
};
LRESULT CALLBACK managerProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) {
    try {
      auto state = std::make_unique<Manager>();
      auto *mode = static_cast<ui::ThemeMode *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
      state->theme.reset(mode ? *mode : ui::configuredTheme());
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.release()));
      return TRUE;
    } catch (...) { return FALSE; }
  }
  auto *state = reinterpret_cast<Manager *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return DefWindowProcW(window, message, w, l);
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0); delete state;
    return DefWindowProcW(window, message, w, l);
  }
  if (message == ui::themeChangedMessage()) {
    state->theme.reset(ui::configuredTheme()); state->theme.apply(window); state->style(); return 0;
  }
  if (message == WM_CTLCOLORLISTBOX &&
      (reinterpret_cast<HWND>(l) == state->pins || reinterpret_cast<HWND>(l) == state->history)) {
    const auto dc = reinterpret_cast<HDC>(w);
    SetTextColor(dc, state->theme.colors.text);
    SetBkColor(dc, state->theme.colors.background);
    return reinterpret_cast<LRESULT>(state->theme.background);
  }
  LRESULT themed = 0;
  if (state->theme.handle(window, message, w, l, themed)) return themed;
  try {
    if (message == WM_CREATE) {
      auto dpi = GetDpiForWindow(window);
      state->dpi = dpi; state->font = ui::makeFont(dpi);
      state->icons = std::make_unique<icons::Cache>(window, iconsReady, state->px(16));
      state->folder = icons::read(icons::Source::system(SIID_FOLDER), state->px(16));
      state->tips = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
          WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
      SetWindowTheme(state->tips, L"", L""); SendMessageW(state->tips, TTM_SETMAXTIPWIDTH, 0, state->px(520));
      auto add = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
        HWND control = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                     window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        state->controls.push_back(control);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE); return control;
      };
      add(L"STATIC", lang::text(L"置顶文件夹（最多 64 个）"), 0, 30);
      add(L"STATIC", lang::text(L"历史记录（最近 20 条）"), 0, 31);
      DWORD listStyle = WS_TABSTOP | WS_CLIPCHILDREN | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS;
      state->pins = add(L"LISTBOX", lang::text(L"置顶文件夹"), listStyle, 10);
      state->history = add(L"LISTBOX", lang::text(L"历史记录"), listStyle, 11);
      state->prepareList(state->pins, window); state->prepareList(state->history, window);
      add(L"BUTTON", lang::text(L"添加…"), WS_TABSTOP, 20);
      add(L"BUTTON", lang::text(L"移除"), WS_TABSTOP, 21);
      add(L"BUTTON", lang::text(L"上移"), WS_TABSTOP, 22);
      add(L"BUTTON", lang::text(L"下移"), WS_TABSTOP, 23);
      add(L"BUTTON", lang::text(L"置顶所选"), WS_TABSTOP, 24);
      add(L"BUTTON", lang::text(L"清空历史…"), WS_TABSTOP, 25);
      state->theme.apply(window); state->layout(window); state->refresh(); return 0;
    }
    if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
      state->theme.reset(state->theme.mode); state->theme.apply(window); state->style(); return 0;
    }
    if (message == WM_MEASUREITEM) { reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = state->px(30); return TRUE; }
    if (message == WM_DRAWITEM) { state->draw(*reinterpret_cast<DRAWITEMSTRUCT *>(l)); return TRUE; }
    if (message == iconsReady && !state->done) {
      InvalidateRect(state->pins, nullptr, FALSE); InvalidateRect(state->history, nullptr, FALSE); return 0;
    }
    if (message == WM_NOTIFY && reinterpret_cast<NMHDR *>(l)->code == TTN_GETDISPINFOW) {
      auto *info = reinterpret_cast<NMTTDISPINFOW *>(l); HWND list = reinterpret_cast<HWND>(info->hdr.idFrom);
      if (list == state->pins || list == state->history) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(list, &point);
        auto hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
        const auto &paths = list == state->pins ? state->data.pins : state->data.history;
        if (!HIWORD(hit) && size_t(LOWORD(hit)) < paths.size()) info->lpszText = const_cast<LPWSTR>(paths[LOWORD(hit)].c_str());
      }
      return 0;
    }
    if (message == WM_SIZE) { state->layout(window); return 0; }
    if (message == WM_DPICHANGED_AFTERPARENT) { state->scale(window, GetDpiForWindow(window)); return 0; }
    if (message == WM_DPICHANGED) { state->scale(window, HIWORD(w)); return 0; }
    if (message == WM_COMMAND) {
      const int id = LOWORD(w);
      if (id == IDOK || id == IDCANCEL) { SendMessageW(GetParent(window), message, w, l); return 0; }
      auto pinIndex = SendMessageW(state->pins, LB_GETCURSEL, 0, 0);
      auto historyIndex = SendMessageW(state->history, LB_GETCURSEL, 0, 0);
      if (id == 20) {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
          throw fn::Error{lang::text(L"无法打开文件夹选择窗口。")};
        DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(lang::text(L"选择要置顶的文件夹"));
        if (SUCCEEDED(dialog->Show(window))) {
          Microsoft::WRL::ComPtr<IShellItem> item; PWSTR path = nullptr;
          if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::wstring value(path); CoTaskMemFree(path); pin(value, true);
          }
        }
      } else if (id >= 21 && id <= 23 && pinIndex >= 0 && size_t(pinIndex) < state->data.pins.size()) {
        auto path = state->data.pins[size_t(pinIndex)];
        if (id == 21) pin(path, false); else movePin(path, id == 22 ? -1 : 1);
      } else if (id == 24 && historyIndex >= 0 && size_t(historyIndex) < state->data.history.size()) pin(state->data.history[size_t(historyIndex)], true);
      else if (id == 25 && ui::message(window, lang::text(L"清空访问历史和访问次数？置顶文件夹将保留。"), lang::text(L"快速文件夹"), MB_YESNO | MB_ICONQUESTION) == IDYES) clearHistory();
      else return 0;
      state->refresh();
      if (id == 22 || id == 23) SendMessageW(state->pins, LB_SETCURSEL,
          static_cast<WPARAM>(std::clamp<LRESULT>(pinIndex + (id == 22 ? -1 : 1), 0, static_cast<LRESULT>(state->data.pins.size()) - 1)), 0);
      return 0;
    }
    if (message == WM_CLOSE) { SendMessageW(GetParent(window), WM_CLOSE, 0, 0); return 0; }
    if (message == WM_DESTROY) { state->done = true; state->icons.reset(); if (state->tips) DestroyWindow(state->tips); state->tips = nullptr; return 0; }
  } catch (const fn::Error &error) {
    if (message == WM_CREATE) return -1;
    ui::message(window, error.message.c_str(), lang::text(L"快速文件夹"), MB_OK | MB_ICONERROR);
  } catch (...) {
    if (message == WM_CREATE) return -1;
    ui::message(window, lang::text(L"操作未完成，请稍后重试。"), lang::text(L"快速文件夹"), MB_OK | MB_ICONERROR);
  }
  return DefWindowProcW(window, message, w, l);
}
}
HWND createManagerPage(HINSTANCE instance, HWND parent, ui::ThemeMode theme) {
  WNDCLASSW cls{}; cls.lpfnWndProc = managerProc; cls.hInstance = instance;
  cls.lpszClassName = L"FolderNote.QuickFolders.ManagerPage";
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&cls);
  return CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, lang::text(L"置顶与历史"),
      WS_CHILD | WS_CLIPCHILDREN, 0, 0, 700, 400, parent, nullptr, instance, &theme);
}
void setManagerTheme(HWND window, ui::ThemeMode mode) {
  if (!IsWindow(window)) return;
  auto *state = reinterpret_cast<Manager *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return;
  state->theme.reset(mode); state->theme.apply(window); state->style();
}
void refreshManagerPage(HWND window) {
  if (!IsWindow(window)) return;
  auto *state = reinterpret_cast<Manager *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state) state->refresh();
}
int manage(HINSTANCE, HWND owner, ui::ThemeMode) {
  wchar_t path[32768]{};
  if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return 1;
  auto result = ShellExecuteW(owner, L"open", path, L"--quick-manage", nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    ui::message(owner, lang::text(L"无法打开 FolderNote 设置，请从开始菜单重试。"), L"FolderNote", MB_OK | MB_ICONERROR);
    return 1;
  }
  return 0;
}
}
