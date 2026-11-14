#include "desktop_groups.h"
#include "desktop_box_icons.h"
#include "desktop_groups_color_picker.h"
#include "desktop_groups_glass.h"
#include "desktop_groups_hold.h"
#include "desktop_groups_model.h"
#include "desktop_groups_menu.h"
#include "desktop_groups_item_menu.h"
#include "desktop_groups_render.h"
#ifdef FOLDERNOTE_WINUI
#include "desktop_groups_winui.h"
#endif
#include "desktop_groups_visibility.h"
#include "theme_scroll.h"
#include "core.h"
#include "language.h"
#include "quick_folders.h"
#include "quick_popup.h"
#include "resource.h"
#include "theme.h"
#include "window_position.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <commoncontrols.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace desktop_groups {
namespace {
constexpr auto receiverClass = L"FolderNote.DesktopGroups.Receiver";
constexpr auto panelClass = L"FolderNote.DesktopGroups.Panel";
constexpr auto sceneClass = L"FolderNote.DesktopGroups.Scene";
constexpr auto mutexName = L"Local\\FolderNote.DesktopGroups";
constexpr auto runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto runName = L"FolderNote.DesktopGroups";
constexpr UINT manageMessage = WM_APP + 1, trayMessage = WM_APP + 2, rejectedMessage = WM_APP + 3;
constexpr UINT dragMessage = WM_APP + 4, refreshMessage = WM_APP + 5;
constexpr UINT createMessage = WM_APP + 6;
constexpr UINT detachBoxTabMessage = WM_APP + 41;
constexpr UINT dragTimer = 2;
constexpr UINT headerTimer = 3;
constexpr UINT_PTR boxMoveTimer = 1;
constexpr UINT syncScrollMessage = WM_APP + 40;
constexpr UINT finishBoxGestureMessage = WM_APP + 42;
constexpr UINT updatePagesMessage = WM_APP + 43, goToPageMessage = WM_APP + 44;
UINT appearanceMessage() {
  static const UINT message = RegisterWindowMessageW(L"FolderNote.DesktopGroups.Appearance.v2");
  return message;
}
constexpr WPARAM previewRadiusMask = 0x1f;
constexpr WPARAM previewOpacityMask = 0x7f;
constexpr WPARAM previewGlass = 1u << 12, previewShadow = 1u << 13, previewEnabled = 1u << 14;
enum Command { NewGroup = 1, RenameGroup, DeleteGroup, OpenItem, RemoveItem, ToggleGroup,
               AddTab, MapFolder, RemoveTab, IconView, ListViewMode, ManualSort, NameSort, TypeSort, ModifiedSort,
               BoxColor, ResetBoxColor, MoveTabLeft, MoveTabRight, BoxIcon, ClearBoxIcon,
               TitleText, TitleIcon, TitleIconAndText, ScrollDisplay, PageDisplay };
constexpr LRESULT resizeTitle = 0x100;
using quick::popup::Branch;
using quick::popup::Symbol;

constexpr auto ungroupedPlacement = L"DesktopGroupsUngroupedPlacement";
constexpr auto ungroupedOpacity = L"DesktopGroupsUngroupedOpacity";
constexpr auto ungroupedTint = L"DesktopGroupsUngroupedTint";
constexpr auto ungroupedTitleHeight = L"DesktopGroupsUngroupedTitleHeight";
constexpr auto ungroupedTitleStyle = L"DesktopGroupsUngroupedTitleStyle";
constexpr auto ungroupedTitleIcon = L"DesktopGroupsUngroupedTitleIcon";
constexpr auto ungroupedDisplayMode = L"DesktopGroupsUngroupedDisplayMode";
constexpr auto ungroupedItemSpacing = L"DesktopGroupsUngroupedItemSpacing";
constexpr auto desktopSettings = L"Software\\FolderNote";
DWORD appearanceValue(const wchar_t *name, DWORD fallback) {
  DWORD value = fallback, bytes = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, desktopSettings, name, RRF_RT_REG_DWORD,
                      nullptr, &value, &bytes) == ERROR_SUCCESS ? value : fallback;
}
struct UngroupedPlacement { RECT bounds; DWORD view; DWORD sort; DWORD collapsed; int iconSize = defaultIconSize; };

struct HueColor {
  int hue;
  double saturation, lightness;

  static HueColor fromRgb(COLORREF color, bool dark) {
    const double r = GetRValue(color) / 255.0, g = GetGValue(color) / 255.0, b = GetBValue(color) / 255.0;
    const double low = std::min({r, g, b}), high = std::max({r, g, b});
    const double chroma = high - low, lightness = (high + low) / 2;
    if (chroma < 0.02) return {215, 0.52, dark ? 0.28 : 0.75};
    const double sector = high == r ? (g - b) / chroma : high == g ? (b - r) / chroma + 2 : (r - g) / chroma + 4;
    return {(static_cast<int>(std::lround(sector * 60)) + 360) % 360,
            chroma / (1 - std::abs(2 * lightness - 1)), lightness};
  }

  COLORREF at(int degrees) const {
    const double chroma = (1 - std::abs(2 * lightness - 1)) * saturation;
    const double base = lightness - chroma / 2;
    const double hueSector = degrees / 60.0;
    const auto channel = [&](double offset) {
      const auto position = std::fmod(hueSector + offset, 6);
      return static_cast<BYTE>(std::lround(255 * (base + chroma * std::clamp(std::min(position, 4 - position), 0.0, 1.0))));
    };
    return RGB(channel(2), channel(0), channel(4));
  }
};

const Tab *extraTab(const Group &group, size_t index) {
  return index && index <= group.tabs.size() ? &group.tabs[index - 1] : nullptr;
}

const std::vector<std::wstring> &manualItems(const Group &group, size_t index) {
  const auto tab = extraTab(group, index);
  return tab ? tab->paths : group.paths;
}

std::vector<std::wstring> &manualItems(Group &group, size_t index) {
  return index ? group.tabs[index - 1].paths : group.paths;
}

std::optional<std::vector<std::wstring>> ungroupedItems(const std::vector<Group> &groups) {
  std::vector<std::wstring> grouped;
  for (size_t i = 1; i < groups.size(); ++i) {
    const auto &group = groups[i];
    grouped.insert(grouped.end(), group.paths.begin(), group.paths.end());
    for (const auto &tab : group.tabs) grouped.insert(grouped.end(), tab.paths.begin(), tab.paths.end());
  }
  std::vector<std::wstring> result;
  Microsoft::WRL::ComPtr<IShellFolder> desktop;
  Microsoft::WRL::ComPtr<IEnumIDList> entries;
  if (FAILED(SHGetDesktopFolder(&desktop))) return std::nullopt;
  const auto status = desktop->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &entries);
  if (FAILED(status)) return std::nullopt;
  if (!entries) return result;
  PITEMID_CHILD id = nullptr;
  while (entries->Next(1, &id, nullptr) == S_OK) {
    const std::unique_ptr<ITEMIDLIST, decltype(&CoTaskMemFree)> entry(id, CoTaskMemFree);
    PWSTR raw = nullptr;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemWithParent(nullptr, desktop.Get(), reinterpret_cast<PCUITEMID_CHILD>(id), IID_PPV_ARGS(&item))) ||
        FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw)) || !raw) continue;
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
    const auto normalized = quick::normalize(raw);
    if (normalized.empty() ||
        std::none_of(grouped.begin(), grouped.end(), [&](const auto &item) { return samePath(item, normalized); }))
      result.push_back(normalized.empty() ? std::wstring(raw) : normalized);
  }
  return result;
}

HWND receiver() { return FindWindowW(receiverClass, nullptr); }
constexpr auto readyProperty = L"FolderNote.DesktopGroups.Ready";

bool startup(const std::wstring &exe) {
  HKEY key = nullptr;
  if (exe.empty()) {
    auto status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    status = RegDeleteValueW(key, runName);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  }
  if (RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return false;
  const auto command = L"\"" + exe + L"\" --desktop-groups";
  const auto status = RegSetValueExW(key, runName, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                                     static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool launch(const std::wstring &exe, bool manage) {
  auto command = L"\"" + exe + L"\" --desktop-groups";
  if (manage) command += L"-manage";
  STARTUPINFOW startupInfo{sizeof(startupInfo)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &process))
    return false;
  if (manage) AllowSetForegroundWindow(process.dwProcessId);
  CloseHandle(process.hThread);
  bool ready = manage;
  for (int i = 0; !manage && i < 100; ++i) {
    if (const auto window = receiver(); window && GetPropW(window, readyProperty)) {
      ready = true;
      break;
    }
    if (WaitForSingleObject(process.hProcess, 50) != WAIT_TIMEOUT) break;
  }
  CloseHandle(process.hProcess);
  return ready;
}

bool notifyManager(HWND window, UINT message = manageMessage) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid) AllowSetForegroundWindow(pid);
  return PostMessageW(window, message, 0, 0) != FALSE;
}

bool stopProcess() {
  auto window = receiver();
  if (!window) return true;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  PostMessageW(window, WM_CLOSE, 0, 0);
  if (!process) return !IsWindow(window);
  const auto result = WaitForSingleObject(process, 2500);
  CloseHandle(process);
  return result == WAIT_OBJECT_0;
}

void report(HWND owner, const wchar_t *message) {
  ui::message(owner, message, lang::text(L"桌面盒子"), MB_OK | MB_ICONERROR);
}

void menuItem(Branch &menu, unsigned id, const wchar_t *text, Symbol symbol, bool selected = false) {
  quick::popup::Item item;
  item.title = lang::text(text);
  item.command = id;
  item.symbol = symbol;
  item.checked = selected;
  menu.items.push_back(std::move(item));
}

void separator(Branch &menu) {
  quick::popup::Item item;
  item.row = quick::popup::Row::Separator;
  menu.items.push_back(std::move(item));
}

std::wstring pickFolder(HWND owner) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
  FILEOPENDIALOGOPTIONS options{};
  if (FAILED(dialog->GetOptions(&options)) ||
      FAILED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST))) return {};
  dialog->SetTitle(lang::text(L"选择要映射的文件夹"));
  Microsoft::WRL::ComPtr<IShellItem> item;
  PWSTR path = nullptr;
  if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)) ||
      FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(path, CoTaskMemFree);
  return quick::normalize(path);
}

struct BoxDialog {
  const wchar_t *title;
  std::wstring *name;
  std::wstring prompt;
  ui::Theme theme;
  HFONT font = nullptr, heading = nullptr;
  HWND edit = nullptr;

  ~BoxDialog() {
    if (font) DeleteObject(font);
    if (heading) DeleteObject(heading);
  }
};

INT_PTR CALLBACK boxDialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
  constexpr int editId = 100, dialogWidth = 356, cornerRadius = 12;
  auto *state = reinterpret_cast<BoxDialog *>(GetWindowLongPtrW(dialog, DWLP_USER));
  const auto px = [dialog](int value) { return MulDiv(value, GetDpiForWindow(dialog), 96); };
  const auto field = [&] { return RECT{px(24), px(79), px(332), px(119)}; };
  switch (message) {
  case WM_INITDIALOG: {
    state = reinterpret_cast<BoxDialog *>(l);
    SetWindowLongPtrW(dialog, DWLP_USER, l);
    const auto work = ui::mouseWorkArea();
    const int height = state->name ? 180 : 164;
    ui::centerLogical(dialog, work, dialogWidth, height);
    ui::centerLogical(dialog, work, dialogWidth, height);
    const auto dpi = GetDpiForWindow(dialog);
    state->font = ui::makeFont(dpi);
    state->heading = ui::makeFont(dpi, 21, FW_SEMIBOLD);
    const auto control = [&](const wchar_t *cls, const wchar_t *text, DWORD style,
                             int x, int y, int width, int height, INT_PTR id) {
      const auto child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
          px(x), px(y), px(width), px(height), dialog, reinterpret_cast<HMENU>(id), nullptr, nullptr);
      if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
      return child;
    };
    if (state->name) {
      if (const auto label = control(L"STATIC", state->prompt.c_str(), SS_NOPREFIX, 24, 58, 308, 19, -1))
        ui::muted(label);
      state->edit = control(L"EDIT", state->name->c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 36, 88, 284, 22, editId);
      if (!state->edit) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      SendMessageW(state->edit, EM_SETLIMITTEXT, groupNameLimit, 0);
    } else {
      control(L"STATIC", state->prompt.c_str(), SS_NOPREFIX, 24, 58, 308, 42, -1);
    }
    const int buttonTop = height - 52;
    const auto cancel = control(L"BUTTON", lang::text(L"取消"), WS_TABSTOP, 140, buttonTop, 92, 32, IDCANCEL);
    const auto confirm = control(L"BUTTON", state->name ? lang::text(L"确定") : state->title,
                                 BS_DEFPUSHBUTTON | WS_TABSTOP, 240, buttonTop, 92, 32, IDOK);
    if (!cancel || !confirm) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
    if (state->name && state->name->find_first_not_of(L" \t\r\n") == std::wstring::npos)
      EnableWindow(confirm, FALSE);
    state->theme.apply(dialog);
    const DWMNCRENDERINGPOLICY frame = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(dialog, DWMWA_NCRENDERING_POLICY, &frame, sizeof(frame));
    RECT area{};
    GetClientRect(dialog, &area);
    if (const auto region = createBoxRegion(area.right, area.bottom, px(cornerRadius * 2)))
      if (!SetWindowRgn(dialog, region, FALSE)) DeleteObject(region);
    SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetFocus(state->edit ? state->edit : cancel);
    if (state->edit) SendMessageW(state->edit, EM_SETSEL, 0, -1);
    return FALSE;
  }
  case WM_PAINT:
    if (state) {
      PAINTSTRUCT paint{};
      const auto dc = BeginPaint(dialog, &paint);
      RECT area{};
      GetClientRect(dialog, &area);
      const auto &colors = state->theme.colors;
      FillRect(dc, &area, state->theme.background);
      if (const auto outline = createBoxRegion(area.right, area.bottom, px(cornerRadius * 2))) {
        const auto previousColor = SetDCBrushColor(dc, colors.border);
        FrameRgn(dc, outline, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)), 1, 1);
        SetDCBrushColor(dc, previousColor);
        DeleteObject(outline);
      }
      if (state->edit)
        ui::rounded(dc, field(), colors.field, GetFocus() == state->edit ? colors.accent : colors.border, px(8));
      const auto previous = SelectObject(dc, state->heading);
      ui::text(dc, state->title, {px(24), px(18), px(332), px(50)}, colors.text);
      SelectObject(dc, previous);
      EndPaint(dialog, &paint);
      return TRUE;
    }
    break;
  case WM_COMMAND:
    if (!state) break;
    if (LOWORD(w) == editId && (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) {
      const auto bounds = field();
      InvalidateRect(dialog, &bounds, FALSE);
      return TRUE;
    }
    if (LOWORD(w) == editId && HIWORD(w) == EN_CHANGE) {
      wchar_t input[groupNameLimit + 1]{};
      GetWindowTextW(state->edit, input, ARRAYSIZE(input));
      EnableWindow(GetDlgItem(dialog, IDOK), std::wstring_view(input).find_first_not_of(L" \t\r\n") != std::wstring_view::npos);
      return TRUE;
    }
    if (LOWORD(w) == IDOK) {
      if (state->name) {
        wchar_t buffer[groupNameLimit + 1]{};
        GetWindowTextW(state->edit, buffer, ARRAYSIZE(buffer));
        const std::wstring_view value(buffer);
        const auto start = value.find_first_not_of(L" \t\r\n");
        if (start == value.npos) return TRUE;
        *state->name = value.substr(start, value.find_last_not_of(L" \t\r\n") - start + 1);
      }
      EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (LOWORD(w) != IDCANCEL) break;
    [[fallthrough]];
  case WM_CLOSE:
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
  LRESULT result{};
  if (state && state->theme.handle(dialog, message, w, l, result)) {
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC) return result;
    return TRUE;
  }
  return FALSE;
}

bool showBoxDialog(HINSTANCE instance, HWND owner, const wchar_t *title, std::wstring *name, std::wstring prompt) {
  struct Template {
    DLGTEMPLATE dialog;
    WORD menu = 0, cls = 0, title[64]{};
  };
  alignas(DWORD) Template layout{};
  layout.dialog.style = WS_POPUP | WS_CLIPCHILDREN | DS_CENTER;
  layout.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW;
  layout.dialog.cx = 220;
  layout.dialog.cy = 110;
  BoxDialog state{lang::text(title), name, std::move(prompt)};
  wcsncpy_s(reinterpret_cast<wchar_t *>(layout.title), ARRAYSIZE(layout.title), state.title, _TRUNCATE);
  const auto result = DialogBoxIndirectParamW(instance, &layout.dialog, owner, boxDialog, reinterpret_cast<LPARAM>(&state));
  if (result == -1) fn::fail(lang::text(L"无法打开桌面盒子。"));
  return result == IDOK;
}

bool askName(HINSTANCE instance, HWND owner, const wchar_t *title, std::wstring &name) {
  const auto label = !wcscmp(title, L"添加标签") ? L"标签名称" : L"盒子名称";
  return showBoxDialog(instance, owner, title, &name, lang::text(label));
}

std::wstring pickBoxIcon(HWND owner) {
  wchar_t path[32768]{};
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = owner;
  dialog.lpstrFile = path;
  dialog.nMaxFile = ARRAYSIZE(path);
  dialog.lpstrTitle = lang::text(L"选择盒子图标");
  dialog.lpstrFilter = L"Icon (*.ico)\0*.ico\0All files (*.*)\0*.*\0\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&dialog) ? std::wstring(path) : std::wstring{};
}

RECT visibleBounds(RECT bounds) {
  MONITORINFO monitor{sizeof(monitor)};
  const auto target = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(target, &monitor)) return bounds;
  const auto work = monitor.rcWork;
  const auto width = std::clamp(bounds.right - bounds.left, 1L, work.right - work.left);
  const auto height = std::clamp(bounds.bottom - bounds.top, 1L, work.bottom - work.top);
  const auto x = std::clamp(bounds.left, work.left, work.right - width);
  const auto y = std::clamp(bounds.top, work.top, work.bottom - height);
  return {x, y, x + width, y + height};
}

class Desktop;
enum class DockSide { None, Left, Top, Right, Bottom };

DockSide touchingWorkEdge(const RECT &bounds, DockSide preferred, POINT movement = {}) {
  MONITORINFO monitor{sizeof(monitor)};
  if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) return DockSide::None;
  const auto &work = monitor.rcWork;
  if (bounds.left < work.left || bounds.top < work.top || bounds.right > work.right || bounds.bottom > work.bottom)
    return DockSide::None;
  const bool left = bounds.left == work.left, right = bounds.right == work.right;
  const bool top = bounds.top == work.top, bottom = bounds.bottom == work.bottom;
  if ((left && preferred == DockSide::Left) || (right && preferred == DockSide::Right) ||
      (top && preferred == DockSide::Top) || (bottom && preferred == DockSide::Bottom)) return preferred;
  if (std::abs(movement.y) > std::abs(movement.x)) {
    if (movement.y < 0 && top) return DockSide::Top;
    if (movement.y > 0 && bottom) return DockSide::Bottom;
  }
  if (left) return DockSide::Left;
  if (right) return DockSide::Right;
  if (top) return DockSide::Top;
  return bottom ? DockSide::Bottom : DockSide::None;
}

struct Insertion {
  int item = -1;
  bool after = false;
  RECT marker{};
};

struct Panel {
#ifdef FOLDERNOTE_WINUI
  std::unique_ptr<winui::Surface> surface;
  RECT surfaceBounds{};
  bool headerActions = false;
  void syncSurfaceHeader();
#endif
  bool hasSurface() const {
#ifdef FOLDERNOTE_WINUI
    return surface && surface->available();
#else
    return false;
#endif
  }
  Desktop &desktop;
  size_t index;
  HWND window = nullptr, list = nullptr, toggle = nullptr;
  HWND tabs = nullptr;
  HWND shadow = nullptr;
  SIZE shadowSize{};
  UINT shadowDpi = 0;
  int shadowDiameter = -1;
  int renderedShadowOffset = 0, renderedShadowSpread = 0;
  DWORD renderedShadowOpacity = 0;
  void updateShadow();
  HWND viewButtons[2]{};
  void updateHeaderActions(POINT cursor);
  ui::ScrollBar verticalScroll, horizontalScroll;
  HWND scrollCorner = nullptr;
  size_t activeTab = 0;
  std::vector<std::wstring> displayed;
  std::vector<std::wstring> orderedItems;
  int currentPage = 0, pageCount = 0, pageCapacity = 0, pageWheelRemainder = 0;
  bool pageLayoutPending = false;
  int pageSize(const Group &group) const;
  void renderPage(const Group &group);
  void queuePageLayout();
  void goToPage(int page);
  bool pageWheel(int delta);
  RECT pageFooter() const;
  int firstPageDot() const;
  int visiblePageDots() const;
  std::wstring mappedFolder;
  struct MappedItem { std::wstring path; ULONGLONG modified; DWORD attributes; };
  std::vector<MappedItem> mappedItems;
  std::future<std::vector<MappedItem>> mappedResult;
  std::vector<RECT> tabAreas;
  HFONT font = nullptr, titleFont = nullptr;
  HIMAGELIST scaledIcons = nullptr;
  int renderedIconSize = 0, wheelRemainder = 0;
  using Thumbnail = std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)>;
  struct ThumbnailBatch {
    std::atomic_bool canceled = false;
    std::mutex mutex;
    std::vector<std::pair<int, Thumbnail>> ready;
  };
  std::shared_ptr<ThumbnailBatch> thumbnails;
  std::map<std::wstring, Thumbnail> thumbnailCache;
  int thumbnailCacheSize = 0;
  int hoverRow = -1;
  bool scrollSyncPending = false;
  bool windowPositionReady = false;
  DockSide dockSide = DockSide::None;
  bool dockHidden = false;
  ULONGLONG dockHideAt = 0;
  RECT dockTab(SIZE size) const;
  HWND dockTrigger = nullptr;
  static LRESULT CALLBACK dockTriggerProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  bool showDockedBox(bool show);
  void tickDock(POINT cursor, ULONGLONG now);
  void setDockSide(DockSide side);
  int titleHeightPreview = 0;
  std::wstring cachedTitleIcon;
  HICON titleIcon = nullptr;
  HICON headerIcon();
  std::optional<int> opacityPreview;
  std::optional<int> spacingPreview;
  std::optional<COLORREF> colorPreview;
  std::optional<COLORREF> mergeTint;
  int backgroundOpacity() const;
  COLORREF backgroundColor() const;
  void applyBackground();
  bool saveColor(std::optional<COLORREF> tint);
  bool saveSpacing(int spacing);
  bool movingOrSizing = false;
  RECT gestureBounds{};
  RECT gestureTarget{};
  HWND alignmentGuides[2]{};
  void snapGesture(RECT &bounds);
  ULONGLONG lastCompositionFrame = 0;
  void stopGesture();
  void updateGestureTarget(POINT point);
  void updateGestureFrame();
  void finishGesture(bool accept);
  POINT gestureStart{};
  POINT tabPress{};
  size_t pressedTab = 0;
  std::optional<size_t> hoveredTab;
  WPARAM gestureEdge = 0;
  bool gestureEnding = false;
  void place(RECT bounds);
  Panel(Desktop &owner, size_t position) : desktop(owner), index(position) {}
  ~Panel() {
#ifdef FOLDERNOTE_WINUI
    surface.reset();
#endif
    stopGesture();
    if (thumbnails) thumbnails->canceled = true;
    if (shadow) DestroyWindow(shadow);
    if (window) DestroyWindow(window);
    if (font) DeleteObject(font);
    if (titleFont) DeleteObject(titleFont);
    if (titleIcon) DestroyIcon(titleIcon);
    if (scaledIcons) ImageList_Destroy(scaledIcons);
  }
  int selection() const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
  void applyTheme(const ui::Palette &colors);
  void paintBackground(HDC dc, POINT offset = {});
  int headerHeight() const;
  int cornerDiameter() const;
  SIZE shapeSize{};
  int shapeDiameter = 0;
  bool updateShape();
  bool mapped() const;
  void readMappedFolder();
  void showTab(size_t tab);
  void zoom(int delta);
  void loadThumbnails(int pixels);
  void applyThumbnails();
  void paintTabs(const DRAWITEMSTRUCT &item);
  void paint(HDC dc, bool backgroundOnly);
  bool paintItem(HDC dc, int row);
  void setCollapsed();
  void refresh(const Group &group);
  void resize();
  void syncScroll();
  void queueScrollSync();
  void scrollTo(int position, bool horizontal);
  Insertion insertion(POINT point) const;
  void autoscroll(POINT point);
  void contextMenu(POINT point, bool title = false);
