#pragma once
// Internal types and helpers shared by the desktop box implementation files.
// Not part of the public interface; include desktop_groups.h from other modules.
#include "desktop_groups.h"
#include "quick_find.h"
#include "quick_find_controls.h"
#include "quick_find_popup.h"
#include "desktop_box_icons.h"
#include "desktop_groups_color_picker.h"
#include "desktop_groups_icon_source.h"
#include "desktop_groups_model.h"
#include "desktop_groups_menu.h"
#include "desktop_groups_item_menu.h"
#include "desktop_groups_process.h"
#include "desktop_groups_preview.h"
#include "desktop_groups_preview_keys.h"
#include "desktop_groups_search.h"
#include "desktop_groups_zorder.h"
#include "desktop_groups_wallpaper.h"
#include "desktop_groups_glass.h"
#include "desktop_groups_render.h"
#include "desktop_groups_storage.h"
#ifdef FOLDERNOTE_WINUI
#include "desktop_groups_winui.h"
#endif
#include "desktop_groups_visibility.h"
#include "theme_scroll.h"
#include "core.h"
#include "language.h"
#include "navigation.h"
#include "quick_folders.h"
#include "middle_click.h"
#include "quick_hit.h"
#include "quick_popup.h"
#include "quick_transfer.h"
#include "resource.h"
#include "theme.h"
#include "window_position.h"
#include <commctrl.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commoncontrols.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace desktop_groups::detail {
constexpr auto receiverClass = L"FolderNote.DesktopGroups.Receiver";
constexpr auto panelClass = L"FolderNote.DesktopGroups.Panel";
constexpr auto tabPreviewClass = L"FolderNote.DesktopGroups.TabPreview";
constexpr auto mutexName = L"Local\\FolderNote.DesktopGroups";
constexpr auto runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto runName = L"FolderNote.DesktopGroups";
constexpr UINT manageMessage = WM_APP + 1, rejectedMessage = WM_APP + 3;
constexpr UINT dragMessage = WM_APP + 4, refreshMessage = WM_APP + 5;
constexpr UINT createMessage = WM_APP + 6;
constexpr UINT previewSelectedMessage = WM_APP + 8;
constexpr UINT locateSearchMessage = WM_APP + 9;
constexpr UINT dropBoxTabMessage = WM_APP + 41;
constexpr UINT headerTimer = 3, transferTimer = 4;
constexpr UINT_PTR boxMoveTimer = 1;
constexpr UINT_PTR tabDragTimer = 2;
constexpr UINT_PTR collapseHoldTimer = 3;
constexpr UINT_PTR blurPreviewTimer = 4;
constexpr ULONGLONG collapseHoldDelay = 350;
constexpr UINT_PTR hoverTabTimer = 1;
constexpr UINT syncScrollMessage = WM_APP + 40;
constexpr UINT finishBoxGestureMessage = WM_APP + 42;
constexpr UINT updatePagesMessage = WM_APP + 43, goToPageMessage = WM_APP + 44;
constexpr UINT renameCreatedBoxMessage = WM_APP + 45;
constexpr UINT beginItemRenameMessage = WM_APP + 46, commitItemRenameMessage = WM_APP + 47;
constexpr UINT desktopDblClickMessage = WM_APP + 48, applyThumbnailsMessage = WM_APP + 49;
constexpr UINT showFolderHoverMessage = WM_APP + 50, transferDoneMessage = WM_APP + 51;
constexpr auto transferTokenPrefix = L"?transfer\\";

inline bool isTransferToken(const std::wstring &path) {
  return path.starts_with(transferTokenPrefix);
}
inline UINT appearanceMessage() {
  static const UINT message = RegisterWindowMessageW(L"FolderNote.DesktopGroups.Appearance.v2");
  return message;
}
constexpr WPARAM previewRadiusMask = 0x1f;
constexpr WPARAM previewOpacityMask = 0x7f;
constexpr WPARAM previewGlass = 1u << 12, previewShadow = 1u << 13, previewEnabled = 1u << 14;
enum Command { NewGroup = 1, RenameGroup, DeleteGroup, OpenItem, RemoveItem, RenameItem, ToggleGroup,
               MapFolder, RemoveTab, IconView, ListViewMode, ManualSort, NameSort, TypeSort, ModifiedSort,
               BoxColor, ResetBoxColor, MoveTabLeft, MoveTabRight, BoxIcon, ClearBoxIcon,
               TitleText, TitleIcon, ScrollDisplay, PageDisplay,
               ToggleBoxGlass, ToggleAutoCollapse,
               CopyItems, CutItems, PasteIntoBox, DeleteItems, PermanentDeleteItems, ItemProperties,
               NewBoxFolder, NewBoxTextFile, ToggleHoverMenu, EditAutoRule,
               ToggleHideAllBoxes, ToggleDoubleClickHide, ExportBackup, ImportBackup,
               IconSize24, IconSize32, IconSize48, IconSize64, IconSize96,
               OpenFolderInExplorer, RenameTab, DetachTabToBox };
constexpr LRESULT resizeTitle = 0x100;
using quick::popup::Branch;
using quick::popup::Symbol;

constexpr auto desktopSettings = L"Software\\FolderNote";
inline DWORD appearanceValue(const wchar_t *name, DWORD fallback) {
  DWORD value = fallback, bytes = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, desktopSettings, name, RRF_RT_REG_DWORD,
                      nullptr, &value, &bytes) == ERROR_SUCCESS ? value : fallback;
}
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

inline const Tab *extraTab(const Group &group, size_t index) {
  return index && index <= group.tabs.size() ? &group.tabs[index - 1] : nullptr;
}

inline const std::vector<std::wstring> &manualItems(const Group &group, size_t index) {
  const auto tab = extraTab(group, index);
  return tab ? tab->paths : group.paths;
}

inline std::vector<std::wstring> &manualItems(Group &group, size_t index) {
  return index ? group.tabs[index - 1].paths : group.paths;
}

inline HWND receiver() { return FindWindowW(receiverClass, nullptr); }
constexpr auto readyProperty = L"FolderNote.DesktopGroups.Ready";
constexpr auto legacyManagedIconsProperty = L"FolderNote.DesktopGroups.ManagedIcons";

inline bool startup(const std::wstring &exe) {
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

inline bool receiverReady(HWND window, const DesktopLaunchContext &context) {
  return window && GetPropW(window, readyProperty) &&
         desktopReceiverStatus(window, context) == DesktopReceiverStatus::Compatible;
}

inline bool launch(const std::wstring &exe, const wchar_t *arguments, const DesktopLaunchContext &context, bool waitReady) {
  if (context.mode == DesktopLaunchMode::Unavailable) return false;
  PROCESS_INFORMATION process{};
  if (context.mode == DesktopLaunchMode::Explorer) {
    const auto delegated = std::wstring(arguments) + L" --from-desktop-shell";
    if (!executeFromDesktop(exe, delegated.c_str(), context)) return false;
  } else {
    auto command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startupInfo{sizeof(startupInfo)};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &process))
      return false;
    if (!waitReady) AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
  }
  bool ready = !waitReady;
  for (int i = 0; waitReady && i < 100; ++i) {
    if ((ready = receiverReady(receiver(), context))) break;
    if (!process.hProcess) Sleep(50);
    else if (WaitForSingleObject(process.hProcess, 50) != WAIT_TIMEOUT) break;
  }
  if (process.hProcess) CloseHandle(process.hProcess);
  return ready;
}

