#include "quick_find_popup.h"
#include "language.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

namespace quick_find {
namespace {
constexpr auto popupClass = L"ShibaKit.QuickFind.Candidates";
constexpr UINT confirmMessage = WM_APP + 1;
constexpr UINT iconsReady = WM_APP + 2;
constexpr int rowHeight = 30;
thread_local Popup *watched = nullptr;
}
Popup::~Popup() {
  close();
  iconCache.reset();
  if (window) DestroyWindow(window);
  if (font) DeleteObject(font);
}
bool Popup::contains(HWND value) const { return window && (value == window || IsChild(window, value)); }
void Popup::begin(HWND anchor, const std::vector<std::wstring> &items, Choose callback, std::vector<CandidateIcon> itemIcons,
                  const std::vector<std::wstring> &matchLabels) {
  close();
  if (watched && watched != this) watched->close();
  options = settings();
  context = GetForegroundWindow();
  GUITHREADINFO info{sizeof(info)};
  GetGUIThreadInfo(GetWindowThreadProcessId(context, nullptr), &info); focus = info.hwndFocus;
  GetWindowRect(anchor, &anchorRect);
  POINT pointer{};
  if (options.atPointer && GetCursorPos(&pointer)) anchorRect = {pointer.x, pointer.y, pointer.x + 1, pointer.y + 1};
  dpi = GetDpiForWindow(anchor); if (!dpi) dpi = 96;
  labels = items; names.clear();
  icons = std::move(itemIcons); icons.resize(labels.size());
  for (const auto &label : matchLabels.size() == labels.size() ? matchLabels : labels) names.emplace_back(label);
  choose = std::move(callback);
  theme.reset(ui::configuredTheme());
  if (!window) {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = proc;
    cls.lpszClassName = popupClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, popupClass, lang::text(L"快速定位"),
        WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, GetAncestor(anchor, GA_ROOT), nullptr, cls.hInstance, this);
    if (!window) return;
    caption = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0,
        window, reinterpret_cast<HMENU>(1), cls.hInstance, nullptr);
    list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | LBS_NOTIFY |
        LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0,
        window, reinterpret_cast<HMENU>(2), cls.hInstance, nullptr);
    count = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_RIGHT, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(3), cls.hInstance, nullptr);
    scroll.create(window, 4, theme, [this](int top) { SendMessageW(list, LB_SETTOPINDEX, top, 0); }, true);
    SetWindowSubclass(list, listProc, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(GetAncestor(anchor, GA_ROOT)));
  iconCache.reset();
  if (options.showIcons) iconCache = std::make_unique<quick::icons::Cache>(window, iconsReady, px(16));
  if (font) DeleteObject(font);
  font = ui::makeFont(dpi);
  for (auto child : {caption, list, count}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  theme.apply(window);
  SendMessageW(list, LB_SETITEMHEIGHT, 0, px(rowHeight));
  opened = true;
  show = IsWindowVisible(GetAncestor(anchor, GA_ROOT)) != FALSE;
  if (show) {
    watched = this;
    mouse = SetWindowsHookExW(WH_MOUSE_LL, outsideMouse, GetModuleHandleW(nullptr), 0);
    SetTimer(window, 1, 100, nullptr);
  }
}
void Popup::close() {
  opened = false; refreshing = false; buffer.clear();
  if (watched == this) watched = nullptr;
  if (mouse) { UnhookWindowsHookEx(mouse); mouse = nullptr; }
  if (scroll.window()) SendMessageW(scroll.window(), WM_CANCELMODE, 0, 0);
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
  refreshing = true;
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
  SetWindowTextW(caption, buffer.c_str());
  SetWindowTextW(count, rows.empty() ? lang::text(L"无匹配") : std::to_wstring(rows.size()).c_str());
  MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST), &monitor);
  const int width = std::min(px(384), int(monitor.rcWork.right - monitor.rcWork.left));
  const int availableRows = std::max(1, (int(monitor.rcWork.bottom - monitor.rcWork.top) - px(42)) / px(rowHeight));
  pageRows = std::min({int(rows.size()), int(options.visibleRows), availableRows});
  const int height = rows.empty() ? px(38) : px(42) + pageRows * px(rowHeight);
  const bool overflow = int(rows.size()) > pageRows;
  const int desiredX = options.atPointer ? int(anchorRect.left) + px(8) : int(anchorRect.left + anchorRect.right - width) / 2;
  const int desiredY = int(anchorRect.top) + px(12);
  const int x = std::clamp(desiredX, int(monitor.rcWork.left), int(monitor.rcWork.right) - width);
  const int y = std::clamp(desiredY, int(monitor.rcWork.top), std::max(int(monitor.rcWork.top), int(monitor.rcWork.bottom) - height));
  MoveWindow(caption, px(14), px(10), std::max(0, width - px(106)), px(22), TRUE);
  MoveWindow(count, std::max(0, width - px(82)), px(10), px(68), px(22), TRUE);
  MoveWindow(list, px(6), px(36), std::max(0, width - px(12) - (overflow ? px(12) : 0)), pageRows * px(rowHeight), TRUE);
  // Native listboxes may preserve an old top row while resizing; each new
  // query starts with its highest-ranked match visible.
  SendMessageW(list, LB_SETTOPINDEX, 0, 0);
  ShowWindow(list, rows.empty() ? SW_HIDE : SW_SHOWNA);
  scroll.move(width - px(16), px(36), px(10), pageRows * px(rowHeight));
  refreshing = false;
  syncScroll();
  if (auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, px(12), px(12))) {
    if (!SetWindowRgn(window, region, FALSE)) DeleteObject(region);
  }
  SetWindowPos(window, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
  InvalidateRect(window, nullptr, TRUE); InvalidateRect(list, nullptr, TRUE);
}
void Popup::syncScroll() {
  if (refreshing || !scroll.window()) return;
  const int top = std::max(0, static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
  scroll.configure(static_cast<int>(rows.size()), std::max(1, pageRows), top, dpi);
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
      else selected += value == VK_UP ? -1 : value == VK_DOWN ? 1 : value == VK_PRIOR ? -pageRows : pageRows;
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
    else if (message == WM_MOUSEWHEEL && PostMessageW(watched->window, WM_MOUSEWHEEL,
        MAKEWPARAM(0, HIWORD(mouse->mouseData)), MAKELPARAM(mouse->pt.x, mouse->pt.y))) return 1;
  }
  return CallNextHookEx(nullptr, code, message, data);
}
LRESULT CALLBACK Popup::listProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &self = *reinterpret_cast<Popup *>(data);
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_MOUSEWHEEL) { self.scroll.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0; }
  if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE) {
    const auto hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, l);
    if (!HIWORD(hit) && LOWORD(hit) < self.rows.size()) {
      SendMessageW(window, LB_SETCURSEL, LOWORD(hit), 0);
      if (message == WM_LBUTTONUP) PostMessageW(self.window, confirmMessage, 0, 0);
    }
    return 0;
  }
  const auto result = DefSubclassProc(window, message, w, l);
  if (message == LB_SETTOPINDEX || message == LB_SETCURSEL) self.syncScroll();
  return result;
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
      self->window = self->list = self->caption = self->count = nullptr;
      self->close();
      return DefWindowProcW(window, message, w, l);
    }
    if (message == WM_CLOSE) { self->close(); return 0; }
    if (message == iconsReady) { if (self->active() && self->list) InvalidateRect(self->list, nullptr, FALSE); return 0; }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_MOUSEWHEEL) { self->scroll.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0; }
    if (message == WM_VSCROLL && reinterpret_cast<HWND>(l) == self->scroll.window()) { self->scroll.command(w); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
      PAINTSTRUCT paint{};
      const auto dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(w);
      RECT rect{}; GetClientRect(window, &rect);
      ui::rounded(dc, rect, self->theme.colors.field, self->theme.colors.border, self->px(6));
      if (message == WM_PAINT) EndPaint(window, &paint);
      return 0;
    }
    if (message == WM_CTLCOLORSTATIC) {
      const auto dc = reinterpret_cast<HDC>(w);
      SetTextColor(dc, reinterpret_cast<HWND>(l) == self->count ? self->theme.colors.muted : self->theme.colors.text);
      SetBkColor(dc, self->theme.colors.field);
      return reinterpret_cast<LRESULT>(self->theme.field);
    }
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
        FillRect(item->hDC, &rect, self->theme.field);
        if (item->itemState & ODS_SELECTED) {
          auto highlight = rect; InflateRect(&highlight, -self->px(1), -self->px(1));
          ui::rounded(item->hDC, highlight, colors.hover, colors.hover, self->px(4));
        }
        rect.left += self->px(10); rect.right -= self->px(8);
        const auto row = self->rows[item->itemID];
        const auto &source = self->icons[row];
        auto icon = self->options.showIcons ? source.image : quick::icons::Image{};
        if (!icon && self->iconCache) icon = self->iconCache->get(source.source);
        if (!icon && self->iconCache) icon = self->iconCache->get(quick::icons::Source::system(SIID_DOCNOASSOC));
        if (icon && icon->handle) DrawIconEx(item->hDC, rect.left, (rect.top + rect.bottom - self->px(16)) / 2,
            icon->handle, self->px(16), self->px(16), 0, nullptr, DI_NORMAL);
        if (self->options.showIcons) rect.left += self->px(24);
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