#ifdef FOLDERNOTE_WINUI
  void openSurface();
#endif
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
};

class Desktop {
public:
  HINSTANCE instance;
  HWND window = nullptr;
  HWND scene = nullptr;
#ifdef FOLDERNOTE_WINUI
  bool useWinui = false;
#endif
  RECT sceneBounds{};
  std::vector<Group> groups;
  std::vector<LooseItem> looseItems;
  std::vector<HWND> looseWindows;
  std::vector<std::unique_ptr<Panel>> panels;
  std::vector<std::wstring> roots = desktopRoots();
  ui::Theme theme;
  GlassBackground looseGlass;
  DesktopHold hold;
  Appearance style = configuredAppearance();
  bool previewingAppearance = false;
  UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  bool quitting = false;
  bool menuOpen = false;
  bool editing = false;
  bool dragging = false;
  HWND dragWindow = nullptr;
  HWND dragPreview = nullptr;
  HICON dragIcon = nullptr;
  SIZE dragSize{};
  RECT dragIconRect{}, dragLabelRect{};
  bool dragListMode = false;
  std::wstring dragLabel;
  HWND dropHighlight = nullptr;
  RECT dropMarker{};
  ULONGLONG lastDragScroll = 0;
  std::wstring dragPath;
  std::vector<std::wstring> dragItems;
  ULONGLONG lastFolderRefresh = 0;

  explicit Desktop(HINSTANCE module) : instance(module) {
    auto layout = loadLayout();
    groups = std::move(layout.groups);
    looseItems = std::move(layout.looseItems);
    Group ungrouped;
    ungrouped.name = lang::text(L"待整理桌面");
    const auto work = ui::mouseWorkArea();
    const auto dpi = GetDpiForSystem();
    ungrouped.bounds = {work.left + 16, work.top + 16, work.left + MulDiv(350, dpi, 96),
                        work.top + MulDiv(460, dpi, 96)};
    ungrouped.sort = Sort::Name;
    if (const auto opacity = appearanceValue(ungroupedOpacity, MAXDWORD); opacity <= 100)
      ungrouped.opacity = static_cast<int>(opacity);
    if (const auto tint = appearanceValue(ungroupedTint, CLR_NONE); tint <= 0x00ffffff)
      ungrouped.tint = tint;
    ungrouped.titleHeight = std::clamp<DWORD>(appearanceValue(ungroupedTitleHeight, 36), 28, 80);
    ungrouped.titleStyle = static_cast<TitleStyle>(std::min<DWORD>(appearanceValue(ungroupedTitleStyle, 0), 2));
    if (appearanceValue(ungroupedDisplayMode, 0) == 1) ungrouped.displayMode = DisplayMode::Pages;
    ungrouped.itemSpacing = std::min<DWORD>(maxItemSpacing, appearanceValue(ungroupedItemSpacing, 0));
    DWORD iconBytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedTitleIcon, RRF_RT_REG_SZ,
                     nullptr, nullptr, &iconBytes) == ERROR_SUCCESS && iconBytes <= 65536) {
      std::wstring path(iconBytes / sizeof(wchar_t), L'\0');
      if (!path.empty() && RegGetValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedTitleIcon, RRF_RT_REG_SZ,
                                      nullptr, path.data(), &iconBytes) == ERROR_SUCCESS) {
        path.resize(wcslen(path.c_str()));
        ungrouped.titleIcon = std::move(path);
      }
    }
    UngroupedPlacement placement{};
    DWORD bytes = sizeof(placement);
    if (RegGetValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedPlacement, RRF_RT_REG_BINARY,
                     nullptr, &placement, &bytes) == ERROR_SUCCESS &&
        (bytes == sizeof(placement) || bytes == offsetof(UngroupedPlacement, iconSize)) &&
        placement.bounds.left < placement.bounds.right && placement.bounds.top < placement.bounds.bottom &&
        static_cast<LONGLONG>(placement.bounds.right) - placement.bounds.left <= INT_MAX &&
        static_cast<LONGLONG>(placement.bounds.bottom) - placement.bounds.top <= INT_MAX &&
        placement.view <= static_cast<DWORD>(View::List) && placement.sort <= static_cast<DWORD>(Sort::Modified)) {
      ungrouped.bounds = placement.bounds;
      ungrouped.view = static_cast<View>(placement.view);
      ungrouped.sort = static_cast<Sort>(placement.sort);
      ungrouped.collapsed = placement.collapsed != 0;
      if (bytes == sizeof(placement) && std::find(iconSizes.begin(), iconSizes.end(), placement.iconSize) != iconSizes.end())
        ungrouped.iconSize = placement.iconSize;
    }
    groups.insert(groups.begin(), std::move(ungrouped));
    auto items = ungroupedItems(groups);
    if (!items) throw fn::Error{lang::text(L"无法接管桌面图标。")};
    groups.front().paths = std::move(*items);
    std::erase_if(groups.front().paths, [&](const auto &path) {
      return std::any_of(looseItems.begin(), looseItems.end(), [&](const auto &item) { return samePath(item.path, path); });
    });
  }
  ~Desktop() {
    if (dragPreview) DestroyWindow(dragPreview);
    if (dragIcon) DestroyIcon(dragIcon);
    hold.stop();
    panels.clear();
    if (scene) DestroyWindow(scene);
    if (window) DestroyWindow(window);
  }
  void tray(DWORD action) const;
  bool openPanel(size_t index);
  void updateScene();
  void highlight(HWND target);
  void setAppearance(const Appearance &value);
  void lower(HWND raisedPanel = nullptr) const;
  Panel *mergeTargetAt(POINT point, const Panel &source) const;
  Panel *panelAt(POINT point) const;
  bool desktopDropAt(POINT point) const;
  bool commit(std::vector<Group> changed, HWND owner, const std::vector<LooseItem> *placements = nullptr,
              bool refreshContents = true);
  void refreshLoose();
  bool mergePanels(size_t source, size_t target);
  void detachPanelTab(size_t index, size_t tab, POINT point);
  void beginDrag(HWND source, std::wstring path, HICON icon, HWND list = nullptr, int row = -1,
                 bool listMode = false);
  static LRESULT CALLBACK looseProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
  void manage();
  void action(Panel *panel, Command command, size_t tab = 0);
  void drag(Panel &panel, int index);
  bool drop(Panel &panel, HDROP files, POINT point);
  bool assign(Panel &panel, const std::vector<std::wstring> &paths, POINT point);
  void finishDrag(HWND source, bool apply);
  void updateDragFeedback();
  static LRESULT CALLBACK dragPreviewProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  void refreshUngrouped();
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
};

class DropTarget final : public IDropTarget {
  Panel &panel;
  ULONG references = 1;
  bool files = false;
  static FORMATETC format() { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }
  DWORD effect(DWORD allowed) const {
    if (!files || panel.mapped() || panel.desktop.editing || panel.desktop.quitting) return DROPEFFECT_NONE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return allowed & DROPEFFECT_COPY;
  }
public:
  explicit DropTarget(Panel &value) : panel(value) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
    *out = static_cast<IDropTarget *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references;
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effects) override {
    auto spec = format();
    files = data->QueryGetData(&spec) == S_OK;
    *effects = effect(*effects);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effects) override {
    *effects = effect(*effects);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override { files = false; return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL point, DWORD *effects) override {
    const auto allowed = effect(*effects);
    *effects = DROPEFFECT_NONE;
    files = false;
    auto spec = format();
    STGMEDIUM medium{};
    if (allowed && SUCCEEDED(data->GetData(&spec, &medium))) {
      try {
        if (panel.desktop.drop(panel, static_cast<HDROP>(medium.hGlobal), {point.x, point.y})) *effects = allowed;
      } catch (...) {
        PostMessageW(panel.window, rejectedMessage, 0, 0);
      }
      ReleaseStgMedium(&medium);
    }
    return S_OK;
  }
};

void Panel::applyTheme(const ui::Palette &colors) {
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    applyBackground();
    syncSurfaceHeader();
  }
#endif
  updateShape();
  // The native list uses a solid background.
  ListView_SetBkColor(list, colors.field);
  ListView_SetTextBkColor(list, CLR_NONE);
  ListView_SetTextColor(list, colors.text);
  const auto track = colors.highContrast ? colors.field : colors.hover;
  verticalScroll.setTrackColor(track);
  horizontalScroll.setTrackColor(track);
  if (scrollCorner) InvalidateRect(scrollCorner, nullptr, FALSE);
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
}

int Panel::backgroundOpacity() const {
  return opacityPreview.value_or(desktop.groups[index].opacity.value_or(desktop.style.opacity));
}

COLORREF Panel::backgroundColor() const {
  const auto &colors = desktop.theme.colors;
  if (colors.highContrast) return colors.field;
  return colorPreview.value_or(mergeTint.value_or(desktop.groups[index].tint.value_or(colors.field)));
}

void Panel::applyBackground() {
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    const auto &colors = desktop.theme.colors;
    const auto opacity = static_cast<BYTE>(MulDiv(backgroundOpacity(), 255, 100));
    surface->appearance(colors.dark, colors.highContrast, backgroundColor(), opacity, desktop.style.glass);
    return;
  }
#endif
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
}

bool Panel::saveColor(std::optional<COLORREF> tint) {
  if (desktop.quitting) return false;
  if (desktop.groups[index].tint == tint) return true;
  if (index) {
    auto changed = desktop.groups;
    changed[index].tint = tint;
    return desktop.commit(std::move(changed), window, nullptr, false);
  }
  const DWORD value = tint.value_or(CLR_NONE);
  const auto result = RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedTint, REG_DWORD, &value, sizeof(value));
  if (result != ERROR_SUCCESS) {
    report(window, lang::text(L"无法保存桌面盒子"));
    return false;
  }
  desktop.groups.front().tint = tint;
  return true;
}

bool Panel::saveSpacing(int spacing) {
  if (desktop.quitting) return false;
  if (desktop.groups[index].itemSpacing == spacing) return true;
  if (index) {
    auto changed = desktop.groups;
    changed[index].itemSpacing = spacing;
    return desktop.commit(std::move(changed), window, nullptr, false);
  }
  const DWORD value = spacing;
  if (RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedItemSpacing,
                      REG_DWORD, &value, sizeof(value)) != ERROR_SUCCESS) {
    report(window, lang::text(L"无法保存桌面盒子"));
    return false;
  }
  desktop.groups.front().itemSpacing = spacing;
  return true;
}

#ifdef FOLDERNOTE_WINUI
void Panel::syncSurfaceHeader() {
  if (!hasSurface()) return;
  const auto &group = desktop.groups[index];
  const auto &colors = desktop.theme.colors;
  std::vector<std::wstring> titles{group.name};
  for (const auto &tab : group.tabs) titles.push_back(tab.name);
  surface->header(titles, activeTab, group.collapsed, headerActions, desktop.dropHighlight == window,
                  desktop.style.radius, colors.text, colors.muted, colors.border, colors.accent,
                  titleHeightPreview ? titleHeightPreview : group.titleHeight, group.titleStyle, headerIcon());
}
#endif

int Panel::headerHeight() const {
  return MulDiv(titleHeightPreview ? titleHeightPreview : desktop.groups[index].titleHeight, GetDpiForWindow(window), 96);
}

HICON Panel::headerIcon() {
  const auto &group = desktop.groups[index];
  if (group.titleStyle == TitleStyle::Text) return nullptr;
  if (cachedTitleIcon != group.titleIcon || !titleIcon) {
    const auto previous = titleIcon;
    titleIcon = nullptr;
    cachedTitleIcon = group.titleIcon;
    if (!cachedTitleIcon.empty()) {
      if (const auto extension = fn::fs::path(cachedTitleIcon).extension().wstring(); !_wcsicmp(extension.c_str(), L".ico"))
        titleIcon = static_cast<HICON>(LoadImageW(nullptr, cachedTitleIcon.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
      if (!titleIcon) {
        SHFILEINFOW file{};
        if (SHGetFileInfoW(cachedTitleIcon.c_str(), 0, &file, sizeof(file), SHGFI_ICON | SHGFI_SMALLICON))
          titleIcon = file.hIcon;
      }
    }
    if (!titleIcon) {
      SHSTOCKICONINFO stock{sizeof(stock)};
      if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &stock))) titleIcon = stock.hIcon;
    }
    if (previous) DestroyIcon(previous);
  }
  return titleIcon;
}

bool Panel::mapped() const {
  const auto tab = extraTab(desktop.groups[index], activeTab);
  return tab && !tab->folder.empty();
}

void Desktop::setAppearance(const Appearance &value) {
  if (style == value) return;
  const auto previous = std::exchange(style, value);
  const bool shadowChanged = previous.shadow != value.shadow || previous.shadowOpacity != value.shadowOpacity ||
                             previous.shadowBlur != value.shadowBlur || previous.shadowOffset != value.shadowOffset;
  for (const auto &panel : panels) {
#ifdef FOLDERNOTE_WINUI
    if (panel->hasSurface()) {
      if (previous.opacity != value.opacity || previous.glass != value.glass) {
        panel->applyBackground();
      }
      if (previous.radius != value.radius) panel->syncSurfaceHeader();
    }
#endif
    if (previous.radius != value.radius) {
      panel->updateShape();
      panel->queuePageLayout();
      if (!panel->hasSurface()) InvalidateRect(panel->window, nullptr, FALSE);
    }
    if (shadowChanged || previous.radius != value.radius) panel->updateShadow();
  }
  if (shadowChanged) lower();
}

void Panel::readMappedFolder() {
  if (mappedResult.valid() || mappedFolder.empty()) return;
  std::packaged_task<std::vector<MappedItem>()> read([folder = mappedFolder] {
    std::vector<MappedItem> items;
    std::error_code error;
    const fn::fs::directory_iterator end;
    for (fn::fs::directory_iterator entry(folder, error); !error && entry != end; entry.increment(error)) {
      const auto path = entry->path().wstring();
      WIN32_FILE_ATTRIBUTE_DATA data{};
      if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
          !(data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
        const auto modified = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                              data.ftLastWriteTime.dwLowDateTime;
        items.push_back({path, modified, data.dwFileAttributes});
      }
    }
    return items;
  });
  auto result = read.get_future();
  std::thread(std::move(read)).detach();
  mappedResult = std::move(result);
}

void Panel::showTab(size_t tab) {
  if (tab > desktop.groups[index].tabs.size() || activeTab == tab || desktop.dragging) return;
  activeTab = tab;
  currentPage = pageWheelRemainder = 0;
#ifdef FOLDERNOTE_WINUI
  syncSurfaceHeader();
#endif
  refresh(desktop.groups[index]);
  InvalidateRect(tabs, nullptr, FALSE);
}

void Panel::paintTabs(const DRAWITEMSTRUCT &item) {
  const auto &colors = desktop.theme.colors;
  const auto &group = desktop.groups[index];
  POINT offset{};
  MapWindowPoints(tabs, window, &offset, 1);
  const auto buffer = CreateCompatibleDC(item.hDC);
  const auto bitmap = buffer ? CreateCompatibleBitmap(item.hDC, item.rcItem.right, item.rcItem.bottom) : nullptr;
  const auto previousBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
  const bool buffered = previousBitmap && previousBitmap != HGDI_ERROR;
  const HDC dc = buffered ? buffer : item.hDC;
  paintBackground(dc, {-offset.x, -offset.y});
  const auto dpi = GetDpiForWindow(window);
  const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
  const auto oldMode = SetBkMode(dc, TRANSPARENT);
  POINT cursor{};
  GetCursorPos(&cursor);
  ScreenToClient(tabs, &cursor);
  for (size_t i = 0; i < tabAreas.size(); ++i) {
    auto box = tabAreas[i];
    InflateRect(&box, -px(1), 0);
    if (IsRectEmpty(&box)) continue;
    const bool selected = i == activeTab;
    const bool hot = PtInRect(&box, cursor);
    if (selected || hot) {
      const auto fill = selected && colors.highContrast ? colors.accent : selected ? colors.hover : colors.surface;
      auto upper = box;
      upper.bottom += px(5);
      ui::rounded(dc, upper, fill, fill, px(5));
    } else if (i + 1 < tabAreas.size() && i + 1 != activeTab) {
      const auto previous = SetDCBrushColor(dc, colors.border);
      const RECT divider{box.right, box.top + px(7), box.right + 1, box.bottom - px(7)};
      FillRect(dc, &divider, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      SetDCBrushColor(dc, previous);
    }
    const auto previous = SelectObject(dc, font);
    InflateRect(&box, -px(8), 0);
    const auto &title = i ? group.tabs[i - 1].name : group.name;
    ui::text(dc, title, box, selected && colors.highContrast ? colors.onAccent :
             selected || hot ? colors.text : colors.muted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previous);
  }
  SetBkMode(dc, oldMode);
  if (buffered) {
    BitBlt(item.hDC, 0, 0, item.rcItem.right, item.rcItem.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, previousBitmap);
  }
  if (bitmap) DeleteObject(bitmap);
  if (buffer) DeleteDC(buffer);
}

LRESULT CALLBACK tabInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data);
  constexpr UINT_PTR hoverTabTimer = 1;
  const auto tabAt = [&](POINT point) -> std::optional<size_t> {
    for (size_t i = 0; i < panel.tabAreas.size(); ++i)
      if (PtInRect(&panel.tabAreas[i], point)) return i;
    return std::nullopt;
  };
  const auto clearHover = [&] {
    KillTimer(control, hoverTabTimer);
    panel.hoveredTab.reset();
  };
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_MOUSELEAVE) {
    clearHover();
    InvalidateRect(control, nullptr, FALSE);
  }
  if (message == WM_TIMER && w == hoverTabTimer) {
    const auto pending = panel.hoveredTab;
    clearHover();
    POINT cursor{};
    if (pending && !panel.pressedTab && !panel.desktop.menuOpen && GetCursorPos(&cursor) &&
        WindowFromPoint(cursor) == control && ScreenToClient(control, &cursor) && tabAt(cursor) == pending)
      panel.showTab(*pending);
    return 0;
  }
  if (message == WM_LBUTTONDOWN) {
    clearHover();
    SetFocus(control);
    const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    for (size_t i = 1; i < panel.tabAreas.size(); ++i) {
      if (!PtInRect(&panel.tabAreas[i], point)) continue;
      panel.pressedTab = i;
      panel.tabPress = point;
      SetForegroundWindow(panel.desktop.scene);
      SetCapture(control);
      if (GetCapture() != control) panel.pressedTab = 0;
      return 0;
    }
  }
  if (message == WM_CAPTURECHANGED) panel.pressedTab = 0;
  if (panel.pressedTab && message == WM_MOUSEMOVE) {
    POINT screen{};
    GetCursorPos(&screen);
    SetCursor(LoadCursorW(nullptr, !panel.desktop.panelAt(screen) && panel.desktop.desktopDropAt(screen) ? IDC_SIZEALL : IDC_ARROW));
    return 0;
  }
  if (panel.pressedTab && message == WM_LBUTTONUP) {
    const auto tab = std::exchange(panel.pressedTab, 0);
    const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (GetCapture() == control) ReleaseCapture();
    POINT screen{};
    GetCursorPos(&screen);
    if ((std::abs(point.x - panel.tabPress.x) >= GetSystemMetrics(SM_CXDRAG) ||
         std::abs(point.y - panel.tabPress.y) >= GetSystemMetrics(SM_CYDRAG)) &&
        !panel.desktop.panelAt(screen) && panel.desktop.desktopDropAt(screen))
      panel.desktop.detachPanelTab(panel.index, tab, screen);
    else panel.showTab(tab);
    return 0;
  }
  const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
  if (message == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
    TrackMouseEvent(&track);
    auto next = tabAt(point);
    if (next != panel.hoveredTab) {
      clearHover();
      panel.hoveredTab = next;
      if (next && *next != panel.activeTab) SetTimer(control, hoverTabTimer, 140, nullptr);
      InvalidateRect(control, nullptr, FALSE);
    }
  }
  if (message == WM_LBUTTONUP)
    if (const auto tab = tabAt(point)) panel.showTab(*tab);
  if (message == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT) && !panel.tabAreas.empty()) {
    clearHover();
    const auto count = panel.tabAreas.size();
    panel.showTab((panel.activeTab + (w == VK_LEFT ? count - 1 : 1)) % count);
    return 0;
  }
  return DefSubclassProc(control, message, w, l);
}