inline bool notifyManager(HWND window, UINT message = manageMessage, POINT point = {}) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid) AllowSetForegroundWindow(pid);
  return PostMessageW(window, message, static_cast<WPARAM>(static_cast<INT_PTR>(point.x)), point.y) != FALSE;
}

inline bool stopProcess(HWND window = receiver()) {
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

inline void report(HWND owner, const wchar_t *message) {
  ui::message(owner, message, lang::text(L"桌面盒子"), MB_OK | MB_ICONERROR);
}

inline void menuItem(Branch &menu, unsigned id, const wchar_t *text, Symbol symbol, bool selected = false) {
  quick::popup::Item item;
  item.title = lang::text(text);
  item.command = id;
  item.symbol = symbol;
  item.checked = selected;
  menu.items.push_back(std::move(item));
}

inline void submenuItem(Branch &menu, const wchar_t *text, Symbol symbol, std::shared_ptr<Branch> children) {
  if (!children || children->items.empty()) return;
  quick::popup::Item item;
  item.title = lang::text(text);
  item.symbol = symbol;
  item.children = std::move(children);
  menu.items.push_back(std::move(item));
}

inline void separator(Branch &menu) {
  quick::popup::Item item;
  item.row = quick::popup::Row::Separator;
  menu.items.push_back(std::move(item));
}

inline std::wstring pickFolder(HWND owner) {
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
  bool allowEmpty = false;
  size_t maxLength = groupNameLimit;
  ui::Theme theme;
  HFONT font = nullptr, heading = nullptr;
  HWND edit = nullptr;

  ~BoxDialog() {
    if (font) DeleteObject(font);
    if (heading) DeleteObject(heading);
  }
};

inline INT_PTR CALLBACK boxDialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
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
      SendMessageW(state->edit, EM_SETLIMITTEXT, state->maxLength, 0);
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
    if (state->name && !state->allowEmpty && state->name->find_first_not_of(L" \t\r\n") == std::wstring::npos)
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
      if (state->allowEmpty) {
        EnableWindow(GetDlgItem(dialog, IDOK), TRUE);
      } else {
        std::wstring input(state->maxLength + 1, L'\0');
        const auto len = GetWindowTextW(state->edit, input.data(), static_cast<int>(input.size()));
        input.resize(len > 0 ? static_cast<size_t>(len) : 0);
        EnableWindow(GetDlgItem(dialog, IDOK), std::wstring_view(input).find_first_not_of(L" \t\r\n") != std::wstring_view::npos);
      }
      return TRUE;
    }
    if (LOWORD(w) == IDOK) {
      if (state->name) {
        std::wstring buffer(state->maxLength + 1, L'\0');
        const auto len = GetWindowTextW(state->edit, buffer.data(), static_cast<int>(buffer.size()));
        buffer.resize(len > 0 ? static_cast<size_t>(len) : 0);
        const std::wstring_view value(buffer);
        const auto start = value.find_first_not_of(L" \t\r\n");
        if (start == value.npos) {
          if (!state->allowEmpty) return TRUE;
          state->name->clear();
        } else {
          *state->name = value.substr(start, value.find_last_not_of(L" \t\r\n") - start + 1);
        }
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

inline bool showBoxDialog(HINSTANCE instance, HWND owner, const wchar_t *title, std::wstring *name, std::wstring prompt,
                   bool allowEmpty = false, size_t maxLength = groupNameLimit) {
  struct Template {
    DLGTEMPLATE dialog;
    WORD menu = 0, cls = 0, title[64]{};
  };
  alignas(DWORD) Template layout{};
  layout.dialog.style = WS_POPUP | WS_CLIPCHILDREN | DS_CENTER;
  layout.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW;
  layout.dialog.cx = 220;
  layout.dialog.cy = 110;
  BoxDialog state{lang::text(title), name, std::move(prompt), allowEmpty, maxLength};
  wcsncpy_s(reinterpret_cast<wchar_t *>(layout.title), ARRAYSIZE(layout.title), state.title, _TRUNCATE);
  const auto result = DialogBoxIndirectParamW(instance, &layout.dialog, owner, boxDialog, reinterpret_cast<LPARAM>(&state));
  if (result == -1) fn::fail(lang::text(L"无法打开桌面盒子。"));
  return result == IDOK;
}

inline std::wstring pickBoxIcon(HWND owner) {
  wchar_t path[32768]{};
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = owner;
  dialog.lpstrFile = path;
  dialog.nMaxFile = ARRAYSIZE(path);
  dialog.lpstrTitle = lang::text(L"更换图标");
  dialog.lpstrFilter = L"Icon (*.ico)\0*.ico\0All files (*.*)\0*.*\0\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&dialog) ? std::wstring(path) : std::wstring{};
}

inline RECT currentWorkArea(RECT bounds, HMONITOR target = nullptr) {
  MONITORINFO monitor{sizeof(monitor)};
  if (!target) target = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
  return GetMonitorInfoW(target, &monitor) ? monitor.rcWork : RECT{};
}

inline RECT visibleBounds(RECT bounds, HMONITOR target = nullptr) {
  const auto work = currentWorkArea(bounds, target);
  if (IsRectEmpty(&work)) return bounds;
  const auto width = std::clamp(bounds.right - bounds.left, 1L, work.right - work.left);
  const auto height = std::clamp(bounds.bottom - bounds.top, 1L, work.bottom - work.top);
  const auto x = std::clamp(bounds.left, work.left, work.right - width);
  const auto y = std::clamp(bounds.top, work.top, work.bottom - height);
  return {x, y, x + width, y + height};
}

inline RECT resolvedGroupBounds(const Group &group, HMONITOR target = nullptr) {
  const auto work = currentWorkArea(group.bounds, target);
  if (IsRectEmpty(&work)) return group.bounds;
  return scaleBoundsToWorkArea(group.bounds, group.referenceWorkArea, work);
}

class Desktop;
enum class DockSide { None, Left, Top, Right, Bottom };

inline DockSide touchingWorkEdge(const RECT &bounds, DockSide preferred, POINT movement = {}) {
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
  Microsoft::WRL::ComPtr<IDropTarget> dropTarget;
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
  quick_find::Popup findPopup;
  std::vector<std::wstring> findNames;
  bool findInput(UINT message, WPARAM key);
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
  struct CachedShellInfo {
    std::wstring label;
    int smallIcon = -1;
    int largeIcon = -1;
    std::optional<std::wstring> hoverFolder;
  };
  std::map<std::wstring, CachedShellInfo> shellInfoCache;
  HIMAGELIST cachedSmallSysImages = nullptr;
  HIMAGELIST cachedLargeSysImages = nullptr;
  int hoveredFolderRow = -1;
  int suppressedHoverRow = -1;
  ULONGLONG hoveredFolderSince = 0;
  struct TransferEntry {
    uint64_t id = 0;
    std::wstring tokenPath;
    std::wstring realPath;
    std::wstring iconSourcePath;
    std::wstring displayLabel;
    size_t tab = 0;
    size_t insertSlot = SIZE_MAX;
    bool isDirectory = false;
    bool placeholder = false;
    bool outgoing = false;
    bool dragLifted = false;
    bool finished = false;
    double visualProgress = 0.08;
    std::shared_ptr<std::atomic<double>> sharedProgress;
    ULONGLONG startedAt = 0;
    ULONGLONG finishedAt = 0;
  };
  std::vector<TransferEntry> activeTransfers;
  void syncSurfaceTransfers();
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
  std::optional<int> blurPreview;
  int renderedBlur = defaultBlurStrength;
  const WallpaperFrame *tintedWallpaperSource = nullptr;
  COLORREF tintedWallpaperColor = 0;
  int tintedWallpaperOpacity = -1;
  std::vector<DWORD> tintedWallpaperPixels;
  std::optional<int> spacingPreview;
  std::optional<COLORREF> colorPreview;
  std::optional<COLORREF> mergeTint;
  int backgroundOpacity() const;
  int backgroundBlur() const;
  bool backgroundGlass() const;
  COLORREF backgroundColor() const;
  void applyBackground();
  bool saveColor(std::optional<COLORREF> tint);
  bool saveSpacing(int spacing);
  bool autoCollapsed = false;
  ULONGLONG autoCollapseAt = 0;
  bool collapsed() const;
  void tickAutoCollapse(bool pointerInside, ULONGLONG now);
  bool movingOrSizing = false;
  RECT gestureBounds{};
  RECT gestureTarget{};
  HWND alignmentGuides[2]{};
  void snapGesture(RECT &bounds);
  ULONGLONG lastCompositionFrame = 0;
  void stopGesture();
  void beginGesture(WPARAM edge, POINT point, bool capture);
  void updateGestureTarget(POINT point);
  void updateGestureFrame();
  void finishGesture(bool accept);
  POINT gestureStart{};
  std::optional<size_t> pressedTab;
  std::optional<size_t> hoveredTab;
  int tabWheelRemainder = 0;
  HWND titleEdit = nullptr;
  std::wstring pendingTitle;
  void beginTitleRename();
  void queueTitleRename(std::wstring name);
  void finishTitleEdit(bool cancel);
  static LRESULT CALLBACK titleEditInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
  HWND itemEdit = nullptr;
  std::wstring itemEditPath, itemEditOriginal;
  struct PendingItemRename { std::wstring path, originalName, newName; };
  std::optional<PendingItemRename> pendingItemRename;
  void beginItemRename(int row);
  void queueItemRename(std::wstring path, std::wstring originalName, std::wstring newName);
  void finishItemEdit(bool cancel);
  static LRESULT CALLBACK itemEditInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
  WPARAM gestureEdge = 0;
  bool gestureEnding = false;
  bool gestureCanMerge = true;
  struct CollapsePress { POINT start; ULONGLONG pressedAt; bool armed = false, moving = false, canceled = false; };
  std::optional<CollapsePress> collapsePress;
  void beginCollapseHold(POINT start);
  void moveCollapseHold(POINT screen);
  bool finishCollapseHold();
  void cancelCollapseHold();
  void showCollapseMove(bool show);
  struct TabDrag {
    size_t tab;
    POINT start, anchor;
    RECT previewBounds{};
    HWND preview = nullptr;
    bool canDetach = false;
    HWND target = nullptr;
    size_t slot = 0;
  };
  std::optional<TabDrag> tabDrag;
  std::optional<size_t> tabInsertionMark;
  struct TabDrop { size_t tab; RECT bounds; HWND target; size_t slot; };
  std::optional<TabDrop> pendingTabDrop;
  void beginTabDrag(size_t tab, POINT screen, RECT tabBounds);
  void moveTabDrag(POINT screen);
  void endTabDrag(POINT screen);
  void cancelTabDrag();
  static LRESULT CALLBACK tabPreviewProc(HWND window, UINT message, WPARAM w, LPARAM l);
  void place(RECT bounds);
  Panel(Desktop &owner, size_t position) : desktop(owner), index(position) {}
  ~Panel() {
    if (window) KillTimer(window, collapseHoldTimer);
    if (tabDrag && tabDrag->preview) DestroyWindow(tabDrag->preview);
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
  bool previewSpaceDown = false;
  bool preview(int row);
  void chooseMatch(int match, bool focus = false);
  void applyTheme(const ui::Palette &colors);
  void paintBackground(HDC dc, POINT offset = {});
  int headerHeight() const;
  int cornerDiameter() const;
  SIZE shapeSize{};
  int shapeDiameter = 0;
  bool updateShape();
  bool mapped() const;
  bool folderView() const;
  std::wstring activeFolder() const;
  std::wstring hoverFolderPathAt(int row);
  void tickFolderHover(POINT cursor, bool insideWindow, ULONGLONG now);
  void showFolderHoverMenu(int row);
  bool handleShortcut(UINT vk, bool ctrl, bool shift, bool alt);
  std::vector<std::wstring> selectedResolvedPaths() const;
  void selectAndRevealPath(const std::wstring &path, bool startRename = false);
  void readMappedFolder();
  void showTab(size_t tab, bool allowDuringDrag = false);
  std::optional<size_t> tabAt(POINT screen) const;
  std::optional<size_t> tabInsertion(POINT screen) const;
  void showTabInsertion(std::optional<size_t> slot);
  void paintTabInsertion(HDC dc) const;
  bool wheelOnTab(POINT screen, int delta);
  bool wheelTabs(int delta);
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
  bool middleDown = false;
  POINT middleDownPoint{};
  void handleMiddleClick(POINT screen);
#ifdef FOLDERNOTE_WINUI
  void openSurface();
#endif
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
};

class Desktop {
public:
  HINSTANCE instance;
  HWND window = nullptr;
#ifdef FOLDERNOTE_WINUI
  bool useWinui = false;
#endif
  std::vector<Group> groups;
  std::vector<std::unique_ptr<Panel>> panels;
  std::vector<std::wstring> roots = desktopRoots();
  ui::Theme theme;
  Appearance style = configuredAppearance();
  bool previewingAppearance = false;
  UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  bool quitting = false;
  bool menuOpen = false;
  bool editing = false;
  bool dragging = false;
  bool oleDragActive = false;
  bool oleDragFromMapped = false;
  HWND dragWindow = nullptr;
  struct InternalDrop {
    HWND targetPanel = nullptr;
    std::wstring folder;
    POINT point{};
    DWORD effect = DROPEFFECT_MOVE;
  };
  std::optional<InternalDrop> pendingInternalDrop;
  HWND dropHighlight = nullptr;
  RECT dropMarker{};
  ULONGLONG lastDragScroll = 0;
  std::vector<std::wstring> dragItems;
  std::vector<std::wstring> cutPaths;
  uint64_t nextTransferId = 1;
  bool transferTimerRunning = false;
  struct CompletedTransfer {
    uint64_t id = 0;
    HWND panelWindow = nullptr;
    std::wstring sourcePath;
    std::wstring resultPath;
    bool isMove = false;
    bool intoMapped = false;
  };
  struct TransferCompletionQueue {
    std::mutex mutex;
    std::vector<CompletedTransfer> items;
  };
  std::shared_ptr<TransferCompletionQueue> completionQueue = std::make_shared<TransferCompletionQueue>();
  void ensureTransferTimer();
  void addSettlePulse(Panel &panel, const std::wstring &storedPath);
  void addOutgoingFarewell(Panel &panel, const std::wstring &removedPath, size_t slotHint, bool isDir);
  bool enqueueAsyncTransferIntoPanel(Panel &panel, const std::wstring &sourcePath, size_t insertSlot, bool isMove);
  void handleCompletedTransfers();
  void tickTransfers();
  bool boxesHidden = appearanceValue(L"DesktopGroupsHidden", 0) != 0;
  ULONGLONG lastFolderRefresh = 0;
  HWND host = nullptr;
  bool rebuilding = false;
  std::map<int, std::unique_ptr<DesktopGlass>> glass;
  std::unique_ptr<PreviewKeys> previewKeys;
  std::optional<PreviewTarget> previewTarget() const;
  std::optional<search::Entry> pendingSearch;
  ULONGLONG searchDeadline = 0;
  void locateSearch();
  void updateGlass(HWND raisedPanel = nullptr);
  bool ensureHost();

  explicit Desktop(HINSTANCE module) : instance(module), groups(load()) {
    ensureHiddenBoxStorage(roots);
  }
  ~Desktop() {
    quitting = true;
    previewKeys.reset();
    panels.clear();
    if (window) DestroyWindow(window);
  }
  bool openPanel(size_t index);
  void updateShadows();
  void highlight(HWND target);
  void clearDropFeedback();
  void setAppearance(const Appearance &value);
  void lower(HWND raisedPanel = nullptr);
  Panel *mergeTargetAt(POINT point, const Panel &source) const;
  Panel *panelAt(POINT point) const;
  Panel *panelForWindow(HWND handle) const;
  bool desktopDropAt(POINT point) const;
  std::wstring dropFolder(Panel *panel, POINT point) const;
  bool canMoveInto(std::span<const std::wstring> paths, const std::wstring &folder) const;
  void moveInto(std::span<const std::wstring> paths, const std::wstring &folder, HWND owner,
                DWORD effect = DROPEFFECT_MOVE);
  std::wstring boxItem(const std::wstring &path) const;
  bool commit(std::vector<Group> changed, HWND owner, bool refreshContents = true);
  bool releaseItems(std::vector<Group> changed, std::span<const std::wstring> paths, HWND owner,
                    bool refreshContents = true);
  bool mergePanels(size_t source, size_t target);
  void detachPanelTab(size_t index, size_t tab, RECT bounds);
  void movePanelTab(Panel &source, size_t tab, Panel &target, size_t slot, bool selectMoved);
  bool ungroup(std::span<const std::wstring> paths, HWND owner);
  void manage();
  void action(Panel *panel, Command command, size_t tab = 0);
  void createBox(POINT point, HWND owner);
  void renameBox(Panel &panel, std::wstring name);
  void renameItem(Panel &panel, std::wstring oldPath, std::wstring originalName, std::wstring newName);
  void drag(Panel &panel, int index);
  bool assign(Panel &panel, std::span<const std::wstring> paths, POINT point, bool fromExplorer = false);
  bool assignCopyOrLink(Panel &panel, std::span<const std::wstring> paths, POINT point, DWORD effect);
  void copySelection(Panel &panel, bool cut);
  void pasteIntoPanel(Panel &panel);
  void deleteSelection(Panel &panel, bool permanent);
  void createNewItemInPanel(Panel &panel, bool isFolder);
  void applyAutoRules();
  void cleanupMovedCutItems();
  void setAllBoxesHidden(bool hidden);
  bool doubleClickHideEnabled() const;
  void toggleDoubleClickHide();
  void exportBackup(HWND owner);
  void importBackup(HWND owner);
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
};

inline std::vector<std::wstring> desktopDropPaths(IDataObject *data) {
  auto paths = desktopShellDropItems(data);
  FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  struct Medium {
    STGMEDIUM data{};
    ~Medium() { if (data.tymed != TYMED_NULL) ReleaseStgMedium(&data); }
  } medium;
  if (SUCCEEDED(data->GetData(&format, &medium.data)) && medium.data.tymed == TYMED_HGLOBAL) {
    const auto files = static_cast<HDROP>(medium.data.hGlobal);
    const auto count = DragQueryFileW(files, 0xffffffff, nullptr, 0);
    bool readFile = false;
    for (UINT i = 0; i < count; ++i) {
      std::wstring path(static_cast<size_t>(DragQueryFileW(files, i, nullptr, 0)) + 1, L'\0');
      path.resize(DragQueryFileW(files, i, path.data(), static_cast<UINT>(path.size())));
      if (path.empty()) continue;
      if (!readFile) {
        // Prefer the explicit file paths when the source provides them. A
        // Shell list may use a second spelling of the same files.
        std::erase_if(paths, [](const auto &item) { return !isDesktopShellItem(item); });
        readFile = true;
      }
      paths.push_back(std::move(path));
    }
  }
  return paths;
}

inline void setDropEffect(IDataObject *data, const wchar_t *name, DWORD effect) {
  const auto formatId = RegisterClipboardFormatW(name);
  if (!formatId) return;
  STGMEDIUM medium{};
  medium.tymed = TYMED_HGLOBAL;
  medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(effect));
  if (!medium.hGlobal) return;
  if (auto *value = static_cast<DWORD *>(GlobalLock(medium.hGlobal))) {
    *value = effect;
    GlobalUnlock(medium.hGlobal);
    FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (SUCCEEDED(data->SetData(&format, &medium, TRUE))) return;
  }
  GlobalFree(medium.hGlobal);
}

inline std::optional<DWORD> readDropEffect(IDataObject *data, const wchar_t *name) {
  if (!data) return std::nullopt;
  const auto formatId = RegisterClipboardFormatW(name);
  if (!formatId) return std::nullopt;
  FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium)) || medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return std::nullopt;
  std::optional<DWORD> result;
  if (GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
    if (const auto *value = static_cast<const DWORD *>(GlobalLock(medium.hGlobal))) {
      result = *value;
      GlobalUnlock(medium.hGlobal);
    }
  }
  ReleaseStgMedium(&medium);
  return result;
}

