#include "quick_find_popup.h"
#include "language.h"
#include <commctrl.h>
#include <algorithm>

namespace quick_find {
namespace {
constexpr auto popupClass = L"FolderNote.QuickFind.Candidates";
constexpr UINT confirmMessage = WM_APP + 1;
constexpr UINT iconsReady = WM_APP + 2;
thread_local Popup *watched = nullptr;
}
Popup::~Popup() {
  close();
  iconCache.reset();
  if (window) DestroyWindow(window);
  if (font) DeleteObject(font);
}
bool Popup::contains(HWND value) const { return window && (value == window || IsChild(window, value)); }
void Popup::begin(HWND anchor, const std::vector<std::wstring> &items, Choose callback, std::vector<CandidateIcon> itemIcons) {
  close();
  if (watched && watched != this) watched->close();
  context = GetForegroundWindow();
  GUITHREADINFO info{sizeof(info)};
  GetGUIThreadInfo(GetWindowThreadProcessId(context, nullptr), &info); focus = info.hwndFocus;
  GetWindowRect(anchor, &anchorRect);
  POINT pointer{};
  if (GetCursorPos(&pointer)) anchorRect = {pointer.x, pointer.y, pointer.x + 1, pointer.y + 1};
  dpi = GetDpiForWindow(anchor); if (!dpi) dpi = 96;
  labels = items; names.clear();
  icons = std::move(itemIcons); icons.resize(labels.size());
  for (const auto &label : labels) names.emplace_back(label);
  choose = std::move(callback);
  theme.reset(ui::configuredTheme());
  if (!window) {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = proc;
    cls.lpszClassName = popupClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, popupClass, lang::text(L"快速定位"),
        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, 0, 0, 0, 0, GetAncestor(anchor, GA_ROOT), nullptr, cls.hInstance, this);
    if (!window) return;
    caption = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 0, 0,
        window, reinterpret_cast<HMENU>(1), cls.hInstance, nullptr);
    list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
        LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0,
        window, reinterpret_cast<HMENU>(2), cls.hInstance, nullptr);
    hint = CreateWindowExW(0, L"STATIC", lang::text(L"↑↓ 选择 · Enter 确认 · Esc 关闭"),
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(3), cls.hInstance, nullptr);
    ui::muted(hint);
    SetWindowSubclass(list, listProc, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(GetAncestor(anchor, GA_ROOT)));
  iconCache = std::make_unique<quick::icons::Cache>(window, iconsReady, px(16));
  if (font) DeleteObject(font);
  font = ui::makeFont(dpi);
  for (auto child : {caption, list, hint}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  theme.apply(window);
  SendMessageW(list, LB_SETITEMHEIGHT, 0, px(32));
  opened = true;
  show = IsWindowVisible(GetAncestor(anchor, GA_ROOT)) != FALSE;
  if (show) {
    watched = this;
    mouse = SetWindowsHookExW(WH_MOUSE_LL, outsideMouse, GetModuleHandleW(nullptr), 0);
    SetTimer(window, 1, 100, nullptr);
  }
}
void Popup::close() {
  opened = false; buffer.clear();
  if (watched == this) watched = nullptr;
  if (mouse) { UnhookWindowsHookEx(mouse); mouse = nullptr; }
  if (window) { KillTimer(window, 1); ShowWindow(window, SW_HIDE); }
}
void Popup::character(wchar_t value) {
  if (!opened) return;
  if (value == L'\b') {
    if (!buffer.empty()) buffer.pop_back();
    if (!buffer.empty() && buffer.back() >= 0xD800 && buffer.back() <= 0xDBFF) buffer.pop_back();
    if (buffer.empty()) { close(); return; }
  } else if (inputCharacter(value) && buffer.size() < 128) buffer += value;
  else return;
  refresh();
}
void Popup::refresh() {
  rows.clear();
  std::vector<std::pair<int, int>> matches;
  for (size_t i = 0; i < names.size(); ++i) {
    const auto rank = names[i].rank(buffer);
    if (rank >= 0) matches.emplace_back(rank, static_cast<int>(i));
  }
  std::stable_sort(matches.begin(), matches.end(), [](auto a, auto b) { return a.first < b.first; });
  SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  for (const auto &[rank, row] : matches) {
    rows.push_back(row);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(labels[row].c_str()));
  }
  if (!rows.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
  SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  const auto title = buffer + L"  ·  " + std::to_wstring(rows.size()) + L" " + lang::text(L"个匹配项目");
  SetWindowTextW(caption, title.c_str());
  SetWindowTextW(hint, lang::text(rows.empty() ? L"没有匹配项目，继续输入或按 Backspace 修改" : L"↑↓ 选择 · Enter 确认 · Esc 关闭"));
  MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST), &monitor);
  const int width = std::min(px(430), int(monitor.rcWork.right - monitor.rcWork.left));
  const int height = px(78 + 32 * std::clamp(static_cast<int>(rows.size()), 1, 9));
  const int x = std::clamp(int(anchorRect.left) + px(8), int(monitor.rcWork.left), int(monitor.rcWork.right) - width);
  const int y = std::clamp(int(anchorRect.top) + px(12), int(monitor.rcWork.top), std::max(int(monitor.rcWork.top), int(monitor.rcWork.bottom) - height));
  MoveWindow(caption, px(14), px(12), width - px(28), px(24), TRUE);
  MoveWindow(list, px(8), px(44), width - px(18), height - px(78), TRUE);
  MoveWindow(hint, px(14), height - px(28), width - px(28), px(22), TRUE);
  SetWindowPos(window, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
  InvalidateRect(window, nullptr, TRUE); InvalidateRect(list, nullptr, TRUE);
}
void Popup::confirm() {
  const auto selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<size_t>(selected) >= rows.size()) return;
  auto callback = choose;
  const auto row = rows[selected];
  close();
  if (callback) callback(row);
}
bool Popup::key(UINT value) {
  if (!opened) return false;
  if (value == VK_ESCAPE) { close(); return true; }
  if (value == VK_RETURN) { confirm(); return true; }
  if (value == VK_BACK) { character(L'\b'); return true; }
  if (value == VK_UP || value == VK_DOWN || value == VK_PRIOR || value == VK_NEXT || value == VK_HOME || value == VK_END) {
    if (!rows.empty()) {
      int selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
      if (value == VK_HOME) selected = 0;
      else if (value == VK_END) selected = static_cast<int>(rows.size()) - 1;
      else selected += value == VK_UP ? -1 : value == VK_DOWN ? 1 : value == VK_PRIOR ? -9 : 9;
      selected = std::clamp(selected, 0, static_cast<int>(rows.size()) - 1);
      SendMessageW(list, LB_SETCURSEL, selected, 0);
    }
    return true;
  }
  if (value == VK_SPACE || value == VK_TAB) close();
  return false;
}
LRESULT CALLBACK Popup::outsideMouse(int code, WPARAM message, LPARAM data) {
  if (code == HC_ACTION && watched && watched->active() &&
      (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_MOUSEWHEEL)) {
    const auto mouse = reinterpret_cast<const MSLLHOOKSTRUCT *>(data);
    if (!watched->contains(WindowFromPoint(mouse->pt))) PostMessageW(watched->window, WM_CLOSE, 0, 0);
  }
  return CallNextHookEx(nullptr, code, message, data);
}
LRESULT CALLBACK Popup::listProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &self = *reinterpret_cast<Popup *>(data);
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE) {
    const auto hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, l);
    if (!HIWORD(hit) && LOWORD(hit) < self.rows.size()) {
      SendMessageW(window, LB_SETCURSEL, LOWORD(hit), 0);
      if (message == WM_LBUTTONUP) PostMessageW(self.window, confirmMessage, 0, 0);
    }
    return 0;
  }
  return DefSubclassProc(window, message, w, l);
}
LRESULT CALLBACK Popup::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto self = reinterpret_cast<Popup *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<Popup *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) return DefWindowProcW(window, message, w, l);
  try {
    if (message == WM_NCDESTROY) {
      self->iconCache.reset();
      self->window = self->list = self->caption = self->hint = nullptr;
      self->close();
      return DefWindowProcW(window, message, w, l);
    }
    if (message == WM_CLOSE) { self->close(); return 0; }
    if (message == iconsReady) { if (self->active() && self->list) InvalidateRect(self->list, nullptr, FALSE); return 0; }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == confirmMessage) { if (self->active()) self->confirm(); return 0; }
    if (message == WM_TIMER && self->active()) {
      GUITHREADINFO info{sizeof(info)};
      GetGUIThreadInfo(GetWindowThreadProcessId(self->context, nullptr), &info);
      if (GetForegroundWindow() != self->context || info.hwndFocus != self->focus) self->close();
      return 0;
    }
    if (message == WM_DRAWITEM) {
      const auto item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item->itemID != UINT(-1) && item->itemID < self->rows.size()) {
        auto rect = item->rcItem;
        const auto &colors = self->theme.colors;
        auto brush = CreateSolidBrush(item->itemState & ODS_SELECTED ? colors.hover : colors.field);
        FillRect(item->hDC, &rect, brush); DeleteObject(brush);
        rect.left += self->px(10); rect.right -= self->px(8);
        const auto row = self->rows[item->itemID];
        const auto &source = self->icons[row];
        auto icon = source.image;
        if (!icon && self->iconCache) icon = self->iconCache->get(source.source);
        if (!icon && self->iconCache) icon = self->iconCache->get(quick::icons::Source::system(SIID_DOCNOASSOC));
        if (icon && icon->handle) DrawIconEx(item->hDC, rect.left, (rect.top + rect.bottom - self->px(16)) / 2,
            icon->handle, self->px(16), self->px(16), 0, nullptr, DI_NORMAL);
        rect.left += self->px(24);
        auto old = SelectObject(item->hDC, self->font); SetBkMode(item->hDC, TRANSPARENT);
        ui::text(item->hDC, self->labels[self->rows[item->itemID]], rect, colors.text);
        SelectObject(item->hDC, old);
      }
      return TRUE;
    }
    LRESULT result = 0;
    if (self->theme.handle(window, message, w, l, result)) return result;
  } catch (...) { self->close(); }
  return DefWindowProcW(window, message, w, l);
}
}