LRESULT CALLBACK iconInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data);
  if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
    int row = -1;
    if (message == WM_MOUSEMOVE) {
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
      TrackMouseEvent(&track);
      LVHITTESTINFO hit{};
      hit.pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) row = ListView_HitTest(control, &hit);
    }
    if (row != panel.hoverRow) {
      const int previous = std::exchange(panel.hoverRow, row);
      if (previous >= 0) ListView_RedrawItems(control, previous, previous);
      if (row >= 0) ListView_RedrawItems(control, row, row);
    }
  }
  if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)) {
    panel.zoom(GET_WHEEL_DELTA_WPARAM(w));
    return 0;
  }
  if (message == WM_MOUSEWHEEL) {
    if (panel.pageWheel(GET_WHEEL_DELTA_WPARAM(w))) return 0;
    if (const int old = std::exchange(panel.hoverRow, -1); old >= 0)
      ListView_RedrawItems(control, old, old);
    if (panel.verticalScroll.maximum()) {
      panel.verticalScroll.wheel(GET_WHEEL_DELTA_WPARAM(w));
      return 0;
    }
    return DefSubclassProc(control, message, w, l);
  }
  if (message == WM_STYLECHANGED || message == WM_SHOWWINDOW)
    panel.queueScrollSync();
  return DefSubclassProc(control, message, w, l);
}

LRESULT CALLBACK toggleInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT || message == WM_PRINTCLIENT) {
    PAINTSTRUCT paint{};
    const auto dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(w);
    RECT box{};
    GetClientRect(control, &box);
    const auto id = GetDlgCtrlID(control);
    const auto state = SendMessageW(control, BM_GETSTATE, 0, 0);
    UINT flags = state & BST_PUSHED ? ODS_SELECTED : 0;
    if (GetFocus() == control) flags |= ODS_FOCUS;
    if (SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) flags |= ODS_NOFOCUSRECT;
    DRAWITEMSTRUCT item{ODT_BUTTON, static_cast<UINT>(id), 0, ODA_DRAWENTIRE, flags, control, dc, box, 0};
    SendMessageW(GetParent(control), WM_DRAWITEM, id, reinterpret_cast<LPARAM>(&item));
    if (message == WM_PAINT) EndPaint(control, &paint);
    return 0;
  }
  if (message == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
    TrackMouseEvent(&track);
    InvalidateRect(control, nullptr, FALSE);
  } else if (message == WM_MOUSELEAVE) {
    InvalidateRect(control, nullptr, FALSE);
  }
  return DefSubclassProc(control, message, w, l);
}

void Panel::zoom(int delta) {
  if (desktop.dragging || desktop.editing || desktop.quitting) return;
  constexpr int sizes[]{0, 24, 32, 48, 64, 96};
  const auto &group = desktop.groups[index];
  const auto current = group.view == View::List ? 0 : group.iconSize;
  const int step = static_cast<int>(std::find(std::begin(sizes), std::end(sizes), current) - std::begin(sizes));
  wheelRemainder += delta;
  const auto ticks = wheelRemainder / WHEEL_DELTA;
  wheelRemainder %= WHEEL_DELTA;
  const auto next = std::clamp(step + ticks, 0, static_cast<int>(std::size(sizes)) - 1);
  if (next == step) return;
  auto changed = desktop.groups;
  changed[index].view = next ? View::Icons : View::List;
  if (next) changed[index].iconSize = sizes[next];
  if (!desktop.commit(std::move(changed), window, nullptr, false)) return;
  renderPage(desktop.groups[index]);
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) surface->animateZoom(next > step);
#endif
}

void Panel::loadThumbnails(int pixels) {
  if (thumbnails) thumbnails->canceled = true;
  thumbnails.reset();
  if (!scaledIcons || displayed.empty()) return;
  auto batch = std::make_shared<ThumbnailBatch>();
  thumbnails = batch;
  auto paths = displayed;
  if (thumbnailCache.size() >= 256) {
    std::erase_if(thumbnailCache, [&](const auto &entry) {
      return std::find(paths.begin(), paths.end(), entry.first) == paths.end();
    });
  }
  for (auto &path : paths) if (thumbnailCache.contains(path)) path.clear();
  if (std::all_of(paths.begin(), paths.end(), [](const auto &path) { return path.empty(); })) return;
  std::thread([batch, paths = std::move(paths), pixels] {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    try {
      for (size_t i = 0; i < paths.size() && !batch->canceled; ++i) {
        if (paths[i].empty()) continue;
        Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
        if (FAILED(SHCreateItemFromParsingName(paths[i].c_str(), nullptr, IID_PPV_ARGS(&factory)))) continue;
        HBITMAP image = nullptr;
        auto result = factory->GetImage({pixels, pixels}, SIIGBF_THUMBNAILONLY | SIIGBF_CROPTOSQUARE | SIIGBF_SCALEUP, &image);
        if (FAILED(result)) result = factory->GetImage({pixels, pixels}, SIIGBF_ICONONLY | SIIGBF_SCALEUP, &image);
        Thumbnail owned(image, DeleteObject);
        if (SUCCEEDED(result) && owned) {
          std::lock_guard lock(batch->mutex);
          batch->ready.emplace_back(static_cast<int>(i), std::move(owned));
        }
      }
    } catch (...) {}
    CoUninitialize();
  }).detach();
}

void Panel::applyThumbnails() {
  if (!thumbnails || !scaledIcons) return;
  std::vector<std::pair<int, Thumbnail>> ready;
  {
    std::lock_guard lock(thumbnails->mutex);
    ready.swap(thumbnails->ready);
  }
  RECT dirty{};
  for (auto &[row, image] : ready) {
    if (row < 0 || static_cast<size_t>(row) >= displayed.size()) continue;
    BITMAP dimensions{};
    if (!GetObjectW(image.get(), sizeof(dimensions), &dimensions) ||
        dimensions.bmWidth != renderedIconSize || dimensions.bmHeight != renderedIconSize) continue;
    LVITEMW item{};
    item.mask = LVIF_IMAGE;
    item.iItem = row;
    if (!ListView_GetItem(list, &item)) continue;
    if (item.iImage >= 0) {
      if (!ImageList_Replace(scaledIcons, item.iImage, image.get(), nullptr)) continue;
    } else {
      item.iImage = ImageList_Add(scaledIcons, image.get(), nullptr);
      if (item.iImage >= 0) ListView_SetItem(list, &item);
    }
    if (thumbnailCache.size() < 256) thumbnailCache.try_emplace(displayed[row], std::move(image));
#ifdef FOLDERNOTE_WINUI
    if (surface && surface->available() && item.iImage >= 0) {
      const auto icon = ImageList_GetIcon(scaledIcons, item.iImage, ILD_TRANSPARENT);
      if (icon) {
        surface->refreshImage(row, icon);
        DestroyIcon(icon);
      }
    }
#endif
    RECT itemBounds{};
    if (ListView_GetItemRect(list, row, &itemBounds, LVIR_ICON)) UnionRect(&dirty, &dirty, &itemBounds);
  }
  if (!IsRectEmpty(&dirty)) InvalidateRect(list, &dirty, FALSE);
}

RECT Panel::dockTab(SIZE size) const {
  const auto dpi = GetDpiForWindow(window);
  const LONG length = MulDiv(96, dpi, 96);
  const LONG depth = MulDiv(8, dpi, 96);
  const LONG width = std::min(length, size.cx);
  const LONG height = std::min(length, size.cy);
  const LONG x = (size.cx - width) / 2;
  const LONG y = (size.cy - height) / 2;
  switch (dockSide) {
  case DockSide::Left: return {0, y, std::min(depth, size.cx), y + height};
  case DockSide::Right: return {std::max(0L, size.cx - depth), y, size.cx, y + height};
  case DockSide::Top: return {x, 0, x + width, std::min(depth, size.cy)};
  case DockSide::Bottom: return {x, std::max(0L, size.cy - depth), x + width, size.cy};
  case DockSide::None: return {};
  }
  return {};
}

bool Panel::showDockedBox(bool show) {
  if (dockHidden == !show) return true;
  if (!show && !dockTrigger) {
    dockTrigger = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD, 0, 0, 0, 0,
                                  window, nullptr, desktop.instance, nullptr);
    if (!dockTrigger || !SetWindowSubclass(dockTrigger, dockTriggerProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
      if (dockTrigger) DestroyWindow(std::exchange(dockTrigger, nullptr));
      return false;
    }
  }
  dockHidden = !show;
  if (!updateShape()) {
    dockHidden = show;
    return false;
  }
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) surface->show(show);
#endif
  if (show) ShowWindow(dockTrigger, SW_HIDE);
  InvalidateRect(window, nullptr, FALSE);
  updateShadow();
  if (show && shadow) desktop.lower();
  return true;
}

LRESULT CALLBACK Panel::dockTriggerProc(HWND trigger, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data);
  if (message == WM_NCHITTEST) return HTCLIENT;
  if (message == WM_SETCURSOR) {
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return TRUE;
  }
  if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
    panel.dockHideAt = 0;
    panel.showDockedBox(true);
    return 0;
  }
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(trigger, &paint);
    RECT area{};
    GetClientRect(trigger, &area);
    const auto &colors = panel.desktop.theme.colors;
    const auto previous = SetDCBrushColor(dc, colors.field);
    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    FillRect(dc, &area, brush);
    SetDCBrushColor(dc, colors.accent);
    FrameRect(dc, &area, brush);
    SetDCBrushColor(dc, previous);
    EndPaint(trigger, &paint);
    return 0;
  }
  return DefSubclassProc(trigger, message, w, l);
}

void Panel::setDockSide(DockSide side) {
  if (!showDockedBox(true)) return;
  dockSide = side;
  dockHideAt = 0;
}

void Panel::tickDock(POINT cursor, ULONGLONG now) {
  if (dockSide == DockSide::None || !IsWindowVisible(window) || desktop.quitting ||
      movingOrSizing || gestureEdge) return;
  RECT bounds{};
  if (!GetWindowRect(window, &bounds)) return;
  if (touchingWorkEdge(bounds, dockSide) != dockSide) {
    setDockSide(DockSide::None);
    return;
  }
  const auto captured = GetCapture();
  if (desktop.menuOpen || desktop.editing || (captured && GetAncestor(captured, GA_ROOT) == window)) {
    dockHideAt = 0;
    return;
  }
  const auto hit = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
  if (hit == window) {
    dockHideAt = 0;
    showDockedBox(true);
  } else if (!dockHidden) {
    if (!dockHideAt) dockHideAt = now + 450;
    else if (now >= dockHideAt && !desktop.dragging) showDockedBox(false);
  }
}

bool Panel::updateShape() {
  RECT bounds{};
  if (!GetWindowRect(window, &bounds)) return false;
  const auto width = bounds.right - bounds.left;
  const auto height = bounds.bottom - bounds.top;
  if (width <= 0 || height <= 0) return false;
  const auto diameter = cornerDiameter();
  const int signature = dockHidden ? -1 - static_cast<int>(dockSide) : diameter;
  if (shapeSize.cx == width && shapeSize.cy == height && shapeDiameter == signature) return true;
  const auto tab = dockTab({width, height});
  auto region = dockHidden ? CreateRectRgnIndirect(&tab) : createBoxRegion(width, height, diameter);
  if (!region) return false;
  shapeSize = {width, height};
  shapeDiameter = signature;
  if (!SetWindowRgn(window, region, TRUE)) {
    DeleteObject(region);
    shapeSize = {};
    return false;
  }
  if (dockHidden && dockTrigger)
    SetWindowPos(dockTrigger, HWND_TOP, tab.left, tab.top, tab.right - tab.left, tab.bottom - tab.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  return true;
}

int Panel::cornerDiameter() const {
  return MulDiv(static_cast<int>(desktop.style.radius) * 2, GetDpiForWindow(window), 96);
}

void Panel::paintBackground(HDC dc, POINT offset) {
  if (hasSurface()) return;
  RECT area{};
  GetClientRect(window, &area);
  OffsetRect(&area, offset.x, offset.y);
  const auto previous = SetDCBrushColor(dc, backgroundColor());
  FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  SetDCBrushColor(dc, previous);
}

RECT Panel::pageFooter() const {
  RECT footer{};
  GetClientRect(window, &footer);
  const auto dpi = GetDpiForWindow(window);
  footer.left += MulDiv(12, dpi, 96);
  footer.right -= MulDiv(20, dpi, 96);
  footer.top = std::max(0L, footer.bottom - MulDiv(26, dpi, 96));
  return footer;
}

int Panel::visiblePageDots() const {
  const auto footer = pageFooter();
  const int step = std::max(1, MulDiv(16, GetDpiForWindow(window), 96));
  return std::min(pageCount, static_cast<int>(std::max(1L, (footer.right - footer.left) / step)));
}

int Panel::firstPageDot() const {
  const int visible = visiblePageDots();
  return std::clamp(currentPage - visible / 2, 0, pageCount - visible);
}

void Panel::paint(HDC dc, bool backgroundOnly) {
  if (hasSurface()) return;
  paintBackground(dc);
  if (backgroundOnly) return;
  RECT area{};
  GetClientRect(window, &area);
  const auto &colors = desktop.theme.colors;
  const auto dpi = GetDpiForWindow(window);
  const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
  const int pad = px(14), headerBottom = headerHeight();
  const auto &group = desktop.groups[index];
  RECT button{};
  GetWindowRect(IsWindowVisible(viewButtons[0]) ? viewButtons[0] : toggle, &button);
  MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT *>(&button), 2);
  RECT header{pad, 0, button.left - px(5), headerBottom};
  auto old = SelectObject(dc, titleFont);
  const auto oldMode = SetBkMode(dc, TRANSPARENT);
  if (group.tabs.empty()) {
    if (const auto icon = headerIcon()) {
      DrawIconEx(dc, header.left, (header.bottom - px(18)) / 2, icon, px(18), px(18), 0, nullptr, DI_NORMAL);
      header.left += px(24);
    }
    if (group.titleStyle != TitleStyle::Icon) ui::text(dc, group.name, header, colors.text);
  }
  SetBkMode(dc, oldMode);
  SelectObject(dc, old);
  const auto oldColor = SetDCBrushColor(dc, boxOutlineColor(backgroundColor(), colors));
  const auto brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
  const auto diameter = cornerDiameter();
  if (auto outline = createBoxRegion(area.right, area.bottom, diameter)) {
    const bool target = desktop.dropHighlight == window;
    if (target) SetDCBrushColor(dc, colors.accent);
    FrameRgn(dc, outline, brush, target ? 2 : 1, target ? 2 : 1);
    DeleteObject(outline);
  }
  if (pageCount && !group.collapsed) {
    const auto footer = pageFooter();
    const int count = visiblePageDots(), first = firstPageDot();
    const int step = std::max(1, px(16));
    const int left = (footer.left + footer.right - count * step) / 2;
    const int y = (footer.top + footer.bottom) / 2;
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int dot = 0; dot < count; ++dot) {
      const bool active = first + dot == currentPage;
      const int x = left + dot * step + step / 2, radius = std::max(1, px(active ? 4 : 2));
      SetDCBrushColor(dc, active ? colors.text : colors.muted);
      Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
  }
  SetDCBrushColor(dc, oldColor);
}

bool Panel::paintItem(HDC dc, int row) {
  const bool details = desktop.groups[index].view == View::List;
  RECT bounds{}, label{}, icon{};
  if (!ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS) ||
      !ListView_GetItemRect(list, row, &label, LVIR_LABEL) ||
      !ListView_GetItemRect(list, row, &icon, LVIR_ICON)) return false;
  if (!RectVisible(dc, &bounds)) return true;
  wchar_t name[1024]{};
  LVITEMW item{};
  item.mask = LVIF_TEXT | LVIF_IMAGE;
  item.iItem = row;
  item.pszText = name;
  item.cchTextMax = ARRAYSIZE(name);
  if (!ListView_GetItem(list, &item)) return false;
  const auto images = ListView_GetImageList(list, details ? LVSIL_SMALL : LVSIL_NORMAL);
  const auto image = images && item.iImage >= 0 ? ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT) : nullptr;
  const bool selected = (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
  const bool painted = drawFenceItem(dc, bounds, label, icon, image, name, desktop.theme.colors,
                                    selected, row == hoverRow, details, GetDpiForWindow(window));
  if (image) DestroyIcon(image);
  return painted;
}

void Panel::setCollapsed() {
  const auto &group = desktop.groups[index];
  const bool collapsed = group.collapsed;
  RECT current{};
  GetWindowRect(window, &current);
  RECT bounds{current.left, current.top, current.right,
              current.top + (collapsed ? headerHeight() : group.bounds.bottom - group.bounds.top)};
  bounds = visibleBounds(bounds);
  SetWindowTextW(toggle, lang::text(collapsed ? L"展开盒子" : L"折叠盒子"));
  if (collapsed && GetFocus() == list) SetFocus(toggle);
  bool nativeVisible = !collapsed;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    nativeVisible = false;
    syncSurfaceHeader();
  }
#endif
  ShowWindow(list, nativeVisible ? SW_SHOWNA : SW_HIDE);
  ShowWindow(tabs, !hasSurface() && !group.tabs.empty() ? SW_SHOWNA : SW_HIDE);
  place(bounds);
}