constexpr auto recycleBinShellPath = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

inline bool isRecycleBinShellItem(const std::wstring &path) {
  return samePath(path, recycleBinShellPath);
}

inline std::wstring recycleBinDisplayName() {
  Microsoft::WRL::ComPtr<IShellItem> item;
  PWSTR raw = nullptr;
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item))) &&
      item && SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) && raw) {
    std::wstring name(raw);
    CoTaskMemFree(raw);
    if (!name.empty()) return name;
  }
  return std::wstring(lang::text(L"回收站"));
}

inline std::optional<CLSID> readTargetClsid(IDataObject *data) {
  if (!data) return std::nullopt;
  const auto formatId = RegisterClipboardFormatW(CFSTR_TARGETCLSID);
  if (!formatId) return std::nullopt;
  FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium)) || medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return std::nullopt;
  std::optional<CLSID> result;
  if (GlobalSize(medium.hGlobal) >= sizeof(CLSID)) {
    if (const auto *value = static_cast<const CLSID *>(GlobalLock(medium.hGlobal))) {
      result = *value;
      GlobalUnlock(medium.hGlobal);
    }
  }
  ReleaseStgMedium(&medium);
  return result;
}

inline bool desktopIconAt(HWND view, POINT point) {
  using Microsoft::WRL::ComPtr;
  const HWND under = WindowFromPoint(point);
  if (!view || !IsWindowVisible(view) || !under || !IsWindowVisible(under) ||
      (under != view && !IsChild(view, under))) {
    return false;
  }
  ComPtr<IUIAutomation> ui;
  ComPtr<IUIAutomation2> options;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui))) || !ui ||
      FAILED(ui.As(&options)) || !options || FAILED(options->put_ConnectionTimeout(120)) ||
      FAILED(options->put_TransactionTimeout(120))) {
    return false;
  }
  ComPtr<IUIAutomationElement> root, element;
  ComPtr<IUIAutomationTreeWalker> walker;
  if (FAILED(ui->ElementFromHandle(view, &root)) || !root ||
      FAILED(ui->ElementFromPoint(point, &element)) || !element ||
      FAILED(ui->get_RawViewWalker(&walker)) || !walker) {
    return false;
  }
  for (unsigned depth = 0; element && depth < 12; ++depth) {
    BOOL equal = FALSE;
    if (SUCCEEDED(ui->CompareElements(root.Get(), element.Get(), &equal)) && equal) return false;
    CONTROLTYPEID type{};
    if (FAILED(element->get_CurrentControlType(&type))) return false;
    if (type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId) return true;
    ComPtr<IUIAutomationElement> parent;
    if (FAILED(walker->GetParentElement(element.Get(), &parent))) return false;
    element = std::move(parent);
  }
  return false;
}

