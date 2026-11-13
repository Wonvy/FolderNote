#include "quick_popup.h"
#include "language.h"
#include "window_position.h"
#include "core.h"
#include "quick_icons.h"
#include <shlobj.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>

namespace quick::popup {
namespace {
constexpr wchar_t panelClass[] = L"FolderNote.QuickFolders.Popup.v2";
constexpr UINT checkFocus = WM_APP + 71, contextKey = WM_APP + 72, dismiss = WM_APP + 73, iconsReady = WM_APP + 74;
constexpr UINT_PTR hoverTimer = 1, loadingTimer = 2, tipTimer = 3;
wchar_t actionGlyph(Symbol symbol) {
  switch (symbol) {
  case Symbol::Star: return L'\xE718';
  case Symbol::Unpin: return L'\xE77A';
  case Symbol::Settings: return L'\xE713';
  case Symbol::Trash: return L'\xE74D';
  case Symbol::Window: return L'\xE78B';
  case Symbol::Copy: return L'\xE8C8';
  case Symbol::Move: return L'\xE8DE';
  case Symbol::Visibility: return L'\xE890';
  case Symbol::AddFolder: return L'\xE8F4';
  default: return 0;
  }
}
struct Session;
struct Panel {
  Session *session = nullptr;
  HWND window = nullptr, list = nullptr;
  std::shared_ptr<Branch> branch;
  size_t level = 0;
  int selected = -1, selectedAction = -1;
  int px(int value) const;
};
struct Session {
  HINSTANCE instance;
  ui::Theme theme;
  UINT dpi = 96;
  HFONT font = nullptr, smallFont = nullptr, symbols = nullptr;
  HICON folder = nullptr;
  std::unique_ptr<icons::Cache> iconCache;
  std::vector<std::unique_ptr<Panel>> panels;
  unsigned selected = 0;
  bool done = false, armed = false;
  HWND anchor = nullptr;
  HWND tooltip = nullptr;
  std::wstring tipText;
  POINT tipPoint{};
  bool activated = false;
  size_t active = 0, pendingLevel = 0;
  int pendingRow = -1, downRow = -1, downAction = -1;
  size_t downLevel = 0;
  POINT lastMouse{LONG_MIN, LONG_MIN};
  explicit Session(HINSTANCE value, ui::ThemeMode mode) : instance(value) { theme.reset(mode); }
  ~Session() { close(); DeleteObject(font); DeleteObject(smallFont); DeleteObject(symbols); if (folder) DestroyIcon(folder); }
  int px(int value) const { return MulDiv(value, dpi, 96); }
  HWND root() const { return panels.empty() ? nullptr : panels[0]->window; }
  TOOLINFOW tipInfo() const {
    TOOLINFOW info{sizeof(info)}; info.hwnd = root(); info.uId = 1; return info;
  }
  void hideTip() {
    if (root()) KillTimer(root(), tipTimer);
    if (tooltip) { auto info = tipInfo(); SendMessageW(tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info)); }
    tipText.clear();
  }
  void queueTip(const Panel &panel, int row, int action, POINT point) {
    std::wstring text;
    if (row >= 0 && size_t(row) < panel.branch->items.size()) {
      const auto &item = panel.branch->items[row];
      if (action >= 0 && size_t(action) < item.actions.size()) text = item.actions[action].title;
      else if (item.symbol == Symbol::More) text = item.title;
      else if (!item.iconPath.empty()) text = item.iconPath;
    }
    if (text == tipText) return;
    hideTip(); tipText = std::move(text); tipPoint = point;
    if (!tipText.empty()) SetTimer(root(), tipTimer, 600, nullptr);
  }
  void showTip() {
    KillTimer(root(), tipTimer);
    if (done || tipText.empty() || !IsWindowVisible(root())) return;
    if (!tooltip) {
      tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
          WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
          root(), nullptr, instance, nullptr);
      if (!tooltip) return;
      // Tooltip colour messages require unthemed drawing; use our system-derived
      // palette in both light/dark mode without activating a second window.
      SetWindowTheme(tooltip, L"", L"");
      auto info = tipInfo(); info.uFlags = TTF_TRACK | TTF_ABSOLUTE; info.lpszText = tipText.data();
      SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
      SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, px(360));
      SendMessageW(tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
    }
    auto info = tipInfo(); info.lpszText = tipText.data();
    SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    SendMessageW(tooltip, TTM_SETTIPBKCOLOR, theme.colors.surface, 0);
    SendMessageW(tooltip, TTM_SETTIPTEXTCOLOR, theme.colors.text, 0);
    SendMessageW(tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(tipPoint.x + px(12), tipPoint.y + px(22)));
    SendMessageW(tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
  }
  bool family(HWND window) const {
    if (!window) return false;
    if (window == tooltip) return true;
    return std::any_of(panels.begin(), panels.end(), [&](const auto &panel) {
      return window == panel->window || IsChild(panel->window, window);
    });
  }
  bool context(HWND window) const {
    return family(window) || (!activated && anchor && window == anchor && IsWindow(anchor));
  }
  void observeForeground(HWND window) {
    if (!armed || done) return;
    if (family(window)) activated = true;
    else if (!context(window)) finish();
  }
  void finish(unsigned command = 0) {
    if (done) return;
    done = true; selected = command; armed = false;
    hideTip();
    if (root()) { KillTimer(root(), hoverTimer); KillTimer(root(), loadingTimer); }
    if (family(GetCapture())) ReleaseCapture();
    for (auto &panel : panels) ShowWindow(panel->window, SW_HIDE);
    // A WinEvent/sent-message callback may run inside GetMessage, which keeps
    // waiting after the callback returns. Wake it so the loop can observe done
    // and release the singleton, hooks and transient process immediately.
    if (root()) PostMessageW(root(), WM_NULL, 0, 0);
  }
  void close() {
    armed = false;
    hideTip();
    if (tooltip) { DestroyWindow(tooltip); tooltip = nullptr; }
    iconCache.reset();
    if (family(GetCapture())) ReleaseCapture();
    while (!panels.empty()) { auto panel = std::move(panels.back()); panels.pop_back(); if (IsWindow(panel->window)) DestroyWindow(panel->window); }
  }
  void trim(size_t count) {
    if (panels.size() > count) hideTip();
    while (panels.size() > count) { auto panel = std::move(panels.back()); panels.pop_back(); DestroyWindow(panel->window); }
    active = std::min(active, panels.size() - 1);
  }
  int height(const Item &item) const { return px(item.row == Row::Separator ? 9 : item.row == Row::Toolbar ? 34 : 28); }
  bool actionable(const Item &item) const {
    if (!item.actions.empty()) return std::any_of(item.actions.begin(), item.actions.end(), [](const auto &action) { return action.enabled; });
    return item.row == Row::Action && (item.command || item.children);
  }
  int actionWidth(const IconAction &action) const { return px(action.text.empty() ? 28 : 52); }
  int actionsWidth(const Item &item, int first, int last) const {
    int width = 0;
    for (int i = first; i < last; ++i) width += actionWidth(item.actions[i]) + px(4);
    return width;
  }
  RECT actionRect(const Item &item, RECT row, int action) const {
    int size = px(28);
    if (item.row == Row::Tabs) {
      const int count = static_cast<int>(item.actions.size());
      const int gap = px(4);
      const int left = row.left + px(5);
      const int width = (row.right - left - px(5) - (count - 1) * gap) / count;
      const int start = left + action * (width + gap);
      return {start, row.top, start + width, row.bottom};
    }
    int leading = static_cast<int>(std::min<size_t>(item.leadingActions, item.actions.size()));
    int left = action < leading ? row.left + px(5) + actionsWidth(item, 0, action) :
        row.right - px(5) - actionsWidth(item, leading, static_cast<int>(item.actions.size())) + actionsWidth(item, leading, action);
    int top = row.top + (row.bottom - row.top - size) / 2;
    return {left, top, left + actionWidth(item.actions[action]), top + size};
  }
  void populate(Panel &panel) {
    SendMessageW(panel.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(panel.list, LB_RESETCONTENT, 0, 0);
    for (const auto &item : panel.branch->items) {
      std::wstring accessible = item.title;
      for (const auto &action : item.actions) {
        if (!accessible.empty()) accessible += lang::text(L"；");
        accessible += action.title;
        if (!action.enabled) accessible += lang::text(L"（不可用）");
        else if (action.checked) accessible += lang::text(L"（已选）");
      }
      int row = static_cast<int>(SendMessageW(panel.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(accessible.c_str())));
      SendMessageW(panel.list, LB_SETITEMHEIGHT, row, height(item));
    }
    panel.selected = -1; panel.selectedAction = -1;
    SendMessageW(panel.list, WM_SETREDRAW, TRUE, 0); InvalidateRect(panel.window, nullptr, TRUE); InvalidateRect(panel.list, nullptr, TRUE);
  }
  void select(Panel &panel, int row, int action = -1) {
    if (row < 0 || size_t(row) >= panel.branch->items.size() || !actionable(panel.branch->items[row])) row = -1;
    if (row >= 0 && !panel.branch->items[row].actions.empty()) {
      const auto &actions = panel.branch->items[row].actions;
      if (action < 0) action = int(std::find_if(actions.begin(), actions.end(), [](const auto &value) { return value.enabled; }) - actions.begin());
      if (size_t(action) >= actions.size() || !actions[action].enabled) { row = -1; action = -1; }
    }
    else action = -1;
    if (panel.selected == row && panel.selectedAction == action) return;
    panel.selected = row; panel.selectedAction = action; SendMessageW(panel.list, LB_SETCURSEL, row, 0);
    InvalidateRect(panel.list, nullptr, FALSE);
  }
  void place(Panel &panel, POINT point, bool child, RECT parent = {}, bool keepTop = false) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT work = monitor.rcWork;
    int content = px(16); for (const auto &item : panel.branch->items) content += height(item);
    int width = std::min(px(260), int(work.right - work.left));
    const auto availableHeight = keepTop ? work.bottom - point.y : work.bottom - work.top - px(16);
    int heightPixels = std::min(content, static_cast<int>(availableHeight));
    heightPixels = std::max(px(45), heightPixels);
    int x = point.x, y = point.y;
    if (child && x + width > work.right) x = parent.left - width + px(3);
    x = std::clamp(x, int(work.left), std::max(int(work.left), int(work.right) - width));
    y = std::clamp(y, int(work.top), std::max(int(work.top), int(work.bottom) - heightPixels));
    SetWindowPos(panel.window, HWND_TOP, x, y, width, heightPixels, SWP_NOACTIVATE);
    MoveWindow(panel.list, px(6), px(8), width - px(12), heightPixels - px(16), TRUE);
    // Win10 has no DWM rounded-corner preference. Give it the same outline.
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, heightPixels + 1, px(12), px(12));
    if (!SetWindowRgn(panel.window, region, FALSE)) DeleteObject(region);
  }
  Panel &create(std::shared_ptr<Branch> branch, POINT point, bool child, RECT parent = {});
  void dispatch(unsigned command, bool switchingTab = false) {
    // Copy the handler: rebuilding the model may replace its own callback.
    auto invoke = panels.front()->branch->invoke;
    if (!invoke || !invoke(command)) { finish(command); return; }
    hideTip(); KillTimer(root(), hoverTimer); KillTimer(root(), loadingTimer);
    pendingRow = downRow = downAction = -1;
    if (!switchingTab) lastMouse = {LONG_MIN, LONG_MIN};
    trim(1); active = 0;
    auto &panel = *panels.front();
    const int row = panel.selected, action = panel.selectedAction;
    const auto top = SendMessageW(panel.list, LB_GETTOPINDEX, 0, 0);
    RECT bounds{}; GetWindowRect(root(), &bounds);
    populate(panel); place(panel, {bounds.left, bounds.top}, false, {}, switchingTab);
    if (switchingTab) SendMessageW(panel.list, LB_SETTOPINDEX, top, 0);
    select(panel, row, action);
  }
  void expand(size_t level, int row, bool keyboard = false) {
    if (done || level >= panels.size()) return;
    auto &parent = *panels[level];
    if (row < 0 || size_t(row) >= parent.branch->items.size()) return;
    auto branch = parent.branch->items[row].children;
    if (!branch) { trim(level + 1); return; }
    if (panels.size() > level + 1 && panels[level + 1]->branch == branch) { if (keyboard) active = level + 1; return; }
    trim(level + 1);
    if (branch->poll && branch->poll(*branch)) branch->poll = {};
    RECT bounds{}, item{}; GetWindowRect(parent.window, &bounds);
    SendMessageW(parent.list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item));
    MapWindowPoints(parent.list, nullptr, reinterpret_cast<POINT *>(&item), 2);
    auto &child = create(branch, {bounds.right - px(3), item.top - px(8)}, true, bounds);
    if (IsWindowVisible(root())) ShowWindow(child.window, SW_SHOWNOACTIVATE);
    if (branch->poll) SetTimer(root(), loadingTimer, 50, nullptr);
    if (keyboard) { active = level + 1; move(1); }
  }
  void activate(size_t level, int row, int action = -1) {
    if (done || level >= panels.size()) return;
    auto &panel = *panels[level];
    if (row < 0 || size_t(row) >= panel.branch->items.size()) return;
    auto &item = panel.branch->items[row];
    if (!actionable(item)) return;
    if (!item.actions.empty()) {
      int index = action < 0 ? std::max(0, panel.selectedAction) : action;
      if (size_t(index) < item.actions.size() && item.actions[index].enabled &&
          (item.row != Row::Tabs || !item.actions[index].checked)) dispatch(item.actions[index].command, item.row == Row::Tabs);
      return;
    }
    // A folder's primary action always wins, even when it has a submenu.
    if (item.command) dispatch(item.command); else expand(level, row, true);
  }
  struct Hit { Panel *panel; int row, action; };
  Hit hit(POINT point) {
    for (size_t i = panels.size(); i-- > 0;) {
      auto &panel = *panels[i]; RECT rect{}; GetClientRect(panel.list, &rect);
      POINT local = point; ScreenToClient(panel.list, &local);
      if (!PtInRect(&rect, local)) continue;
      auto hit = SendMessageW(panel.list, LB_ITEMFROMPOINT, 0, MAKELPARAM(local.x, local.y));
      int row = HIWORD(hit) ? -1 : LOWORD(hit);
      int action = -1;
      if (row >= 0 && size_t(row) < panel.branch->items.size() && !panel.branch->items[row].actions.empty()) {
        RECT bounds{}; SendMessageW(panel.list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&bounds));
        const auto &item = panel.branch->items[row];
        for (size_t index = 0; index < item.actions.size(); ++index) {
          auto button = actionRect(item, bounds, static_cast<int>(index));
          if (PtInRect(&button, local)) { action = static_cast<int>(index); break; }
        }
        if (action < 0) row = -1;
      }
      return {&panel, row, action};
    }
    return {nullptr, -1, -1};
  }
  void mouse(POINT point) {
    if (done) return;
    if (point.x == lastMouse.x && point.y == lastMouse.y) return;
    lastMouse = point;
    auto [panel, row, action] = hit(point);
    if (!panel) { hideTip(); return; } // Keep the child open across its narrow joining edge.
    queueTip(*panel, row, action, point);
    active = panel->level;
    if (row >= 0 && action >= 0) {
      const auto &item = panel->branch->items[row];
      if (item.row == Row::Tabs && item.actions[action].enabled && !item.actions[action].checked) {
        select(*panel, row, action);
        activate(panel->level, row, action);
        return;
      }
    }
    if (row == panel->selected && action == panel->selectedAction) return;
    select(*panel, row, action); KillTimer(root(), hoverTimer);
    pendingLevel = panel->level; pendingRow = panel->selected;
    // Delay switching sibling branches so a diagonal move can reach a submenu.
    SetTimer(root(), hoverTimer, 220, nullptr);
  }
  void move(int direction) {
    if (panels.empty()) return;
    auto &panel = *panels[active]; int count = static_cast<int>(panel.branch->items.size());
    int row = panel.selected;
    if (row < 0) row = direction > 0 ? -1 : 0;
    for (int i = 0; i < count; ++i) {
      row = (row + direction + count) % count;
      if (actionable(panel.branch->items[row])) { select(panel, row); break; }
    }
    trim(active + 1);
  }
  void key(WPARAM key) {
    if (done || panels.empty()) return;
    hideTip();
    KillTimer(root(), hoverTimer);
    auto &panel = *panels[active];
    if (panel.selected >= 0 && !panel.branch->items[panel.selected].actions.empty() && (key == VK_LEFT || key == VK_RIGHT)) {
      const auto &actions = panel.branch->items[panel.selected].actions;
      int count = static_cast<int>(actions.size()), next = panel.selectedAction;
      for (int i = 0; i < count; ++i) {
        next = (next + (key == VK_RIGHT ? 1 : count - 1)) % count;
        if (actions[next].enabled) { select(panel, panel.selected, next); break; }
      }
      return;
    }
    switch (key) {
    case VK_ESCAPE: finish(); break;
    case VK_UP: move(-1); break;
    case VK_DOWN: move(1); break;
    case VK_RIGHT: expand(active, panels[active]->selected, true); break;
    case VK_LEFT: if (active) { auto level = active; trim(level); active = level - 1; } break;
    case VK_RETURN: case VK_SPACE: activate(active, panels[active]->selected); break;
    case VK_HOME: panels[active]->selected = -1; move(1); break;
    case VK_END: panels[active]->selected = -1; move(-1); break;
    case VK_TAB: finish(); break;
    }
  }
  icons::Image imageFor(const Item &item) {
    if (!iconCache) return {};
    if (!item.iconPath.empty()) return iconCache->get(icons::Source::file(item.iconPath));
    switch (item.symbol) {
    case Symbol::Desktop: return iconCache->get(icons::Source::folder(FOLDERID_Desktop));
    case Symbol::Downloads: return iconCache->get(icons::Source::folder(FOLDERID_Downloads));
    case Symbol::Documents: return iconCache->get(icons::Source::folder(FOLDERID_Documents));
    case Symbol::History: return iconCache->get(icons::Source::folder(FOLDERID_Recent));
    default: return {};
    }
  }
  void draw(const DRAWITEMSTRUCT &draw, Panel &panel) {
    if (draw.itemID >= panel.branch->items.size()) return;
    const auto &item = panel.branch->items[draw.itemID]; const auto &p = theme.colors;
    RECT rect = draw.rcItem; HDC dc = draw.hDC;
    FillRect(dc, &rect, theme.card);
    if (item.row == Row::Separator) {
      RECT line{rect.left + px(8), (rect.top + rect.bottom) / 2, rect.right - px(8), (rect.top + rect.bottom) / 2 + 1};
      auto brush = CreateSolidBrush(p.border); FillRect(dc, &line, brush); DeleteObject(brush); return;
    }
    bool selectedRow = panel.selected == int(draw.itemID) && actionable(item);
    if (selectedRow && item.actions.empty()) ui::rounded(dc, rect, p.hover, p.hover, px(4));
    bool caption = item.row == Row::Heading || item.row == Row::Hint || item.row == Row::Toolbar;
    auto old = SelectObject(dc, caption ? smallFont : font);
    auto color = caption ? p.muted : selectedRow && p.highContrast ? p.onAccent : p.text;
    int leading = static_cast<int>(std::min<size_t>(item.leadingActions, item.actions.size()));
    RECT label = rect;
    label.left += px(item.row == Row::Action ? 31 : 9) + actionsWidth(item, 0, leading);
    label.right -= px(22) + actionsWidth(item, leading, int(item.actions.size()));
    if (item.symbol == Symbol::More) {
      SelectObject(dc, symbols); ui::text(dc, L"\xE712", rect, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else ui::text(dc, item.title, label, color);
    if (item.row == Row::Action && item.symbol != Symbol::More) {
      int x = rect.left + px(7), y = rect.top + (rect.bottom - rect.top - px(16)) / 2;
      wchar_t glyph = item.iconPath.empty() ? actionGlyph(item.symbol) : L'\0';
      if (glyph) {
        SelectObject(dc, symbols); RECT iconRect{x, rect.top, x + px(16), rect.bottom};
        ui::text(dc, std::wstring(1, glyph), iconRect, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      } else {
        auto image = imageFor(item); HICON icon = image ? image->handle : folder;
        if (icon) DrawIconEx(dc, x, y, icon, px(16), px(16), 0, nullptr, DI_NORMAL);
      }
      if (item.children) {
        SelectObject(dc, symbols); RECT arrow{rect.right - px(20), rect.top, rect.right - px(4), rect.bottom};
        ui::text(dc, L"\xE76C", arrow, p.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
    }
    for (size_t index = 0; index < item.actions.size(); ++index) {
      const auto &action = item.actions[index];
      auto bounds = actionRect(item, rect, static_cast<int>(index));
      bool hot = action.enabled && selectedRow && panel.selectedAction == int(index);
      const bool accented = action.checked && (item.row != Row::Tabs || p.highContrast);
      if (action.checked || hot) {
        const auto fill = accented ? p.accent : p.hover;
        ui::rounded(dc, bounds, fill, fill, px(4));
      }
      SelectObject(dc, action.text.empty() ? symbols : font);
      auto iconColor = !action.enabled ? p.muted : accented || (hot && p.highContrast) ? p.onAccent : p.text;
      ui::text(dc, action.text.empty() ? std::wstring(1, actionGlyph(action.symbol)) : action.text, bounds, iconColor,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, old);
  }
};
int Panel::px(int value) const { return session->px(value); }
// A popup launched by the optional background helper is not guaranteed to have
// foreground permission. It remains usable without stealing focus from Explorer.
// These observers exist only during the popup's message loop, never while idle.
thread_local Session *watched = nullptr;
bool navigationKey(DWORD key) {
  switch (key) {
  case VK_ESCAPE: case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
  case VK_RETURN: case VK_SPACE: case VK_HOME: case VK_END: case VK_TAB: return true;
  default: return false;
  }
}
void CALLBACK foregroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
  // WinEvents may be queued: inspect current foreground instead of the old event.
  if (watched) watched->observeForeground(GetForegroundWindow());
}
LRESULT CALLBACK outsideMouse(int code, WPARAM message, LPARAM data) {
  if (code == HC_ACTION && watched && watched->armed && !watched->done) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
      auto *mouse = reinterpret_cast<MSLLHOOKSTRUCT *>(data);
      if (!watched->family(WindowFromPoint(mouse->pt))) PostMessageW(watched->root(), dismiss, 0, 0);
      break;
    }
    }
  }
  // An outside click must still reach the window the user actually clicked.
  return CallNextHookEx(nullptr, code, message, data);
}
LRESULT CALLBACK contextKeyboard(int code, WPARAM message, LPARAM data) {
  if (code == HC_ACTION && watched && watched->armed && !watched->done) {
    HWND foreground = GetForegroundWindow();
    if (!watched->context(foreground)) PostMessageW(watched->root(), dismiss, 0, 0);
    else if (!watched->family(foreground)) {
      auto *key = reinterpret_cast<KBDLLHOOKSTRUCT *>(data);
      bool modified = (key->flags & LLKHF_ALTDOWN) ||
          ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000);
      if (!modified && navigationKey(key->vkCode)) {
        if (message == WM_KEYDOWN) PostMessageW(watched->root(), contextKey, key->vkCode, 0);
        return 1; // Menu navigation must not also navigate or dismiss its anchor.
      }
      if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) PostMessageW(watched->root(), dismiss, 0, 0);
    }
  }
  return CallNextHookEx(nullptr, code, message, data);
}
struct ContextWatch {
  HWINEVENTHOOK foreground = nullptr;
  HHOOK mouse = nullptr, keyboard = nullptr;
  explicit ContextWatch(Session &session) {
    watched = &session;
    foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
    mouse = SetWindowsHookExW(WH_MOUSE_LL, outsideMouse, session.instance, 0);
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, contextKeyboard, session.instance, 0);
  }
  bool ready() const { return foreground && mouse && keyboard; }
  ~ContextWatch() {
    watched = nullptr;
    if (foreground) UnhookWinEvent(foreground);
    if (mouse) UnhookWindowsHookEx(mouse);
    if (keyboard) UnhookWindowsHookEx(keyboard);
  }
};
LRESULT CALLBACK listProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data); auto &session = *panel.session;
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, listProc, id);
  if (message == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
  }
  if (message == WM_MOUSELEAVE) session.hideTip();
  if (message == WM_KEYDOWN) { session.key(w); return 0; }
  if (message == WM_SYSKEYDOWN) session.finish();
  // No background mouse capture is needed. Native hit-testing delivers events
  // to each visible list; route them through the same tested menu controller.
  if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
      message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN) {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    MapWindowPoints(window, session.root(), &point, 1);
    return SendMessageW(session.root(), message, w, MAKELPARAM(point.x, point.y));
  }
  if (message == WM_MOUSEWHEEL) return SendMessageW(session.root(), message, w, l);
  if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
  if (message == WM_KILLFOCUS && session.armed && !session.family(reinterpret_cast<HWND>(w))) PostMessageW(session.root(), checkFocus, 0, 0);
  auto result = DefSubclassProc(window, message, w, l);
  if (message == WM_PAINT) {
    RECT client{}; GetClientRect(window, &client);
    int total = 0, offset = 0;
    int top = static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0));
    for (size_t i = 0; i < panel.branch->items.size(); ++i) {
      int height = session.height(panel.branch->items[i]); total += height;
      if (int(i) < top) offset += height;
    }
    if (total > client.bottom && client.bottom > 0) {
      int size = std::max<int>(session.px(18), client.bottom * client.bottom / total);
      int y = std::min(client.bottom - size, offset * client.bottom / total);
      RECT thumb{client.right - session.px(3), y, client.right, y + size};
      HDC dc = GetDC(window); auto brush = CreateSolidBrush(session.theme.colors.border);
      FillRect(dc, &thumb, brush); DeleteObject(brush); ReleaseDC(window, dc);
    }
  }
  return result;
}
LRESULT CALLBACK panelProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
  auto *panel = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!panel) return DefWindowProcW(window, message, w, l);
  auto &session = *panel->session;
  if (message == ui::themeChangedMessage()) {
    session.hideTip();
    session.theme.reset(ui::configuredTheme());
    for (auto &item : session.panels) { InvalidateRect(item->window, nullptr, TRUE); InvalidateRect(item->list, nullptr, TRUE); }
    return 0;
  }
  try {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); RECT rect{}; GetClientRect(window, &rect);
      ui::rounded(dc, rect, session.theme.colors.surface, session.theme.colors.border, panel->px(6));
      EndPaint(window, &paint); return 0;
    }
    case WM_CTLCOLORLISTBOX:
      SetTextColor(reinterpret_cast<HDC>(w), session.theme.colors.text);
      SetBkColor(reinterpret_cast<HDC>(w), session.theme.colors.surface); return reinterpret_cast<LRESULT>(session.theme.card);
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = panel->px(31); return TRUE;
    case WM_DRAWITEM: session.draw(*reinterpret_cast<DRAWITEMSTRUCT *>(l), *panel); return TRUE;
    case WM_MOUSEACTIVATE: return panel->level ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_ACTIVATEAPP:
      if (!w) session.observeForeground(GetForegroundWindow()); return 0;
    case WM_ACTIVATE:
      if (LOWORD(w) != WA_INACTIVE) {
        session.activated = true;
        if (!panel->level && panel->list) SetFocus(panel->list);
      } else if (session.armed) session.observeForeground(l ? reinterpret_cast<HWND>(l) : GetForegroundWindow());
      return 0;
    case WM_KILLFOCUS:
      if (session.armed && !session.family(reinterpret_cast<HWND>(w))) PostMessageW(session.root(), checkFocus, 0, 0); return 0;
    case checkFocus: session.observeForeground(GetForegroundWindow()); return 0;
    case contextKey: session.key(w); return 0;
    case dismiss: session.finish(); return 0;
    case iconsReady:
      if (!session.done) for (auto &item : session.panels) InvalidateRect(item->list, nullptr, FALSE);
      return 0;
    case WM_CAPTURECHANGED: if (session.armed && !session.family(reinterpret_cast<HWND>(l))) session.finish(); return 0;
    case WM_CANCELMODE: case WM_CLOSE: session.finish(); return 0;
    case WM_KEYDOWN: session.key(w); return 0;
    case WM_SYSKEYDOWN: session.finish(); return 0;
    case WM_MOUSEMOVE: {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(window, &point); session.mouse(point); return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
      session.hideTip();
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(window, &point);
      auto [hit, row, action] = session.hit(point);
      if (!hit || message != WM_LBUTTONDOWN) { session.finish(); return 0; }
      session.downLevel = hit->level; session.downRow = row; session.downAction = action; session.select(*hit, row, action); return 0;
    }
    case WM_LBUTTONUP: {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(window, &point);
      auto [hit, row, action] = session.hit(point);
      if (hit && hit->level == session.downLevel && row == session.downRow && action == session.downAction) session.activate(hit->level, row, action);
      session.downRow = -1; return 0;
    }
    case WM_MOUSEWHEEL: {
      session.hideTip(); KillTimer(session.root(), hoverTimer); session.pendingRow = -1;
      auto [hit, row, action] = session.hit({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); (void)row; (void)action;
      if (hit) {
        int top = static_cast<int>(SendMessageW(hit->list, LB_GETTOPINDEX, 0, 0));
        int next = top - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 3;
        session.trim(hit->level + 1); session.select(*hit, -1);
        SendMessageW(hit->list, LB_SETTOPINDEX, std::clamp(next, 0, std::max(0, int(hit->branch->items.size()) - 1)), 0);
      }
      return 0;
    }
    case WM_TIMER:
      if (session.done) return 0;
      if (w == hoverTimer) {
        KillTimer(session.root(), hoverTimer);
        if (session.pendingLevel < session.panels.size()) {
          if (session.pendingRow < 0) session.trim(session.pendingLevel + 1);
          else session.expand(session.pendingLevel, session.pendingRow);
        }
      } else if (w == tipTimer) session.showTip();
      else if (w == loadingTimer) {
        bool pending = false;
        for (auto &item : session.panels) if (item->branch->poll) {
          if (item->branch->poll(*item->branch)) {
            item->branch->poll = {}; session.populate(*item);
            RECT rect{}; GetWindowRect(item->window, &rect);
            session.place(*item, {rect.left, rect.top}, false);
          } else pending = true;
        }
        if (!pending) KillTimer(session.root(), loadingTimer);
      }
      return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
      session.hideTip();
      session.theme.reset(session.theme.mode);
      for (auto &item : session.panels) { InvalidateRect(item->window, nullptr, TRUE); InvalidateRect(item->list, nullptr, TRUE); }
      return 0;
    case WM_DPICHANGED:
      // Initial creation/placement can notify a DPI change before the menu is
      // shown. That is not a loss of focus and must not cancel its first opening.
      if (session.armed) session.finish();
      return 0;
    }
  } catch (...) { session.finish(); }
  return DefWindowProcW(window, message, w, l);
}
Panel &Session::create(std::shared_ptr<Branch> branch, POINT point, bool child, RECT parent) {
  auto panel = std::make_unique<Panel>(); panel->session = this; panel->branch = std::move(branch); panel->level = panels.size();
  auto *value = panel.get();
  panel->window = CreateWindowExW(WS_EX_TOOLWINDOW | (child ? WS_EX_NOACTIVATE : 0), panelClass, lang::text(L"快速文件夹"),
      WS_POPUP | WS_CLIPCHILDREN, point.x, point.y, 1, 1, child ? root() : nullptr, nullptr, instance, value);
  if (!panel->window) throw std::bad_alloc();
  if (!child) {
    dpi = GetDpiForWindow(panel->window); font = ui::makeFont(dpi); smallFont = ui::makeFont(dpi, 12);
    symbols = ui::makeSymbolFont(dpi);
    SHSTOCKICONINFO icon{sizeof(icon)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &icon))) folder = icon.hIcon;
  }
  panel->list = CreateWindowExW(0, L"LISTBOX", lang::text(L"文件夹位置"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
      0, 0, 1, 1, panel->window, reinterpret_cast<HMENU>(1), instance, nullptr);
  if (!panel->list) { DestroyWindow(panel->window); throw std::bad_alloc(); }
  SendMessageW(panel->list, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  SetWindowSubclass(panel->list, listProc, 1, reinterpret_cast<DWORD_PTR>(value));
  panels.push_back(std::move(panel));
  if (!child) {
    iconCache = std::make_unique<icons::Cache>(value->window, iconsReady, px(16));
  }
  populate(*value); place(*value, point, child, parent);
  BOOL dark = theme.colors.dark; DwmSetWindowAttribute(value->window, 20, &dark, sizeof(dark));
  DWORD corners = 2; DwmSetWindowAttribute(value->window, 33, &corners, sizeof(corners));
  return *value;
}
void registerPanel(HINSTANCE instance) {
  WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = panelProc; cls.lpszClassName = panelClass;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.style = CS_DROPSHADOW;
  RegisterClassW(&cls);
}
}
unsigned show(HINSTANCE instance, POINT point, std::shared_ptr<Branch> root, ui::ThemeMode theme, HWND anchor) {
  registerPanel(instance);
  Session session(instance, theme); auto &panel = session.create(std::move(root), point, false);
  session.anchor = anchor;
  ContextWatch watch(session);
  if (!watch.ready()) throw fn::Error{lang::text(L"无法启动快速菜单的窗口交互，请关闭后重新开启中键快速定位。")};
  if (!IsWindow(anchor) || GetForegroundWindow() != anchor) return 0;
  // Show first, without requesting or requiring a foreground handoff. A real
  // click can activate the menu normally; arrows/Esc also work before that.
  session.armed = true;
  SetWindowPos(panel.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  MSG message{};
  while (!session.done) {
    int result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) { if (!result) PostQuitMessage(static_cast<int>(message.wParam)); break; }
    TranslateMessage(&message); DispatchMessageW(&message);
  }
  return session.selected;
}
}