void Panel::place(RECT bounds) {
  SetWindowPos(window, nullptr, bounds.left, bounds.top,
               bounds.right - bounds.left, bounds.bottom - bounds.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
  if (windowPositionReady) updateShadow();
}

void Panel::stopGesture() {
  if (window) KillTimer(window, boxMoveTimer);
#ifdef FOLDERNOTE_WINUI
  if (surface) {
    surface->onFrame({});
    surface->animateReflow(false);
  }
#endif
  for (auto &guide : alignmentGuides)
    if (guide) DestroyWindow(std::exchange(guide, nullptr));
  if (std::exchange(mergeTint, std::nullopt) && window && !desktop.quitting) applyBackground();
  lastCompositionFrame = 0;
  gestureEnding = false;
}

void Panel::snapGesture(RECT &bounds) {
  if (GetAsyncKeyState(VK_MENU) < 0) {
    for (const auto guide : alignmentGuides) if (guide) ShowWindow(guide, SW_HIDE);
    return;
  }
  struct Snap {
    LONG delta = 0, position = 0, from = 0, to = 0;
    int distance = INT_MAX;
  } horizontal, vertical;
  const auto dpi = GetDpiForWindow(window);
  const int distance = std::max(1, MulDiv(8, dpi, 96));
  const int minWidth = MulDiv(190, dpi, 96);
  const int minHeight = std::max(MulDiv(112, dpi, 96), headerHeight() + MulDiv(44, dpi, 96));
  const bool moving = gestureEdge == HTCAPTION;
  const bool left = gestureEdge == HTLEFT || gestureEdge == HTTOPLEFT || gestureEdge == HTBOTTOMLEFT;
  const bool right = gestureEdge == HTRIGHT || gestureEdge == HTTOPRIGHT || gestureEdge == HTBOTTOMRIGHT;
  const bool top = gestureEdge == HTTOP || gestureEdge == HTTOPLEFT || gestureEdge == HTTOPRIGHT;
  const bool bottom = gestureEdge == HTBOTTOM || gestureEdge == HTBOTTOMLEFT || gestureEdge == HTBOTTOMRIGHT;
  const auto consider = [&](Snap &axis, LONG source, LONG target, LONG from, LONG to) {
    const auto delta = target - source;
    if (const auto gap = std::abs(delta); gap <= distance && gap < axis.distance)
      axis = {delta, target, from, to, static_cast<int>(gap)};
  };
  const auto compare = [&](RECT target) {
    for (const auto x : {target.left, target.right}) {
      const auto from = std::min(bounds.top, target.top), to = std::max(bounds.bottom, target.bottom);
      if (moving || (left && bounds.right - x >= minWidth)) consider(horizontal, bounds.left, x, from, to);
      if (moving || (right && x - bounds.left >= minWidth)) consider(horizontal, bounds.right, x, from, to);
    }
    for (const auto y : {target.top, target.bottom}) {
      const auto from = std::min(bounds.left, target.left), to = std::max(bounds.right, target.right);
      if (moving || (top && bounds.bottom - y >= minHeight)) consider(vertical, bounds.top, y, from, to);
      if (moving || (bottom && y - bounds.top >= minHeight)) consider(vertical, bounds.bottom, y, from, to);
    }
  };
  const auto monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
  for (const auto &panel : desktop.panels) {
    if (panel.get() == this || !IsWindowVisible(panel->window)) continue;
    RECT other{};
    if (GetWindowRect(panel->window, &other) && MonitorFromRect(&other, MONITOR_DEFAULTTONEAREST) == monitor)
      compare(other);
  }
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfoW(monitor, &info)) compare(info.rcWork);
  if (moving) OffsetRect(&bounds, horizontal.delta, vertical.delta);
  else {
    if (left) bounds.left += horizontal.delta;
    if (right) bounds.right += horizontal.delta;
    if (top) bounds.top += vertical.delta;
    if (bottom) bounds.bottom += vertical.delta;
  }
  const int thickness = std::max(1, MulDiv(1, dpi, 96));
  for (int axis = 0; axis < 2; ++axis) {
    const auto &snap = axis == 0 ? horizontal : vertical;
    auto &guide = alignmentGuides[axis];
    if (snap.distance == INT_MAX) {
      if (guide) ShowWindow(guide, SW_HIDE);
      continue;
    }
    if (!guide) {
      guide = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                              L"STATIC", nullptr, WS_POPUP | SS_OWNERDRAW, 0, 0, 0, 0,
                              window, nullptr, desktop.instance, nullptr);
      if (!guide) continue;
      SetLayeredWindowAttributes(guide, 0, 220, LWA_ALPHA);
      SetWindowSubclass(guide, [](HWND item, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) -> LRESULT {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
          PAINTSTRUCT paint{};
          const auto dc = BeginPaint(item, &paint);
          SetDCBrushColor(dc, reinterpret_cast<Panel *>(data)->desktop.theme.colors.accent);
          FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
          EndPaint(item, &paint);
          return 0;
        }
        return DefSubclassProc(item, message, w, l);
      }, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    const bool x = axis == 0;
    const RECT line{x ? snap.position : snap.from, x ? snap.from : snap.position,
                    x ? snap.position + thickness : snap.to, x ? snap.to : snap.position + thickness};
    RECT current{};
    if (!IsWindowVisible(guide) || !GetWindowRect(guide, &current) || !EqualRect(&line, &current))
      SetWindowPos(guide, HWND_TOPMOST, line.left, line.top, line.right - line.left, line.bottom - line.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

void Panel::updateGestureTarget(POINT point) {
  const auto dx = point.x - gestureStart.x, dy = point.y - gestureStart.y;
  auto bounds = gestureBounds;
  if (gestureEdge == HTCAPTION) {
    OffsetRect(&bounds, dx, dy);
    gestureTarget = visibleBounds(bounds);
    snapGesture(gestureTarget);
    return;
  }
  if (gestureEdge == resizeTitle) {
    const auto dpi = GetDpiForWindow(window);
    const auto &group = desktop.groups[index];
    const int available = group.collapsed ? 80 : MulDiv(bounds.bottom - bounds.top, 96, dpi) - 44;
    titleHeightPreview = std::clamp(group.titleHeight + MulDiv(dy, 96, dpi), 28, std::clamp(available, 28, 80));
    return;
  }
  const auto dpi = GetDpiForWindow(window);
  const int minWidth = MulDiv(190, dpi, 96);
  const int minHeight = std::max(MulDiv(112, dpi, 96), headerHeight() + MulDiv(44, dpi, 96));
  if (gestureEdge == HTLEFT || gestureEdge == HTTOPLEFT || gestureEdge == HTBOTTOMLEFT)
    bounds.left = std::min<LONG>(bounds.left + dx, bounds.right - minWidth);
  if (gestureEdge == HTRIGHT || gestureEdge == HTTOPRIGHT || gestureEdge == HTBOTTOMRIGHT)
    bounds.right = std::max<LONG>(bounds.right + dx, bounds.left + minWidth);
  if (gestureEdge == HTTOP || gestureEdge == HTTOPLEFT || gestureEdge == HTTOPRIGHT)
    bounds.top = std::min<LONG>(bounds.top + dy, bounds.bottom - minHeight);
  if (gestureEdge == HTBOTTOM || gestureEdge == HTBOTTOMLEFT || gestureEdge == HTBOTTOMRIGHT)
    bounds.bottom = std::max<LONG>(bounds.bottom + dy, bounds.top + minHeight);
  gestureTarget = visibleBounds(bounds);
  snapGesture(gestureTarget);
}

void Panel::updateGestureFrame() {
  if (!gestureEdge || gestureEnding) return;
  if (GetAsyncKeyState(VK_ESCAPE) < 0) {
    gestureEnding = PostMessageW(window, finishBoxGestureMessage, FALSE, 0) != FALSE;
    return;
  }
  if (GetAsyncKeyState(VK_LBUTTON) >= 0) {
    gestureEnding = PostMessageW(window, finishBoxGestureMessage, TRUE, 0) != FALSE;
    return;
  }
  POINT cursor{};
  if (!GetCursorPos(&cursor)) return;
  const int previousHeight = titleHeightPreview;
  updateGestureTarget(cursor);
  if (gestureEdge == resizeTitle) {
    if (titleHeightPreview != previousHeight) {
      if (desktop.groups[index].collapsed) setCollapsed();
      else resize();
      InvalidateRect(window, nullptr, FALSE);
    }
    return;
  }
  RECT current{};
  GetWindowRect(window, &current);
  if (gestureEdge != HTCAPTION) {
    if (!EqualRect(&current, &gestureTarget)) place(gestureTarget);
    return;
  }
  const auto merge = !EqualRect(&gestureTarget, &gestureBounds) ? desktop.mergeTargetAt(cursor, *this) : nullptr;
  const auto tint = merge ? std::optional(merge->backgroundColor()) : std::nullopt;
  if (mergeTint != tint) {
    mergeTint = tint;
    applyBackground();
  }
  desktop.highlight(merge ? merge->window : nullptr);
  if (current.left == gestureTarget.left && current.top == gestureTarget.top) return;
  if (!SetWindowPos(window, nullptr, gestureTarget.left, gestureTarget.top, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) return;
  updateShadow();
}

void Panel::finishGesture(bool accept) {
  const auto edge = std::exchange(gestureEdge, 0);
  const auto start = gestureBounds;
  RECT current{};
  const bool hasBounds = GetWindowRect(window, &current) != FALSE;
  POINT cursor{};
  const bool hasCursor = accept && GetCursorPos(&cursor);
  if (hasCursor) {
    gestureEdge = edge;
    updateGestureTarget(cursor);
    gestureEdge = 0;
  }
  if (edge == resizeTitle) {
    const int height = std::exchange(titleHeightPreview, 0);
    stopGesture();
    movingOrSizing = false;
    if (GetCapture() == window) ReleaseCapture();
    if (accept && height && height != desktop.groups[index].titleHeight) {
      if (index) {
        auto changed = desktop.groups;
        changed[index].titleHeight = height;
        desktop.commit(std::move(changed), window, nullptr, false);
      } else {
        const DWORD saved = height;
        if (RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedTitleHeight, REG_DWORD,
                           &saved, sizeof(saved)) == ERROR_SUCCESS) desktop.groups.front().titleHeight = height;
        else report(window, lang::text(L"无法保存桌面盒子"));
      }
    }
    if (desktop.groups[index].collapsed) setCollapsed();
    resize();
    desktop.lower();
    return;
  }
  auto *merge = hasCursor && edge == HTCAPTION && !EqualRect(&start, &gestureTarget)
                    ? desktop.mergeTargetAt(cursor, *this) : nullptr;
  if (accept && edge == HTCAPTION && hasBounds) {
    gestureTarget.right = gestureTarget.left + current.right - current.left;
    gestureTarget.bottom = gestureTarget.top + current.bottom - current.top;
  }
  const auto target = accept ? visibleBounds(gestureTarget) : start;
  stopGesture();
  desktop.highlight(nullptr);
  if (GetCapture() == window) ReleaseCapture();
  if (merge) {
    movingOrSizing = false;
    if (desktop.mergePanels(index, merge->index)) return;
    place(start);
    desktop.lower();
    return;
  }
  if (!hasBounds || !EqualRect(&current, &target)) place(target);
  if (accept && !EqualRect(&start, &target)) {
    SendMessageW(window, WM_EXITSIZEMOVE, edge == HTCAPTION, 0);
  } else {
    movingOrSizing = false;
    if (!hasSurface()) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
  }
  desktop.lower();
}

void Panel::refresh(const Group &group) {
  if (activeTab > group.tabs.size()) activeTab = 0;
  const auto tab = extraTab(group, activeTab);
  const bool folderMapped = tab && !tab->folder.empty();
  const auto folder = folderMapped ? tab->folder : std::wstring{};
  if (folder != mappedFolder) {
    mappedFolder = folder;
    mappedItems.clear();
    mappedResult = {};
  }
  auto items = manualItems(group, activeTab);
  if (folderMapped) {
    if (mappedResult.valid() && mappedResult.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try { mappedItems = mappedResult.get(); } catch (...) {}
    }
    readMappedFolder();
    for (const auto &entry : mappedItems) items.push_back(entry.path);
  }
  const auto sort = folderMapped && group.sort == Sort::Manual ? Sort::Name : group.sort;
  if (sort != Sort::Manual) {
    struct SortKey { std::wstring path, name, type; ULONGLONG modified = 0; };
    std::vector<SortKey> keys;
    keys.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
      auto &path = items[i];
      const fn::fs::path file(path);
      SortKey key{std::move(path), file.filename().wstring(), file.extension().wstring()};
      if (sort == Sort::Modified) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (folderMapped) key.modified = mappedItems[i].modified;
        else if (GetFileAttributesExW(key.path.c_str(), GetFileExInfoStandard, &data))
          key.modified = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                         data.ftLastWriteTime.dwLowDateTime;
      }
      keys.push_back(std::move(key));
    }
    std::stable_sort(keys.begin(), keys.end(), [&](const SortKey &a, const SortKey &b) {
      if (sort == Sort::Modified && a.modified != b.modified) return a.modified > b.modified;
      if (sort == Sort::Type) {
        const auto result = StrCmpLogicalW(a.type.c_str(), b.type.c_str());
        if (result) return result < 0;
      }
      const auto name = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
      return name ? name < 0 : a.path < b.path;
    });
    for (size_t i = 0; i < keys.size(); ++i) items[i] = std::move(keys[i].path);
  }
  orderedItems = std::move(items);
  renderPage(group);
}

int Panel::pageSize(const Group &group) const {
  if (group.displayMode != DisplayMode::Pages) return 0;
  const auto dpi = GetDpiForWindow(window);
  const int spacing = spacingPreview.value_or(group.itemSpacing);
#ifdef FOLDERNOTE_WINUI
  if (hasSurface())
    return surface->pageCapacity(group.view == View::List, group.iconSize,
                                 titleHeightPreview ? titleHeightPreview : group.titleHeight,
                                 desktop.style.radius, spacing);
#endif
  RECT area{};
  GetWindowRect(list, &area);
  const int width = area.right - area.left;
  const int height = area.bottom - area.top;
  if (group.view == View::List) {
    RECT item{};
    const int row = ListView_GetView(list) == LV_VIEW_DETAILS && ListView_GetItemRect(list, 0, &item, LVIR_BOUNDS)
        ? item.bottom - item.top : MulDiv(24, dpi, 96);
    return std::max(1, height / std::max(1, row));
  }
  const int columnWidth = MulDiv(std::max(80, group.iconSize + 28) + spacing, dpi, 96);
  const int rowHeight = MulDiv(group.iconSize + 46 + spacing, dpi, 96);
  return std::max(1, width / columnWidth) * std::max(1, height / rowHeight);
}

void Panel::queuePageLayout() {
  if (desktop.groups[index].displayMode != DisplayMode::Pages || desktop.groups[index].collapsed ||
      pageLayoutPending || pageSize(desktop.groups[index]) == pageCapacity) return;
  pageLayoutPending = PostMessageW(window, updatePagesMessage, 0, 0) != FALSE;
}

void Panel::goToPage(int page) {
  const auto &group = desktop.groups[index];
  if (!pageCount || group.collapsed || group.displayMode != DisplayMode::Pages ||
      desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting) return;
  const int next = std::clamp(page, 0, pageCount - 1);
  if (currentPage == next) return;
  currentPage = next;
  renderPage(group);
}

bool Panel::pageWheel(int delta) {
  const auto &group = desktop.groups[index];
  if (group.displayMode != DisplayMode::Pages || group.collapsed) return false;
  if (desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting) return true;
  pageWheelRemainder += delta;
  const int ticks = pageWheelRemainder / WHEEL_DELTA;
  pageWheelRemainder %= WHEEL_DELTA;
  if (ticks) PostMessageW(window, goToPageMessage, TRUE, -ticks);
  return true;
}

void Panel::renderPage(const Group &group) {
  if (!group.collapsed || !pageCapacity || group.displayMode == DisplayMode::Scroll)
    pageCapacity = pageSize(group);
  const auto size = static_cast<int>(orderedItems.size());
  pageCount = pageCapacity ? std::max(1, size / pageCapacity + (size % pageCapacity != 0)) : 0;
  currentPage = std::clamp(currentPage, 0, std::max(0, pageCount - 1));
  const int first = pageCount ? currentPage * pageCapacity : 0;
  const int last = pageCount ? first + std::min(pageCapacity, size - first) : size;
  std::vector<std::wstring> items(orderedItems.begin() + first, orderedItems.begin() + last);
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) surface->pagination(currentPage, pageCount, [this](int page) {
    PostMessageW(window, goToPageMessage, FALSE, page);
  });
#endif
  if (!hasSurface()) {
    const auto footer = pageFooter();
    InvalidateRect(window, &footer, FALSE);
  }
  const bool folderMapped = mapped();
  const bool listMode = group.view == View::List;
  const auto view = listMode ? LV_VIEW_DETAILS : LV_VIEW_ICON;
  const int pixels = listMode ? 0 : MulDiv(group.iconSize, GetDpiForWindow(window), 96);
  const auto oldView = ListView_GetView(list);
  const auto oldIconSize = renderedIconSize;
  if (items == displayed && ListView_GetView(list) == static_cast<DWORD>(view) && renderedIconSize == pixels) {
    if (tabAreas.size() != (group.tabs.empty() ? 0 : group.tabs.size() + 1)) resize();
    return;
  }
  const int top = ListView_GetTopIndex(list);
  const bool keepTop = !pageCount && listMode && ListView_GetView(list) == LV_VIEW_DETAILS && top > 0 &&
                       static_cast<size_t>(top) < displayed.size();
  const auto topPath = keepTop ? displayed[top] : std::wstring{};
  std::vector<std::wstring> selectedPaths;
  for (int row = ListView_GetNextItem(list, -1, LVNI_SELECTED); row >= 0;
       row = ListView_GetNextItem(list, row, LVNI_SELECTED)) {
    if (static_cast<size_t>(row) < displayed.size()) selectedPaths.push_back(displayed[row]);
  }
  const auto focused = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
  const auto focusedPath = focused >= 0 && static_cast<size_t>(focused) < displayed.size() ?
      displayed[focused] : std::wstring{};
  hoverRow = -1;
  displayed = std::move(items);
  SetWindowTextW(window, group.name.c_str());
  const bool redrawNative = (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0;
  if (redrawNative) SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  ListView_SetView(list, view);
  ListView_DeleteAllItems(list);
  ListView_SetImageList(list, nullptr, LVSIL_NORMAL);
  if (scaledIcons) ImageList_Destroy(scaledIcons);
  if (pixels != thumbnailCacheSize) {
    thumbnailCache.clear();
    thumbnailCacheSize = pixels;
  }
  scaledIcons = pixels ? ImageList_Create(pixels, pixels, ILC_COLOR32 | ILC_MASK, 0, 16) : nullptr;
  renderedIconSize = pixels;
  Microsoft::WRL::ComPtr<IImageList> systemImages;
  if (scaledIcons) {
    SHGetImageList(pixels > 48 ? SHIL_JUMBO : pixels > 32 ? SHIL_EXTRALARGE : SHIL_LARGE,
                   IID_PPV_ARGS(&systemImages));
    ListView_SetImageList(list, scaledIcons, LVSIL_NORMAL);
  }
  bool imageListSet = false;
  for (size_t i = 0; i < displayed.size(); ++i) {
    const auto &path = displayed[i];
    const auto name = fn::fs::path(path).filename().wstring();
    SHFILEINFOW icon{};
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    if (folderMapped) {
      const auto found = std::find_if(mappedItems.begin(), mappedItems.end(), [&](const auto &item) { return item.path == path; });
      if (found != mappedItems.end()) attributes = found->attributes;
    } else if (index) attributes = GetFileAttributesW(path.c_str());
    PIDLIST_ABSOLUTE id = nullptr;
    DWORD_PTR images = 0;
    std::wstring label = name;
    if (!folderMapped && (index == 0 || attributes != INVALID_FILE_ATTRIBUTES) &&
        SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr))) {
      images = SHGetFileInfoW(reinterpret_cast<const wchar_t *>(id), 0, &icon, sizeof(icon),
                             SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX |
                             (listMode ? SHGFI_SMALLICON : SHGFI_LARGEICON));
      if (images && icon.szDisplayName[0]) label = icon.szDisplayName;
      CoTaskMemFree(id);
    }
    if (!images)
      images = SHGetFileInfoW(name.c_str(), attributes == INVALID_FILE_ATTRIBUTES ? FILE_ATTRIBUTE_NORMAL : attributes,
                             &icon, sizeof(icon), SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES |
                             (listMode ? SHGFI_SMALLICON : SHGFI_LARGEICON));
    if (images && !imageListSet && !scaledIcons) {
      ListView_SetImageList(list, reinterpret_cast<HIMAGELIST>(images), listMode ? LVSIL_SMALL : LVSIL_NORMAL);
      imageListSet = true;
    }
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.iItem = static_cast<int>(i);
    item.iImage = images ? icon.iIcon : I_IMAGENONE;
    if (scaledIcons) {
      item.iImage = I_IMAGENONE;
      HICON bitmap = nullptr;
      if (const auto cached = thumbnailCache.find(path); cached != thumbnailCache.end()) {
        item.iImage = ImageList_Add(scaledIcons, cached->second.get(), nullptr);
      } else if (images && systemImages && SUCCEEDED(systemImages->GetIcon(icon.iIcon, ILD_TRANSPARENT, &bitmap))) {
        const auto scaled = static_cast<HICON>(CopyImage(bitmap, IMAGE_ICON, pixels, pixels, 0));
        item.iImage = ImageList_AddIcon(scaledIcons, scaled ? scaled : bitmap);
        if (scaled) DestroyIcon(scaled);
        DestroyIcon(bitmap);
      }
    }
    item.pszText = label.data();
    ListView_InsertItem(list, &item);
    UINT state = std::any_of(selectedPaths.begin(), selectedPaths.end(), [&](const auto &selected) {
      return samePath(path, selected);
    }) ? LVIS_SELECTED : 0;
    if (!focusedPath.empty() && samePath(path, focusedPath)) state |= LVIS_FOCUSED;
    if (state) ListView_SetItemState(list, static_cast<int>(i), state, LVIS_SELECTED | LVIS_FOCUSED);
  }
  if (keepTop && !displayed.empty()) {
    const auto found = std::find_if(displayed.begin(), displayed.end(),
                                    [&](const auto &path) { return samePath(path, topPath); });
    const int row = found == displayed.end() ? std::min(top, static_cast<int>(displayed.size()) - 1) :
                                              static_cast<int>(found - displayed.begin());
    RECT item{};
    if (ListView_GetItemRect(list, 0, &item, LVIR_BOUNDS))
      ListView_Scroll(list, 0, (row - ListView_GetTopIndex(list)) * (item.bottom - item.top));
  }
  if (redrawNative) SendMessageW(list, WM_SETREDRAW, TRUE, 0);
#ifdef FOLDERNOTE_WINUI
  if (surface && surface->available()) {
    std::vector<winui::Item> entries;
    entries.reserve(displayed.size());
    const auto images = ListView_GetImageList(list, listMode ? LVSIL_SMALL : LVSIL_NORMAL);
    for (size_t i = 0; i < displayed.size(); ++i) {
      wchar_t label[1024]{};
      ListView_GetItemText(list, static_cast<int>(i), 0, label, ARRAYSIZE(label));
      LVITEMW item{};
      item.mask = LVIF_IMAGE;
      item.iItem = static_cast<int>(i);
      const auto icon = images && ListView_GetItem(list, &item) && item.iImage >= 0 ?
                            ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT) : nullptr;
      entries.push_back({label, displayed[i], icon});
    }
    surface->rows(entries, listMode, group.iconSize, spacingPreview.value_or(group.itemSpacing));
    for (const auto &entry : entries) if (entry.image) DestroyIcon(entry.image);
    std::vector<int> selection;
    for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0;) selection.push_back(row);
    surface->select(selection);
  }
#endif
  loadThumbnails(pixels);
  InvalidateRect(window, nullptr, FALSE);
  if (tabAreas.size() != (group.tabs.empty() ? 0 : group.tabs.size() + 1) ||
      ListView_GetView(list) != oldView || renderedIconSize != oldIconSize) resize();
  else if (IsWindowVisible(list)) queueScrollSync();
  if (IsWindowVisible(list))
    RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
  queuePageLayout();
}

void Panel::resize() {
  if (!list) return;
  RECT area{};
  GetClientRect(window, &area);
  const int header = headerHeight();
  const auto dpi = GetDpiForWindow(window);
  const int pad = MulDiv(6, dpi, 96);
  const int buttonWidth = MulDiv(30, dpi, 96);
  MoveWindow(toggle, std::max(0L, area.right - buttonWidth - pad), pad, buttonWidth, header - 2 * pad, TRUE);
  for (int i = 0; i < 2; ++i)
    MoveWindow(viewButtons[i], std::max(0L, area.right - pad - buttonWidth * (3 - i)), pad,
               buttonWidth, header - 2 * pad, TRUE);
  const auto &group = desktop.groups[index];
  const int spacing = spacingPreview.value_or(group.itemSpacing);
  const auto tabCount = group.tabs.size() + 1;
  const int width = std::max(0L, area.right - 2 * pad);
  tabAreas.clear();
  if (!group.tabs.empty()) {
    const int tabLeft = MulDiv(6, dpi, 96);
    // Reserve the collapse/view buttons and a draggable part of the title bar.
    const int available = std::max(0L, area.right - pad - 3 * buttonWidth - MulDiv(16, dpi, 96) - tabLeft);
    const int tabWidth = std::min(MulDiv(116, dpi, 96), available / static_cast<int>(tabCount));
    const int tabHeight = header - pad;
    MoveWindow(tabs, tabLeft, pad, tabWidth * static_cast<int>(tabCount), tabHeight, TRUE);
    for (size_t i = 0; i < tabCount; ++i) {
      tabAreas.push_back({static_cast<LONG>(i * tabWidth), 0,
                          static_cast<LONG>((i + 1) * tabWidth), tabHeight});
    }
  }
  ShowWindow(tabs, hasSurface() || group.tabs.empty() ? SW_HIDE : SW_SHOWNA);
  InvalidateRect(tabs, nullptr, FALSE);
  const int contentTop = header + MulDiv(10, dpi, 96);
  const bool nativePaging = !hasSurface() && group.displayMode == DisplayMode::Pages;
  const int contentBottom = nativePaging ? pageFooter().top : area.bottom - pad;
  const auto listStyle = GetWindowLongPtrW(list, GWL_STYLE);
  const auto nextStyle = nativePaging ? listStyle | LVS_NOSCROLL : listStyle & ~LVS_NOSCROLL;
  if (listStyle != nextStyle) {
    SetWindowLongPtrW(list, GWL_STYLE, nextStyle);
    SetWindowPos(list, nullptr, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
  }
  MoveWindow(list, pad, contentTop, width, std::max(0, contentBottom - contentTop), TRUE);
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    if (!EqualRect(&surfaceBounds, &area)) {
      surfaceBounds = area;
      surface->place(area);
    }
    syncSurfaceHeader();
    surface->itemSpacing(spacing);
    ShowWindow(list, SW_HIDE);
  }
#endif
  if (group.view == View::List) ListView_SetColumnWidth(list, 0, std::max(0, width -
      (nativePaging ? 0 : GetSystemMetricsForDpi(SM_CXVSCROLL, dpi))));
  else ListView_SetIconSpacing(list, MulDiv(std::max(80, group.iconSize + 28) + spacing, dpi, 96),
                               MulDiv(group.iconSize + 46 + spacing, dpi, 96));
  syncScroll();
  queuePageLayout();
}

void Panel::updateHeaderActions(POINT cursor) {
  RECT bounds{};
  GetWindowRect(window, &bounds);
  bounds.bottom = bounds.top + headerHeight();
  const auto hit = WindowFromPoint(cursor);
  const bool show = IsWindowVisible(window) && PtInRect(&bounds, cursor) &&
      (hit == window || IsChild(window, hit)) &&
      !movingOrSizing && !desktop.menuOpen && !desktop.dragging;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    if (headerActions != show) {
      headerActions = show;
      surface->showActions(show);
    }
    return;
  }
#endif
  for (auto button : viewButtons) {
    if (button && (IsWindowVisible(button) != FALSE) != show) {
      ShowWindow(button, show ? SW_SHOWNA : SW_HIDE);
      InvalidateRect(window, nullptr, FALSE);
    }
  }
}

void Panel::queueScrollSync() {
  if (!scrollSyncPending) scrollSyncPending = PostMessageW(window, syncScrollMessage, 0, 0) != FALSE;
}