inline Microsoft::WRL::ComPtr<IDataObject> createDragDataObject(HWND owner, std::span<const std::wstring> paths) {
  if (paths.empty()) return nullptr;
  using ItemId = std::unique_ptr<void, decltype(&CoTaskMemFree)>;
  std::vector<ItemId> ownedIds;
  std::vector<PCIDLIST_ABSOLUTE> ids;
  ownedIds.reserve(paths.size());
  ids.reserve(paths.size());
  for (const auto &path : paths) {
    PIDLIST_ABSOLUTE id = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) || !id) continue;
    ownedIds.emplace_back(static_cast<void *>(id), CoTaskMemFree);
    ids.push_back(id);
  }
  if (ids.empty()) return nullptr;
  Microsoft::WRL::ComPtr<IDataObject> dataObject;
  Microsoft::WRL::ComPtr<IShellItemArray> shellItems;
  if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &shellItems)) && shellItems) {
    shellItems->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dataObject));
  }
  if (!dataObject && ids.size() == 1) {
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(ids.front(), IID_PPV_ARGS(&folder), &child)) && folder && child) {
      folder->GetUIObjectOf(owner, 1, &child, IID_IDataObject, nullptr,
                            reinterpret_cast<void **>(dataObject.GetAddressOf()));
    }
  }
  if (dataObject) {
    Microsoft::WRL::ComPtr<IDataObjectAsyncCapability> async;
    if (SUCCEEDED(dataObject.As(&async)) && async) async->SetAsyncMode(VARIANT_FALSE);
  }
  return dataObject;
}