void Panel::syncScroll() {
  if (!verticalScroll.window() || !horizontalScroll.window() || !scrollCorner) return;
  const auto dpi = GetDpiForWindow(window);
  const auto &group = desktop.groups[index];
  const auto style = GetWindowLongPtrW(list, GWL_STYLE);
  const auto syncBar = [&](ui::ScrollBar &bar, bool horizontal) -> RECT {
    SCROLLINFO range{sizeof(range), SIF_RANGE | SIF_PAGE | SIF_POS};
    SCROLLBARINFO native{sizeof(native)};
    if (group.collapsed || group.displayMode == DisplayMode::Pages || !IsWindowVisible(list) ||
        !(style & (horizontal ? WS_HSCROLL : WS_VSCROLL)) ||
        !GetScrollInfo(list, horizontal ? SB_HORZ : SB_VERT, &range) ||
        !GetScrollBarInfo(list, horizontal ? OBJID_HSCROLL : OBJID_VSCROLL, &native) ||
        IsRectEmpty(&native.rcScrollBar)) {
      bar.configure(0, 1, 0, dpi);
      return {};
    }
    auto area = native.rcScrollBar;
    RECT previous{};
    const bool move = !GetWindowRect(bar.window(), &previous) || !EqualRect(&previous, &area);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&area), 2);
    if (move)
      SetWindowPos(bar.window(), nullptr, area.left, area.top, area.right - area.left,
                   area.bottom - area.top, SWP_NOACTIVATE | SWP_NOZORDER);
    const int line = !horizontal && group.view == View::List ? 1 : MulDiv(30, dpi, 96);
    bar.configure(range.nMax - range.nMin + 1, static_cast<int>(range.nPage), range.nPos - range.nMin, dpi, line);
    return bar.maximum() ? area : RECT{};
  };
  const auto vertical = syncBar(verticalScroll, false);
  const auto horizontal = syncBar(horizontalScroll, true);
  if (IsRectEmpty(&vertical) || IsRectEmpty(&horizontal)) {
    if (IsWindowVisible(scrollCorner)) ShowWindow(scrollCorner, SW_HIDE);
  }
  else {
    SetWindowPos(scrollCorner, HWND_TOP, vertical.left, horizontal.top, vertical.right - vertical.left,
                 horizontal.bottom - horizontal.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

void Panel::scrollTo(int position, bool horizontal) {
  SCROLLINFO current{sizeof(current), SIF_RANGE | SIF_POS};
  if (!GetScrollInfo(list, horizontal ? SB_HORZ : SB_VERT, &current)) return;
  int delta = position - (current.nPos - current.nMin);
  if (!horizontal && desktop.groups[index].view == View::List) {
    RECT row{};
    if (!ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS)) return;
    delta *= row.bottom - row.top;
  }
  if (delta) {
    ListView_Scroll(list, horizontal ? delta : 0, horizontal ? 0 : delta);
    RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
  }
  queueScrollSync();
}

void Panel::contextMenu(POINT point, bool title) {
  if (desktop.dragging || desktop.quitting || desktop.menuOpen || desktop.editing) return;
  const auto menu = std::make_shared<Branch>();
  menu->compact = true;
  const bool keyboard = point.x == -1 && point.y == -1;
  if (!keyboard) {
    auto client = point;
    if (ScreenToClient(window, &client)) title = client.y >= 0 && client.y < headerHeight();
  }
  std::optional<size_t> selectedTab;
  if (title && !keyboard) {
#ifdef FOLDERNOTE_WINUI
    if (hasSurface()) selectedTab = surface->tabAt(point);
    else
#endif
    if (IsWindowVisible(tabs)) {
      auto local = point;
      if (ScreenToClient(tabs, &local))
        for (size_t i = 0; i < tabAreas.size(); ++i)
          if (PtInRect(&tabAreas[i], local)) { selectedTab = i; break; }
    }
  }
  int row = -1;
  if (!title) {
    LVHITTESTINFO hit{};
    hit.pt = point;
    ScreenToClient(list, &hit.pt);
    if (IsWindowVisible(list)) row = keyboard ? selection() : ListView_HitTest(list, &hit);
#ifdef FOLDERNOTE_WINUI
    if (hasSurface()) row = keyboard ? selection() : surface->hit(point);
#endif
  }
  if (row >= 0 && static_cast<size_t>(row) < displayed.size()) {
    if (!keyboard) {
      ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
      ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    POINT itemPoint = point;
    if (keyboard) {
      RECT bounds{};
      ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS);
      itemPoint = {bounds.left, bounds.bottom};
      ClientToScreen(list, &itemPoint);
    }
    const auto path = displayed[row];
    bool remove = false;
    desktop.menuOpen = true;
    bool shown = false;
    try { shown = showItemMenu(window, path, itemPoint, index && !mapped(), remove); }
    catch (...) { desktop.menuOpen = false; throw; }
    desktop.menuOpen = false;
    if (desktop.quitting) return;
    if (remove) PostMessageW(window, WM_COMMAND, RemoveItem, 0);
    if (shown) return;
    menuItem(*menu, OpenItem, L"打开", Symbol::Folder);
    if (index && !mapped()) menuItem(*menu, RemoveItem, L"移出盒子", Symbol::Unpin);
    separator(*menu);
  }
  const auto &group = desktop.groups[index];
  if (title) {
    quick::popup::Item spacing;
    spacing.title = lang::text(L"项目间距");
    spacing.slider = quick::popup::Slider{0, maxItemSpacing, group.itemSpacing,
      [this](int value) {
        spacingPreview = value;
        resize();
      },
      [this](int value) { return saveSpacing(value); }, false, false};
    menu->items.push_back(std::move(spacing));
    separator(*menu);
    if (!desktop.theme.colors.highContrast) {
      const auto initial = group.tint;
      const auto hue = HueColor::fromRgb(backgroundColor(), desktop.theme.colors.dark);
      quick::popup::Item item;
      item.title = lang::text(L"盒子色相");
      item.slider = quick::popup::Slider{0, 359, hue.hue,
        [this, hue, initial](int value) {
          colorPreview = value == hue.hue && initial ? *initial : hue.at(value);
          applyBackground();
        },
        [this, hue, initial](int value) {
          const auto tint = value == hue.hue && initial ? *initial : hue.at(value);
          const bool saved = saveColor(tint);
          colorPreview.reset();
          applyBackground();
          return saved;
        }, true};
      menu->items.push_back(std::move(item));
      menuItem(*menu, BoxColor, L"盒子颜色", Symbol::Settings);
      if (group.tint) menuItem(*menu, ResetBoxColor, L"恢复主题颜色", Symbol::Settings);
      separator(*menu);
    }
    if (hasSurface() && !desktop.theme.colors.highContrast) {
      quick::popup::Item item;
      item.title = lang::text(L"背景不透明度");
      item.slider = quick::popup::Slider{0, 100, backgroundOpacity(),
        [this](int value) {
          opacityPreview = value;
          applyBackground();
        },
        [this](int value) {
          if (desktop.quitting) return false;
          bool saved = false;
          if (index) {
            auto changed = desktop.groups;
            changed[index].opacity = value;
            saved = desktop.commit(std::move(changed), window, nullptr, false);
          } else {
            const DWORD opacity = value;
            const auto result = RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedOpacity,
                                                REG_DWORD, &opacity, sizeof(opacity));
            if (result == ERROR_SUCCESS) {
              desktop.groups.front().opacity = value;
              saved = true;
            } else {
              report(window, lang::text(L"无法保存桌面盒子"));
            }
          }
          opacityPreview.reset();
          applyBackground();
          return saved;
        }};
      menu->items.push_back(std::move(item));
      separator(*menu);
    }
    menuItem(*menu, NewGroup, L"新建盒子", Symbol::AddFolder);
    menuItem(*menu, BoxIcon, L"选择盒子图标", Symbol::Folder);
    if (!group.titleIcon.empty()) menuItem(*menu, ClearBoxIcon, L"清除自定义图标", Symbol::Folder);
    menuItem(*menu, TitleIconAndText, L"图标和文字", Symbol::Window, group.titleStyle == TitleStyle::IconAndText);
    menuItem(*menu, TitleIcon, L"仅图标", Symbol::Window, group.titleStyle == TitleStyle::Icon);
    menuItem(*menu, TitleText, L"仅文字", Symbol::Window, group.titleStyle == TitleStyle::Text);
    menuItem(*menu, ToggleGroup, group.collapsed ? L"展开盒子" : L"折叠盒子", Symbol::Window);
    if (index) {
      menuItem(*menu, AddTab, L"添加标签", Symbol::AddFolder);
      menuItem(*menu, MapFolder, L"映射文件夹", Symbol::Folder);
      const auto targetTab = selectedTab.value_or(activeTab);
      if (targetTab) menuItem(*menu, RemoveTab, L"移除标签", Symbol::Unpin);
      if (selectedTab && *selectedTab > 1) menuItem(*menu, MoveTabLeft, L"向左移动", Symbol::Move);
      if (selectedTab && *selectedTab > 0 && *selectedTab < group.tabs.size())
        menuItem(*menu, MoveTabRight, L"向右移动", Symbol::Move);
      separator(*menu);
      menuItem(*menu, RenameGroup, L"重命名盒子", Symbol::Settings);
      menuItem(*menu, DeleteGroup, L"删除盒子", Symbol::Trash);
    }
  } else {
    menuItem(*menu, IconView, L"图标", Symbol::Folder, group.view == View::Icons);
    menuItem(*menu, ListViewMode, L"列表", Symbol::Window, group.view == View::List);
    separator(*menu);
    menuItem(*menu, ScrollDisplay, L"滚动显示", Symbol::Window, group.displayMode == DisplayMode::Scroll);
    menuItem(*menu, PageDisplay, L"分页显示", Symbol::Documents, group.displayMode == DisplayMode::Pages);
    separator(*menu);
    menuItem(*menu, ManualSort, L"手动排序", Symbol::Move, group.sort == Sort::Manual);
    menuItem(*menu, NameSort, L"按名称", Symbol::Folder, group.sort == Sort::Name);
    menuItem(*menu, TypeSort, L"按类型", Symbol::Folder, group.sort == Sort::Type);
    menuItem(*menu, ModifiedSort, L"按修改时间", Symbol::History, group.sort == Sort::Modified);
  }
  if (keyboard) {
    point = {12, 12};
    RECT item{};
    if (!title && selection() >= 0 && ListView_GetItemRect(list, selection(), &item, LVIR_BOUNDS))
      point = {item.left, item.bottom};
    ClientToScreen(title ? window : list, &point);
  }
  desktop.menuOpen = true;
  const auto clearPreview = [&] {
    desktop.menuOpen = false;
    if (std::exchange(spacingPreview, std::nullopt)) resize();
    if (opacityPreview || colorPreview) {
      opacityPreview.reset();
      colorPreview.reset();
      applyBackground();
    }
  };
  unsigned command = 0;
  try { command = quick::popup::show(desktop.instance, point, menu, desktop.theme.mode, GetForegroundWindow()); }
  catch (...) {
    clearPreview();
    throw;
  }
  clearPreview();
  if (command) {
    const auto tab = selectedTab.value_or(activeTab);
    const auto tabCommand = command == RemoveTab || command == MoveTabLeft || command == MoveTabRight;
    PostMessageW(window, WM_COMMAND, command, tabCommand ? static_cast<LPARAM>(tab) : 0);
  }
}

LRESULT CALLBACK Panel::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto panel = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    panel = static_cast<Panel *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    panel->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
  }
  if (!panel) return DefWindowProcW(window, message, w, l);
  auto &desktop = panel->desktop;
  try {
    switch (message) {
    case WM_CREATE: {
      panel->list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_ICON |
                                       LVS_AUTOARRANGE | LVS_SHAREIMAGELISTS | LVS_SHOWSELALWAYS,
                                     0, 0, 0, 0, window, nullptr, desktop.instance, nullptr);
      panel->toggle = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 0, 0, window, reinterpret_cast<HMENU>(ToggleGroup), desktop.instance, nullptr);
      for (int i = 0; i < 2; ++i) {
        const auto command = i ? ListViewMode : IconView;
        panel->viewButtons[i] = CreateWindowExW(0, L"BUTTON", lang::text(i ? L"列表" : L"图标"),
            WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(command)), desktop.instance, nullptr);
        if (!panel->viewButtons[i] || !SetWindowSubclass(panel->viewButtons[i], toggleInput, 1, 0)) return -1;
      }
      panel->tabs = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_TABSTOP | SS_NOTIFY | SS_OWNERDRAW,
                                    0, 0, 0, 0, window, nullptr, desktop.instance, nullptr);
      if (!panel->list || !panel->toggle || !panel->tabs ||
          !SetWindowSubclass(panel->tabs, tabInput, 1, reinterpret_cast<DWORD_PTR>(panel)) ||
          !SetWindowSubclass(panel->list, iconInput, 1, reinterpret_cast<DWORD_PTR>(panel)) ||
          !SetWindowSubclass(panel->toggle, toggleInput, 1, 0)) return -1;
      panel->font = ui::makeFont(GetDpiForWindow(window));
      panel->titleFont = ui::makeFont(GetDpiForWindow(window), 14, FW_SEMIBOLD);
      SendMessageW(panel->list, WM_SETFONT, reinterpret_cast<WPARAM>(panel->font), FALSE);
      SendMessageW(panel->toggle, WM_SETFONT, reinterpret_cast<WPARAM>(panel->titleFont), FALSE);
      ListView_SetExtendedListViewStyle(panel->list, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
      SetWindowLongPtrW(panel->list, GWL_STYLE, GetWindowLongPtrW(panel->list, GWL_STYLE) | LVS_NOCOLUMNHEADER);
      LVCOLUMNW column{};
      column.mask = LVCF_WIDTH;
      column.cx = 120;
      ListView_InsertColumn(panel->list, 0, &column);
      panel->verticalScroll.create(window, 0, desktop.theme, [panel](int position) { panel->scrollTo(position, false); }, true);
      panel->horizontalScroll.create(window, 0, desktop.theme, [panel](int position) { panel->scrollTo(position, true); }, true, true);
      const auto paintTrack = [panel](HWND child, HDC dc) {
        const auto saved = SaveDC(dc);
        if (!saved) return;
        POINT offset{};
        MapWindowPoints(child, panel->window, &offset, 1);
        OffsetViewportOrgEx(dc, -offset.x, -offset.y, nullptr);
        panel->paint(dc, true);
        RestoreDC(dc, saved);
      };
      panel->verticalScroll.setBackground(paintTrack);
      panel->horizontalScroll.setBackground(paintTrack);
      panel->scrollCorner = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_OWNERDRAW,
                                            0, 0, 0, 0, window, nullptr, desktop.instance, nullptr);
      if (!panel->scrollCorner) return -1;
      for (const auto bar : {panel->verticalScroll.window(), panel->horizontalScroll.window()})
        SetWindowPos(bar, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
      desktop.theme.apply(window);
      auto dropTarget = new DropTarget(*panel);
      const auto registered = RegisterDragDrop(window, dropTarget);
      dropTarget->Release();
      if (FAILED(registered)) return -1;
      return 0;
    }
    case WM_NCCALCSIZE:
      return 0;
    case WM_NCPAINT:
      return 0;
    case WM_NCACTIVATE:
      return DefWindowProcW(window, message, w, -1);
    case WM_NCHITTEST: {
      if (panel->dockHidden) return HTCLIENT;
      RECT bounds{};
      GetWindowRect(window, &bounds);
      const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      const auto dpi = GetDpiForWindow(window);
      const int border = MulDiv(8, dpi, 96);
      if (!desktop.groups[panel->index].collapsed) {
        const bool left = point.x < bounds.left + border, right = point.x >= bounds.right - border;
        const int corner = std::max(2 * border, panel->cornerDiameter() / 2);
        const bool nearLeft = point.x < bounds.left + corner, nearRight = point.x >= bounds.right - corner;
        const int grip = MulDiv(20, dpi, 96);
        if (point.x >= bounds.right - grip && point.y >= bounds.bottom - grip) return HTBOTTOMRIGHT;
        if (point.y < bounds.top + border) return nearLeft ? HTTOPLEFT : nearRight ? HTTOPRIGHT : HTTOP;
        if (point.y >= bounds.bottom - border) return nearLeft ? HTBOTTOMLEFT : nearRight ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return point.y < bounds.top + corner ? HTTOPLEFT :
                         point.y >= bounds.bottom - corner ? HTBOTTOMLEFT : HTLEFT;
        if (right) return point.y < bounds.top + corner ? HTTOPRIGHT :
                          point.y >= bounds.bottom - corner ? HTBOTTOMRIGHT : HTRIGHT;
      }
      const int header = panel->headerHeight();
      const bool collapsed = desktop.groups[panel->index].collapsed;
      if (panel->hasSurface() && point.y >= bounds.top + header - MulDiv(3, dpi, 96) &&
          point.y < bounds.top + header + (collapsed ? 0 : MulDiv(4, dpi, 96))) return resizeTitle;
#ifdef FOLDERNOTE_WINUI
      if (panel->hasSurface() && panel->surface->headerInteractive(point)) return HTCLIENT;
#endif
      RECT button{};
      GetWindowRect(panel->toggle, &button);
      if (IsWindowVisible(panel->toggle) && PtInRect(&button, point)) return HTCLIENT;
      if (IsWindowVisible(panel->tabs) && GetWindowRect(panel->tabs, &button) && PtInRect(&button, point))
        return HTCLIENT;
      for (const auto viewButton : panel->viewButtons) {
        if (viewButton && IsWindowVisible(viewButton) && GetWindowRect(viewButton, &button) && PtInRect(&button, point))
          return HTCLIENT;
      }
      return point.y < bounds.top + panel->headerHeight() ? HTCAPTION : HTCLIENT;
    }
    case WM_SETCURSOR: {
      if (panel->dockHidden) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
      }
      LPCWSTR cursor = nullptr;
      switch (LOWORD(l)) {
      case HTLEFT: case HTRIGHT: cursor = IDC_SIZEWE; break;
      case HTTOP: case HTBOTTOM: case resizeTitle: cursor = IDC_SIZENS; break;
      case HTTOPLEFT: case HTBOTTOMRIGHT: cursor = IDC_SIZENWSE; break;
      case HTTOPRIGHT: case HTBOTTOMLEFT: cursor = IDC_SIZENESW; break;
      }
      if (cursor) {
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
      }
      break;
    }
    case WM_NCLBUTTONDBLCLK:
      if (w == HTCAPTION) PostMessageW(window, WM_COMMAND, ToggleGroup, 0);
      return 0;
    case WM_NCLBUTTONDOWN:
      if (w == HTCAPTION || w == resizeTitle || (w >= HTLEFT && w <= HTBOTTOMRIGHT)) {
        if (desktop.dragging || desktop.editing || desktop.menuOpen || panel->gestureEdge) return 0;
        if (!GetWindowRect(window, &panel->gestureBounds)) return 0;
        panel->gestureStart = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        panel->gestureEdge = w;
        panel->gestureTarget = panel->gestureBounds;
        desktop.lower(window);
        SetCapture(window);
        SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
        bool paced = false;
#ifdef FOLDERNOTE_WINUI
        if (panel->hasSurface()) {
          panel->surface->animateReflow(w != HTCAPTION);
          paced = panel->surface->onFrame([panel] {
            panel->lastCompositionFrame = GetTickCount64();
            panel->updateGestureFrame();
          });
        }
#endif
        if (paced) panel->lastCompositionFrame = GetTickCount64();
        // A no-activate box may not receive mouse messages outside its bounds.
        // Poll while dragging, even if the composition clock pauses.
        if (!SetTimer(window, boxMoveTimer, 16, nullptr) && !paced) panel->finishGesture(false);
        return 0;
      }
      break;
    case WM_TIMER:
      if (w == boxMoveTimer) {
        if (GetTickCount64() - panel->lastCompositionFrame >= 32) panel->updateGestureFrame();
        return 0;
      }
      break;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PRINTCLIENT:
      panel->paint(reinterpret_cast<HDC>(w), true);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      auto dc = BeginPaint(window, &paint);
      if (!panel->dockHidden) panel->paint(dc, false);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DRAWITEM: {
      const auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item.hwndItem == panel->scrollCorner) {
        const auto saved = SaveDC(item.hDC);
        if (saved) {
          POINT offset{};
          MapWindowPoints(panel->scrollCorner, window, &offset, 1);
          OffsetViewportOrgEx(item.hDC, -offset.x, -offset.y, nullptr);
          panel->paint(item.hDC, true);
          RestoreDC(item.hDC, saved);
        }
        return TRUE;
      }
      if (item.hwndItem == panel->tabs) {
        panel->paintTabs(item);
        return TRUE;
      }
      const bool grid = item.hwndItem == panel->viewButtons[0];
      const bool rows = item.hwndItem == panel->viewButtons[1];
      if (item.hwndItem != panel->toggle && !grid && !rows) break;
      const auto &colors = desktop.theme.colors;
      const auto box = item.rcItem;
      POINT offset{};
      MapWindowPoints(item.hwndItem, window, &offset, 1);
      panel->paintBackground(item.hDC, {-offset.x, -offset.y});
      POINT cursor{};
      GetCursorPos(&cursor);
      ScreenToClient(item.hwndItem, &cursor);
      const bool hot = PtInRect(&box, cursor);
      const bool focus = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
      const bool pressed = (item.itemState & ODS_SELECTED) != 0;
      const auto dpi = GetDpiForWindow(window);
      const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
      const auto glyphColor = hot || focus || pressed ? colors.text : colors.muted;
      if (focus) {
        auto frame = box;
        InflateRect(&frame, -px(2), -px(1));
        DrawFocusRect(item.hDC, &frame);
      }
      const auto centerX = (box.left + box.right) / 2;
      const auto centerY = (box.top + box.bottom) / 2;
      if (grid || rows) {
        const auto oldColor = SetDCBrushColor(item.hDC, glyphColor);
        const auto brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        const auto iconPixel = [dpi](int value) { return MulDiv(value - 12, 16 * dpi, 24 * 96); };
        if (grid) {
          for (const int y : icons::gridStarts) for (const int x : icons::gridStarts) {
            const RECT square{centerX + iconPixel(x), centerY + iconPixel(y),
                              centerX + iconPixel(x + 7), centerY + iconPixel(y + 7)};
            FrameRect(item.hDC, &square, brush);
          }
        } else {
          for (const int y : icons::listRows) {
            const LONG top = centerY + iconPixel(y);
            const RECT dot{centerX + iconPixel(3), top, centerX + iconPixel(3) + px(1), top + px(1)};
            const RECT line{centerX + iconPixel(8), top, centerX + iconPixel(21), top + px(1)};
            FillRect(item.hDC, &dot, brush);
            FillRect(item.hDC, &line, brush);
          }
        }
        SetDCBrushColor(item.hDC, oldColor);
        return TRUE;
      }
      const int halfWidth = px(4), halfHeight = px(2);
      const int direction = desktop.groups[panel->index].collapsed ? 1 : -1;
      POINT arrow[]{{centerX - halfWidth, centerY - direction * halfHeight},
                    {centerX, centerY + direction * halfHeight},
                    {centerX + halfWidth, centerY - direction * halfHeight}};
      if (auto pen = CreatePen(PS_SOLID, std::max(1, px(1)), glyphColor)) {
        auto previous = SelectObject(item.hDC, pen);
        Polyline(item.hDC, arrow, ARRAYSIZE(arrow));
        SelectObject(item.hDC, previous);
        DeleteObject(pen);
      }
      return TRUE;
    }
    case syncScrollMessage:
      panel->scrollSyncPending = false;
      panel->syncScroll();
      return 0;
    case updatePagesMessage:
      panel->pageLayoutPending = false;
      if (!desktop.groups[panel->index].collapsed) panel->renderPage(desktop.groups[panel->index]);
      return 0;
    case goToPageMessage:
      panel->goToPage(static_cast<int>(l) + (w ? panel->currentPage : 0));
      return 0;
    case WM_MOUSEWHEEL:
      if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) panel->zoom(GET_WHEEL_DELTA_WPARAM(w));
      else if (!panel->pageWheel(GET_WHEEL_DELTA_WPARAM(w)))
        panel->verticalScroll.wheel(GET_WHEEL_DELTA_WPARAM(w));
      return 0;
    case WM_ENTERSIZEMOVE:
      panel->movingOrSizing = true;
      return 0;
    case WM_SIZE:
      if (w != SIZE_MINIMIZED) panel->updateShape();
      panel->resize();
      return 0;
    case WM_GETMINMAXINFO: {
      auto &info = *reinterpret_cast<MINMAXINFO *>(l);
      const auto dpi = GetDpiForWindow(window);
      info.ptMinTrackSize = {MulDiv(190, dpi, 96), desktop.groups[panel->index].collapsed
                             ? panel->headerHeight()
                             : std::max(MulDiv(112, dpi, 96), panel->headerHeight() + MulDiv(44, dpi, 96))};
      return 0;
    }
    case WM_DPICHANGED: {
      auto font = ui::makeFont(HIWORD(w));
      auto titleFont = ui::makeFont(HIWORD(w), 14, FW_SEMIBOLD);
      SendMessageW(panel->list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      if (panel->font) DeleteObject(panel->font);
      panel->font = font;
      SendMessageW(panel->toggle, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
      if (panel->titleFont) DeleteObject(panel->titleFont);
      panel->titleFont = titleFont;
      auto rect = reinterpret_cast<RECT *>(l);
      panel->place(*rect);
      if (desktop.groups[panel->index].collapsed) panel->setCollapsed();
      panel->updateShape();
      panel->refresh(desktop.groups[panel->index]);
      return 0;
    }
    case WM_EXITSIZEMOVE: {
      panel->movingOrSizing = false;
      if (!panel->hasSurface()) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
      if (!panel->windowPositionReady) return 0;
      if (desktop.quitting) return 0;
      auto changed = desktop.groups;
      auto &group = changed[panel->index];
      RECT bounds{};
      GetWindowRect(window, &bounds);
      if (group.collapsed) bounds.bottom = bounds.top + (group.bounds.bottom - group.bounds.top);
      group.bounds = bounds;
      if (!desktop.commit(std::move(changed), window, nullptr, false)) {
        const auto saved = visibleBounds(desktop.groups[panel->index].bounds);
        panel->place(saved);
        panel->setCollapsed();
      } else if (w) {
        GetWindowRect(window, &bounds);
        const POINT movement{bounds.left - panel->gestureBounds.left, bounds.top - panel->gestureBounds.top};
        panel->setDockSide(touchingWorkEdge(bounds, panel->dockSide, movement));
      }
      return 0;
    }
    case WM_SETFOCUS:
      SetFocus(desktop.groups[panel->index].collapsed ? panel->toggle : panel->list);
      return 0;
    case WM_CONTEXTMENU:
      panel->contextMenu({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
      return 0;
    case WM_NCRBUTTONUP:
      panel->contextMenu({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
      return 0;
    case WM_NOTIFY: {
      const auto notification = reinterpret_cast<NMHDR *>(l);
      if (notification->hwndFrom == panel->list && notification->code == NM_CUSTOMDRAW) {
        const auto &draw = *reinterpret_cast<NMCUSTOMDRAW *>(l);
        const bool mark = desktop.dropHighlight == window && !IsRectEmpty(&desktop.dropMarker);
        if (draw.dwDrawStage == CDDS_PREPAINT) {
          // The item renderer blends onto the destination. Reset the entire
          // dirty background before drawing any items at their new positions.
          const int saved = SaveDC(draw.hdc);
          if (saved) {
            RECT area{};
            GetClientRect(panel->list, &area);
            IntersectClipRect(draw.hdc, area.left, area.top, area.right, area.bottom);
            POINT offset{};
            MapWindowPoints(panel->list, window, &offset, 1);
            OffsetViewportOrgEx(draw.hdc, -offset.x, -offset.y, nullptr);
            panel->paint(draw.hdc, true);
            RestoreDC(draw.hdc, saved);
          }
          return CDRF_NOTIFYITEMDRAW | (mark ? CDRF_NOTIFYPOSTPAINT : 0);
        }
        if (draw.dwDrawStage == CDDS_ITEMPREPAINT && !desktop.theme.colors.highContrast)
          return panel->paintItem(draw.hdc, static_cast<int>(draw.dwItemSpec)) ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
        if (draw.dwDrawStage == CDDS_POSTPAINT && mark) {
          const auto previous = SetDCBrushColor(draw.hdc, desktop.theme.colors.accent);
          FillRect(draw.hdc, &desktop.dropMarker, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
          SetDCBrushColor(draw.hdc, previous);
          return CDRF_DODEFAULT;
        }
      }
      if (notification->hwndFrom == panel->list && notification->code == LVN_ENDSCROLL) {
        panel->queueScrollSync();
        return 0;
      }
      if (notification->code == LVN_GETEMPTYMARKUP) {
        auto markup = reinterpret_cast<NMLVEMPTYMARKUP *>(l);
        markup->dwFlags = EMF_CENTERED;
        wcsncpy_s(markup->szMarkup, lang::text(panel->mapped() ? L"映射文件夹为空" :
                   panel->index ? L"将桌面文件拖到这里" : L"暂无未分组项目"), _TRUNCATE);
        return TRUE;
      }
      if (notification->code == NM_DBLCLK && reinterpret_cast<NMITEMACTIVATE *>(l)->iItem >= 0)
        PostMessageW(window, WM_COMMAND, OpenItem, 0);
      if (notification->code == LVN_BEGINDRAG)
        PostMessageW(window, dragMessage, reinterpret_cast<NMLISTVIEW *>(l)->iItem, 0);
      if (notification->code == LVN_KEYDOWN) {
        const auto key = reinterpret_cast<NMLVKEYDOWN *>(l)->wVKey;
        if (key == VK_RETURN || key == VK_DELETE)
          PostMessageW(window, WM_COMMAND, key == VK_RETURN ? OpenItem : RemoveItem, 0);
      }
      if (notification->code == LVN_GETINFOTIPW) {
        auto tip = reinterpret_cast<NMLVGETINFOTIPW *>(l);
        const auto &items = panel->displayed;
        if (tip->iItem >= 0 && static_cast<size_t>(tip->iItem) < items.size())
          lstrcpynW(tip->pszText, items[tip->iItem].c_str(), tip->cchTextMax);
      }
      break;
    }
    case WM_VSCROLL:
    case WM_HSCROLL: {
      auto &bar = message == WM_HSCROLL ? panel->horizontalScroll : panel->verticalScroll;
      if (l && reinterpret_cast<HWND>(l) == bar.window()) {
        bar.command(w);
        return 0;
      }
      break;
    }
    case WM_COMMAND:
      if (const auto command = static_cast<Command>(LOWORD(w));
          command == RemoveTab || command == MoveTabLeft || command == MoveTabRight)
        desktop.action(panel, command, static_cast<size_t>(l));
      else
        desktop.action(panel, command);
      return 0;
    case dragMessage:
      if (GetAsyncKeyState(VK_LBUTTON) < 0) desktop.drag(*panel, static_cast<int>(w));
      return 0;
    case detachBoxTabMessage:
      if (w && w <= desktop.groups[panel->index].tabs.size())
        desktop.detachPanelTab(panel->index, w, {GET_X_LPARAM(l), GET_Y_LPARAM(l)});
      return 0;
    case finishBoxGestureMessage:
      if (panel->gestureEdge) panel->finishGesture(w != FALSE);
      return 0;
    case WM_LBUTTONUP:
      if (panel->gestureEdge) {
        panel->finishGesture(true);
        return 0;
      }
      if (desktop.dragging) {
        desktop.finishDrag(window, true);
        return 0;
      }
      if (!panel->hasSurface() && panel->pageCount && !desktop.groups[panel->index].collapsed) {
        const auto footer = panel->pageFooter();
        const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&footer, point)) {
          const int step = std::max(1, MulDiv(16, GetDpiForWindow(window), 96));
          const int count = panel->visiblePageDots();
          const int left = (footer.left + footer.right - count * step) / 2;
          if (point.x >= left && point.x < left + count * step)
            panel->goToPage(panel->firstPageDot() + (point.x - left) / step);
          return 0;
        }
      }
      break;
    case WM_CAPTURECHANGED:
      // WinUI can transfer native capture while an edge press is being routed.
      // The frame clock owns resize tracking until release or cancellation.
      if (panel->gestureEdge && panel->gestureEdge != HTCAPTION) return 0;
      [[fallthrough]];
    case WM_CANCELMODE:
      if (panel->gestureEdge) {
        panel->finishGesture(false);
        return 0;
      }
      desktop.finishDrag(window, false);
      break;
    case rejectedMessage:
      report(window, lang::text(L"只能添加个人或公共桌面上的文件和文件夹。"));
      return 0;
    case WM_CLOSE:
      return 0;
    case WM_DESTROY:
      RevokeDragDrop(window);
      break;
    case WM_NCDESTROY:
      panel->window = nullptr;
      break;
    }
  } catch (...) {
    if (message == WM_CREATE) return -1;
    report(window, lang::text(L"无法打开桌面盒子。"));
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}

#ifdef FOLDERNOTE_WINUI
void Panel::openSurface() {
  winui::Events events;
  events.selection = [this](std::vector<int> rows) {
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
    for (const auto row : rows) ListView_SetItemState(list, row, LVIS_SELECTED, LVIS_SELECTED);
  };
  events.open = [this](int row) {
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(list, row, LVIS_SELECTED, LVIS_SELECTED);
    PostMessageW(window, WM_COMMAND, OpenItem, 0);
  };
  events.context = [this](POINT point) { contextMenu(point); };
  events.zoom = [this](int delta) { zoom(delta); };
  events.wheel = [this](int delta) { return pageWheel(delta); };
  events.beginDrag = [this](int row) { PostMessageW(window, dragMessage, row, 0); };
  events.tab = [this](size_t tab) { showTab(tab); };
  events.tabDropped = [this](size_t tab, POINT start, POINT end) {
    const auto dpi = GetDpiForWindow(window);
    if ((std::abs(end.x - start.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
         std::abs(end.y - start.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi)) &&
        !desktop.panelAt(end) && desktop.desktopDropAt(end))
      PostMessageW(window, detachBoxTabMessage, tab, MAKELPARAM(end.x, end.y));
    else
      showTab(tab);
  };
  events.headerCommand = [this](int button) {
    constexpr Command commands[]{IconView, ListViewMode, ToggleGroup};
    if (button >= 0 && button < static_cast<int>(std::size(commands)))
      PostMessageW(window, WM_COMMAND, commands[button], 0);
  };
  events.headerDrag = [this](POINT point) {
    const auto position = MAKELPARAM(point.x, point.y);
    if (SendMessageW(window, WM_NCHITTEST, 0, position) == HTCAPTION)
      SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, position);
  };
  surface = std::make_unique<winui::Surface>(window, std::move(events));
  if (hasSurface()) {
    // Let the island compose directly over the desktop when it has no backdrop.
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_NOREDIRECTIONBITMAP);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    for (auto control : {list, toggle, tabs, viewButtons[0], viewButtons[1]}) ShowWindow(control, SW_HIDE);
    surface->show(true);
  }
}
#endif

bool Desktop::openPanel(size_t index) {
  auto panel = std::make_unique<Panel>(*this, index);
  const auto bounds = visibleBounds(groups[index].bounds);
  auto handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, panelClass, groups[index].name.c_str(),
                                WS_POPUP | WS_CLIPCHILDREN,
                                bounds.left, bounds.top,
                                bounds.right - bounds.left, bounds.bottom - bounds.top,
                                nullptr, nullptr, instance, panel.get());
  if (!handle) return false;
  constexpr COLORREF noBorder = 0xfffffffe;
  DwmSetWindowAttribute(handle, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
#ifdef FOLDERNOTE_WINUI
  if (useWinui) panel->openSurface();
#endif
  // The client paints the only outline in both renderers.
  const DWMNCRENDERINGPOLICY frame = DWMNCRP_DISABLED;
  DwmSetWindowAttribute(handle, DWMWA_NCRENDERING_POLICY, &frame, sizeof(frame));
  panel->updateShape();
  panel->applyTheme(theme.colors);
  panel->resize();
  panel->refresh(groups[index]);
  panel->setCollapsed();
  panel->windowPositionReady = true;
  panels.push_back(std::move(panel));
  updateScene();
  lower(handle);
  return true;
}

void Panel::updateShadow() {
  const auto &style = desktop.style;
  if (dockHidden || !style.shadow || !style.shadowOpacity || desktop.theme.colors.highContrast) {
    if (shadow) DestroyWindow(std::exchange(shadow, nullptr));
    shadowSize = {};
    return;
  }
  RECT box{};
  if (!GetWindowRect(window, &box)) return;
  const UINT dpi = GetDpiForWindow(window);
  const int spread = std::max(1, MulDiv(static_cast<int>(style.shadowBlur), dpi, 96));
  const int offset = MulDiv(static_cast<int>(style.shadowOffset), dpi, 96);
  const int width = box.right - box.left + 2 * spread, height = box.bottom - box.top + 2 * spread;
  const int diameter = cornerDiameter();
  if (!shadow) {
    shadow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"STATIC", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, desktop.instance, nullptr);
    if (!shadow) return;
  }
  POINT origin{box.left - spread, box.top - spread + offset};
  if (shadowSize.cx == width && shadowSize.cy == height && shadowDiameter == diameter && shadowDpi == dpi &&
      renderedShadowOpacity == style.shadowOpacity && renderedShadowOffset == offset && renderedShadowSpread == spread) {
    SetWindowPos(shadow, nullptr, origin.x, origin.y, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    return;
  }
  SIZE size{width, height};
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
  void *data = nullptr;
  const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0);
  const auto dc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
  const auto old = dc ? SelectObject(dc, bitmap) : nullptr;
  const auto alpha = static_cast<BYTE>(MulDiv(static_cast<int>(style.shadowOpacity), 255, 100));
  if (old && old != HGDI_ERROR && drawBoxShadow(static_cast<DWORD *>(data), width, height, spread, offset, diameter, alpha)) {
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (UpdateLayeredWindow(shadow, nullptr, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA)) {
      shadowSize = size;
      shadowDiameter = diameter;
      shadowDpi = dpi;
      renderedShadowOpacity = style.shadowOpacity;
      renderedShadowOffset = offset;
      renderedShadowSpread = spread;
    }
  }
  if (dc) DeleteDC(dc);
  if (bitmap) DeleteObject(bitmap);
}

void Desktop::updateScene() {
  for (const auto &panel : panels) panel->updateShadow();
  auto region = CreateRectRgn(0, 0, 0, 0);
  if (!region) return;
  for (const auto child : looseWindows) {
    RECT bounds{};
    if (!GetWindowRect(child, &bounds)) continue;
    OffsetRect(&bounds, -sceneBounds.left, -sceneBounds.top);
    if (auto shape = CreateRectRgnIndirect(&bounds)) {
      CombineRgn(region, region, shape, RGN_OR);
      DeleteObject(shape);
    }
  }
  if (!SetWindowRgn(scene, region, TRUE)) DeleteObject(region);
}

void Desktop::highlight(HWND target) {
  if (dropHighlight == target) return;
  const auto previous = std::exchange(dropHighlight, target);
  for (const auto &panel : panels) {
    if (panel->window != previous && panel->window != target) continue;
#ifdef FOLDERNOTE_WINUI
    if (panel->hasSurface()) {
      panel->surface->highlight(panel->window == target);
      continue;
    }
#endif
    InvalidateRect(panel->window, nullptr, FALSE);
  }
}

Panel *Desktop::mergeTargetAt(POINT point, const Panel &source) const {
  if (!source.index) return nullptr;
  const auto hit = WindowFromPoint(point);
  if (!hit || (!panelAt(point) && !desktopDropAt(point))) return nullptr;
  for (auto child = GetTopWindow(nullptr); child; child = GetWindow(child, GW_HWNDNEXT)) {
    if (child == source.window || !IsWindowVisible(child)) continue;
    const auto panel = std::find_if(panels.begin(), panels.end(), [child](const auto &candidate) {
      return candidate->window == child;
    });
    if (panel == panels.end()) continue;
    RECT bounds{};
    if (!GetWindowRect(child, &bounds) || !PtInRect(&bounds, point)) continue;
    const auto region = CreateRectRgn(0, 0, 0, 0);
    const bool inside = !region || GetWindowRgn(child, region) == ERROR ||
                        PtInRegion(region, point.x - bounds.left, point.y - bounds.top);
    if (region) DeleteObject(region);
    if (inside) return (*panel)->index ? panel->get() : nullptr;
  }
  return nullptr;
}

Panel *Desktop::panelAt(POINT point) const {
  const auto root = GetAncestor(WindowFromPoint(point), GA_ROOT);
  const auto found = std::find_if(panels.begin(), panels.end(), [root](const auto &panel) { return panel->window == root; });
  return found == panels.end() ? nullptr : found->get();
}

bool Desktop::desktopDropAt(POINT point) const {
  const auto hit = WindowFromPoint(point);
  if (hit == scene || std::find(looseWindows.begin(), looseWindows.end(), hit) != looseWindows.end()) return true;
  const auto root = GetAncestor(hit, GA_ROOT);
  if (quick::isDesktop(root)) return true;

  // Show Desktop may expose a wallpaper-only WorkerW owned by Explorer.
  DWORD shellPid = 0, hitPid = 0;
  GetWindowThreadProcessId(GetShellWindow(), &shellPid);
  GetWindowThreadProcessId(root, &hitPid);
  wchar_t name[32]{};
  return hit == root && shellPid && hitPid == shellPid &&
         GetClassNameW(root, name, ARRAYSIZE(name)) && wcscmp(name, L"WorkerW") == 0;
}

void Desktop::tray(DWORD action) const {
  NOTIFYICONDATAW icon{sizeof(icon)};
  icon.hWnd = window;
  icon.uID = 1;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = trayMessage;
  icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_FOLDERNOTE));
  wcsncpy_s(icon.szTip, lang::text(L"桌面盒子"), _TRUNCATE);
  Shell_NotifyIconW(action, &icon);
}

bool Desktop::commit(std::vector<Group> changed, HWND owner, const std::vector<LooseItem> *placements,
                     bool refreshContents) {
  try {
    auto next = placements ? *placements : looseItems;
    std::erase_if(next, [&](const auto &item) {
      const auto contains = [&](const auto &paths) {
        return std::any_of(paths.begin(), paths.end(), [&](const auto &path) { return samePath(path, item.path); });
      };
      return std::any_of(changed.begin() + 1, changed.end(), [&](const auto &group) {
        return contains(group.paths) || std::any_of(group.tabs.begin(), group.tabs.end(),
                                                   [&](const auto &tab) { return contains(tab.paths); });
      });
    });
    saveLayout({std::vector<Group>(changed.begin() + 1, changed.end()), next});
    const auto &ungrouped = changed.front();
    const auto &previous = groups.front();
    if (!EqualRect(&ungrouped.bounds, &previous.bounds) || ungrouped.view != previous.view ||
        ungrouped.sort != previous.sort || ungrouped.collapsed != previous.collapsed || ungrouped.iconSize != previous.iconSize) {
      const UngroupedPlacement placement{ungrouped.bounds, static_cast<DWORD>(ungrouped.view),
                                         static_cast<DWORD>(ungrouped.sort), ungrouped.collapsed, ungrouped.iconSize};
      const auto error = RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedPlacement,
                                        REG_BINARY, &placement, sizeof(placement));
      if (error != ERROR_SUCCESS) fn::fail(lang::text(L"无法保存桌面盒子"), error);
    }
    groups = std::move(changed);
    if (placements || next.size() != looseItems.size()) {
      looseItems = std::move(next);
      refreshLoose();
    }
    if (refreshContents) PostMessageW(window, refreshMessage, 0, 0);
    return true;
  } catch (const fn::Error &error) {
    report(owner, error.message.c_str());
  }
  return false;
}

void Desktop::refreshUngrouped() {
  auto items = ungroupedItems(groups);
  if (items) std::erase_if(*items, [&](const auto &path) {
    return std::any_of(looseItems.begin(), looseItems.end(), [&](const auto &item) { return samePath(item.path, path); });
  });
  if (!items) return;
  std::sort(items->begin(), items->end());
  auto previous = groups.front().paths;
  std::sort(previous.begin(), previous.end());
  if (previous == *items) return;
  groups.front().paths = std::move(*items);
  panels.front()->refresh(groups.front());
}

bool Desktop::mergePanels(size_t source, size_t target) {
  if (editing || dragging || quitting) return false;
  auto changed = groups;
  const auto owner = panels[source]->window;
  const auto firstNewTab = groups[target].tabs.size() + 1;
  if (!mergeGroups(changed, source, target) || !commit(std::move(changed), owner)) return false;
  panels.erase(panels.begin() + source);
  for (size_t i = source; i < panels.size(); ++i) panels[i]->index = i;
  auto &destination = *panels[target - (source < target ? 1 : 0)];
  destination.showTab(firstNewTab);
  updateScene();
  lower();
  return true;
}