inline void attachDragImage(IDataObject *data, HICON icon, SIZE iconSize, POINT iconAnchor,
                     const std::wstring &label, size_t itemCount, UINT dpi) {
  if (!data || !icon || iconSize.cx <= 0 || iconSize.cy <= 0) return;
  Microsoft::WRL::ComPtr<IDragSourceHelper> helper;
  if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))) || !helper)
    return;
  Microsoft::WRL::ComPtr<IDragSourceHelper2> helper2;
  if (SUCCEEDED(helper.As(&helper2)) && helper2) helper2->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT);

  const bool showLabel = itemCount <= 1 && !label.empty();
  const bool multiStack = itemCount > 1;
  const LONG stackOffset = multiStack ? std::max(4L, static_cast<LONG>(MulDiv(6, dpi, 96))) : 0;
  const LONG textGap = showLabel ? std::max(2L, static_cast<LONG>(MulDiv(4, dpi, 96))) : 0;
  const LONG textHeight = showLabel ? std::max(24L, static_cast<LONG>(MulDiv(34, dpi, 96))) : 0;
  const LONG canvasW = showLabel
      ? std::max<LONG>(iconSize.cx + MulDiv(36, dpi, 96), MulDiv(104, dpi, 96))
      : iconSize.cx + stackOffset * 2;
  const LONG canvasH = showLabel ? (iconSize.cy + textGap + textHeight) : (iconSize.cy + stackOffset * 2);
  const LONG iconLeft = showLabel ? (canvasW - iconSize.cx) / 2 : stackOffset;
  const LONG iconTop = showLabel ? 0 : stackOffset;

  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), canvasW, -canvasH, 1, 32, BI_RGB};
  void *pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!bitmap || !pixels) {
    if (bitmap) DeleteObject(bitmap);
    return;
  }

  const auto drawIconToArgb = [](HICON hIcon, LONG w, LONG h, std::vector<DWORD> &out) -> bool {
    out.assign(static_cast<size_t>(w) * h, 0);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *buf = nullptr;
    HBITMAP tmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &buf, nullptr, 0);
    if (!tmp || !buf) {
      if (tmp) DeleteObject(tmp);
      return false;
    }
    HDC mem = CreateCompatibleDC(nullptr);
    bool ok = false;
    if (mem) {
      const auto prev = SelectObject(mem, tmp);
      if (prev && prev != HGDI_ERROR) {
        auto *raw = static_cast<DWORD *>(buf);
        const size_t total = static_cast<size_t>(w) * h;
        std::fill_n(raw, total, 0);
        ok = DrawIconEx(mem, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL) != FALSE;
        if (ok) {
          const bool hasAlpha = std::any_of(raw, raw + total, [](DWORD px) { return (px >> 24) != 0; });
          if (!hasAlpha) {
            std::fill_n(raw, total, 0x00ff00ffu);
            ok = DrawIconEx(mem, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL) != FALSE;
            for (size_t i = 0; ok && i < total; ++i) {
              out[i] = (raw[i] & 0x00ffffffu) == 0x00ff00ffu ? 0u : (0xff000000u | (raw[i] & 0x00ffffffu));
            }
          } else {
            for (size_t i = 0; i < total; ++i) {
              const DWORD px = raw[i];
              const BYTE a = static_cast<BYTE>(px >> 24);
              const BYTE r = static_cast<BYTE>(((px >> 16) & 0xff) * a / 255);
              const BYTE g = static_cast<BYTE>(((px >> 8) & 0xff) * a / 255);
              const BYTE b = static_cast<BYTE>((px & 0xff) * a / 255);
              out[i] = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(r) << 16) |
                       (static_cast<DWORD>(g) << 8) | static_cast<DWORD>(b);
            }
          }
        }
        SelectObject(mem, prev);
      }
      DeleteDC(mem);
    }
    DeleteObject(tmp);
    return ok;
  };

  std::vector<DWORD> iconPixels;
  if (!drawIconToArgb(icon, iconSize.cx, iconSize.cy, iconPixels)) {
    DeleteObject(bitmap);
    return;
  }

  auto *canvas = static_cast<DWORD *>(pixels);
  std::fill_n(canvas, static_cast<size_t>(canvasW) * canvasH, 0u);
  const auto blendSprite = [&](LONG dstX, LONG dstY, BYTE opacity) {
    for (LONG y = 0; y < iconSize.cy; ++y) {
      const LONG cy = dstY + y;
      if (cy < 0 || cy >= canvasH) continue;
      for (LONG x = 0; x < iconSize.cx; ++x) {
        const LONG cx = dstX + x;
        if (cx < 0 || cx >= canvasW) continue;
        const DWORD src = iconPixels[static_cast<size_t>(y) * iconSize.cx + x];
        const BYTE sa = static_cast<BYTE>((src >> 24) * opacity / 255);
        if (!sa) continue;
        const BYTE sr = static_cast<BYTE>(((src >> 16) & 0xff) * opacity / 255);
        const BYTE sg = static_cast<BYTE>(((src >> 8) & 0xff) * opacity / 255);
        const BYTE sb = static_cast<BYTE>((src & 0xff) * opacity / 255);
        DWORD &dst = canvas[static_cast<size_t>(cy) * canvasW + cx];
        const BYTE da = static_cast<BYTE>(dst >> 24);
        const BYTE dr = static_cast<BYTE>((dst >> 16) & 0xff);
        const BYTE dg = static_cast<BYTE>((dst >> 8) & 0xff);
        const BYTE db = static_cast<BYTE>(dst & 0xff);
        const BYTE inv = static_cast<BYTE>(255 - sa);
        const BYTE outA = static_cast<BYTE>(sa + da * inv / 255);
        const BYTE outR = static_cast<BYTE>(sr + dr * inv / 255);
        const BYTE outG = static_cast<BYTE>(sg + dg * inv / 255);
        const BYTE outB = static_cast<BYTE>(sb + db * inv / 255);
        dst = (static_cast<DWORD>(outA) << 24) | (static_cast<DWORD>(outR) << 16) |
              (static_cast<DWORD>(outG) << 8) | static_cast<DWORD>(outB);
      }
    }
  };

  if (multiStack) {
    blendSprite(iconLeft + stackOffset, std::max(0L, iconTop - stackOffset / 2), 140);
  }
  blendSprite(iconLeft, iconTop, 226);

  if (showLabel || multiStack) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc) {
      const auto old = SelectObject(dc, bitmap);
      if (old && old != HGDI_ERROR) {
        Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d))) && d2d &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown **>(dwrite.GetAddressOf()))) && dwrite) {
          D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
              D2D1_RENDER_TARGET_TYPE_DEFAULT,
              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
          Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
          RECT bindRect{0, 0, canvasW, canvasH};
          if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &target)) && target &&
              SUCCEEDED(target->BindDC(dc, &bindRect))) {
            target->BeginDraw();
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            if (showLabel) {
              const float fontPx = static_cast<float>(std::max(11, MulDiv(12, dpi, 96)));
              Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
              if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                     fontPx, L"", &fmt)) && fmt) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis)) && ellipsis) {
                  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                  fmt->SetTrimming(&trim, ellipsis.Get());
                }
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
                target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.82f), &shadowBrush);
                target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &textBrush);
                const float left = 2.0f;
                const float top = static_cast<float>(iconTop + iconSize.cy + textGap);
                const float right = static_cast<float>(canvasW - 2);
                const float bottom = static_cast<float>(canvasH);
                if (shadowBrush) {
                  target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), fmt.Get(),
                                    D2D1::RectF(left + 1.0f, top + 1.0f, right + 1.0f, bottom + 1.0f), shadowBrush.Get());
                }
                if (textBrush) {
                  target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), fmt.Get(),
                                    D2D1::RectF(left, top, right, bottom), textBrush.Get());
                }
              }
            } else if (multiStack) {
              const auto countText = std::to_wstring(itemCount);
              const float fontPx = static_cast<float>(std::max(11, MulDiv(12, dpi, 96)));
              Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
              if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                     fontPx, L"", &fmt)) && fmt) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const float badgeH = static_cast<float>(std::max(18, MulDiv(20, dpi, 96)));
                const float badgeW = std::max(badgeH, badgeH * 0.65f + countText.size() * fontPx * 0.62f);
                const float cx = iconLeft + iconSize.cx * 0.5f;
                const float cy = iconTop + iconSize.cy * 0.5f;
                const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                    D2D1::RectF(cx - badgeW * 0.5f, cy - badgeH * 0.5f, cx + badgeW * 0.5f, cy + badgeH * 0.5f),
                    4.0f, 4.0f);
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fillBrush;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
                target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.42f, 0.78f, 0.95f), &fillBrush);
                target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &borderBrush);
                if (fillBrush) target->FillRoundedRectangle(pill, fillBrush.Get());
                if (borderBrush) {
                  target->DrawRoundedRectangle(pill, borderBrush.Get(), 1.5f);
                  target->DrawTextW(countText.c_str(), static_cast<UINT32>(countText.size()), fmt.Get(),
                                    pill.rect, borderBrush.Get());
                }
              }
            }
            target->EndDraw();
          }
        }
        SelectObject(dc, old);
      }
      DeleteDC(dc);
    }
  }

  SHDRAGIMAGE image{};
  image.sizeDragImage = {canvasW, canvasH};
  image.ptOffset = {std::clamp(iconLeft + iconAnchor.x, 0L, canvasW - 1),
                    std::clamp(iconTop + iconAnchor.y, 0L, canvasH - 1)};
  image.hbmpDragImage = bitmap;
  image.crColorKey = CLR_NONE;
  if (FAILED(helper->InitializeFromBitmap(&image, data))) DeleteObject(bitmap);
}

inline void setDropDescription(IDataObject *data, DROPIMAGETYPE type, const wchar_t *message = L"",
                        const wchar_t *insert = L"") {
  if (!data) return;
  static const UINT formatId = RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION);
  if (!formatId) return;
  STGMEDIUM medium{};
  medium.tymed = TYMED_HGLOBAL;
  medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPDESCRIPTION));
  if (!medium.hGlobal) return;
  if (auto *desc = static_cast<DROPDESCRIPTION *>(GlobalLock(medium.hGlobal))) {
    desc->type = type;
    if (message) wcsncpy_s(desc->szMessage, message, _TRUNCATE);
    if (insert) wcsncpy_s(desc->szInsert, insert, _TRUNCATE);
    GlobalUnlock(medium.hGlobal);
    FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (SUCCEEDED(data->SetData(&format, &medium, TRUE))) return;
  }
  GlobalFree(medium.hGlobal);
}

inline bool itemEditInfo(const std::wstring &path, std::wstring &editText, int &selectLength) {
  editText.clear();
  selectLength = 0;
  if (path.empty()) return false;
  PIDLIST_ABSOLUTE parsed = nullptr;
  if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr)) || !parsed) return false;
  const std::unique_ptr<void, decltype(&CoTaskMemFree)> owned(static_cast<void *>(parsed), CoTaskMemFree);
  Microsoft::WRL::ComPtr<IShellFolder> folder;
  PCUITEMID_CHILD child = nullptr;
  if (FAILED(SHBindToParent(parsed, IID_PPV_ARGS(&folder), &child)) || !folder || !child) return false;
  SFGAOF attributes = SFGAO_CANRENAME | SFGAO_FOLDER;
  if (FAILED(folder->GetAttributesOf(1, &child, &attributes)) || !(attributes & SFGAO_CANRENAME)) return false;
  STRRET str{};
  if (FAILED(folder->GetDisplayNameOf(child, SHGDN_FOREDITING | SHGDN_INFOLDER, &str))) return false;
  wchar_t buffer[MAX_PATH * 2]{};
  if (FAILED(StrRetToBufW(&str, child, buffer, ARRAYSIZE(buffer))) || !buffer[0]) return false;
  editText = buffer;
  selectLength = static_cast<int>(editText.size());
  if (!(attributes & SFGAO_FOLDER) && !isDesktopShellItem(path)) {
    const auto extension = fn::fs::path(path).extension().wstring();
    if (!extension.empty() && editText.size() > extension.size() &&
        _wcsicmp(editText.c_str() + editText.size() - extension.size(), extension.c_str()) == 0) {
      selectLength = static_cast<int>(editText.size() - extension.size());
    }
  }
  return true;
}

class DropTarget final : public IDropTarget {
  Panel &panel;
  ULONG references = 1;
  Microsoft::WRL::ComPtr<IDropTargetHelper> dragImage;
  Microsoft::WRL::ComPtr<IDataObject> currentData;
  bool files = false;
  bool hasShellItems = false;
  DROPIMAGETYPE lastDescType = DROPIMAGE_INVALID;
  std::wstring lastDescMessage;
  std::wstring lastDescInsert;
  std::optional<size_t> dragHoverTab;
  ULONGLONG dragHoverTabSince = 0;