void Desktop::detachPanelTab(size_t index, size_t tab, POINT point) {
  if (editing || dragging || quitting) return;
  auto changed = groups;
  const auto owner = panels[index]->window;
  const auto dpi = GetDpiForWindow(owner);
  const auto width = MulDiv(360, dpi, 96), height = MulDiv(254, dpi, 96);
  const auto bounds = visibleBounds({point.x - width / 2, point.y, point.x - width / 2 + width, point.y + height});
  const auto previous = groups;
  if (!detachTab(changed, index, tab, bounds) || !commit(std::move(changed), owner)) return;
  if (!openPanel(groups.size() - 1)) {
    commit(previous, owner);
    report(owner, lang::text(L"无法打开桌面盒子。"));
    return;
  }
  panels[index]->activeTab = 0;
  panels[index]->refresh(groups[index]);
  if (!groups.back().tabs.empty()) panels.back()->showTab(1);
}

void Desktop::refreshLoose() {
  for (auto child : looseWindows) if (child) DestroyWindow(child);
  looseWindows.assign(looseItems.size(), nullptr);
  const auto dpi = GetDpiForWindow(scene);
  const int width = MulDiv(92, dpi, 96), height = MulDiv(78, dpi, 96);
  for (size_t i = 0; i < looseItems.size(); ++i) {
    const auto &item = looseItems[i];
    if (desktopItem(roots, item.path).empty()) continue;
    const auto p = item.position;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &monitor)) continue;
    const auto x = std::clamp(p.x, monitor.rcWork.left, monitor.rcWork.right);
    const auto y = std::clamp(p.y, monitor.rcWork.top, monitor.rcWork.bottom);
    const auto bounds = visibleBounds({x - width / 2, y - height / 2, x - width / 2 + width, y - height / 2 + height});
    auto child = CreateWindowExW(0, L"STATIC", fn::fs::path(item.path).filename().c_str(),
                                 WS_CHILD | WS_CLIPSIBLINGS | SS_NOTIFY | SS_CENTER, bounds.left - sceneBounds.left,
                                 bounds.top - sceneBounds.top, width, height, scene, nullptr, instance, nullptr);
    if (!child) continue;
    if (!SetWindowSubclass(child, looseProc, i + 1, reinterpret_cast<DWORD_PTR>(this))) {
      DestroyWindow(child);
      continue;
    }
    looseWindows[i] = child;
    SetWindowPos(child, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  updateScene();
}

LRESULT CALLBACK Desktop::looseProc(HWND child, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto &desktop = *reinterpret_cast<Desktop *>(data);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(child, looseProc, id);
    return DefSubclassProc(child, message, w, l);
  }
  if (id == 0 || id > desktop.looseItems.size()) return DefSubclassProc(child, message, w, l);
  const auto &path = desktop.looseItems[id - 1].path;
  switch (message) {
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(child, &paint);
    RECT area{};
    GetClientRect(child, &area);
    const auto dpi = GetDpiForWindow(child);
    desktop.looseGlass.paint(child, dc, area, desktop.theme.colors.field, 140, true);
    const int size = MulDiv(32, dpi, 96);
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) {
      DrawIconEx(dc, (area.right - size) / 2, MulDiv(6, dpi, 96), info.hIcon, size, size, 0, nullptr, DI_NORMAL);
      DestroyIcon(info.hIcon);
    }
    area.top = MulDiv(43, dpi, 96);
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    ui::text(dc, fn::fs::path(path).filename().native(), area, desktop.theme.colors.text,
             DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    EndPaint(child, &paint);
    return 0;
  }
  case WM_LBUTTONDOWN:
    if (!desktop.dragging && !desktop.editing && !desktop.menuOpen) {
      POINT point{};
      GetCursorPos(&point);
      SetPropW(child, L"FolderNote.LoosePressed", reinterpret_cast<HANDLE>(1));
      SetWindowLongPtrW(child, GWLP_USERDATA, MAKELPARAM(point.x, point.y));
      SetForegroundWindow(desktop.scene);
      SetCapture(child);
    }
    return 0;
  case WM_MOUSEMOVE:
    if (GetPropW(child, L"FolderNote.LoosePressed")) {
      if (!(w & MK_LBUTTON)) RemovePropW(child, L"FolderNote.LoosePressed");
      else {
        const auto start = GetWindowLongPtrW(child, GWLP_USERDATA);
        POINT point{};
        GetCursorPos(&point);
        if (std::abs(point.x - GET_X_LPARAM(start)) >= GetSystemMetrics(SM_CXDRAG) ||
            std::abs(point.y - GET_Y_LPARAM(start)) >= GetSystemMetrics(SM_CYDRAG)) {
          RemovePropW(child, L"FolderNote.LoosePressed");
          if (GetCapture() == child) ReleaseCapture();
          SHFILEINFOW info{};
          SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON);
          desktop.beginDrag(child, path, info.hIcon);
        }
      }
    }
    return 0;
  case WM_LBUTTONUP:
    if (GetCapture() == child) ReleaseCapture();
    [[fallthrough]];
  case WM_CAPTURECHANGED:
    RemovePropW(child, L"FolderNote.LoosePressed");
    return 0;
  case WM_LBUTTONDBLCLK:
    ShellExecuteW(desktop.scene, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
  case WM_CONTEXTMENU: {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    bool removed = false;
    desktop.menuOpen = true;
    try { showItemMenu(desktop.scene, path, point, false, removed); } catch (...) {}
    desktop.menuOpen = false;
    return 0;
  }
  }
  return DefSubclassProc(child, message, w, l);
}

void Desktop::lower(HWND raisedPanel) const {
  if (panels.empty()) return;
  if (std::any_of(panels.begin(), panels.end(), [](const auto &panel) { return panel->movingOrSizing; })) return;
  const auto active = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(active, &pid);
  if (pid == GetCurrentProcessId() && active != window && (menuOpen || editing || dragging)) return;

  // Keep the panels immediately above the desktop icon host, behind regular applications.
  HWND host = GetShellWindow();
  DWORD shellPid = 0;
  GetWindowThreadProcessId(host, &shellPid);
  wchar_t activeClass[32]{};
  GetClassNameW(active, activeClass, ARRAYSIZE(activeClass));
  const bool showingDesktop = shellPid && pid == shellPid && wcscmp(activeClass, L"WorkerW") == 0;
  if (showingDesktop) {
    // Show Desktop raises a separate wallpaper window, which does not host the icon list.
    host = active;
  } else {
    for (auto candidate = GetTopWindow(nullptr); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
      if (IsWindowVisible(candidate) && candidate != host && quick::isDesktop(candidate)) {
        host = candidate;
        break;
      }
    }
  }
  if (!host) return;
  std::vector<HWND> boxes;
  for (auto candidate = GetTopWindow(nullptr); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
    if (std::any_of(panels.begin(), panels.end(), [candidate](const auto &panel) { return panel->window == candidate; }))
      boxes.push_back(candidate);
  }
  if (const auto raised = std::find(boxes.begin(), boxes.end(), raisedPanel); raised != boxes.end())
    std::rotate(boxes.begin(), raised, raised + 1);
  auto above = GetWindow(host, GW_HWNDPREV);
  const auto belongsToDesktop = [&](HWND candidate) {
    if (candidate == scene || std::find(boxes.begin(), boxes.end(), candidate) != boxes.end()) return true;
    return std::any_of(panels.begin(), panels.end(), [candidate](const auto &panel) { return panel->shadow == candidate; });
  };
  while (above && (!IsWindowVisible(above) || belongsToDesktop(above))) above = GetWindow(above, GW_HWNDPREV);
  const bool hostTopmost = showingDesktop || (GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
  if (hostTopmost) {
    if (!above || !(GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) above = HWND_TOPMOST;
  } else if (!above || (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
    above = HWND_TOP;
  }
  constexpr UINT flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
  auto placeBehind = [&](HWND item) {
    if (IsIconic(item)) ShowWindow(item, SW_SHOWNOACTIVATE);
    const bool topmost = (GetWindowLongPtrW(item, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (topmost != hostTopmost)
      SetWindowPos(item, hostTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    const auto visible = IsWindowVisible(item) != FALSE;
    auto predecessor = GetWindow(item, GW_HWNDPREV);
    while (predecessor && !IsWindowVisible(predecessor)) predecessor = GetWindow(predecessor, GW_HWNDPREV);
    const bool positioned = (above == HWND_TOP || above == HWND_TOPMOST)
        ? !predecessor || (!hostTopmost && (GetWindowLongPtrW(predecessor, GWL_EXSTYLE) & WS_EX_TOPMOST))
        : predecessor == above;
    if (!visible || !positioned)
      SetWindowPos(item, above, 0, 0, 0, 0, flags | (visible ? 0 : SWP_SHOWWINDOW));
    above = item;
  };
  for (const auto box : boxes) placeBehind(box);
  placeBehind(scene);
  for (const auto &panel : panels)
    if (panel->shadow && panel->shadowSize.cx) placeBehind(panel->shadow);
}
void Desktop::manage() {
  if (menuOpen || editing || dragging) return;
  menuOpen = true;
  auto menu = std::make_shared<Branch>();
  menuItem(*menu, NewGroup, L"新建盒子", Symbol::AddFolder);
  if (!panels.empty()) separator(*menu);
  for (size_t i = 0; i < panels.size(); ++i) {
    quick::popup::Item item;
    item.title = groups[i].name;
    item.command = static_cast<unsigned>(i + 100);
    item.symbol = Symbol::Window;
    menu->items.push_back(std::move(item));
  }
  POINT point{};
  GetCursorPos(&point);
  unsigned command = 0;
  try { command = quick::popup::show(instance, point, menu, theme.mode, GetForegroundWindow()); }
  catch (...) { menuOpen = false; throw; }
  menuOpen = false;
  PostMessageW(window, WM_NULL, 0, 0);
  if (quitting) return;
  if (command == NewGroup) action(nullptr, NewGroup);
  if (command >= 100 && command - 100 < panels.size()) {
    auto &panel = *panels[command - 100];
    panel.showDockedBox(true);
    lower(panel.window);
    panel.contextMenu({-1, -1}, true);
  }
}

void Desktop::action(Panel *panel, Command command, size_t tabIndex) {
  if (editing || dragging || quitting) return;
  struct EditScope {
    bool &active;
    explicit EditScope(bool &flag) : active(flag) { active = true; }
    ~EditScope() { active = false; }
  } scope(editing);
  const auto owner = panel ? panel->window : window;
  if (command == NewGroup) {
    Group group;
    if (!askName(instance, panel ? owner : nullptr, L"新建盒子", group.name) || quitting) return;
    const auto work = ui::mouseWorkArea();
    const auto dpi = GetDpiForSystem();
    group.bounds = ui::centeredRect(work, MulDiv(360, dpi, 96), MulDiv(254, dpi, 96));
    const auto offset = static_cast<LONG>(groups.size() % 7) * MulDiv(24, dpi, 96);
    OffsetRect(&group.bounds, offset, offset);
    group.bounds = visibleBounds(group.bounds);
    auto changed = groups;
    changed.push_back(std::move(group));
    if (!commit(std::move(changed), owner)) return;
    if (!openPanel(groups.size() - 1)) {
      auto previous = groups;
      previous.pop_back();
      commit(std::move(previous), owner);
      report(owner, lang::text(L"无法打开桌面盒子。"));
    } else {
      lower();
    }
    return;
  }
  if (!panel || quitting) return;
  const auto index = panel->index;
  const int selected = panel->selection();
  switch (command) {
  case BoxIcon: case ClearBoxIcon: case TitleText: case TitleIcon: case TitleIconAndText: {
    auto next = groups[index];
    if (command == BoxIcon) {
      next.titleIcon = pickBoxIcon(owner);
      if (next.titleIcon.empty() || quitting) return;
      if (next.titleStyle == TitleStyle::Text) next.titleStyle = TitleStyle::IconAndText;
    } else if (command == ClearBoxIcon) {
      next.titleIcon.clear();
    } else {
      next.titleStyle = command == TitleText ? TitleStyle::Text :
                        command == TitleIcon ? TitleStyle::Icon : TitleStyle::IconAndText;
    }
    if (index) {
      auto changed = groups;
      changed[index] = std::move(next);
      if (!commit(std::move(changed), owner, nullptr, false)) return;
    } else {
      const DWORD titleMode = static_cast<DWORD>(next.titleStyle);
      const auto iconResult = RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedTitleIcon, REG_SZ,
          next.titleIcon.c_str(), static_cast<DWORD>((next.titleIcon.size() + 1) * sizeof(wchar_t)));
      const auto styleResult = RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedTitleStyle,
                                               REG_DWORD, &titleMode, sizeof(titleMode));
      if (iconResult != ERROR_SUCCESS || styleResult != ERROR_SUCCESS) {
        report(owner, lang::text(L"无法保存桌面盒子"));
        return;
      }
      groups.front() = std::move(next);
    }
    panel->resize();
    InvalidateRect(owner, nullptr, FALSE);
    return;
  }
  case BoxColor:
  case ResetBoxColor: {
    std::optional<COLORREF> tint;
    if (command == BoxColor) {
      tint = pickBoxColor(instance, owner, panel->backgroundColor(), [panel](COLORREF color) {
        panel->colorPreview = color;
        panel->applyBackground();
      });
      panel->colorPreview.reset();
      if (!tint || quitting) {
        panel->applyBackground();
        return;
      }
    }
    panel->saveColor(tint);
    panel->applyBackground();
    return;
  }
  case MoveTabLeft:
  case MoveTabRight: {
    const auto count = groups[index].tabs.size();
    if (!index || !tabIndex || tabIndex > count || (command == MoveTabLeft ? tabIndex == 1 : tabIndex == count)) return;
    const auto target = command == MoveTabLeft ? tabIndex - 1 : tabIndex + 1;
    auto changed = groups;
    std::swap(changed[index].tabs[tabIndex - 1], changed[index].tabs[target - 1]);
    if (!commit(std::move(changed), owner, nullptr, false)) return;
    if (panel->activeTab == tabIndex) panel->activeTab = target;
    else if (panel->activeTab == target) panel->activeTab = tabIndex;
#ifdef FOLDERNOTE_WINUI
    panel->syncSurfaceHeader();
#endif
    InvalidateRect(panel->tabs, nullptr, FALSE);
    return;
  }
  case AddTab:
  case MapFolder: {
    Tab tab;
    if (command == MapFolder) {
      tab.folder = pickFolder(owner);
      if (tab.folder.empty() || quitting) return;
      const fn::fs::path folder(tab.folder);
      tab.name = folder.filename().wstring();
      if (tab.name.empty()) tab.name = tab.folder;
      tab.name.resize(std::min(tab.name.size(), groupNameLimit));
    } else if (!askName(instance, owner, L"添加标签", tab.name) || quitting) return;
    auto changed = groups;
    changed[index].tabs.push_back(std::move(tab));
    changed[index].collapsed = false;
    if (commit(std::move(changed), owner)) {
      panel->showTab(groups[index].tabs.size());
      panel->setCollapsed();
    }
    return;
  }
  case RemoveTab: {
    if (!tabIndex || tabIndex > groups[index].tabs.size()) return;
    auto changed = groups;
    auto &tabs = changed[index].tabs;
    tabs.erase(tabs.begin() + tabIndex - 1);
    if (commit(std::move(changed), owner, nullptr, false)) {
      if (panel->activeTab == tabIndex) panel->activeTab = 0;
      else if (panel->activeTab > tabIndex) --panel->activeTab;
      panel->refresh(groups[index]);
      panel->resize();
    }
    return;
  }
  case ScrollDisplay: case PageDisplay: {
    const auto mode = command == PageDisplay ? DisplayMode::Pages : DisplayMode::Scroll;
    if (groups[index].displayMode == mode) return;
    if (index) {
      auto changed = groups;
      changed[index].displayMode = mode;
      if (!commit(std::move(changed), owner, nullptr, false)) return;
    } else {
      const DWORD value = mode == DisplayMode::Pages;
      if (RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, ungroupedDisplayMode,
                          REG_DWORD, &value, sizeof(value)) != ERROR_SUCCESS) {
        report(owner, lang::text(L"无法保存桌面盒子"));
        return;
      }
      groups.front().displayMode = mode;
    }
    panel->currentPage = panel->pageWheelRemainder = 0;
    panel->resize();
    panel->renderPage(groups[index]);
    return;
  }
  case IconView: case ListViewMode:
  case ManualSort: case NameSort: case TypeSort: case ModifiedSort: {
    auto changed = groups;
    auto &group = changed[index];
    if (command == IconView || command == ListViewMode) group.view = command == IconView ? View::Icons : View::List;
    else group.sort = static_cast<Sort>(command - ManualSort);
    if (commit(std::move(changed), owner)) {
      panel->refresh(groups[index]);
      for (const auto button : panel->viewButtons) InvalidateRect(button, nullptr, FALSE);
    }
    return;
  }
  case ToggleGroup: {
    auto changed = groups;
    changed[index].collapsed = !changed[index].collapsed;
    if (commit(std::move(changed), owner)) panel->setCollapsed();
    return;
  }
  case RenameGroup: {
    auto name = groups[index].name;
    if (!askName(instance, owner, L"重命名盒子", name) || quitting) return;
    auto changed = groups;
    changed[index].name = std::move(name);
    if (commit(std::move(changed), owner)) {
      SetWindowTextW(owner, groups[index].name.c_str());
#ifdef FOLDERNOTE_WINUI
      panel->syncSurfaceHeader();
#endif
      InvalidateRect(owner, nullptr, FALSE);
      InvalidateRect(panel->tabs, nullptr, FALSE);
    }
    return;
  }
  case DeleteGroup: {
    if (!showBoxDialog(instance, owner, L"删除盒子", nullptr,
                       lang::format(L"删除盒子“{0}”？盒子中的文件会保留。", {groups[index].name})) || quitting) return;
    auto changed = groups;
    changed.erase(changed.begin() + index);
    if (commit(std::move(changed), owner)) {
      panels.erase(panels.begin() + index);
      for (size_t i = index; i < panels.size(); ++i) panels[i]->index = i;
      updateScene();
    }
    return;
  }
  case OpenItem:
    if (selected >= 0 && static_cast<size_t>(selected) < panel->displayed.size()) {
      const auto &item = panel->displayed[selected];
      const auto path = !index || panel->mapped() ? item : desktopItem(roots, item);
      if (path.empty() ||
          reinterpret_cast<INT_PTR>(ShellExecuteW(owner, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        report(owner, lang::text(L"无法打开此桌面项目。"));
    }
    return;
  case RemoveItem:
    if (index && !panel->mapped() && selected >= 0 && static_cast<size_t>(selected) < panel->displayed.size()) {
      auto changed = groups;
      auto &items = manualItems(changed[index], panel->activeTab);
      std::erase_if(items, [&](const auto &item) { return samePath(item, panel->displayed[selected]); });
      if (commit(std::move(changed), owner)) panel->refresh(groups[index]);
    }
    return;
  default:
    return;
  }
}

void Desktop::drag(Panel &panel, int index) {
  const auto &items = panel.displayed;
  if (index < 0 || static_cast<size_t>(index) >= items.size() || panel.mapped() || editing || dragging || quitting) return;
  const auto path = desktopItem(roots, items[index]);
  if (path.empty()) return;
  LVITEMW item{};
  item.mask = LVIF_IMAGE;
  item.iItem = index;
  const auto images = ListView_GetImageList(panel.list, groups[panel.index].view == View::List ? LVSIL_SMALL : LVSIL_NORMAL);
  beginDrag(panel.window, path, images && ListView_GetItem(panel.list, &item) && item.iImage >= 0 ?
                                    ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT) : nullptr,
            panel.list, index, groups[panel.index].view == View::List);
  if (dragging && (ListView_GetItemState(panel.list, index, LVIS_SELECTED) & LVIS_SELECTED)) {
    dragItems.clear();
    for (int row = -1; (row = ListView_GetNextItem(panel.list, row, LVNI_SELECTED)) >= 0;) {
      if (static_cast<size_t>(row) >= items.size()) continue;
      if (auto selected = desktopItem(roots, items[row]); !selected.empty()) dragItems.push_back(std::move(selected));
    }
  }
}

void Desktop::beginDrag(HWND source, std::wstring path, HICON icon, HWND list, int row, bool listMode) {
  dragPath = std::move(path);
  dragItems = {dragPath};
  dragWindow = source;
  dragIcon = icon;
  dragListMode = listMode;
  dragLabel = fn::fs::path(dragPath).filename().native();
  const auto dpi = GetDpiForWindow(source);
  const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
  dragSize = {px(92), px(78)};
  dragIconRect = {px(30), px(6), px(62), px(38)};
  dragLabelRect = {0, px(43), dragSize.cx, dragSize.cy};
  RECT bounds{}, image{}, label{};
  if (list && ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS) &&
      ListView_GetItemRect(list, row, &image, LVIR_ICON) && ListView_GetItemRect(list, row, &label, LVIR_LABEL)) {
    if (listMode) bounds.right = std::min<LONG>(bounds.right, std::max<LONG>(label.left + px(180), image.right));
    dragSize = {std::max(1L, bounds.right - bounds.left), std::max(1L, bounds.bottom - bounds.top)};
    OffsetRect(&image, -bounds.left, -bounds.top);
    OffsetRect(&label, -bounds.left, -bounds.top);
    label.right = std::min(label.right, dragSize.cx);
    dragIconRect = image;
    dragLabelRect = label;
    wchar_t text[1024]{};
    ListView_GetItemText(list, row, 0, text, ARRAYSIZE(text));
    if (*text) dragLabel = text;
  }
  dragging = SetTimer(window, dragTimer, 10, nullptr) != 0;
  if (!dragging) {
    dragWindow = nullptr;
    dragPath.clear();
    dragItems.clear();
    if (dragIcon) DestroyIcon(std::exchange(dragIcon, nullptr));
    return;
  }
  dragPreview = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                               L"STATIC", nullptr,
                               WS_POPUP,
                               0, 0, 0, 0, window, nullptr, instance, nullptr);
  if (dragPreview) {
    if (!SetWindowSubclass(dragPreview, dragPreviewProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
      DestroyWindow(std::exchange(dragPreview, nullptr));
      return;
    }
    SetLayeredWindowAttributes(dragPreview, 0, 225, LWA_ALPHA);
    SendMessageW(dragPreview, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    const auto dpi = GetDpiForWindow(source);
    const auto diameter = MulDiv(14, dpi, 96);
    auto shape = CreateRoundRectRgn(0, 0, dragSize.cx, dragSize.cy, diameter, diameter);
    if (shape && !SetWindowRgn(dragPreview, shape, FALSE)) DeleteObject(shape);
  }
  updateDragFeedback();
}

LRESULT CALLBACK Desktop::dragPreviewProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, dragPreviewProc, id);
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT) {
    const auto &desktop = *reinterpret_cast<Desktop *>(data);
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(window, &paint);
    RECT area{};
    GetClientRect(window, &area);
    const auto &colors = desktop.theme.colors;
    const auto dpi = GetDpiForWindow(desktop.dragWindow);
    ui::rounded(dc, area, colors.surface, colors.border, MulDiv(7, dpi, 96));
    drawFenceItem(dc, area, desktop.dragLabelRect, desktop.dragIconRect, desktop.dragIcon,
                  desktop.dragLabel, colors, false, false, desktop.dragListMode, dpi);
    EndPaint(window, &paint);
    return 0;
  }
  return DefSubclassProc(window, message, w, l);
}

void Desktop::updateDragFeedback() {
  POINT point{};
  if (!GetCursorPos(&point)) return;
  auto *target = panelAt(point);
  const auto found = std::find_if(panels.begin(), panels.end(), [target](const auto &panel) {
    return target == panel.get() && !panel->mapped();
  });
  const auto highlighted = found == panels.end() ? nullptr : (*found)->window;
  RECT marker{};
  if (highlighted) {
    auto &panel = **found;
    panel.autoscroll(point);
    marker = panel.insertion(point).marker;
  }
  const bool changedTarget = highlighted != dropHighlight;
  if (changedTarget) {
    if (dropHighlight) {
      const auto previous = std::find_if(panels.begin(), panels.end(), [&](const auto &panel) {
        return panel->window == dropHighlight;
      });
      if (previous != panels.end()) InvalidateRect((*previous)->list, &dropMarker, FALSE);
    }
    highlight(highlighted);
  }
  if (!EqualRect(&marker, &dropMarker)) {
    if (highlighted) {
      InvalidateRect((*found)->list, &dropMarker, FALSE);
      InvalidateRect((*found)->list, &marker, FALSE);
    }
    dropMarker = marker;
  } else if (changedTarget && highlighted && !IsRectEmpty(&marker)) {
    // A different panel can have an insertion marker at the same local point.
    InvalidateRect((*found)->list, &marker, FALSE);
  }
  SetCursor(LoadCursorW(nullptr, (target ? !target->mapped() : desktopDropAt(point)) ? IDC_SIZEALL : IDC_NO));
  if (dragPreview) {
    const int dpi = GetDpiForWindow(dragWindow);
    const int x = point.x + MulDiv(14, dpi, 96), y = point.y + MulDiv(14, dpi, 96);
    if (!IsWindowVisible(dragPreview)) {
      SetWindowPos(dragPreview, HWND_TOPMOST, x, y, dragSize.cx, dragSize.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      UpdateWindow(dragPreview);
    } else {
      RECT previous{};
      if (GetWindowRect(dragPreview, &previous) && (previous.left != x || previous.top != y))
        SetWindowPos(dragPreview, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
}

void Desktop::finishDrag(HWND source, bool apply) {
  if (!dragging || source != dragWindow) return;
  dragging = false;
  KillTimer(window, dragTimer);
  if (dragPreview) DestroyWindow(std::exchange(dragPreview, nullptr));
  if (dragIcon) DestroyIcon(std::exchange(dragIcon, nullptr));
  if (dropHighlight) {
    const auto previous = std::find_if(panels.begin(), panels.end(), [&](const auto &panel) {
      return panel->window == dropHighlight;
    });
    if (previous != panels.end()) InvalidateRect((*previous)->list, &dropMarker, FALSE);
    highlight(nullptr);
  }
  dropMarker = {};
  lastDragScroll = 0;
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  dragPath.clear();
  auto paths = std::move(dragItems);
  POINT point{};
  dragWindow = nullptr;
  if (!apply || quitting || !GetCursorPos(&point)) return;
  if (auto target = panelAt(point)) {
    assign(*target, paths, point);
  } else if (desktopDropAt(point)) {
    auto changed = groups;
    const auto included = [&](const auto &path) {
      return std::any_of(paths.begin(), paths.end(), [&](const auto &candidate) { return samePath(candidate, path); });
    };
    for (auto group = changed.begin() + 1; group != changed.end(); ++group) {
      const auto remove = [&](auto &paths) {
        std::erase_if(paths, included);
      };
      remove(group->paths);
      for (auto &tab : group->tabs) remove(tab.paths);
    }
    auto placements = looseItems;
    std::erase_if(placements, [&](const auto &item) { return included(item.path); });
    const int spacing = MulDiv(92, GetDpiForWindow(scene), 96);
    MONITORINFO monitor{sizeof(monitor)};
    const bool hasWorkArea = GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
    const LONG startX = point.x;
    for (auto &path : paths) {
      placements.push_back({std::move(path), point});
      point.x += spacing;
      if (hasWorkArea && point.x >= monitor.rcWork.right - spacing / 2) {
        point.x = startX;
        point.y += spacing;
      }
    }
    commit(std::move(changed), source, &placements);
  }
}

Insertion Panel::insertion(POINT point) const {
  const auto &group = desktop.groups[index];
  Insertion insertion;
#ifdef FOLDERNOTE_WINUI
  if (surface && surface->available()) {
    if (!index || group.sort != Sort::Manual || group.collapsed) return insertion;
    insertion.item = surface->hit(point);
    RECT item{};
    if (insertion.item >= 0 && surface->itemBounds(insertion.item, item)) {
      insertion.after = group.view == View::List ? point.y >= (item.top + item.bottom) / 2 :
                                                  point.x >= (item.left + item.right) / 2;
    }
    return insertion;
  }
#endif
  if (!index || group.sort != Sort::Manual || !IsWindowVisible(list)) return insertion;
  LVHITTESTINFO hit{};
  hit.pt = point;
  ScreenToClient(list, &hit.pt);
  insertion.item = ListView_HitTest(list, &hit);
  const bool rows = group.view == View::List;
  const auto thickness = std::max(2, MulDiv(3, GetDpiForWindow(window), 96));
  RECT item{};
  if (insertion.item >= 0 && ListView_GetItemRect(list, insertion.item, &item, rows ? LVIR_BOUNDS : LVIR_ICON)) {
    insertion.after = rows ? hit.pt.y > (item.top + item.bottom) / 2 : hit.pt.x > (item.left + item.right) / 2;
  } else {
    RECT client{};
    GetClientRect(list, &client);
    const auto last = ListView_GetItemCount(list) - 1;
    if (!PtInRect(&client, hit.pt) || last < 0 || !ListView_GetItemRect(list, last, &item, rows ? LVIR_BOUNDS : LVIR_ICON))
      return insertion;
    insertion.after = true;
  }
  if (rows) {
    const auto edge = insertion.after ? item.bottom : item.top;
    insertion.marker = {item.left, edge - thickness / 2, item.right, edge - thickness / 2 + thickness};
  } else {
    const auto edge = insertion.after ? item.right + thickness : item.left - thickness;
    insertion.marker = {edge - thickness / 2, item.top, edge - thickness / 2 + thickness, item.bottom};
  }
  RECT client{};
  GetClientRect(list, &client);
  IntersectRect(&insertion.marker, &insertion.marker, &client);
  return insertion;
}

void Panel::autoscroll(POINT point) {
  if (desktop.groups[index].displayMode == DisplayMode::Pages) return;
#ifdef FOLDERNOTE_WINUI
  if (surface && surface->available()) {
    const auto now = GetTickCount64();
    if (now - desktop.lastDragScroll >= 75 && surface->scrollAt(point)) desktop.lastDragScroll = now;
    return;
  }
#endif
  if (!IsWindowVisible(list)) return;
  const auto now = GetTickCount64();
  if (now - desktop.lastDragScroll < 75) return;
  ScreenToClient(list, &point);
  RECT client{};
  GetClientRect(list, &client);
  const auto dpi = GetDpiForWindow(window);
  const int edge = MulDiv(20, dpi, 96);
  if (point.x < client.left || point.y < client.top || point.x >= client.right || point.y >= client.bottom) return;
  const auto direction = [edge](int position, int extent) {
    return position < std::min(edge, extent / 2) ? -1 : position >= std::max(extent - edge, extent / 2) ? 1 : 0;
  };
  const auto vertical = direction(point.y, client.bottom);
  const auto horizontal = direction(point.x, client.right);
  if (vertical && verticalScroll.maximum())
    verticalScroll.scrollTo(verticalScroll.position() + vertical *
                            (desktop.groups[index].view == View::List ? 1 : MulDiv(22, dpi, 96)));
  if (horizontal && horizontalScroll.maximum())
    horizontalScroll.scrollTo(horizontalScroll.position() + horizontal * MulDiv(22, dpi, 96));
  desktop.lastDragScroll = now;
}

bool Desktop::drop(Panel &panel, HDROP files, POINT point) {
  const auto count = DragQueryFileW(files, 0xffffffff, nullptr, 0);
  std::vector<std::wstring> paths;
  for (UINT i = 0; i < count; ++i) {
    std::wstring path(static_cast<size_t>(DragQueryFileW(files, i, nullptr, 0)) + 1, L'\0');
    path.resize(DragQueryFileW(files, i, path.data(), static_cast<UINT>(path.size())));
    paths.push_back(std::move(path));
  }
  return assign(panel, paths, point);
}

bool Desktop::assign(Panel &panel, const std::vector<std::wstring> &paths, POINT point) {
  if (editing || quitting || panel.mapped()) return false;
  auto changed = groups;
  auto placements = looseItems;
  bool rejected = false, accepted = false, updated = false;
  const auto insertion = panel.insertion(point);
  auto &items = manualItems(changed[panel.index], panel.activeTab);
  const auto anchor = insertion.item >= 0 && static_cast<size_t>(insertion.item) < panel.displayed.size()
      ? std::find(items.begin(), items.end(), panel.displayed[insertion.item]) : items.end();
  size_t insertAt = static_cast<size_t>(anchor - items.begin()) + (anchor != items.end() && insertion.after);
  for (const auto &candidate : paths) {
    const auto path = desktopItem(roots, candidate);
    if (path.empty()) rejected = true;
    else {
      accepted = true;
      const auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) { return samePath(item, path); });
      const bool exists = found != items.end();
      const size_t from = exists ? static_cast<size_t>(found - items.begin()) : items.size();
      if (!panel.index) {
        updated |= std::erase_if(placements, [&](const auto &item) { return samePath(item.path, path); }) != 0;
        for (size_t groupIndex = 1; groupIndex < changed.size(); ++groupIndex) {
          auto remove = [&](auto &list) {
            updated |= std::erase_if(list, [&](const auto &item) { return samePath(item, path); }) != 0;
          };
          remove(changed[groupIndex].paths);
          for (auto &tab : changed[groupIndex].tabs) remove(tab.paths);
        }
        continue;
      }
      if (!exists && !addItem(changed, panel.index, path, panel.activeTab)) continue;
      if (groups[panel.index].sort != Sort::Manual) {
        updated |= !exists;
        continue;
      }
      size_t to = insertAt - (from < insertAt ? 1 : 0);
      if (from < to) std::rotate(items.begin() + from, items.begin() + from + 1, items.begin() + to + 1);
      if (to < from) std::rotate(items.begin() + to, items.begin() + from, items.begin() + from + 1);
      updated |= !exists || to != from;
      insertAt = to + 1;
    }
  }
  if (rejected) PostMessageW(panel.window, rejectedMessage, 0, 0);
  if (updated) {
    if (!commit(std::move(changed), panel.window, placements.size() != looseItems.size() ? &placements : nullptr)) return false;
    PostMessageW(window, refreshMessage, 0, 0);
  }
  return accepted;
}

LRESULT CALLBACK Desktop::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto desktop = reinterpret_cast<Desktop *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    desktop = static_cast<Desktop *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    desktop->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(desktop));
  }
  if (!desktop) return DefWindowProcW(window, message, w, l);
  try {
    if (message == desktop->taskbarCreated) {
      desktop->tray(NIM_ADD);
      desktop->lower();
      return 0;
    }
    if (const UINT preview = appearanceMessage(); preview && message == preview) {
      desktop->previewingAppearance = (w & previewEnabled) != 0;
      const auto radius = static_cast<DWORD>(w & previewRadiusMask);
      const auto opacity = static_cast<DWORD>((w >> 5) & previewOpacityMask);
      const auto shadowStyle = static_cast<DWORD>(l);
      desktop->setAppearance(desktop->previewingAppearance
          ? Appearance{(w & previewGlass) != 0, std::min<DWORD>(radius, 20),
                       std::clamp<DWORD>(opacity, 25, 100), (w & previewShadow) != 0,
                       std::min<DWORD>(LOBYTE(shadowStyle), 100),
                       std::clamp<DWORD>(HIBYTE(shadowStyle), 1, 32),
                       std::min<DWORD>(HIWORD(shadowStyle), 20)}
          : configuredAppearance());
      return 0;
    }
    if (message == ui::themeChangedMessage() || message == WM_THEMECHANGED || message == WM_SETTINGCHANGE) {
      desktop->theme.reset(ui::configuredTheme());
      if (message == WM_SETTINGCHANGE) desktop->looseGlass.reset();
      if (!desktop->previewingAppearance) desktop->style = configuredAppearance();
      for (const auto &panel : desktop->panels) {
        panel->applyTheme(desktop->theme.colors);
        panel->resize();
      }
      desktop->updateScene();
    }
    switch (message) {
    case WM_TIMER:
      if (w == headerTimer) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
          const auto now = GetTickCount64();
          for (const auto &panel : desktop->panels) {
            panel->tickDock(cursor, now);
            if (!panel->dockHidden) panel->updateHeaderActions(cursor);
          }
        }
        return 0;
      }
      if (w == dragTimer) {
        const bool cancel = GetAsyncKeyState(VK_ESCAPE) < 0;
        if (cancel || GetAsyncKeyState(VK_LBUTTON) >= 0) desktop->finishDrag(desktop->dragWindow, !cancel);
        else desktop->updateDragFeedback();
        return 0;
      }
      if (!enabled()) PostMessageW(window, WM_CLOSE, 0, 0);
      else {
        desktop->lower();
        if (std::any_of(desktop->panels.begin(), desktop->panels.end(),
                        [](const auto &panel) { return panel->movingOrSizing; })) return 0;
        for (const auto &panel : desktop->panels) panel->applyThumbnails();
        const auto now = GetTickCount64();
        if (!desktop->editing && !desktop->dragging && !desktop->menuOpen &&
            now - desktop->lastFolderRefresh > 2000) {
          desktop->lastFolderRefresh = now;
          setDesktopIconsHidden(true);
          desktop->refreshUngrouped();
          for (const auto &panel : desktop->panels)
            if (!panel->movingOrSizing && panel->mapped() && !desktop->groups[panel->index].collapsed)
              panel->refresh(desktop->groups[panel->index]);
        }
      }
      return 0;
    case WM_DISPLAYCHANGE:
      for (const auto &panel : desktop->panels)
        if (panel->gestureEdge) panel->finishGesture(false);
      desktop->looseGlass.reset();
      desktop->sceneBounds = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
      SetWindowPos(desktop->scene, nullptr, desktop->sceneBounds.left, desktop->sceneBounds.top,
                   GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                   SWP_NOACTIVATE | SWP_NOZORDER);
      for (const auto &panel : desktop->panels) {
        panel->setDockSide(DockSide::None);
        panel->place(visibleBounds(desktop->groups[panel->index].bounds));
        panel->setCollapsed();
        RedrawWindow(panel->window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      }
      desktop->refreshLoose();
      return 0;
    case refreshMessage:
      desktop->refreshUngrouped();
      for (size_t i = 0; i < desktop->panels.size(); ++i) desktop->panels[i]->refresh(desktop->groups[i]);
      return 0;
    case trayMessage:
      if (l == WM_RBUTTONUP || l == WM_LBUTTONUP) PostMessageW(window, manageMessage, 0, 0);
      return 0;
    case manageMessage:
      if (!desktop->quitting && IsWindowEnabled(window)) desktop->manage();
      return 0;
    case createMessage:
      if (!desktop->menuOpen) desktop->action(nullptr, NewGroup);
      return 0;
    case WM_CLOSE:
      desktop->quitting = true;
      desktop->finishDrag(desktop->dragWindow, false);
      EndMenu();
      EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM) -> BOOL {
        wchar_t name[16]{};
        GetClassNameW(candidate, name, ARRAYSIZE(name));
        if (wcscmp(name, L"#32770") == 0) PostMessageW(candidate, WM_CLOSE, 0, 0);
        return TRUE;
      }, 0);
      PostQuitMessage(0);
      return 0;
    case WM_DESTROY:
      desktop->tray(NIM_DELETE);
      break;
    case WM_NCDESTROY:
      desktop->window = nullptr;
      break;
    }
  } catch (...) {
    report(window, lang::text(L"无法打开桌面盒子。"));
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
}