  void clearMarker() {
#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) panel.surface->showItemInsertion(std::nullopt);
#endif
    if (!IsRectEmpty(&panel.desktop.dropMarker)) {
      const RECT previous = panel.desktop.dropMarker;
      panel.desktop.dropMarker = {};
      InvalidateRect(panel.list, &previous, FALSE);
    }
  }

  void updateDescription(IDataObject *data, DWORD allowed, const std::wstring &folder, bool groupDrop) {
    if (!data) return;
    DROPIMAGETYPE type = DROPIMAGE_NONE;
    std::wstring message;
    std::wstring insert;
    if (allowed) {
      const auto &group = panel.desktop.groups[panel.index];
      const auto targetLabel = [&]() -> std::wstring {
        if (!folder.empty()) {
          if (isRecycleBinShellItem(folder)) return recycleBinDisplayName();
          const auto targetName = fn::fs::path(folder).filename().wstring();
          return targetName.empty() ? folder : targetName;
        }
        if (panel.activeTab > 0 && panel.activeTab <= group.tabs.size() && !group.tabs[panel.activeTab - 1].name.empty()) {
          return group.tabs[panel.activeTab - 1].name;
        }
        return group.name;
      }();
      if (folder.empty() && groupDrop && allowed == DROPEFFECT_MOVE &&
          panel.desktop.oleDragActive && panel.desktop.dragWindow == panel.window) {
        type = DROPIMAGE_NOIMAGE;
      } else if (allowed == DROPEFFECT_MOVE) {
        type = DROPIMAGE_MOVE;
        message = lang::text(L"移动到 %1");
        insert = targetLabel;
      } else if (allowed == DROPEFFECT_LINK) {
        type = DROPIMAGE_LINK;
        message = lang::text(hasShellItems && !files ? L"添加到 %1" : L"在 %1 中创建链接");
        insert = targetLabel;
      } else {
        type = DROPIMAGE_COPY;
        message = lang::text(L"复制到 %1");
        insert = targetLabel;
      }
    }
    if (type == lastDescType && message == lastDescMessage && insert == lastDescInsert) return;
    lastDescType = type;
    lastDescMessage = message;
    lastDescInsert = insert;
    setDropDescription(data, type, message.c_str(), insert.c_str());
  }

  static DWORD applyModifierEffect(DWORD keyState, DWORD allowed, DWORD preferred) {
    constexpr DWORD keyAlt = 0x20;
    const bool ctrl = (keyState & MK_CONTROL) != 0;
    const bool shift = (keyState & MK_SHIFT) != 0;
    const bool alt = (keyState & keyAlt) != 0;
    if ((alt || (ctrl && shift)) && (allowed & DROPEFFECT_LINK)) return DROPEFFECT_LINK;
    if (ctrl && !shift && (allowed & DROPEFFECT_COPY)) return DROPEFFECT_COPY;
    if (shift && !ctrl && (allowed & DROPEFFECT_MOVE)) return DROPEFFECT_MOVE;
    if (allowed & preferred) return preferred;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return allowed & DROPEFFECT_LINK;
  }

  DWORD externalEffect(DWORD keyState, DWORD allowed) const {
    if (panel.folderView() || panel.desktop.editing || panel.desktop.quitting) return DROPEFFECT_NONE;
    if (files && !hasShellItems) {
      return applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
    }
    if (!hasShellItems) return DROPEFFECT_NONE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return allowed & DROPEFFECT_COPY;
  }

  DWORD evaluate(POINT screen, DWORD keyState, DWORD allowed, std::wstring &folder, bool &groupDrop) const {
    folder.clear();
    groupDrop = false;
    if (panel.desktop.editing || panel.desktop.quitting) return DROPEFFECT_NONE;
    if (panel.desktop.oleDragActive) {
      const auto &paths = panel.desktop.dragItems;
      const bool virtualItems = std::any_of(paths.begin(), paths.end(), isDesktopShellItem);
      auto candidate = !panel.folderView() && virtualItems ? std::wstring{} : panel.desktop.dropFolder(&panel, screen);
      if (candidate.empty() && panel.folderView() && !virtualItems) {
        candidate = panel.activeFolder();
      }
      if (!candidate.empty()) {
        const DWORD chosen = isRecycleBinShellItem(candidate)
            ? (allowed & DROPEFFECT_MOVE)
            : applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
        if (chosen == DROPEFFECT_MOVE ? panel.desktop.canMoveInto(paths, candidate) : (chosen != DROPEFFECT_NONE)) {
          folder = candidate;
          return chosen;
        }
        return DROPEFFECT_NONE;
      }
      if (panel.folderView()) return DROPEFFECT_NONE;
      if (virtualItems) {
        if (!panel.desktop.oleDragFromMapped && (allowed & DROPEFFECT_MOVE)) {
          groupDrop = true;
          return DROPEFFECT_MOVE;
        }
        groupDrop = (allowed & DROPEFFECT_LINK) != 0;
        return allowed & DROPEFFECT_LINK;
      }
      const DWORD chosen = applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
      groupDrop = chosen != DROPEFFECT_NONE;
      return chosen;
    }
    if (files && !hasShellItems) {
      const auto hitTarget = panel.desktop.dropFolder(&panel, screen);
      if (isRecycleBinShellItem(hitTarget) && (allowed & DROPEFFECT_MOVE)) {
        folder = hitTarget;
        return DROPEFFECT_MOVE;
      }
    }
    if (panel.folderView() && files && !hasShellItems) {
      folder = panel.activeFolder();
      return folder.empty() ? DROPEFFECT_NONE : applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
    }
    return externalEffect(keyState, allowed);
  }

  void updateFeedback(POINT screen, DWORD allowed, const std::wstring &folder, bool groupDrop) {
    const auto highlighted = allowed != DROPEFFECT_NONE ? panel.window : nullptr;
    RECT marker{};
    if (groupDrop && folder.empty() && highlighted) {
      panel.autoscroll(screen);
      marker = panel.insertion(screen).marker;
    }
    const bool changedTarget = highlighted != panel.desktop.dropHighlight;
    if (changedTarget) {
      if (panel.desktop.dropHighlight) {
        const auto previous = std::find_if(panel.desktop.panels.begin(), panel.desktop.panels.end(), [&](const auto &item) {
          return item->window == panel.desktop.dropHighlight;
        });
        if (previous != panel.desktop.panels.end()) {
#ifdef FOLDERNOTE_WINUI
          if ((*previous)->hasSurface()) (*previous)->surface->showItemInsertion(std::nullopt);
#endif
          InvalidateRect((*previous)->list, &panel.desktop.dropMarker, FALSE);
        }
      }
      panel.desktop.highlight(highlighted);
    }
#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) {
      panel.surface->showItemInsertion(highlighted && !IsRectEmpty(&marker) ? std::optional<RECT>(marker) : std::nullopt);
    }
#endif
    if (!EqualRect(&marker, &panel.desktop.dropMarker)) {
      if (highlighted && !panel.hasSurface()) {
        InvalidateRect(panel.list, &panel.desktop.dropMarker, FALSE);
        InvalidateRect(panel.list, &marker, FALSE);
      }
      panel.desktop.dropMarker = marker;
    } else if (changedTarget && highlighted && !panel.hasSurface() && !IsRectEmpty(&marker)) {
      InvalidateRect(panel.list, &marker, FALSE);
    }
  }

  void checkSpringLoadedTab(POINT screen) {
    const auto &group = panel.desktop.groups[panel.index];
    const auto tab = !group.tabs.empty() ? panel.tabAt(screen) : std::nullopt;
    if (tab && (*tab != panel.activeTab || panel.collapsed())) {
      const auto now = GetTickCount64();
      if (dragHoverTab != tab) {
        dragHoverTab = tab;
        dragHoverTabSince = now;
      } else if (now - dragHoverTabSince >= 380) {
        dragHoverTab.reset();
        if (panel.collapsed()) {
          panel.desktop.groups[panel.index].collapsed = false;
          panel.autoCollapsed = false;
          panel.setCollapsed();
        }
        panel.showTab(*tab, true);
      }
    } else {
      dragHoverTab.reset();
    }
  }

public:
  explicit DropTarget(Panel &value) : panel(value) {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dragImage));
  }
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
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD *effects) override {
    DragLeave();
    try {
      if (data) {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files = data->QueryGetData(&format) == S_OK;
        if (!files || !(*effects & DROPEFFECT_MOVE)) {
          for (const auto &path : desktopShellDropItems(data)) {
            if (isDesktopShellItem(path)) hasShellItems = true;
            else files = true;
          }
        }
      }
    } catch (...) {
      DragLeave();
    }
    currentData = data;
    POINT screen{point.x, point.y};
    checkSpringLoadedTab(screen);
    std::wstring folder;
    bool groupDrop = false;
    *effects = evaluate(screen, keyState, *effects, folder, groupDrop);
    updateFeedback(screen, *effects, folder, groupDrop);
    updateDescription(data, *effects, folder, groupDrop);
    if (dragImage) dragImage->DragEnter(panel.window, data, &screen, *effects);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD *effects) override {
    POINT screen{point.x, point.y};
    checkSpringLoadedTab(screen);
    std::wstring folder;
    bool groupDrop = false;
    *effects = evaluate(screen, keyState, *effects, folder, groupDrop);
    updateFeedback(screen, *effects, folder, groupDrop);
    updateDescription(currentData.Get(), *effects, folder, groupDrop);
    if (dragImage) dragImage->DragOver(&screen, *effects);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    files = hasShellItems = false;
    dragHoverTab.reset();
    lastDescType = DROPIMAGE_INVALID;
    lastDescMessage.clear();
    lastDescInsert.clear();
    if (currentData) {
      setDropDescription(currentData.Get(), DROPIMAGE_INVALID);
      currentData.Reset();
    }
    clearMarker();
    panel.desktop.highlight(nullptr);
    if (dragImage) dragImage->DragLeave();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keyState, POINTL point, DWORD *effects) override {
    const auto offered = *effects;
    POINT screen{point.x, point.y};
    std::wstring folder;
    bool groupDrop = false;
    const auto allowed = evaluate(screen, keyState, offered, folder, groupDrop);
    *effects = DROPEFFECT_NONE;
    files = hasShellItems = false;
    dragHoverTab.reset();
    lastDescType = DROPIMAGE_INVALID;
    lastDescMessage.clear();
    lastDescInsert.clear();
    currentData.Reset();
    setDropDescription(data, DROPIMAGE_INVALID);
    clearMarker();
    panel.desktop.highlight(nullptr);
    DWORD visualEffect = DROPEFFECT_NONE;
    if (allowed) {
      if (panel.desktop.oleDragActive) {
        panel.desktop.pendingInternalDrop = Desktop::InternalDrop{panel.window, std::move(folder), screen, allowed};
        visualEffect = allowed;
        setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
        setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, DROPEFFECT_NONE);
        *effects = DROPEFFECT_NONE;
      } else {
        try {
          auto paths = (allowed == DROPEFFECT_MOVE || files) ? desktopDropPaths(data) : desktopShellDropItems(data);
          if (!folder.empty()) {
            if (panel.folderView() && samePath(folder, panel.activeFolder()) &&
                (allowed == DROPEFFECT_MOVE || allowed == DROPEFFECT_COPY)) {
              const auto ins = panel.insertion(screen);
              size_t slot = ins.item >= 0
                  ? static_cast<size_t>(ins.item) + (ins.after ? 1 : 0)
                  : panel.orderedItems.size();
              bool anyQueued = false;
              for (const auto &p : paths) {
                if (panel.desktop.enqueueAsyncTransferIntoPanel(panel, p, slot, allowed == DROPEFFECT_MOVE)) {
                  ++slot;
                  anyQueued = true;
                }
              }
              if (anyQueued) {
                visualEffect = allowed;
                setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
                setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, allowed);
                *effects = allowed == DROPEFFECT_MOVE ? (offered & DROPEFFECT_COPY) : allowed;
              }
            } else {
              panel.desktop.moveInto(paths, folder, panel.window, allowed);
              visualEffect = allowed;
              setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
              setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, allowed);
              *effects = allowed == DROPEFFECT_MOVE ? (offered & DROPEFFECT_COPY) : allowed;
            }
          } else if (allowed == DROPEFFECT_MOVE || (hasShellItems && !files)) {
            if (allowed != DROPEFFECT_MOVE)
              std::erase_if(paths, [](const auto &path) { return !isDesktopShellItem(path); });
            if (panel.desktop.assign(panel, paths, screen, true)) {
              visualEffect = allowed;
              if (allowed == DROPEFFECT_MOVE) {
                // The target moved the files. The drag source must not delete anything.
                setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
                setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
                *effects = offered & DROPEFFECT_COPY;
              } else {
                *effects = allowed;
              }
            }
          } else if (panel.desktop.assignCopyOrLink(panel, paths, screen, allowed)) {
            visualEffect = allowed;
            *effects = allowed;
          }
        } catch (...) {
          PostMessageW(panel.window, rejectedMessage, 0, 0);
        }
      }
    }
    if (dragImage) dragImage->Drop(data, &screen, visualEffect);
    return S_OK;
  }
};


// Display name used for box items: shortcuts drop their .lnk/.url extension.
inline std::wstring hoverEntryTitle(const std::wstring &path, bool isDirectory) {
  const fn::fs::path fsPath(path);
  if (!isDirectory) {
    const auto ext = fsPath.extension().wstring();
    if (!_wcsicmp(ext.c_str(), L".lnk") || !_wcsicmp(ext.c_str(), L".url")) {
      const auto stem = fsPath.stem().wstring();
      if (!stem.empty()) return stem;
    }
  }
  auto name = fsPath.filename().wstring();
  return name.empty() ? path : name;
}

class BoxFileMoves {
  const std::vector<std::wstring> &roots;
  HWND owner;
  std::vector<std::pair<std::wstring, std::wstring>> moves;

public:
  BoxFileMoves(const std::vector<std::wstring> &desktopRoots, HWND window, size_t count)
      : roots(desktopRoots), owner(window) {
    moves.reserve(count);
  }
  ~BoxFileMoves() {
    bool restored = true;
    for (auto move = moves.rbegin(); move != moves.rend(); ++move)
      restored &= rollbackBoxMove(move->second, move->first);
    if (!restored)
      report(owner, lang::text(L"无法恢复原文件位置。请在桌面和 .desktop 文件夹中查看文件。"));
  }
  const std::wstring &transfer(const std::wstring &path, bool intoBox) {
    auto original = path;
    auto destination = intoBox ? storeBoxItem(roots, original) : restoreBoxItem(roots, original);
    if (destination.empty()) {
      static const std::wstring empty;
      return empty;
    }
    return moves.emplace_back(std::move(original), std::move(destination)).second;
  }
  void keep() { moves.clear(); }
};

// Native control subclasses installed by Panel::proc.
LRESULT CALLBACK tabInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
LRESULT CALLBACK iconInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
LRESULT CALLBACK toggleInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
}  // namespace desktop_groups::detail