Appearance configuredAppearance() {
  const Appearance defaults;
  const bool glass = appearanceValue(L"DesktopGroupsGlass", defaults.glass) != 0;
  return {glass, std::min<DWORD>(20, appearanceValue(L"DesktopGroupsRadius", defaults.radius)),
          std::clamp<DWORD>(appearanceValue(L"DesktopGroupsOpacity", defaults.opacity), 25, 100),
          appearanceValue(L"DesktopGroupsShadow", defaults.shadow) != 0,
          std::min<DWORD>(100, appearanceValue(L"DesktopGroupsShadowOpacity", defaults.shadowOpacity)),
          std::clamp<DWORD>(appearanceValue(L"DesktopGroupsShadowBlur", defaults.shadowBlur), 1, 32),
          std::min<DWORD>(20, appearanceValue(L"DesktopGroupsShadowOffset", defaults.shadowOffset))};
}

void previewAppearance(const Appearance &appearance) {
  if (const auto window = receiver()) {
    const WPARAM style = previewEnabled | std::min<DWORD>(appearance.radius, 20) |
        (std::clamp<DWORD>(appearance.opacity, 25, 100) << 5) |
        (appearance.glass ? previewGlass : 0) | (appearance.shadow ? previewShadow : 0);
    const auto shadow = MAKELPARAM(MAKEWORD(std::min<DWORD>(appearance.shadowOpacity, 100),
                                          std::clamp<DWORD>(appearance.shadowBlur, 1, 32)),
                                  std::min<DWORD>(appearance.shadowOffset, 20));
    if (const UINT message = appearanceMessage()) PostMessageW(window, message, style, shadow);
  }
}

void restoreAppearance() {
  if (const auto window = receiver()) {
    if (const UINT message = appearanceMessage()) PostMessageW(window, message, 0, 0);
  }
}

bool enabled() {
  DWORD value = 0, bytes = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"DesktopGroupsEnabled", RRF_RT_REG_DWORD,
                      nullptr, &value, &bytes) == ERROR_SUCCESS && value == 1;
}

bool stop() {
  const auto removed = startup({});
  const auto menuRemoved = configureDesktopMenu({});
  return stopProcess() && restoreDesktopIcons() && removed && menuRemoved;
}

bool sync(const std::wstring &installedExecutable) {
  if (!enabled()) return stop();
  try {
    const auto exe = installedExecutable.empty() ? quick::executable() : installedExecutable;
    if (!startup(exe) || !configureDesktopMenu(exe)) return false;
    if (!installedExecutable.empty() && !stopProcess()) return false;
    const auto window = receiver();
    return window ? GetPropW(window, readyProperty) != nullptr : launch(exe, false);
  } catch (...) {
    return false;
  }
}

bool showManager() {
  if (!enabled()) return false;
  if (auto window = receiver()) return notifyManager(window);
  try {
    return launch(quick::executable(), true);
  } catch (...) {
    return false;
  }
}

int run(HINSTANCE instance, bool manage, bool create) {
  if (!enabled()) return 0;
  const auto mutex = CreateMutexW(nullptr, FALSE, mutexName);
  if (!mutex) return 1;
  DWORD status = WaitForSingleObject(mutex, 0);
  if (status == WAIT_TIMEOUT && (manage || create)) {
    for (int i = 0; i < 40 && status == WAIT_TIMEOUT; ++i) {
      if (auto window = receiver(); window && notifyManager(window, create ? createMessage : manageMessage)) break;
      status = WaitForSingleObject(mutex, 50);
    }
  }
  int result = 0;
  if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) {
    if (enabled() && SUCCEEDED(OleInitialize(nullptr))) {
      try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW panel{};
        panel.lpfnWndProc = Panel::proc;
        panel.hInstance = instance;
        panel.lpszClassName = panelClass;
        panel.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        WNDCLASSW manager = panel;
        manager.lpfnWndProc = Desktop::proc;
        manager.lpszClassName = receiverClass;
        panel.style = CS_DBLCLKS;
        WNDCLASSW sceneClassInfo{};
        sceneClassInfo.hInstance = instance;
        sceneClassInfo.lpszClassName = sceneClass;
        sceneClassInfo.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
          if (message == WM_CLOSE) return 0;
          return DefWindowProcW(window, message, w, l);
        };
        if (!RegisterClassW(&panel) || !RegisterClassW(&manager) || !RegisterClassW(&sceneClassInfo)) throw 0;
#ifdef FOLDERNOTE_WINUI
        winui::Runtime xaml;
#endif
        Desktop desktop(instance);
#ifdef FOLDERNOTE_WINUI
        desktop.useWinui = xaml.available();
#endif
        if (!CreateWindowExW(WS_EX_TOOLWINDOW, receiverClass, L"", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, instance, &desktop)) throw 0;
        desktop.sceneBounds = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        desktop.scene = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, sceneClass, L"",
                                       WS_POPUP | WS_CLIPCHILDREN, desktop.sceneBounds.left, desktop.sceneBounds.top,
                                       GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                                       nullptr, nullptr, instance, nullptr);
        if (!desktop.scene) throw 0;
        for (size_t i = 0; i < desktop.groups.size(); ++i)
          if (!desktop.openPanel(i)) throw 0;
        desktop.refreshLoose();
        if (!setDesktopIconsHidden(true))
          throw fn::Error{lang::text(L"无法接管桌面图标。")};
        desktop.hold.start(desktop.window, createMessage);
        desktop.tray(NIM_ADD);
        if (!SetTimer(desktop.window, 1, 400, nullptr)) throw 0;
        if (!SetTimer(desktop.window, headerTimer, 70, nullptr)) throw 0;
        if (!SetPropW(desktop.window, readyProperty, reinterpret_cast<HANDLE>(1))) throw 0;
        if (manage) PostMessageW(desktop.window, manageMessage, 0, 0);
        if (create) PostMessageW(desktop.window, createMessage, 0, 0);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
#ifdef FOLDERNOTE_WINUI
          if (xaml.preTranslate(message)) continue;
#endif
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      } catch (const fn::Error &error) {
        if (manage || create) report(nullptr, error.message.c_str());
        result = 1;
      } catch (...) {
        if (manage) report(nullptr, lang::text(L"无法打开桌面盒子。"));
        result = 1;
      }
      OleUninitialize();
    } else if (enabled()) {
      if (manage) report(nullptr, lang::text(L"无法打开桌面盒子。"));
      result = 1;
    }
    ReleaseMutex(mutex);
  }
  CloseHandle(mutex);
  return result;
}
}
