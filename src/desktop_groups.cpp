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
#include "quick_folders.h"
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

namespace desktop_groups {
namespace {
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

bool isTransferToken(const std::wstring &path) {
  return path.starts_with(transferTokenPrefix);
}
UINT appearanceMessage() {
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
DWORD appearanceValue(const wchar_t *name, DWORD fallback) {
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

HWND receiver() { return FindWindowW(receiverClass, nullptr); }
constexpr auto readyProperty = L"FolderNote.DesktopGroups.Ready";
constexpr auto legacyManagedIconsProperty = L"FolderNote.DesktopGroups.ManagedIcons";

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

bool receiverReady(HWND window, const DesktopLaunchContext &context) {
  return window && GetPropW(window, readyProperty) &&
         desktopReceiverStatus(window, context) == DesktopReceiverStatus::Compatible;
}

bool launch(const std::wstring &exe, const wchar_t *arguments, const DesktopLaunchContext &context, bool waitReady) {
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

bool notifyManager(HWND window, UINT message = manageMessage, POINT point = {}) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid) AllowSetForegroundWindow(pid);
  return PostMessageW(window, message, static_cast<WPARAM>(static_cast<INT_PTR>(point.x)), point.y) != FALSE;
}

bool stopProcess(HWND window = receiver()) {
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

void submenuItem(Branch &menu, const wchar_t *text, Symbol symbol, std::shared_ptr<Branch> children) {
  if (!children || children->items.empty()) return;
  quick::popup::Item item;
  item.title = lang::text(text);
  item.symbol = symbol;
  item.children = std::move(children);
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

bool showBoxDialog(HINSTANCE instance, HWND owner, const wchar_t *title, std::wstring *name, std::wstring prompt,
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

std::wstring pickBoxIcon(HWND owner) {
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

RECT currentWorkArea(RECT bounds, HMONITOR target = nullptr) {
  MONITORINFO monitor{sizeof(monitor)};
  if (!target) target = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
  return GetMonitorInfoW(target, &monitor) ? monitor.rcWork : RECT{};
}

RECT visibleBounds(RECT bounds, HMONITOR target = nullptr) {
  const auto work = currentWorkArea(bounds, target);
  if (IsRectEmpty(&work)) return bounds;
  const auto width = std::clamp(bounds.right - bounds.left, 1L, work.right - work.left);
  const auto height = std::clamp(bounds.bottom - bounds.top, 1L, work.bottom - work.top);
  const auto x = std::clamp(bounds.left, work.left, work.right - width);
  const auto y = std::clamp(bounds.top, work.top, work.bottom - height);
  return {x, y, x + width, y + height};
}

RECT resolvedGroupBounds(const Group &group, HMONITOR target = nullptr) {
  const auto work = currentWorkArea(group.bounds, target);
  if (IsRectEmpty(&work)) return group.bounds;
  return scaleBoundsToWorkArea(group.bounds, group.referenceWorkArea, work);
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

  explicit Desktop(HINSTANCE module) : instance(module), groups(load()) {}
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

std::vector<std::wstring> desktopDropPaths(IDataObject *data) {
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

void setDropEffect(IDataObject *data, const wchar_t *name, DWORD effect) {
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

std::optional<DWORD> readDropEffect(IDataObject *data, const wchar_t *name) {
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

bool isRecycleBinShellItem(const std::wstring &path) {
  return samePath(path, recycleBinShellPath);
}

std::wstring recycleBinDisplayName() {
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

std::optional<CLSID> readTargetClsid(IDataObject *data) {
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

bool desktopIconAt(HWND view, POINT point) {
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

Microsoft::WRL::ComPtr<IDataObject> createDragDataObject(HWND owner, std::span<const std::wstring> paths) {
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

void attachDragImage(IDataObject *data, HICON icon, SIZE iconSize, POINT iconAnchor,
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

void setDropDescription(IDataObject *data, DROPIMAGETYPE type, const wchar_t *message = L"",
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

bool itemEditInfo(const std::wstring &path, std::wstring &editText, int &selectLength) {
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
int Panel::backgroundBlur() const { return blurPreview.value_or(desktop.groups[index].blurStrength); }

bool Panel::backgroundGlass() const {
  return desktop.groups[index].glass.value_or(desktop.style.glass);
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
    surface->appearance(colors.dark, colors.highContrast, backgroundColor(), opacity, backgroundGlass());
    desktop.updateGlass();
    return;
  }
#endif
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
}

bool Panel::saveColor(std::optional<COLORREF> tint) {
  if (desktop.quitting) return false;
  if (desktop.groups[index].tint == tint) return true;
  auto changed = desktop.groups;
  changed[index].tint = tint;
  return desktop.commit(std::move(changed), window, false);
}

bool Panel::saveSpacing(int spacing) {
  if (desktop.quitting) return false;
  if (desktop.groups[index].itemSpacing == spacing) return true;
  auto changed = desktop.groups;
  changed[index].itemSpacing = spacing;
  return desktop.commit(std::move(changed), window, false);
}
#ifdef FOLDERNOTE_WINUI
void Panel::syncSurfaceHeader() {
  if (!hasSurface()) return;
  const auto &group = desktop.groups[index];
  const auto &colors = desktop.theme.colors;
  std::vector<std::wstring> titles{group.name};
  for (const auto &tab : group.tabs) titles.push_back(tab.name);
  surface->header(titles, activeTab, collapsed(), headerActions, desktop.dropHighlight == window,
                  desktop.style.radius, colors.text, colors.muted, colors.border, colors.accent,
                  titleHeightPreview ? titleHeightPreview : group.titleHeight, group.titleStyle,
                  headerIcon());
}
#endif

int Panel::headerHeight() const {
  return MulDiv(titleHeightPreview ? titleHeightPreview : desktop.groups[index].titleHeight, GetDpiForWindow(window), 96);
}

HICON Panel::headerIcon() {
  const auto &group = desktop.groups[index];
  if (!showsTitleIcon(group.titleStyle)) return nullptr;
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
  return !folderForTab(desktop.groups[index], activeTab).empty();
}

std::wstring Panel::activeFolder() const {
  return folderForTab(desktop.groups[index], activeTab);
}

bool Panel::folderView() const {
  return !activeFolder().empty();
}

std::vector<std::wstring> Panel::selectedResolvedPaths() const {
  const bool inFolder = folderView();
  std::vector<std::wstring> paths;
  for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0;) {
    if (static_cast<size_t>(row) >= displayed.size()) continue;
    auto resolved = inFolder ? displayed[row] : desktop.boxItem(displayed[row]);
    if (!resolved.empty()) paths.push_back(std::move(resolved));
  }
  return paths;
}

void Panel::selectAndRevealPath(const std::wstring &path, bool startRename) {
  if (path.empty()) return;
  const auto found = std::find_if(orderedItems.begin(), orderedItems.end(),
                                  [&](const auto &candidate) { return samePath(candidate, path); });
  if (found == orderedItems.end()) return;
  const int match = static_cast<int>(found - orderedItems.begin());
  if (pageCount && pageCapacity && match / pageCapacity != currentPage) goToPage(match / pageCapacity);
  const int row = match - (pageCount ? currentPage * pageCapacity : 0);
  if (row < 0 || static_cast<size_t>(row) >= displayed.size()) return;
  ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_EnsureVisible(list, row, FALSE);
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    surface->select({row});
    surface->reveal(row);
  }
#endif
  if (startRename) PostMessageW(window, beginItemRenameMessage, static_cast<WPARAM>(row), 0);
}

namespace {
struct FolderHoverScan {
  struct Subfolder {
    std::wstring path;
    bool nonEmpty = false;
  };
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false, more = false;
  DWORD error = 0;
  std::vector<Subfolder> folders;
  std::vector<std::wstring> files;
};

bool folderHasVisibleEntries(const std::wstring &path) {
  if (path.empty()) return false;
  const auto extended = path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
  const auto wildcard = extended + (extended.back() == L'\\' ? L"*" : L"\\*");
  WIN32_FIND_DATAW entry{};
  const HANDLE find = FindFirstFileExW(wildcard.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) return false;
  bool hasVisible = false;
  unsigned checked = 0;
  do {
    if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
    if (entry.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
    hasVisible = true;
    break;
  } while (++checked < 512 && FindNextFileW(find, &entry));
  FindClose(find);
  return hasVisible;
}

std::shared_ptr<FolderHoverScan> scanHoverFolder(const std::wstring &path) {
  auto result = std::make_shared<FolderHoverScan>();
  std::thread([result, path] {
    std::vector<std::wstring> rawFolders, files;
    std::vector<FolderHoverScan::Subfolder> folders;
    DWORD error = 0;
    bool more = false;
    try {
      auto extended = path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
      auto wildcard = extended + (extended.back() == L'\\' ? L"*" : L"\\*");
      WIN32_FIND_DATAW entry{};
      HANDLE find = FindFirstFileExW(wildcard.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
                                     nullptr, FIND_FIRST_EX_LARGE_FETCH);
      if (find == INVALID_HANDLE_VALUE) {
        error = GetLastError();
      } else {
        unsigned checked = 0;
        do {
          if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
          if (entry.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
          if (++checked > 4000 || rawFolders.size() + files.size() >= 160) {
            more = true;
            break;
          }
          std::wstring full = path + (path.back() == L'\\' ? L"" : L"\\") + entry.cFileName;
          if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rawFolders.push_back(std::move(full));
          else files.push_back(std::move(full));
        } while (FindNextFileW(find, &entry));
        FindClose(find);
      }
      const auto naturalLess = [](const std::wstring &a, const std::wstring &b) {
        const auto nameA = fn::fs::path(a).filename().wstring();
        const auto nameB = fn::fs::path(b).filename().wstring();
        const int cmp = StrCmpLogicalW(nameA.c_str(), nameB.c_str());
        return cmp != 0 ? cmp < 0 : a < b;
      };
      std::sort(rawFolders.begin(), rawFolders.end(), naturalLess);
      std::sort(files.begin(), files.end(), naturalLess);
      folders.reserve(rawFolders.size());
      for (auto &sub : rawFolders) {
        const bool nonEmpty = folderHasVisibleEntries(sub);
        folders.push_back({std::move(sub), nonEmpty});
      }
    } catch (...) {
      error = ERROR_NOT_ENOUGH_MEMORY;
    }
    {
      std::lock_guard lock(result->mutex);
      result->folders = std::move(folders);
      result->files = std::move(files);
      result->error = error;
      result->more = more;
      result->done = true;
    }
    result->ready.notify_all();
  }).detach();
  return result;
}

std::wstring hoverEntryTitle(const std::wstring &path, bool isDirectory) {
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

std::wstring resolveShortcutFolderTarget(const std::wstring &lnkPath) {
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  Microsoft::WRL::ComPtr<IPersistFile> persist;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link ||
      FAILED(link.As(&persist)) || !persist ||
      FAILED(persist->Load(lnkPath.c_str(), STGM_READ))) {
    return {};
  }
  link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK | (100u << 16));
  wchar_t rawTarget[MAX_PATH]{};
  WIN32_FIND_DATAW findData{};
  if (SUCCEEDED(link->GetPath(rawTarget, ARRAYSIZE(rawTarget), &findData, 0)) && rawTarget[0] &&
      PathIsDirectoryW(rawTarget)) {
    return quick::normalize(rawTarget);
  }
  return {};
}
}

std::wstring Panel::hoverFolderPathAt(int row) {
  if (row < 0 || static_cast<size_t>(row) >= displayed.size()) return {};
  const auto &entryPath = displayed[row];
  if (entryPath.empty() || isTransferToken(entryPath) || isDesktopShellItem(entryPath)) return {};

  auto &slot = shellInfoCache[entryPath];
  if (slot.hoverFolder.has_value()) return *slot.hoverFolder;

  std::wstring resolvedFolder;
  if (folderView()) {
    const auto found = std::find_if(mappedItems.begin(), mappedItems.end(), [&](const auto &m) {
      return m.path == entryPath;
    });
    if (found != mappedItems.end()) {
      if (found->attributes & FILE_ATTRIBUTE_DIRECTORY) resolvedFolder = entryPath;
      else if (!_wcsicmp(fn::fs::path(entryPath).extension().c_str(), L".lnk"))
        resolvedFolder = resolveShortcutFolderTarget(entryPath);
    } else if (PathIsDirectoryW(entryPath.c_str())) {
      resolvedFolder = entryPath;
    }
  } else {
    const auto actual = desktop.boxItem(entryPath);
    if (!actual.empty() && !isDesktopShellItem(actual)) {
      if (PathIsDirectoryW(actual.c_str())) resolvedFolder = actual;
      else if (!_wcsicmp(fn::fs::path(actual).extension().c_str(), L".lnk"))
        resolvedFolder = resolveShortcutFolderTarget(actual);
    }
  }
  slot.hoverFolder = resolvedFolder;
  return resolvedFolder;
}

void Panel::tickFolderHover(POINT cursor, bool insideWindow, ULONGLONG now) {
  if (desktop.quitting || desktop.boxesHidden || desktop.dragging || desktop.editing || desktop.menuOpen ||
      !desktop.groups[index].hoverMenu || collapsed() || dockHidden || movingOrSizing || gestureEdge ||
      tabDrag || itemEdit != nullptr || titleEdit != nullptr ||
      ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0) {
    hoveredFolderRow = -1;
    if (!insideWindow) suppressedHoverRow = -1;
    return;
  }
  if (!insideWindow) {
    hoveredFolderRow = -1;
    suppressedHoverRow = -1;
    return;
  }

  POINT client = cursor;
  if (!ScreenToClient(window, &client) || client.y < headerHeight()) {
    hoveredFolderRow = -1;
    suppressedHoverRow = -1;
    return;
  }

  int row = -1;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) row = surface->hit(cursor);
#endif
  if (!hasSurface() && IsWindowVisible(list)) {
    LVHITTESTINFO hit{};
    hit.pt = cursor;
    ScreenToClient(list, &hit.pt);
    row = ListView_HitTest(list, &hit);
  }
  if (row < 0 || static_cast<size_t>(row) >= displayed.size()) {
    hoveredFolderRow = -1;
    suppressedHoverRow = -1;
    return;
  }
  if (row == suppressedHoverRow) return;
  suppressedHoverRow = -1;

  if (hoverFolderPathAt(row).empty()) {
    hoveredFolderRow = -1;
    return;
  }

  if (row != hoveredFolderRow) {
    hoveredFolderRow = row;
    hoveredFolderSince = now;
    return;
  }

  if (now - hoveredFolderSince >= 360) {
    suppressedHoverRow = row;
    hoveredFolderRow = -1;
    PostMessageW(window, showFolderHoverMessage, static_cast<WPARAM>(row), 0);
  }
}

void Panel::showFolderHoverMenu(int row) {
  if (desktop.quitting || desktop.boxesHidden || desktop.dragging || desktop.editing || desktop.menuOpen ||
      !desktop.groups[index].hoverMenu || collapsed() || dockHidden || movingOrSizing || gestureEdge || tabDrag) {
    return;
  }
  if (((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0) {
    return;
  }
  POINT cursor{};
  if (!GetCursorPos(&cursor) || !insideDesktopWindow(window, WindowFromPoint(cursor))) return;
  int currentHit = -1;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) currentHit = surface->hit(cursor);
#endif
  if (!hasSurface() && IsWindowVisible(list)) {
    LVHITTESTINFO hit{};
    hit.pt = cursor;
    ScreenToClient(list, &hit.pt);
    currentHit = ListView_HitTest(list, &hit);
  }
  if (currentHit != row) return;

  const auto rootFolder = hoverFolderPathAt(row);
  if (rootFolder.empty() || !PathIsDirectoryW(rootFolder.c_str())) return;

  RECT anchorBounds{};
#ifdef FOLDERNOTE_WINUI
  if (hasSurface() && !surface->itemBounds(row, anchorBounds)) return;
#endif
  if (!hasSurface()) {
    if (!ListView_GetItemRect(list, row, &anchorBounds, LVIR_BOUNDS)) return;
    MapWindowPoints(list, nullptr, reinterpret_cast<POINT *>(&anchorBounds), 2);
  }
  if (IsRectEmpty(&anchorBounds)) return;

  struct HoverCmd {
    std::wstring path;
    bool isDir = false;
  };
  struct HoverMenuBuilder {
    std::map<unsigned, HoverCmd> commands;
    unsigned nextId = 1;
    std::shared_ptr<FolderHoverScan> initialScan;

    unsigned addCmd(const std::wstring &path, bool isDir) {
      const unsigned id = nextId++;
      commands.emplace(id, HoverCmd{path, isDir});
      return id;
    }
    std::shared_ptr<Branch> buildBranch(const std::wstring &folderPath, unsigned depth) {
      auto branch = std::make_shared<Branch>();
      if (depth >= 8) {
        branch->items.push_back({lang::text(L"更深的目录请直接打开"), 0, Symbol::Folder, nullptr, quick::popup::Row::Hint});
        return branch;
      }

      branch->items.push_back({lang::text(L"正在读取子文件夹…"), 0, Symbol::Folder, nullptr, quick::popup::Row::Hint});
      auto presetScan = depth == 0 ? initialScan : std::shared_ptr<FolderHoverScan>{};
      branch->poll = [this, folderPath, depth, scanJob = std::move(presetScan)](Branch &target) mutable -> bool {
        if (!scanJob) {
          scanJob = scanHoverFolder(folderPath);
          return false;
        }
        std::unique_lock lock(scanJob->mutex);
        if (!scanJob->done) return false;
        target.items.clear();
        if (scanJob->error && scanJob->error != ERROR_FILE_NOT_FOUND) {
          target.items.push_back({lang::text(L"目录暂不可用或无权访问"), 0, Symbol::Folder, nullptr, quick::popup::Row::Hint});
        } else if (scanJob->folders.empty() && scanJob->files.empty()) {
          target.items.push_back({lang::text(L"文件夹为空"), 0, Symbol::Folder, nullptr, quick::popup::Row::Hint});
        } else {
          for (const auto &sub : scanJob->folders) {
            auto children = sub.nonEmpty ? buildBranch(sub.path, depth + 1) : nullptr;
            target.items.push_back({hoverEntryTitle(sub.path, true), addCmd(sub.path, true),
                                    Symbol::Folder, std::move(children), quick::popup::Row::Action, sub.path});
          }
          if (!scanJob->folders.empty() && !scanJob->files.empty()) {
            target.items.push_back({L"", 0, Symbol::Folder, nullptr, quick::popup::Row::Separator});
          }
          for (const auto &file : scanJob->files) {
            target.items.push_back({hoverEntryTitle(file, false), addCmd(file, false),
                                    Symbol::Documents, nullptr, quick::popup::Row::Action, file});
          }
          if (scanJob->more) {
            target.items.push_back({lang::text(L"更多内容请打开此文件夹"),
                                    addCmd(folderPath, true),
                                    Symbol::Folder, nullptr, quick::popup::Row::Action});
          }
        }
        return true;
      };
      return branch;
    }
  } builder;

  builder.initialScan = scanHoverFolder(rootFolder);
  {
    std::unique_lock lock(builder.initialScan->mutex);
    builder.initialScan->ready.wait_for(lock, std::chrono::milliseconds(120), [&] {
      return builder.initialScan->done;
    });
    if (builder.initialScan->done &&
        (builder.initialScan->error != 0 ||
         (builder.initialScan->folders.empty() && builder.initialScan->files.empty()))) {
      return;
    }
  }

  // Re-verify pointer stayed on the target folder during initial scan.
  if (!GetCursorPos(&cursor) || !insideDesktopWindow(window, WindowFromPoint(cursor))) return;

#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) surface->closeToolTip();
#endif

  auto rootBranch = builder.buildBranch(rootFolder, 0);
  if (rootBranch->poll && rootBranch->poll(*rootBranch)) {
    rootBranch->poll = {};
  }

  desktop.menuOpen = true;
  unsigned chosen = 0;
  try {
    chosen = quick::popup::show(desktop.instance, {anchorBounds.right, anchorBounds.top},
                                rootBranch, desktop.theme.mode, GetForegroundWindow(), anchorBounds);
  } catch (...) {}
  desktop.menuOpen = false;

  POINT afterCursor{};
  if (GetCursorPos(&afterCursor)) {
    int afterRow = -1;
    if (insideDesktopWindow(window, WindowFromPoint(afterCursor))) {
#ifdef FOLDERNOTE_WINUI
      if (hasSurface()) afterRow = surface->hit(afterCursor);
#endif
      if (!hasSurface() && IsWindowVisible(list)) {
        LVHITTESTINFO hit{};
        hit.pt = afterCursor;
        ScreenToClient(list, &hit.pt);
        afterRow = ListView_HitTest(list, &hit);
      }
    }
    if (afterRow != row) suppressedHoverRow = -1;
  }

  if (chosen) {
    const auto found = builder.commands.find(chosen);
    if (found != builder.commands.end()) {
      const auto &targetPath = found->second.path;
      if (!targetPath.empty()) {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, nullptr, targetPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
          report(window, lang::text(L"无法打开此桌面项目。"));
        } else if (found->second.isDir) {
          quick::visitAsync(targetPath);
        }
      }
    }
  }
}

bool Panel::handleShortcut(UINT vk, bool ctrl, bool shift, bool alt) {
  if (desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting || collapsed()) return false;
  if (ctrl && !alt && !shift) {
    if (vk == 'C') { PostMessageW(window, WM_COMMAND, CopyItems, 0); return true; }
    if (vk == 'X') { PostMessageW(window, WM_COMMAND, CutItems, 0); return true; }
    if (vk == 'V') { PostMessageW(window, WM_COMMAND, PasteIntoBox, 0); return true; }
  }
  if (!ctrl && !alt && vk == VK_DELETE) {
    PostMessageW(window, WM_COMMAND, shift ? PermanentDeleteItems : DeleteItems, 0);
    return true;
  }
  if (alt && !ctrl && !shift && vk == VK_RETURN) {
    if (selection() >= 0) {
      PostMessageW(window, WM_COMMAND, ItemProperties, 0);
      return true;
    }
  }
  return false;
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

void Panel::showTab(size_t tab, bool allowDuringDrag) {
  if (tab > desktop.groups[index].tabs.size() || activeTab == tab ||
      (!allowDuringDrag && (desktop.dragging || desktop.menuOpen)))
    return;
  activeTab = tab;
  hoveredFolderRow = -1;
  suppressedHoverRow = -1;
  currentPage = pageWheelRemainder = 0;
#ifdef FOLDERNOTE_WINUI
  syncSurfaceHeader();
#endif
  refresh(desktop.groups[index]);
  InvalidateRect(tabs, nullptr, FALSE);
}

void Panel::beginTitleRename() {
#ifdef FOLDERNOTE_WINUI
  if (hasSurface() && surface->beginTitleRename([this](std::wstring name) { queueTitleRename(std::move(name)); })) return;
#endif
  RECT area{};
  GetClientRect(window, &area);
  const auto dpi = GetDpiForWindow(window);
  const auto inset = MulDiv(10, dpi, 96);
  const auto height = MulDiv(24, dpi, 96);
  titleEdit = CreateWindowExW(0, L"EDIT", desktop.groups[index].name.c_str(),
                              WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                              inset, std::max(0, (headerHeight() - height) / 2),
                              std::max(1L, area.right - MulDiv(110, dpi, 96)), height,
                              window, nullptr, desktop.instance, nullptr);
  if (!titleEdit) return;
  if (!SetWindowSubclass(titleEdit, titleEditInput, 1, reinterpret_cast<DWORD_PTR>(this))) {
    DestroyWindow(std::exchange(titleEdit, nullptr));
    return;
  }
  SendMessageW(titleEdit, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), FALSE);
  SendMessageW(titleEdit, EM_SETLIMITTEXT, groupNameLimit, 0);
  SetWindowPos(titleEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  focusDesktopWindow(window, titleEdit);
  SendMessageW(titleEdit, EM_SETSEL, 0, -1);
}

void Panel::queueTitleRename(std::wstring name) {
  if (name.empty() || desktop.quitting) return;
  pendingTitle = std::move(name);
  PostMessageW(window, renameCreatedBoxMessage, 0, 0);
}

void Panel::finishTitleEdit(bool cancel) {
  if (!titleEdit) return;
  const auto edit = std::exchange(titleEdit, nullptr);
  wchar_t name[groupNameLimit + 1]{};
  if (!cancel) GetWindowTextW(edit, name, ARRAYSIZE(name));
  DestroyWindow(edit);
  if (!cancel) queueTitleRename(name);
  InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK Panel::titleEditInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data);
  if (message == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
    panel.finishTitleEdit(w == VK_ESCAPE);
    return 0;
  }
  if (message == WM_KILLFOCUS && panel.titleEdit == control) {
    panel.finishTitleEdit(false);
    return 0;
  }
  return DefSubclassProc(control, message, w, l);
}

void Panel::beginItemRename(int row) {
  if (desktop.dragging || desktop.quitting || desktop.editing || desktop.menuOpen || collapsed() ||
      row < 0 || static_cast<size_t>(row) >= displayed.size() || isTransferToken(displayed[row])) return;
  const auto path = displayed[row];
  std::wstring editText;
  int selectLength = 0;
  if (!itemEditInfo(path, editText, selectLength)) return;
  ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    surface->select({row});
    if (surface->beginItemRename(row, editText, selectLength,
            [this, path, editText](std::optional<std::wstring> value) {
              if (value) queueItemRename(path, editText, std::move(*value));
            })) return;
  }
#endif
  finishItemEdit(true);
  ListView_EnsureVisible(list, row, FALSE);
  RECT label{}, client{};
  if (!ListView_GetItemRect(list, row, &label, LVIR_LABEL) || !GetClientRect(list, &client)) return;
  const auto dpi = GetDpiForWindow(window);
  const bool listMode = desktop.groups[index].view == View::List;
  const int padX = MulDiv(4, dpi, 96);
  const int minHeight = MulDiv(22, dpi, 96);
  const int height = std::max<int>(minHeight, label.bottom - label.top + MulDiv(4, dpi, 96));
  int left = label.left - padX;
  int width = std::max<int>(MulDiv(84, dpi, 96), label.right - label.left + 2 * padX);
  if (listMode) {
    width = std::max<int>(width, client.right - left - padX);
  } else {
    const int center = (label.left + label.right) / 2;
    width = std::min<int>(std::max<int>(width, MulDiv(104, dpi, 96)), std::max<int>(1, client.right - 2 * padX));
    left = std::clamp<int>(center - width / 2, padX, std::max<int>(padX, client.right - width - padX));
  }
  const int top = std::clamp<int>(label.top - MulDiv(2, dpi, 96), 0, std::max<int>(0, client.bottom - height));
  itemEdit = CreateWindowExW(0, L"EDIT", editText.c_str(),
                             WS_CHILD | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (listMode ? 0 : ES_CENTER),
                             left, top, std::max(1, width), height, list, nullptr, desktop.instance, nullptr);
  if (!itemEdit) return;
  if (!SetWindowSubclass(itemEdit, itemEditInput, 1, reinterpret_cast<DWORD_PTR>(this))) {
    DestroyWindow(std::exchange(itemEdit, nullptr));
    return;
  }
  itemEditPath = path;
  itemEditOriginal = editText;
  SendMessageW(itemEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  SendMessageW(itemEdit, EM_SETLIMITTEXT, 255, 0);
  SetWindowPos(itemEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  focusDesktopWindow(window, itemEdit);
  SendMessageW(itemEdit, EM_SETSEL, 0, selectLength);
}

void Panel::queueItemRename(std::wstring path, std::wstring originalName, std::wstring newName) {
  if (desktop.quitting || path.empty()) return;
  pendingItemRename = PendingItemRename{std::move(path), std::move(originalName), std::move(newName)};
  PostMessageW(window, commitItemRenameMessage, 0, 0);
}

void Panel::finishItemEdit(bool cancel) {
  if (!itemEdit) return;
  const auto edit = std::exchange(itemEdit, nullptr);
  auto path = std::exchange(itemEditPath, {});
  auto original = std::exchange(itemEditOriginal, {});
  wchar_t name[260]{};
  if (!cancel) GetWindowTextW(edit, name, ARRAYSIZE(name));
  DestroyWindow(edit);
  if (!cancel) queueItemRename(std::move(path), std::move(original), name);
  InvalidateRect(list, nullptr, FALSE);
}

LRESULT CALLBACK Panel::itemEditInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data);
  if (message == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
    panel.finishItemEdit(w == VK_ESCAPE);
    return 0;
  }
  if (message == WM_KILLFOCUS && panel.itemEdit == control) {
    panel.finishItemEdit(false);
    return 0;
  }
  return DefSubclassProc(control, message, w, l);
}

std::optional<size_t> Panel::tabAt(POINT screen) const {
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) return surface->tabAt(screen);
#endif
  if (!IsWindowVisible(tabs) || !ScreenToClient(tabs, &screen)) return std::nullopt;
  for (size_t i = 0; i < tabAreas.size(); ++i)
    if (PtInRect(&tabAreas[i], screen)) return i;
  return std::nullopt;
}

std::optional<size_t> Panel::tabInsertion(POINT screen) const {
  if (dockHidden) return std::nullopt;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) return surface->tabInsertion(screen);
#endif
  RECT client{};
  GetClientRect(window, &client);
  const auto dpi = GetDpiForWindow(window);
  client.bottom = headerHeight();
  client.right -= MulDiv(96, dpi, 96);
  auto point = screen;
  if (!ScreenToClient(window, &point) || !PtInRect(&client, point)) return std::nullopt;
  if (tabAreas.empty()) return point.x < std::min(client.right / 2, LONG(MulDiv(70, dpi, 96))) ? 0 : 1;
  ScreenToClient(tabs, &screen);
  for (size_t i = 0; i < tabAreas.size(); ++i)
    if (screen.x < (tabAreas[i].left + tabAreas[i].right) / 2) return i;
  return std::max<size_t>(1, tabAreas.size());
}

void Panel::showTabInsertion(std::optional<size_t> slot) {
  if (tabInsertionMark == slot) return;
  tabInsertionMark = slot;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    surface->showTabInsertion(slot);
    return;
  }
#endif
  RECT area{};
  GetClientRect(window, &area);
  area.bottom = headerHeight();
  InvalidateRect(window, &area, FALSE);
  InvalidateRect(tabs, nullptr, FALSE);
}

void Panel::paintTabInsertion(HDC dc) const {
  if (!tabInsertionMark) return;
  const auto dpi = GetDpiForWindow(window);
  const auto inset = MulDiv(8, dpi, 96);
  const auto width = std::max(2, MulDiv(2, dpi, 96));
  LONG x = MulDiv(125, dpi, 96);
  if (!tabAreas.empty()) {
    const auto slot = *tabInsertionMark;
    x = slot < tabAreas.size() ? tabAreas[slot].left : std::max(0L, tabAreas.back().right - width);
  } else {
    RECT client{};
    GetClientRect(window, &client);
    if (!*tabInsertionMark) x = MulDiv(6, dpi, 96);
    x = std::clamp(x, 0L, std::max(0L, client.right - MulDiv(100, dpi, 96)));
  }
  const RECT mark{x, inset, x + width, std::max<LONG>(inset + width, headerHeight() - inset)};
  const auto old = SetDCBrushColor(dc, desktop.theme.colors.accent);
  FillRect(dc, &mark, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  SetDCBrushColor(dc, old);
}

bool Panel::wheelOnTab(POINT screen, int delta) {
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) return surface->wheelOnTab(screen, delta);
#endif
  if (!tabAt(screen)) return false;
  KillTimer(tabs, hoverTabTimer);
  return wheelTabs(delta);
}

bool Panel::wheelTabs(int delta) {
  const auto count = desktop.groups[index].tabs.size() + 1;
  if (count < 2 || !delta) return false;
  if (tabDrag || pressedTab || gestureEdge || desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting) return true;
  tabWheelRemainder += std::clamp(delta, -WHEEL_DELTA * 12, WHEEL_DELTA * 12);
  const auto steps = tabWheelRemainder / WHEEL_DELTA;
  tabWheelRemainder %= WHEEL_DELTA;
  const auto total = static_cast<std::ptrdiff_t>(count);
  const auto next = (static_cast<std::ptrdiff_t>(activeTab % count) - steps % total + total) % total;
  if (steps) showTab(static_cast<size_t>(next));
  return true;
}

void Panel::beginTabDrag(size_t tab, POINT screen, RECT tabBounds) {
  cancelTabDrag();
  if (desktop.editing || desktop.dragging || desktop.menuOpen || desktop.quitting ||
      tab > desktop.groups[index].tabs.size()) return;
  const auto dpi = GetDpiForWindow(window);
  const auto width = MulDiv(360, dpi, 96);
  tabDrag = TabDrag{tab, screen, {std::clamp(screen.x - tabBounds.left, 0L, LONG(width - 1)),
                                 std::clamp(screen.y - tabBounds.top, 0L, LONG(headerHeight() - 1))}};
  if (!desktop.groups[index].tabs.empty()) SetTimer(window, tabDragTimer, 16, nullptr);
}

void Panel::moveTabDrag(POINT screen) {
  if (!tabDrag || desktop.groups[index].tabs.empty() || tabDrag->tab > desktop.groups[index].tabs.size()) return;
  if (desktop.quitting || desktop.editing || desktop.menuOpen || desktop.dragging) {
    cancelTabDrag();
    return;
  }
  auto &drag = *tabDrag;
  const auto dpi = GetDpiForWindow(window);
  if (!drag.preview && std::abs(screen.x - drag.start.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi) &&
      std::abs(screen.y - drag.start.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi)) return;
  if (!drag.preview) {
    WNDCLASSW type{};
    type.lpfnWndProc = tabPreviewProc;
    type.hInstance = desktop.instance;
    type.lpszClassName = tabPreviewClass;
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    drag.preview = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                                  tabPreviewClass, nullptr, WS_POPUP, 0, 0, 0, 0,
                                  window, nullptr, desktop.instance, this);
    if (!drag.preview) return;
    SetLayeredWindowAttributes(drag.preview, 0, desktop.theme.colors.highContrast ? 255 : 220, LWA_ALPHA);
  }
  const bool wasAllowed = drag.canDetach;
  auto *target = desktop.panelAt(screen);
  auto slot = target ? target->tabInsertion(screen) : std::nullopt;
  if (target && target != this && !target->dockHidden && !slot) slot = desktop.groups[target->index].tabs.size() + 1;
  if (target == this && slot && (*slot == drag.tab || *slot == drag.tab + 1)) slot.reset();
  const auto targetWindow = slot ? target->window : nullptr;
  if (drag.target != targetWindow || drag.slot != slot.value_or(0)) {
    if (drag.target != targetWindow) {
      if (auto *previous = desktop.panelForWindow(drag.target)) previous->showTabInsertion(std::nullopt);
    }
    drag.target = targetWindow;
    drag.slot = slot.value_or(0);
    if (targetWindow) target->showTabInsertion(slot);
  }
  drag.canDetach = !target && desktop.desktopDropAt(screen);
  const LONG x = screen.x - drag.anchor.x, y = screen.y - drag.anchor.y;
  const auto bounds = visibleBounds({x, y, x + MulDiv(360, dpi, 96), y + MulDiv(254, dpi, 96)});
  // The compact preview marks where the detached box's header will appear.
  const auto width = std::min<LONG>(bounds.right - bounds.left, MulDiv(210, dpi, 96));
  const auto height = std::min<LONG>(bounds.bottom - bounds.top, headerHeight());
  if (!IsWindowVisible(drag.preview) || wasAllowed != drag.canDetach) {
    const LONG previewWidth = drag.canDetach ? width : std::min<LONG>(width, MulDiv(144, dpi, 96));
    const auto region = createBoxRegion(previewWidth, height, MulDiv(12, dpi, 96));
    if (region && !SetWindowRgn(drag.preview, region, FALSE)) DeleteObject(region);
    SetWindowPos(drag.preview, HWND_TOPMOST, bounds.left, bounds.top, previewWidth, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(drag.preview, nullptr, FALSE);
  } else if (!EqualRect(&bounds, &drag.previewBounds)) {
    SetWindowPos(drag.preview, nullptr, bounds.left, bounds.top, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
  }
  drag.previewBounds = bounds;
  SetCursor(LoadCursorW(nullptr, drag.canDetach || drag.target ? IDC_SIZEALL : IDC_NO));
}

void Panel::endTabDrag(POINT screen) {
  if (!tabDrag) return;
  moveTabDrag(screen);
  if (!tabDrag) return;
  const auto tab = tabDrag->tab;
  const auto bounds = tabDrag->previewBounds;
  const auto target = tabDrag->target;
  const auto slot = tabDrag->slot;
  const bool detach = tabDrag->preview && tabDrag->canDetach;
  const bool click = !tabDrag->preview && insideDesktopWindow(window, WindowFromPoint(screen));
  cancelTabDrag();
  if (detach || target) {
    pendingTabDrop = TabDrop{tab, bounds, target, slot};
    if (!PostMessageW(window, dropBoxTabMessage, 0, 0)) pendingTabDrop.reset();
  } else if (click) {
    showTab(tab);
  }
}

void Panel::cancelTabDrag() {
  if (!tabDrag) return;
  const auto preview = tabDrag->preview;
  if (!desktop.quitting) {
    if (auto *target = desktop.panelForWindow(tabDrag->target)) target->showTabInsertion(std::nullopt);
  }
  tabDrag.reset();
  if (window) KillTimer(window, tabDragTimer);
  if (preview) {
    DestroyWindow(preview);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  }
}

LRESULT CALLBACK Panel::tabPreviewProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE)
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    const auto panel = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (panel && panel->tabDrag) {
      const auto &group = panel->desktop.groups[panel->index];
      const auto tab = panel->tabDrag->tab;
      if (tab <= group.tabs.size()) {
        const auto &name = tab ? group.tabs[tab - 1].name : group.name;
        const auto tint = tab ? group.tabs[tab - 1].detachedTint : group.primaryDetachedTint;
        const auto &colors = panel->desktop.theme.colors;
        const auto dpi = GetDpiForWindow(window);
        const auto fill = colors.highContrast ? colors.field : tint.value_or(group.tint).value_or(colors.hover);
        RECT rect{};
        GetClientRect(window, &rect);
        SetDCBrushColor(dc, fill);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        ui::rounded(dc, rect, fill, boxOutlineColor(fill, colors), MulDiv(6, dpi, 96));
        InflateRect(&rect, -MulDiv(10, dpi, 96), 0);
        const auto font = SelectObject(dc, panel->titleFont);
        ui::text(dc, name, rect, colors.text);
        if (font) SelectObject(dc, font);
      }
    }
    EndPaint(window, &paint);
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
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
  for (size_t i = 0; i < tabAreas.size(); ++i) {
    auto box = tabAreas[i];
    InflateRect(&box, -px(1), 0);
    if (IsRectEmpty(&box)) continue;
    const bool selected = i == activeTab;
    const bool pressed = pressedTab == i;
    const bool hot = !pressedTab && hoveredTab == i;
    if (selected || hot || pressed) {
      const auto fill = colors.highContrast && (selected || pressed) ? colors.accent
                      : pressed || (selected && hot) ? colors.pressed : selected ? colors.hover : colors.surface;
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
    const bool showText = i || showsTitleText(group.titleStyle);
    if (!i) {
      const auto size = px(16);
      if (const auto icon = headerIcon(); icon && box.right - box.left >= size) {
        const auto x = showText ? box.left : (box.left + box.right - size) / 2;
        DrawIconEx(dc, x, (box.top + box.bottom - size) / 2, icon, size, size, 0, nullptr, DI_NORMAL);
        box.left += size + px(6);
      }
    }
    if (showText) {
      std::wstring title = i ? group.tabs[i - 1].name : group.name;
      ui::text(dc, title, box, (selected || pressed) && colors.highContrast ? colors.onAccent :
               selected || hot || pressed ? colors.text : colors.muted,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, previous);
  }
  paintTabInsertion(dc);
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
  const auto tabAt = [&](POINT point) -> std::optional<size_t> {
    for (size_t i = 0; i < panel.tabAreas.size(); ++i)
      if (PtInRect(&panel.tabAreas[i], point)) return i;
    return std::nullopt;
  };
  const auto clearHover = [&] {
    KillTimer(control, hoverTabTimer);
    panel.hoveredTab.reset();
  };
  const auto clearPress = [&] {
    panel.pressedTab.reset();
    InvalidateRect(control, nullptr, FALSE);
  };
  const auto screenPoint = [&] {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ClientToScreen(control, &point);
    return point;
  };
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_MOUSEWHEEL && !GET_KEYSTATE_WPARAM(w) &&
      panel.wheelOnTab({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, GET_WHEEL_DELTA_WPARAM(w))) return 0;
  if (message == WM_MOUSELEAVE) {
    clearHover();
    panel.tabWheelRemainder = 0;
    InvalidateRect(control, nullptr, FALSE);
  }
  if (message == WM_TIMER && w == hoverTabTimer) {
    const auto pending = panel.hoveredTab;
    KillTimer(control, hoverTabTimer);
    POINT cursor{};
    if (pending && !panel.pressedTab && !panel.desktop.menuOpen && GetCursorPos(&cursor) &&
        WindowFromPoint(cursor) == control && ScreenToClient(control, &cursor) && tabAt(cursor) == pending)
      panel.showTab(*pending);
    return 0;
  }
  if (message == WM_LBUTTONDOWN) {
    const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (const auto tab = tabAt(point)) {
      clearHover();
      SetForegroundWindow(GetAncestor(panel.window, GA_ROOT));
      SetFocus(control);
      SetCapture(control);
      if (GetCapture() != control) {
        panel.showTab(*tab);
        return 0;
      }
      auto bounds = panel.tabAreas[*tab];
      MapWindowPoints(control, HWND_DESKTOP, reinterpret_cast<POINT *>(&bounds), 2);
      panel.pressedTab = tab;
      panel.beginTabDrag(*tab, screenPoint(), bounds);
      InvalidateRect(control, nullptr, FALSE);
      return 0;
    }
  }
  if (panel.pressedTab && (message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
                          (message == WM_KEYDOWN && w == VK_ESCAPE))) {
    clearPress();
    panel.cancelTabDrag();
    if (GetCapture() == control) ReleaseCapture();
    return 0;
  }
  if (panel.pressedTab && message == WM_MOUSEMOVE) {
    panel.moveTabDrag(screenPoint());
    return 0;
  }
  if (panel.pressedTab && message == WM_LBUTTONUP) {
    clearPress();
    const auto point = screenPoint();
    auto client = point;
    if (WindowFromPoint(point) == control && ScreenToClient(control, &client)) panel.hoveredTab = tabAt(client);
    panel.endTabDrag(point);
    if (GetCapture() == control) ReleaseCapture();
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
  if (!panel.pressedTab && message == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT) && !panel.tabAreas.empty()) {
    KillTimer(control, hoverTabTimer);
    const auto count = panel.tabAreas.size();
    panel.showTab((panel.activeTab + (w == VK_LEFT ? count - 1 : 1)) % count);
    return 0;
  }
  return DefSubclassProc(control, message, w, l);
}

LRESULT CALLBACK iconInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto &panel = *reinterpret_cast<Panel *>(data);
  if ((message == WM_SETFOCUS || message == WM_LBUTTONDOWN) && panel.desktop.previewKeys)
    panel.desktop.previewKeys->refresh();
  if (panel.findInput(message, w)) return 0;
  if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !panel.itemEdit) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool win = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
    if (!win) {
      if (ctrl && !shift && !alt && w == 'A') {
        ListView_SetItemState(control, -1, LVIS_SELECTED, LVIS_SELECTED);
        return 0;
      }
      if (panel.handleShortcut(static_cast<UINT>(w), ctrl, shift, alt)) return 0;
    }
  }
  if (message == WM_LBUTTONDOWN) {
    SetForegroundWindow(GetAncestor(panel.window, GA_ROOT));
    SetFocus(control);
  }
  if (message == WM_KEYDOWN && w == VK_SPACE &&
      !((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT) | GetKeyState(VK_MENU) |
         GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)) {
    if (l & (1LL << 30)) { if (panel.previewSpaceDown) return 0; }
    else if ((panel.previewSpaceDown = panel.selection() >= 0 && desktopPreviewAvailable())) return 0;
  }
  if (message == WM_CHAR && w == L' ' && panel.previewSpaceDown) return 0;
  if (message == WM_KEYUP && w == VK_SPACE && std::exchange(panel.previewSpaceDown, false)) {
    if (!((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT) | GetKeyState(VK_MENU) |
           GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)) panel.preview(panel.selection());
    return 0;
  }
  if (message == WM_KILLFOCUS) panel.previewSpaceDown = false;
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

LRESULT CALLBACK toggleInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  if (auto *panel = reinterpret_cast<Panel *>(data)) {
    const auto screenPoint = [&] {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ClientToScreen(control, &point);
      return point;
    };
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
      panel->beginCollapseHold(screenPoint());
    } else if (message == WM_MOUSEMOVE && (w & MK_LBUTTON)) {
      panel->moveCollapseHold(screenPoint());
      if (panel->gestureEdge == HTCAPTION && GetCapture() == control) return 0;
    } else if (message == WM_LBUTTONUP && panel->finishCollapseHold()) {
      SendMessageW(control, BM_SETSTATE, FALSE, 0);
      if (GetCapture() == control) ReleaseCapture();
      return 0;
    } else if (message == WM_SETCURSOR && panel->collapsePress && panel->collapsePress->armed && GetCapture() == control) {
      SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
      return TRUE;
    } else if (message == WM_KEYDOWN && w == VK_ESCAPE && GetCapture() == control) {
      panel->cancelCollapseHold();
      SendMessageW(control, BM_SETSTATE, FALSE, 0);
      if (GetCapture() == control) ReleaseCapture();
      return 0;
    } else if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
      panel->cancelCollapseHold();
    }
  }
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

bool Panel::preview(int row) {
  if (desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting ||
      row < 0 || static_cast<size_t>(row) >= displayed.size()) return false;
  return previewDesktopItem(displayed[row]);
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
  if (!desktop.commit(std::move(changed), window, false)) return;
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
  const HWND panelWindow = window;
  std::thread([batch, paths = std::move(paths), pixels, panelWindow] {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    try {
      for (size_t i = 0; i < paths.size() && !batch->canceled; ++i) {
        if (paths[i].empty()) continue;
        Thumbnail owned(loadDesktopThumbnail(paths[i], pixels), DeleteObject);
        if (owned) {
          bool shouldPost = false;
          {
            std::lock_guard lock(batch->mutex);
            shouldPost = batch->ready.empty();
            batch->ready.emplace_back(static_cast<int>(i), std::move(owned));
          }
          if (shouldPost && !batch->canceled && IsWindow(panelWindow)) {
            PostMessageW(panelWindow, applyThumbnailsMessage, 0, 0);
          }
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
    const auto imageIndex = setDesktopThumbnail(scaledIcons, item.iImage, image.get());
    if (imageIndex < 0) continue;
    if (item.iImage != imageIndex) {
      item.iImage = imageIndex;
      ListView_SetItem(list, &item);
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
      movingOrSizing || gestureEdge || tabDrag || (collapsePress && !collapsePress->canceled)) return;
  RECT bounds{};
  if (!GetWindowRect(window, &bounds)) return;
  if (touchingWorkEdge(bounds, dockSide) != dockSide) {
    setDockSide(DockSide::None);
    return;
  }
  const auto captured = GetCapture();
  if (desktop.menuOpen || desktop.editing || (captured && insideDesktopWindow(window, captured))) {
    dockHideAt = 0;
    return;
  }
  const auto hit = WindowFromPoint(cursor);
  if (insideDesktopWindow(window, hit)) {
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
    if (showsTitleText(group.titleStyle)) {
      ui::text(dc, group.name, header, colors.text);
    }
    paintTabInsertion(dc);
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
  if (pageCount > 1 && !collapsed()) {
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

bool Panel::collapsed() const {
  return autoCollapsed || desktop.groups[index].collapsed;
}

void Panel::tickAutoCollapse(bool pointerInside, ULONGLONG now) {
  const auto &group = desktop.groups[index];
  if (!group.autoCollapse || group.collapsed || dockSide != DockSide::None || !IsWindowVisible(window) ||
      movingOrSizing || gestureEdge || tabDrag || (collapsePress && !collapsePress->canceled) ||
      desktop.menuOpen || desktop.editing || desktop.dragging || desktop.quitting) {
    autoCollapseAt = 0;
    return;
  }
  const auto captured = GetCapture();
  if (captured && insideDesktopWindow(window, captured)) {
    autoCollapseAt = 0;
    return;
  }
  if (pointerInside) {
    autoCollapseAt = 0;
    if (std::exchange(autoCollapsed, false)) setCollapsed();
  } else if (!autoCollapsed) {
    if (!autoCollapseAt) autoCollapseAt = now + 450;
    else if (now >= autoCollapseAt) {
      autoCollapsed = true;
      setCollapsed();
    }
  }
}

void Panel::setCollapsed() {
  const auto &group = desktop.groups[index];
  const bool collapsed = this->collapsed();
  if (collapsed) finishItemEdit(true);
  RECT current{};
  GetWindowRect(window, &current);
  const auto resolved = resolvedGroupBounds(group);
  RECT bounds{current.left, current.top, current.right,
              current.top + (collapsed ? headerHeight() : resolved.bottom - resolved.top)};
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
  placeDesktopWindow(window, bounds);
  if (windowPositionReady) updateShadow();
}

void Panel::beginCollapseHold(POINT start) {
  if (desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting || gestureEdge || tabDrag) return;
  collapsePress = CollapsePress{start, GetTickCount64()};
  SetTimer(window, collapseHoldTimer, 16, nullptr);
}

void Panel::showCollapseMove(bool show) {
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) surface->showCollapseMove(show);
#endif
  InvalidateRect(toggle, nullptr, FALSE);
  SetCursor(LoadCursorW(nullptr, show ? IDC_SIZEALL : IDC_ARROW));
}

void Panel::moveCollapseHold(POINT screen) {
  if (!collapsePress || collapsePress->canceled) return;
  auto &press = *collapsePress;
  if (press.armed) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
  if (press.moving) return;
  if (!press.armed) {
    if (GetTickCount64() - press.pressedAt < collapseHoldDelay) return;
    press.armed = true;
    showCollapseMove(true);
  }
  const auto dpi = GetDpiForWindow(window);
  if (std::abs(screen.x - press.start.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi) &&
      std::abs(screen.y - press.start.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi)) return;
  press.moving = true;
  beginGesture(HTCAPTION, press.start, false);
  if (gestureEdge) updateGestureFrame();
}

bool Panel::finishCollapseHold() {
  if (!collapsePress) return false;
  const auto press = *std::exchange(collapsePress, std::nullopt);
  KillTimer(window, collapseHoldTimer);
  if (press.armed) showCollapseMove(false);
  if (press.moving && gestureEdge && !gestureEnding)
    gestureEnding = PostMessageW(window, finishBoxGestureMessage, TRUE, 0) != FALSE;
  return press.canceled || press.armed || press.moving || GetTickCount64() - press.pressedAt >= collapseHoldDelay;
}

void Panel::cancelCollapseHold() {
  if (!collapsePress || collapsePress->canceled) return;
  collapsePress->canceled = true;
  KillTimer(window, collapseHoldTimer);
  if (std::exchange(collapsePress->armed, false)) showCollapseMove(false);
  if (collapsePress->moving && gestureEdge) finishGesture(false);
}

void Panel::beginGesture(WPARAM edge, POINT point, bool capture) {
  if (desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting || gestureEdge || tabDrag) return;
  if (!GetWindowRect(window, &gestureBounds)) return;
  gestureStart = point;
  gestureEdge = edge;
  gestureCanMerge = capture;
  gestureTarget = gestureBounds;
  desktop.lower(window);
  if (capture) SetCapture(window);
  SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
  bool paced = false;
#ifdef FOLDERNOTE_WINUI
  if (hasSurface()) {
    surface->animateReflow(edge != HTCAPTION);
    paced = surface->onFrame([this] {
      lastCompositionFrame = GetTickCount64();
      updateGestureFrame();
    });
  }
#endif
  if (paced) lastCompositionFrame = GetTickCount64();
  // No-activate boxes may stop receiving pointer movement outside their bounds.
  if (!SetTimer(window, boxMoveTimer, 16, nullptr) && !paced) finishGesture(false);
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
    const int available = collapsed() ? 80 : MulDiv(bounds.bottom - bounds.top, 96, dpi) - 44;
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
  if (collapsePress && collapsePress->armed) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
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
      if (collapsed()) setCollapsed();
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
  const auto merge = gestureCanMerge && !EqualRect(&gestureTarget, &gestureBounds) ? desktop.mergeTargetAt(cursor, *this) : nullptr;
  const auto tint = merge ? std::optional(merge->backgroundColor()) : std::nullopt;
  if (mergeTint != tint) {
    mergeTint = tint;
    applyBackground();
  }
  desktop.highlight(merge ? merge->window : nullptr);
  if (current.left == gestureTarget.left && current.top == gestureTarget.top) return;
  if (!placeDesktopWindow(window, gestureTarget, SWP_NOSIZE | (hasSurface() ? SWP_NOCOPYBITS : 0))) return;
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
      auto changed = desktop.groups;
      changed[index].titleHeight = height;
      desktop.commit(std::move(changed), window, false);
    }
    if (collapsed()) setCollapsed();
    resize();
    desktop.lower();
    return;
  }
  auto *merge = gestureCanMerge && hasCursor && edge == HTCAPTION && !EqualRect(&start, &gestureTarget)
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

void Panel::syncSurfaceTransfers() {
#ifdef FOLDERNOTE_WINUI
  if (!hasSurface()) return;
  std::vector<std::pair<std::wstring, winui::TransferVisual>> visuals;
  visuals.reserve(activeTransfers.size());
  for (const auto &entry : activeTransfers) {
    if (entry.tab != activeTab) continue;
    const double p = entry.dragLifted ? -1.0 : std::clamp(entry.visualProgress, 0.04, 1.0);
    visuals.push_back({entry.tokenPath, {p, entry.outgoing}});
  }
  surface->setTransfers(visuals);
#endif
}

void Panel::refresh(const Group &group) {
  if (activeTab > group.tabs.size()) {
    activeTab = 0;
  }
  const auto folder = activeFolder();
  const bool folderMapped = !folder.empty();
  if (folder != mappedFolder) {
    mappedFolder = folder;
    mappedItems.clear();
    mappedResult = {};
  }
  auto items = folderMapped ? std::vector<std::wstring>{} : manualItems(group, activeTab);
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
  for (const auto &entry : activeTransfers) {
    if (!entry.placeholder || entry.tab != activeTab) continue;
    if (std::any_of(items.begin(), items.end(), [&](const auto &p) { return samePath(p, entry.tokenPath); })) continue;
    const size_t pos = std::min(entry.insertSlot, items.size());
    items.insert(items.begin() + static_cast<ptrdiff_t>(pos), entry.tokenPath);
  }
  if (orderedItems != items) {
    findPopup.close();
    findNames.clear();
    for (const auto &path : items) findNames.emplace_back(fn::fs::path(path).filename().wstring());
  }
  orderedItems = std::move(items);
  renderPage(group);
}

bool Panel::findInput(UINT message, WPARAM key) {
  if (message == WM_LBUTTONDOWN || message == WM_KILLFOCUS) { findPopup.close(); return false; }
  if (quick_find::shortcutModifiers() || desktop.editing || desktop.dragging || desktop.menuOpen || collapsed() || !quick_find::settings().boxes) {
    findPopup.close(); return false;
  }
  if (message == WM_KEYDOWN && findPopup.key(static_cast<UINT>(key))) return true;
  if (message == WM_CHAR && key == L'\b') return true;
  if (message != WM_CHAR || !quick_find::inputCharacter(static_cast<wchar_t>(key))) return false;
  if (!findPopup.active()) {
    std::vector<quick_find::CandidateIcon> icons;
    for (const auto &path : orderedItems) icons.emplace_back(quick::icons::Source::file(path));
    findPopup.begin(window, findNames, [this](int match) { chooseMatch(match); }, std::move(icons));
  }
  findPopup.character(static_cast<wchar_t>(key));
  return true;
}

void Panel::chooseMatch(int match, bool focus) {
    if (match < 0 || size_t(match) >= orderedItems.size()) return;
    if (pageCount && pageCapacity && match / pageCapacity != currentPage) goToPage(match / pageCapacity);
    const int row = match - (pageCount ? currentPage * pageCapacity : 0);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, row, FALSE);
#ifdef FOLDERNOTE_WINUI
    if (hasSurface()) { surface->select({row}); surface->reveal(row); }
#endif
    queueScrollSync();
    if (focus) {
#ifdef FOLDERNOTE_WINUI
      if (hasSurface()) surface->focus(row);
      else
#endif
      focusDesktopWindow(window, list);
    }
    const auto attributes = GetFileAttributesW(orderedItems[match].c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && quick_find::settings().opens((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
      PostMessageW(window, WM_COMMAND, OpenItem, 0);
}

int Panel::pageSize(const Group &group) const {
  if (group.displayMode != DisplayMode::Pages) return 0;
  const auto dpi = GetDpiForWindow(window);
  const int spacing = spacingPreview.value_or(group.itemSpacing);
  const auto fit = [&](bool showDots) {
#ifdef FOLDERNOTE_WINUI
    if (hasSurface())
      return surface->pageCapacity(group.view == View::List, group.iconSize,
                                   titleHeightPreview ? titleHeightPreview : group.titleHeight,
                                   desktop.style.radius, spacing, showDots);
#endif
    RECT area{};
    GetClientRect(window, &area);
    const auto pad = MulDiv(6, dpi, 96);
    const auto width = std::max(0L, area.right - 2 * pad);
    const auto bottom = showDots ? pageFooter().top : area.bottom - pad;
    const auto height = std::max(0L, bottom - headerHeight() - MulDiv(10, dpi, 96));
    if (group.view == View::List) {
      RECT item{};
      const auto row = ListView_GetView(list) == LV_VIEW_DETAILS && ListView_GetItemRect(list, 0, &item, LVIR_BOUNDS)
          ? item.bottom - item.top : static_cast<LONG>(MulDiv(24, dpi, 96));
      return static_cast<int>(std::max(1L, height / std::max(1L, row)));
    }
    const auto columnWidth = MulDiv(std::max(80, group.iconSize + 28) + spacing, dpi, 96);
    const auto rowHeight = MulDiv(group.iconSize + 46 + spacing, dpi, 96);
    return static_cast<int>(std::max(1L, width / columnWidth) * std::max(1L, height / rowHeight));
  };
  // Choose the footer using the full content area so the page count cannot oscillate.
  const auto full = fit(false);
  return orderedItems.size() <= static_cast<size_t>(full) ? full : fit(true);
}

void Panel::queuePageLayout() {
  if (desktop.groups[index].displayMode != DisplayMode::Pages || collapsed() ||
      pageLayoutPending || pageSize(desktop.groups[index]) == pageCapacity) return;
  pageLayoutPending = PostMessageW(window, updatePagesMessage, 0, 0) != FALSE;
}

void Panel::goToPage(int page) {
  const auto &group = desktop.groups[index];
  if (pageCount <= 1 || collapsed() || group.displayMode != DisplayMode::Pages ||
      desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting) return;
  const int next = std::clamp(page, 0, pageCount - 1);
  if (currentPage == next) return;
  currentPage = next;
  renderPage(group);
}

bool Panel::pageWheel(int delta) {
  const auto &group = desktop.groups[index];
  if (group.displayMode != DisplayMode::Pages || collapsed()) return false;
  if (pageCount <= 1) return true;
  if (desktop.dragging || desktop.editing || desktop.menuOpen || desktop.quitting) return true;
  pageWheelRemainder += delta;
  const int ticks = pageWheelRemainder / WHEEL_DELTA;
  pageWheelRemainder %= WHEEL_DELTA;
  if (ticks) PostMessageW(window, goToPageMessage, TRUE, -ticks);
  return true;
}

void Panel::renderPage(const Group &group) {
  const auto hadDots = pageCount > 1;
  if (!collapsed() || !pageCapacity || group.displayMode == DisplayMode::Scroll)
    pageCapacity = pageSize(group);
  const auto size = static_cast<int>(orderedItems.size());
  pageCount = pageCapacity ? std::max(1, size / pageCapacity + (size % pageCapacity != 0)) : 0;
  if (pageCount <= 1) pageWheelRemainder = 0;
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
    if (hadDots != (pageCount > 1)) resize();
    const auto footer = pageFooter();
    InvalidateRect(window, &footer, FALSE);
  }
  const bool folderMapped = folderView();
  const bool listMode = group.view == View::List;
  const auto view = listMode ? LV_VIEW_DETAILS : LV_VIEW_ICON;
  const int pixels = listMode ? 0 : MulDiv(group.iconSize, GetDpiForWindow(window), 96);
  const auto oldView = ListView_GetView(list);
  const auto oldIconSize = renderedIconSize;
  if (items == displayed && ListView_GetView(list) == static_cast<DWORD>(view) && renderedIconSize == pixels) {
    syncSurfaceTransfers();
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
  finishItemEdit(true);
  displayed = std::move(items);
  SetWindowTextW(window, group.name.c_str());
  const bool redrawNative = (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0;
  if (redrawNative) SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  ListView_SetView(list, view);
  ListView_DeleteAllItems(list);
  ListView_SetImageList(list, nullptr, LVSIL_NORMAL);
  if (scaledIcons) ImageList_Destroy(scaledIcons);
  if (pixels && pixels != thumbnailCacheSize) {
    thumbnailCache.clear();
    thumbnailCacheSize = pixels;
  }
  if (shellInfoCache.size() > 512) {
    std::erase_if(shellInfoCache, [&](const auto &entry) {
      return std::find(orderedItems.begin(), orderedItems.end(), entry.first) == orderedItems.end();
    });
  }
  scaledIcons = pixels ? ImageList_Create(pixels, pixels, ILC_COLOR32 | ILC_MASK, 0, 16) : nullptr;
  renderedIconSize = pixels;
  Microsoft::WRL::ComPtr<IImageList> systemImages;
  if (scaledIcons) {
    // Jumbo slots can contain a small icon on a 256px transparent canvas.
    // Use a full-size placeholder until the asynchronous thumbnail arrives.
    SHGetImageList(pixels > 32 ? SHIL_EXTRALARGE : SHIL_LARGE,
                   IID_PPV_ARGS(&systemImages));
    ListView_SetImageList(list, scaledIcons, LVSIL_NORMAL);
  }
  bool imageListSet = false;
  for (size_t i = 0; i < displayed.size(); ++i) {
    const auto &path = displayed[i];
    const TransferEntry *placeholderEntry = nullptr;
    if (isTransferToken(path)) {
      for (const auto &te : activeTransfers) {
        if (te.tokenPath == path) { placeholderEntry = &te; break; }
      }
    }
    const std::wstring &lookupPath = placeholderEntry && !placeholderEntry->iconSourcePath.empty()
                                         ? placeholderEntry->iconSourcePath : path;
    const auto name = fn::fs::path(lookupPath).filename().wstring();
    std::wstring label = placeholderEntry && !placeholderEntry->displayLabel.empty()
                            ? placeholderEntry->displayLabel : name;
    int sysIconIndex = -1;
    DWORD_PTR images = 0;

    auto cachedInfo = shellInfoCache.find(lookupPath);
    if (cachedInfo != shellInfoCache.end() &&
        (listMode ? cachedInfo->second.smallIcon : cachedInfo->second.largeIcon) >= 0 &&
        (listMode ? cachedSmallSysImages : cachedLargeSysImages) != nullptr) {
      if (!placeholderEntry && !cachedInfo->second.label.empty()) label = cachedInfo->second.label;
      sysIconIndex = listMode ? cachedInfo->second.smallIcon : cachedInfo->second.largeIcon;
      images = reinterpret_cast<DWORD_PTR>(listMode ? cachedSmallSysImages : cachedLargeSysImages);
    } else {
      SHFILEINFOW icon{};
      DWORD attributes = INVALID_FILE_ATTRIBUTES;
      if (placeholderEntry) {
        attributes = GetFileAttributesW(lookupPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          attributes = placeholderEntry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        }
      } else if (folderMapped) {
        const auto found = std::find_if(mappedItems.begin(), mappedItems.end(), [&](const auto &item) { return item.path == path; });
        if (found != mappedItems.end()) attributes = found->attributes;
      } else attributes = GetFileAttributesW(lookupPath.c_str());
      PIDLIST_ABSOLUTE id = nullptr;
      if (!folderMapped && (GetFileAttributesW(lookupPath.c_str()) != INVALID_FILE_ATTRIBUTES || isDesktopShellItem(lookupPath)) &&
          SUCCEEDED(SHParseDisplayName(lookupPath.c_str(), nullptr, &id, 0, nullptr))) {
        images = SHGetFileInfoW(reinterpret_cast<const wchar_t *>(id), 0, &icon, sizeof(icon),
                               SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX |
                               (listMode ? SHGFI_SMALLICON : SHGFI_LARGEICON));
        if (!placeholderEntry && images && icon.szDisplayName[0]) label = icon.szDisplayName;
        CoTaskMemFree(id);
      }
      if (!images)
        images = SHGetFileInfoW(name.c_str(), attributes == INVALID_FILE_ATTRIBUTES ? FILE_ATTRIBUTE_NORMAL : attributes,
                               &icon, sizeof(icon), SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES |
                               (listMode ? SHGFI_SMALLICON : SHGFI_LARGEICON));
      if (images) {
        sysIconIndex = icon.iIcon;
        if (listMode) cachedSmallSysImages = reinterpret_cast<HIMAGELIST>(images);
        else cachedLargeSysImages = reinterpret_cast<HIMAGELIST>(images);
        auto &slot = shellInfoCache[lookupPath];
        if (!placeholderEntry && (slot.label.empty() || !folderMapped)) slot.label = label;
        if (listMode) slot.smallIcon = sysIconIndex;
        else slot.largeIcon = sysIconIndex;
      }
    }

    if (images && !imageListSet && !scaledIcons) {
      ListView_SetImageList(list, reinterpret_cast<HIMAGELIST>(images), listMode ? LVSIL_SMALL : LVSIL_NORMAL);
      imageListSet = true;
    }
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    item.iItem = static_cast<int>(i);
    item.iImage = images && sysIconIndex >= 0 ? sysIconIndex : I_IMAGENONE;
    if (scaledIcons) {
      item.iImage = I_IMAGENONE;
      HICON bitmap = nullptr;
      if (const auto cached = thumbnailCache.find(path); cached != thumbnailCache.end()) {
        item.iImage = setDesktopThumbnail(scaledIcons, -1, cached->second.get());
      } else if (images && sysIconIndex >= 0 && systemImages &&
                 SUCCEEDED(systemImages->GetIcon(sysIconIndex, ILD_TRANSPARENT, &bitmap))) {
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
      HICON icon = nullptr;
      if (!surface->hasCachedImage(displayed[i], listMode, group.iconSize)) {
        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = static_cast<int>(i);
        icon = images && ListView_GetItem(list, &item) && item.iImage >= 0
                   ? ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT) : nullptr;
      }
      entries.push_back({label, displayed[i], icon});
    }
    surface->rows(entries, listMode, group.iconSize, spacingPreview.value_or(group.itemSpacing));
    for (const auto &entry : entries) if (entry.image) DestroyIcon(entry.image);
    std::vector<int> selection;
    for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0;) selection.push_back(row);
    surface->select(selection);
    syncSurfaceTransfers();
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
  const int contentTop = header + MulDiv(4, dpi, 96);
  const bool nativePaging = !hasSurface() && group.displayMode == DisplayMode::Pages;
  const int contentBottom = nativePaging && pageCount > 1 ? pageFooter().top : area.bottom - pad;
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
      !movingOrSizing && !tabDrag && !desktop.menuOpen && !desktop.dragging;
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
    if (collapsed() || group.displayMode == DisplayMode::Pages || !IsWindowVisible(list) ||
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
  const auto selectedTab = title && !keyboard ? tabAt(point) : std::nullopt;
  if (selectedTab && *selectedTab != activeTab) showTab(*selectedTab);
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
  if (row >= 0 && static_cast<size_t>(row) < displayed.size() && isTransferToken(displayed[row])) row = -1;
  if (row >= 0 && static_cast<size_t>(row) < displayed.size()) {
    if (!keyboard && !(ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED)) {
      ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
      ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
#ifdef FOLDERNOTE_WINUI
      if (hasSurface()) surface->select({row});
#endif
    }
    POINT itemPoint = point;
    if (keyboard) {
      RECT bounds{};
      ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS);
      itemPoint = {bounds.left, bounds.bottom};
      ClientToScreen(list, &itemPoint);
    }
    std::vector<std::wstring> menuPaths;
    const auto primary = folderView() ? displayed[row] : desktop.boxItem(displayed[row]);
    if (!primary.empty()) menuPaths.push_back(primary);
    for (const auto &sel : selectedResolvedPaths()) {
      if (!sel.empty() && std::none_of(menuPaths.begin(), menuPaths.end(),
                                       [&](const auto &existing) { return samePath(existing, sel); })) {
        menuPaths.push_back(sel);
      }
    }
    if (menuPaths.empty()) menuPaths.push_back(displayed[row]);

    std::vector<std::wstring> existingBefore;
    for (const auto &p : menuPaths) {
      if (!isDesktopShellItem(p) && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
        existingBefore.push_back(p);
    }

    desktop.menuOpen = true;
    ItemMenuResult menuResult;
    try { menuResult = showItemMenu(window, menuPaths, itemPoint, !folderView()); }
    catch (...) { desktop.menuOpen = false; throw; }
    desktop.menuOpen = false;
    if (desktop.quitting) return;
    if (menuResult.remove) {
      PostMessageW(window, WM_COMMAND, RemoveItem, 0);
    } else if (menuResult.rename) {
      PostMessageW(window, beginItemRenameMessage, static_cast<WPARAM>(row), 0);
    } else if (menuResult.deleteItem) {
      PostMessageW(window, WM_COMMAND, menuResult.permanentDelete ? PermanentDeleteItems : DeleteItems, 0);
    } else if (menuResult.invokedShellCommand) {
      if (folderView()) {
        mappedResult = {};
        refresh(desktop.groups[index]);
      } else if (!existingBefore.empty()) {
        for (const auto &p : existingBefore) {
          if (std::none_of(desktop.cutPaths.begin(), desktop.cutPaths.end(),
                           [&](const auto &existing) { return samePath(existing, p); })) {
            desktop.cutPaths.push_back(p);
          }
        }
        desktop.cleanupMovedCutItems();
      }
    }
    if (menuResult.shown) return;
    menuItem(*menu, OpenItem, L"打开", Symbol::Folder);
    menuItem(*menu, RenameItem, L"重命名", Symbol::Settings);
    menuItem(*menu, DeleteItems, L"删除", Symbol::Trash);
    if (!folderView()) menuItem(*menu, RemoveItem, L"移出盒子", Symbol::Unpin);
    separator(*menu);
  }
  const auto &group = desktop.groups[index];
  const auto targetTab = selectedTab.value_or(activeTab);

  const auto makeCompactBranch = [] {
    auto b = std::make_shared<Branch>();
    b->compact = true;
    return b;
  };

  const auto makeNewSubmenu = [&] {
    auto sub = makeCompactBranch();
    menuItem(*sub, NewBoxFolder, L"文件夹", Symbol::AddFolder);
    menuItem(*sub, NewBoxTextFile, L"文本文档", Symbol::Documents);
    return sub;
  };

  const auto makeViewSubmenu = [&] {
    auto sub = makeCompactBranch();
    const bool icons = group.view == View::Icons;
    menuItem(*sub, IconSize24, L"小图标", Symbol::Folder, icons && group.iconSize == 24);
    menuItem(*sub, IconSize32, L"中图标", Symbol::Folder, icons && group.iconSize == 32);
    menuItem(*sub, IconSize48, L"大图标", Symbol::Folder, icons && group.iconSize == 48);
    menuItem(*sub, IconSize64, L"超大图标", Symbol::Folder, icons && group.iconSize == 64);
    menuItem(*sub, IconSize96, L"特大图标", Symbol::Folder, icons && group.iconSize == 96);
    menuItem(*sub, ListViewMode, L"列表", Symbol::Window, group.view == View::List);
    separator(*sub);
    menuItem(*sub, ScrollDisplay, L"滚动显示", Symbol::Window, group.displayMode == DisplayMode::Scroll);
    menuItem(*sub, PageDisplay, L"分页显示", Symbol::Documents, group.displayMode == DisplayMode::Pages);
    separator(*sub);
    quick::popup::Item spacing;
    spacing.title = lang::text(L"项目间距");
    spacing.slider = quick::popup::Slider{0, maxItemSpacing, group.itemSpacing,
      [this](int value) {
        spacingPreview = value;
        resize();
      },
      [this](int value) { return saveSpacing(value); }, false, false};
    sub->items.push_back(std::move(spacing));
    return sub;
  };

  const auto makeSortSubmenu = [&] {
    auto sub = makeCompactBranch();
    const bool inFolder = folderView();
    if (!inFolder) menuItem(*sub, ManualSort, L"手动排序", Symbol::Move, group.sort == Sort::Manual);
    menuItem(*sub, NameSort, L"按名称", Symbol::Folder,
             group.sort == Sort::Name || (inFolder && group.sort == Sort::Manual));
    menuItem(*sub, TypeSort, L"按类型", Symbol::Folder, group.sort == Sort::Type);
    menuItem(*sub, ModifiedSort, L"按修改时间", Symbol::History, group.sort == Sort::Modified);
    return sub;
  };

  const auto makeAppearanceSubmenu = [&] {
    auto sub = makeCompactBranch();
    if (!desktop.theme.colors.highContrast) {
      const auto initial = group.tint;
      const auto hue = HueColor::fromRgb(backgroundColor(), desktop.theme.colors.dark);
      quick::popup::Item hueSlider;
      hueSlider.title = lang::text(L"盒子色相");
      hueSlider.slider = quick::popup::Slider{0, 359, hue.hue,
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
      sub->items.push_back(std::move(hueSlider));
      menuItem(*sub, BoxColor, L"自定义颜色...", Symbol::Settings);
      if (group.tint) menuItem(*sub, ResetBoxColor, L"恢复主题颜色", Symbol::Settings);
      separator(*sub);
    }
    if (hasSurface() && !desktop.theme.colors.highContrast) {
      menuItem(*sub, ToggleBoxGlass, L"毛玻璃效果", Symbol::Visibility, backgroundGlass());
      if (backgroundGlass()) {
        quick::popup::Item blur;
        blur.title = lang::text(L"模糊强度");
        blur.slider = quick::popup::Slider{0, 100, backgroundBlur(),
          [this](int value) {
            blurPreview = value;
            SetTimer(window, blurPreviewTimer, 33, nullptr);
            desktop.lower();
          },
          [this](int value) {
            if (desktop.quitting) return false;
            auto changed = desktop.groups;
            changed[index].blurStrength = value;
            const bool saved = desktop.commit(std::move(changed), window, false);
            blurPreview.reset();
            KillTimer(window, blurPreviewTimer);
            desktop.lower();
            return saved;
          }};
        sub->items.push_back(std::move(blur));
      }
      quick::popup::Item opacityItem;
      opacityItem.title = lang::text(L"背景不透明度");
      opacityItem.slider = quick::popup::Slider{0, 100, backgroundOpacity(),
        [this](int value) {
          opacityPreview = value;
          applyBackground();
        },
        [this](int value) {
          if (desktop.quitting) return false;
          auto changed = desktop.groups;
          changed[index].opacity = value;
          const bool saved = desktop.commit(std::move(changed), window, false);
          opacityPreview.reset();
          applyBackground();
          return saved;
        }};
      sub->items.push_back(std::move(opacityItem));
      separator(*sub);
    }
    menuItem(*sub, TitleText, L"显示标题文字", Symbol::Window, showsTitleText(group.titleStyle));
    menuItem(*sub, TitleIcon, L"显示标题图标", Symbol::Window, showsTitleIcon(group.titleStyle));
    menuItem(*sub, BoxIcon, L"更换标题图标...", Symbol::Folder);
    if (!group.titleIcon.empty()) menuItem(*sub, ClearBoxIcon, L"清除自定义图标", Symbol::Folder);
    return sub;
  };

  const auto makeBehaviorSubmenu = [&] {
    auto sub = makeCompactBranch();
    menuItem(*sub, ToggleAutoCollapse, L"自动折叠隐藏", Symbol::Visibility, group.autoCollapse);
    menuItem(*sub, ToggleHoverMenu, L"悬停展开文件夹菜单", Symbol::Folder, group.hoverMenu);
    if (group.folder.empty())
      menuItem(*sub, EditAutoRule, L"自动归入规则...", Symbol::Settings, !group.autoRule.empty());
    return sub;
  };

  const auto makeTabsSubmenu = [&] {
    auto sub = makeCompactBranch();
    menuItem(*sub, MapFolder, L"添加映射文件夹...", Symbol::AddFolder);
    if (!group.tabs.empty()) {
      separator(*sub);
      menuItem(*sub, RenameTab, L"重命名当前标签...", Symbol::Settings);
      if (targetTab > 0) menuItem(*sub, MoveTabLeft, L"向左移动标签", Symbol::Move);
      if (targetTab < group.tabs.size()) menuItem(*sub, MoveTabRight, L"向右移动标签", Symbol::Move);
      menuItem(*sub, DetachTabToBox, L"拆分为独立盒子", Symbol::Window);
      menuItem(*sub, RemoveTab, L"移除当前标签", Symbol::Unpin);
    }
    return sub;
  };

  const auto makeGlobalSubmenu = [&] {
    auto sub = makeCompactBranch();
    menuItem(*sub, NewGroup, L"新建盒子", Symbol::AddFolder);
    menuItem(*sub, ToggleHideAllBoxes, L"隐藏全部盒子", Symbol::Visibility);
    menuItem(*sub, ToggleDoubleClickHide, L"双击桌面空白处隐藏盒子", Symbol::Visibility,
             desktop.doubleClickHideEnabled());
    separator(*sub);
    menuItem(*sub, ExportBackup, L"导出备份...", Symbol::Documents);
    menuItem(*sub, ImportBackup, L"导入备份...", Symbol::Folder);
    return sub;
  };

  if (title) {
    menuItem(*menu, ToggleGroup, collapsed() ? L"展开盒子" : L"折叠盒子", Symbol::Visibility);
    menuItem(*menu, RenameGroup, L"重命名盒子", Symbol::Settings);
    separator(*menu);
    submenuItem(*menu, L"外观与标题", Symbol::Window, makeAppearanceSubmenu());
    submenuItem(*menu, L"查看方式", Symbol::Folder, makeViewSubmenu());
    submenuItem(*menu, L"排序方式", Symbol::History, makeSortSubmenu());
    submenuItem(*menu, L"行为与自动化", Symbol::Settings, makeBehaviorSubmenu());
    if (!group.tabs.empty()) submenuItem(*menu, L"标签与映射", Symbol::Folder, makeTabsSubmenu());
    else menuItem(*menu, MapFolder, L"添加映射文件夹...", Symbol::AddFolder);
    separator(*menu);
    submenuItem(*menu, L"全局桌面盒子", Symbol::Desktop, makeGlobalSubmenu());
    separator(*menu);
    menuItem(*menu, DeleteGroup, L"删除盒子", Symbol::Trash);
  } else {
    if (row < 0) {
      submenuItem(*menu, L"新建", Symbol::AddFolder, makeNewSubmenu());
      if (clipboardHasPasteableContent()) menuItem(*menu, PasteIntoBox, L"粘贴", Symbol::Copy);
      menuItem(*menu, OpenFolderInExplorer, L"在资源管理器中打开", Symbol::Window);
      separator(*menu);
    }
    submenuItem(*menu, L"查看方式", Symbol::Folder, makeViewSubmenu());
    submenuItem(*menu, L"排序方式", Symbol::History, makeSortSubmenu());
    separator(*menu);
    auto boxSub = makeCompactBranch();
    submenuItem(*boxSub, L"外观与标题", Symbol::Window, makeAppearanceSubmenu());
    submenuItem(*boxSub, L"行为与自动化", Symbol::Settings, makeBehaviorSubmenu());
    if (!group.tabs.empty()) submenuItem(*boxSub, L"标签与映射", Symbol::Folder, makeTabsSubmenu());
    else menuItem(*boxSub, MapFolder, L"添加映射文件夹...", Symbol::AddFolder);
    separator(*boxSub);
    menuItem(*boxSub, ToggleGroup, collapsed() ? L"展开盒子" : L"折叠盒子", Symbol::Visibility);
    menuItem(*boxSub, RenameGroup, L"重命名盒子", Symbol::Settings);
    submenuItem(*boxSub, L"全局桌面盒子", Symbol::Desktop, makeGlobalSubmenu());
    separator(*boxSub);
    menuItem(*boxSub, DeleteGroup, L"删除盒子", Symbol::Trash);
    submenuItem(*menu, L"当前盒子设置", Symbol::Settings, std::move(boxSub));
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
    KillTimer(window, blurPreviewTimer);
    if (std::exchange(blurPreview, std::nullopt)) desktop.lower();
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
    if (command == NewGroup) {
      PostMessageW(desktop.window, createMessage, static_cast<WPARAM>(static_cast<INT_PTR>(point.x)), point.y);
      return;
    }
    if (command == ToggleHideAllBoxes || command == ToggleDoubleClickHide ||
        command == ExportBackup || command == ImportBackup) {
      PostMessageW(desktop.window, WM_COMMAND, command, 0);
      return;
    }
    const auto tabCommand = command == RemoveTab || command == MoveTabLeft || command == MoveTabRight ||
                            command == RenameTab || command == DetachTabToBox ||
                            command == RenameGroup || command == DeleteGroup;
    PostMessageW(window, WM_COMMAND, command, tabCommand ? static_cast<LPARAM>(targetTab) : 0);
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
          !SetWindowSubclass(panel->toggle, toggleInput, 1, reinterpret_cast<DWORD_PTR>(panel))) return -1;
      panel->font = ui::makeFont(GetDpiForWindow(window));
      panel->titleFont = ui::makeFont(GetDpiForWindow(window), 14, FW_SEMIBOLD);
      SendMessageW(panel->list, WM_SETFONT, reinterpret_cast<WPARAM>(panel->font), FALSE);
      SendMessageW(panel->toggle, WM_SETFONT, reinterpret_cast<WPARAM>(panel->titleFont), FALSE);
      ListView_SetExtendedListViewStyle(panel->list, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP);
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
      panel->dropTarget.Attach(new DropTarget(*panel));
      const auto registered = RegisterDragDrop(window, panel->dropTarget.Get());
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
      if (panel->dockHidden || panel->desktop.oleDragActive) return HTCLIENT;
      RECT bounds{};
      GetWindowRect(window, &bounds);
      const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      RECT editBounds{};
      if (panel->titleEdit && GetWindowRect(panel->titleEdit, &editBounds) && PtInRect(&editBounds, point)) return HTCLIENT;
      if (panel->itemEdit && GetWindowRect(panel->itemEdit, &editBounds) && PtInRect(&editBounds, point)) return HTCLIENT;
      const auto dpi = GetDpiForWindow(window);
      const int border = MulDiv(8, dpi, 96);
      if (!panel->collapsed()) {
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
      const bool collapsed = panel->collapsed();
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
      if (panel->desktop.oleDragActive) return TRUE;
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
        panel->beginGesture(w, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}, true);
        return 0;
      }
      break;
    case WM_TIMER:
      if (w == collapseHoldTimer) {
        POINT cursor{};
        if (GetAsyncKeyState(VK_ESCAPE) < 0 || desktop.quitting) panel->cancelCollapseHold();
        else if (GetAsyncKeyState(VK_LBUTTON) >= 0) KillTimer(window, collapseHoldTimer);
        else if (GetCursorPos(&cursor)) panel->moveCollapseHold(cursor);
        return 0;
      }
      if (w == tabDragTimer) {
        POINT cursor{};
        if (GetAsyncKeyState(VK_ESCAPE) < 0 || desktop.quitting) panel->cancelTabDrag();
        else if (panel->tabDrag && GetCursorPos(&cursor)) {
          if (GetAsyncKeyState(VK_LBUTTON) < 0) panel->moveTabDrag(cursor);
          else panel->endTabDrag(cursor);
        }
        return 0;
      }
      if (w == blurPreviewTimer) {
        if (panel->blurPreview && desktop.menuOpen) desktop.lower();
        else KillTimer(window, blurPreviewTimer);
        return 0;
      }
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
      if (panel->collapsePress && panel->collapsePress->armed) {
        if (const auto move = LoadCursorW(nullptr, IDC_SIZEALL))
          DrawIconEx(item.hDC, centerX - px(8), centerY - px(8), move, px(16), px(16), 0, nullptr, DI_NORMAL);
        return TRUE;
      }
      const int halfWidth = px(4), halfHeight = px(2);
      const int direction = panel->collapsed() ? 1 : -1;
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
    case applyThumbnailsMessage:
      panel->applyThumbnails();
      return 0;
    case updatePagesMessage:
      panel->pageLayoutPending = false;
      if (!panel->collapsed()) panel->renderPage(desktop.groups[panel->index]);
      return 0;
    case goToPageMessage:
      panel->goToPage(static_cast<int>(l) + (w ? panel->currentPage : 0));
      return 0;
    case WM_MOUSEWHEEL:
      if (!GET_KEYSTATE_WPARAM(w) && panel->wheelOnTab({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, GET_WHEEL_DELTA_WPARAM(w))) return 0;
      if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) panel->zoom(GET_WHEEL_DELTA_WPARAM(w));
      else if (!panel->pageWheel(GET_WHEEL_DELTA_WPARAM(w)))
        panel->verticalScroll.wheel(GET_WHEEL_DELTA_WPARAM(w));
      return 0;
    case WM_ENTERSIZEMOVE:
      panel->movingOrSizing = true;
      return 0;
    case WM_MOVE:
      if (panel->windowPositionReady) desktop.updateGlass();
      return 0;
    case WM_SIZE:
      if (w != SIZE_MINIMIZED) panel->updateShape();
      panel->resize();
      if (panel->windowPositionReady) desktop.updateGlass();
      return 0;
    case WM_GETMINMAXINFO: {
      auto &info = *reinterpret_cast<MINMAXINFO *>(l);
      const auto dpi = GetDpiForWindow(window);
      info.ptMinTrackSize = {MulDiv(190, dpi, 96), panel->collapsed()
                             ? panel->headerHeight()
                             : std::max(MulDiv(112, dpi, 96), panel->headerHeight() + MulDiv(44, dpi, 96))};
      return 0;
    }
    case WM_DPICHANGED:
    case WM_DPICHANGED_AFTERPARENT: {
      const auto dpi = GetDpiForWindow(window);
      auto font = ui::makeFont(dpi);
      auto titleFont = ui::makeFont(dpi, 14, FW_SEMIBOLD);
      SendMessageW(panel->list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      if (panel->font) DeleteObject(panel->font);
      panel->font = font;
      SendMessageW(panel->toggle, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
      if (panel->titleFont) DeleteObject(panel->titleFont);
      panel->titleFont = titleFont;
      if (message == WM_DPICHANGED) panel->place(*reinterpret_cast<RECT *>(l));
      if (panel->collapsed()) panel->setCollapsed();
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
      if (panel->collapsed()) bounds.bottom = bounds.top + (group.bounds.bottom - group.bounds.top);
      group.bounds = bounds;
      group.referenceWorkArea = currentWorkArea(bounds);
      if (!desktop.commit(std::move(changed), window, false)) {
        const auto saved = resolvedGroupBounds(desktop.groups[panel->index]);
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
#ifdef FOLDERNOTE_WINUI
      if (panel->hasSurface()) { panel->surface->focus(); return 0; }
#endif
      SetFocus(panel->collapsed() ? panel->toggle : panel->list);
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
        wcsncpy_s(markup->szMarkup, lang::text(panel->folderView() ? L"映射文件夹为空" :
                     L"将桌面文件拖到这里"), _TRUNCATE);
        return TRUE;
      }
      if (notification->code == NM_DBLCLK) {
        if (reinterpret_cast<NMITEMACTIVATE *>(l)->iItem >= 0)
          PostMessageW(window, WM_COMMAND, OpenItem, 0);
      }
      if (notification->code == LVN_BEGINDRAG)
        PostMessageW(window, dragMessage, reinterpret_cast<NMLISTVIEW *>(l)->iItem, 0);
      if (notification->code == LVN_KEYDOWN) {
        const auto key = reinterpret_cast<NMLVKEYDOWN *>(l)->wVKey;
        if (key == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000) == 0)
          PostMessageW(window, WM_COMMAND, OpenItem, 0);
        else if (key == VK_F2)
          PostMessageW(window, beginItemRenameMessage, static_cast<WPARAM>(panel->selection()), 0);
      }
      if (notification->code == LVN_GETINFOTIPW) {
        auto tip = reinterpret_cast<NMLVGETINFOTIPW *>(l);
        const auto &items = panel->displayed;
        if (tip->iItem >= 0 && static_cast<size_t>(tip->iItem) < items.size()) {
          const auto &path = items[tip->iItem];
          std::wstring info;
          PIDLIST_ABSOLUTE parsed = nullptr;
          if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr)) && parsed) {
            const std::unique_ptr<void, decltype(&CoTaskMemFree)> owned(static_cast<void *>(parsed), CoTaskMemFree);
            Microsoft::WRL::ComPtr<IShellFolder> folder;
            PCUITEMID_CHILD child = nullptr;
            Microsoft::WRL::ComPtr<IQueryInfo> query;
            if (SUCCEEDED(SHBindToParent(parsed, IID_PPV_ARGS(&folder), &child)) && folder && child &&
                SUCCEEDED(folder->GetUIObjectOf(window, 1, &child, IID_IQueryInfo, nullptr,
                                                reinterpret_cast<void **>(query.GetAddressOf()))) && query) {
              PWSTR raw = nullptr;
              if ((SUCCEEDED(query->GetInfoTip(QITIPF_USESLOWTIP, &raw)) ||
                   SUCCEEDED(query->GetInfoTip(QITIPF_DEFAULT, &raw))) && raw) {
                info = raw;
                CoTaskMemFree(raw);
              }
            }
          }
          lstrcpynW(tip->pszText, info.empty() ? path.c_str() : info.c_str(), tip->cchTextMax);
        }
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
    case renameCreatedBoxMessage:
      desktop.renameBox(*panel, std::exchange(panel->pendingTitle, std::wstring{}));
      return 0;
    case beginItemRenameMessage:
      panel->beginItemRename(static_cast<int>(w));
      return 0;
    case showFolderHoverMessage:
      panel->showFolderHoverMenu(static_cast<int>(w));
      return 0;
    case commitItemRenameMessage:
      if (auto request = std::exchange(panel->pendingItemRename, std::nullopt))
        desktop.renameItem(*panel, std::move(request->path), std::move(request->originalName), std::move(request->newName));
      return 0;
    case WM_COMMAND:
      if (const auto command = static_cast<Command>(LOWORD(w));
          command == RemoveTab || command == MoveTabLeft || command == MoveTabRight ||
          command == RenameTab || command == DetachTabToBox ||
          command == RenameGroup || command == DeleteGroup)
        desktop.action(panel, command, static_cast<size_t>(l));
      else
        desktop.action(panel, command);
      return 0;
    case dragMessage:
      if (GetAsyncKeyState(VK_LBUTTON) < 0) desktop.drag(*panel, static_cast<int>(w));
      return 0;
    case dropBoxTabMessage:
      if (const auto request = std::exchange(panel->pendingTabDrop, std::nullopt)) {
        if (request->target) {
          if (auto *target = desktop.panelForWindow(request->target))
            desktop.movePanelTab(*panel, request->tab, *target, request->slot, true);
        } else desktop.detachPanelTab(panel->index, request->tab, request->bounds);
      }
      return 0;
    case finishBoxGestureMessage:
      if (panel->gestureEdge) panel->finishGesture(w != FALSE);
      return 0;
    case WM_LBUTTONUP:
      if (panel->gestureEdge) {
        panel->finishGesture(true);
        return 0;
      }
      if (!panel->hasSurface() && panel->pageCount > 1 && !panel->collapsed()) {
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
      panel->cancelTabDrag();
      panel->cancelCollapseHold();
      if (panel->gestureEdge) {
        panel->finishGesture(false);
        return 0;
      }
      break;
    case rejectedMessage:
      report(window, lang::text(w ? L"无法移动部分桌面项目。" : L"只能添加桌面或 FolderNote Boxes 中的文件和文件夹。"));
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
  events.rename = [this](int row) { PostMessageW(window, beginItemRenameMessage, static_cast<WPARAM>(row), 0); };
  events.shortcut = [this](UINT vk, bool ctrl, bool shift, bool alt) {
    return handleShortcut(vk, ctrl, shift, alt);
  };
  events.suppressToolTip = [this](int) {
    return desktop.menuOpen;
  };
  events.preview = [this](int row) { return preview(row); };
  events.previewAvailable = [] { return desktopPreviewAvailable(); };
  events.previewFocus = [this] { if (desktop.previewKeys) desktop.previewKeys->refresh(); };
  events.findInput = [this](UINT message, WPARAM key) { return findInput(message, key); };
  events.context = [this](POINT point) { contextMenu(point); };
  events.zoom = [this](int delta) { zoom(delta); };
  events.wheel = [this](int delta) { return pageWheel(delta); };
  events.beginDrag = [this](int row) { PostMessageW(window, dragMessage, row, 0); };
  events.tab = [this](size_t tab) { showTab(tab); };
  events.tabWheel = [this](int delta) { return wheelTabs(delta); };
  events.tabPressed = [this](size_t tab, POINT screen, RECT bounds) { beginTabDrag(tab, screen, bounds); };
  events.tabMoved = [this](POINT screen) { moveTabDrag(screen); };
  events.tabReleased = [this](POINT screen) { endTabDrag(screen); };
  events.tabCanceled = [this] { cancelTabDrag(); };
  events.collapsePressed = [this](POINT start) { beginCollapseHold(start); };
  events.collapseMoved = [this](POINT screen) { moveCollapseHold(screen); };
  events.collapseReleased = [this] { return finishCollapseHold(); };
  events.collapseCanceled = [this] { cancelCollapseHold(); };
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
  surface = std::make_unique<winui::Surface>(window, std::move(events), dropTarget.Get());
  if (hasSurface()) {
    for (auto control : {list, toggle, tabs, viewButtons[0], viewButtons[1]}) ShowWindow(control, SW_HIDE);
    surface->show(true);
  }
}
#endif

bool Desktop::openPanel(size_t index) {
  const bool wasRebuilding = std::exchange(rebuilding, true);
  struct Restore { bool &value; bool previous; ~Restore() { value = previous; } } restore{rebuilding, wasRebuilding};
  auto panel = std::make_unique<Panel>(*this, index);
  const auto bounds = resolvedGroupBounds(groups[index]);
  if (!IsWindow(host)) return false;
  DesktopDpiScope dpiScope(host);
  const auto create = [&](DWORD extendedStyle) {
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | extendedStyle, panelClass, groups[index].name.c_str(),
                           WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, bounds.left, bounds.top,
                           bounds.right - bounds.left, bounds.bottom - bounds.top,
                           window, nullptr, instance, panel.get());
  };
  HWND handle = nullptr;
#ifdef FOLDERNOTE_WINUI
  if (useWinui) {
    // No redirection bitmap must be set before the HWND and its island are created.
    handle = create(WS_EX_NOREDIRECTIONBITMAP);
    if (handle) {
      panel->openSurface();
      if (!panel->hasSurface()) {
        panel.reset();
        panel = std::make_unique<Panel>(*this, index);
        handle = nullptr;
      }
    }
  }
#endif
  if (!handle) handle = create(0);
  if (!handle) return false;
  if (!attachDesktopWindow(handle, host)) return false;
  constexpr COLORREF noBorder = 0xfffffffe;
  DwmSetWindowAttribute(handle, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
  // The client paints the only outline in both renderers.
  const DWMNCRENDERINGPOLICY frame = DWMNCRP_DISABLED;
  DwmSetWindowAttribute(handle, DWMWA_NCRENDERING_POLICY, &frame, sizeof(frame));
  panel->updateShape();
  panel->applyTheme(theme.colors);
  panel->resize();
  panel->refresh(groups[index]);
  panel->setCollapsed();
  panel->windowPositionReady = true;
  if (boxesHidden) {
#ifdef FOLDERNOTE_WINUI
    if (panel->hasSurface()) panel->surface->show(false);
#endif
    ShowWindow(handle, SW_HIDE);
  }
  panels.push_back(std::move(panel));
  updateShadows();
  lower(handle);
  return true;
}

void Panel::updateShadow() {
  const auto &style = desktop.style;
  if (desktop.boxesHidden || dockHidden || !style.shadow || !style.shadowOpacity || desktop.theme.colors.highContrast) {
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
    DesktopDpiScope dpiScope(desktop.host);
    shadow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"STATIC", nullptr, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, desktop.host, nullptr, desktop.instance, nullptr);
    if (!shadow) return;
  }
  POINT origin{box.left - spread, box.top - spread + offset};
  if (shadowSize.cx == width && shadowSize.cy == height && shadowDiameter == diameter && shadowDpi == dpi &&
      renderedShadowOpacity == style.shadowOpacity && renderedShadowOffset == offset && renderedShadowSpread == spread) {
    placeDesktopWindow(shadow, {origin.x, origin.y, origin.x + width, origin.y + height}, SWP_NOSIZE);
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
    auto position = desktopWindowPoint(shadow, origin);
    if (UpdateLayeredWindow(shadow, nullptr, &position, &size, dc, &source, 0, &blend, ULW_ALPHA)) {
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

void Desktop::updateShadows() {
  for (const auto &panel : panels) panel->updateShadow();
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
  const auto hit = WindowFromPoint(point);
  if (!hit || (!panelAt(point) && !desktopDropAt(point))) return nullptr;
  for (auto child = GetTopWindow(host); child; child = GetWindow(child, GW_HWNDNEXT)) {
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
    if (inside) return panel->get();
  }
  return nullptr;
}

Panel *Desktop::panelAt(POINT point) const {
  return panelForWindow(WindowFromPoint(point));
}

Panel *Desktop::panelForWindow(HWND handle) const {
  if (!handle) return nullptr;
  const auto found = std::find_if(panels.begin(), panels.end(), [handle](const auto &panel) { return insideDesktopWindow(panel->window, handle); });
  return found == panels.end() ? nullptr : found->get();
}

bool Desktop::desktopDropAt(POINT point) const {
  const auto hit = WindowFromPoint(point);
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
      report(owner, lang::text(L"无法恢复原文件位置。请在桌面和 FolderNote Boxes 文件夹中查看文件。"));
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

bool Desktop::commit(std::vector<Group> changed, HWND owner, bool refreshContents) {
  try {
    save(changed);
    groups = std::move(changed);
    if (refreshContents) PostMessageW(window, refreshMessage, 0, 0);
    return true;
  } catch (const fn::Error &error) {
    report(owner, error.message.c_str());
  }
  return false;
}

std::wstring Desktop::boxItem(const std::wstring &path) const {
  if (isDesktopShellItem(path)) {
    PIDLIST_ABSOLUTE id = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) && id) {
      CoTaskMemFree(id);
      return path;
    }
    return {};
  }
  if (auto stored = boxStoredItem(roots, path); !stored.empty()) return stored;
  return boxDesktopItem(roots, path);
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
  updateShadows();
  lower();
  return true;
}

void Desktop::detachPanelTab(size_t index, size_t tab, RECT bounds) {
  if (editing || dragging || quitting || index >= panels.size()) return;
  auto changed = groups;
  const auto owner = panels[index]->window;
  const auto previous = groups;
  if (!detachTab(changed, index, tab, bounds)) return;
  changed.back().referenceWorkArea = currentWorkArea(bounds);
  if (!commit(std::move(changed), owner)) return;
  if (!openPanel(groups.size() - 1)) {
    commit(previous, owner);
    report(owner, lang::text(L"无法打开桌面盒子。"));
    return;
  }
  panels[index]->activeTab = 0;
  panels[index]->currentPage = panels[index]->pageWheelRemainder = 0;
  SetWindowTextW(owner, groups[index].name.c_str());
  panels[index]->refresh(groups[index]);
  panels[index]->resize();
}

void Desktop::movePanelTab(Panel &source, size_t tab, Panel &target, size_t slot, bool selectMoved) {
  if (dragging || quitting || source.tabDrag || target.tabDrag) return;
  const bool same = &source == &target;
  auto changed = groups;
  if (!moveTab(changed, source.index, tab, target.index, slot) || !commit(std::move(changed), source.window, false)) return;
  const auto next = slot - (same && slot > tab);
  SetWindowTextW(source.window, groups[source.index].name.c_str());
  if (same) {
    auto &active = source.activeTab;
    if (active == tab) active = next;
    else if (tab < active && active <= next) --active;
    else if (next <= active && active < tab) ++active;
    source.resize();
    if (selectMoved) source.showTab(next);
    return;
  }
  if (source.activeTab == tab) {
    source.activeTab = 0;
    source.currentPage = source.pageWheelRemainder = 0;
    source.refresh(groups[source.index]);
  } else if (source.activeTab > tab) --source.activeTab;
  source.resize();
  SetWindowTextW(target.window, groups[target.index].name.c_str());
  if (target.activeTab >= next) ++target.activeTab;
  target.resize();
  target.showTab(next);
}

std::optional<PreviewTarget> Desktop::previewTarget() const {
  if (quitting || rebuilding || editing || menuOpen || dragging) return std::nullopt;
  const auto focus = GetFocus();
  const auto panel = panelForWindow(focus);
  if (!panel || panel->dockHidden || panel->collapsed() || panel->findPopup.active() || panel->titleEdit ||
      GetForegroundWindow() != GetAncestor(panel->window, GA_ROOT)) return std::nullopt;
  std::optional<int> row;
#ifdef FOLDERNOTE_WINUI
  if (panel->hasSurface()) row = panel->surface->previewRow();
  else
#endif
  if (focus == panel->list) row = panel->selection();
  if (!row) return std::nullopt;
  PreviewTarget target{panel->window, focus, {}};
  if (*row >= 0 && static_cast<size_t>(*row) < panel->displayed.size()) target.path = panel->displayed[*row];
  return target;
}

void Desktop::locateSearch() {
  if (!pendingSearch) return;
  if (boxesHidden) setAllBoxesHidden(false);
  const auto &entry = *pendingSearch;
  if (quitting || rebuilding || editing || menuOpen || dragging || GetTickCount64() > searchDeadline ||
      !search::matches(groups, entry) || entry.group >= panels.size()) { pendingSearch.reset(); return; }
  auto &panel = *panels[entry.group];
  if (panel.movingOrSizing || GetForegroundWindow() != GetAncestor(panel.window, GA_ROOT) ||
      GetFileAttributesW(entry.path.c_str()) == INVALID_FILE_ATTRIBUTES) { pendingSearch.reset(); return; }
  const bool layoutChanged = panel.collapsed() || panel.dockHidden || panel.activeTab != entry.tab;
  if (groups[entry.group].collapsed) {
    auto changed = groups; changed[entry.group].collapsed = false;
    if (!commit(std::move(changed), panel.window, false)) { pendingSearch.reset(); return; }
  }
  panel.autoCollapsed = false;
  panel.autoCollapseAt = panel.dockHideAt = GetTickCount64() + 3000;
  panel.showDockedBox(true);
  panel.setCollapsed();
  panel.showTab(entry.tab);
  panel.refresh(groups[entry.group]);
  if (layoutChanged) return; // Let WinUI update page capacity after expanding or changing tabs.
  const auto found = std::find_if(panel.orderedItems.begin(), panel.orderedItems.end(),
      [&](const auto &path) { return samePath(path, entry.path); });
  if (found == panel.orderedItems.end()) return; // A mapped tab may still be loading asynchronously.
  const int row = static_cast<int>(found - panel.orderedItems.begin());
  pendingSearch.reset();
  lower(panel.window);
  panel.chooseMatch(row, true);
}

bool Desktop::ensureHost() {
  if (rebuilding || quitting) return false;
  const auto next = desktopWindowHost();
  if (next && next == host && panels.size() == groups.size() &&
      std::all_of(panels.begin(), panels.end(), [&](const auto &panel) {
        return IsWindow(panel->window) && GetParent(panel->window) == host;
      })) return true;
  // Modal callbacks may retain a Panel pointer. Rebuild only after they finish;
  // recovery never changes the stored layout or the files in the boxes.
  if (editing || menuOpen || dragging || std::any_of(panels.begin(), panels.end(),
      [](const auto &panel) { return panel->movingOrSizing && IsWindow(panel->window); })) return false;
  rebuilding = true;
  struct Reset { bool &value; ~Reset() { value = false; } } reset{rebuilding};
  panels.clear();
  glass.clear();
  host = next;
  if (!host) return false;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (!openPanel(i)) { panels.clear(); return false; }
  }
  return true;
}

void Desktop::updateGlass(HWND raisedPanel) {
  if (rebuilding || quitting || !IsWindow(host)) return;
  std::vector<GlassArea> areas;
  std::map<int, std::shared_ptr<const WallpaperFrame>> frames;
#ifdef FOLDERNOTE_WINUI
  if (!boxesHidden) {
    // Follow the sibling order that lower() will apply, including the newly
    // raised box, so overlapping boxes retain their own blur on the first frame.
    std::vector<Panel *> ordered;
    for (auto child = GetTopWindow(host); child; child = GetWindow(child, GW_HWNDNEXT)) {
      const auto found = std::find_if(panels.begin(), panels.end(), [child](const auto &p) { return p->window == child; });
      if (found != panels.end()) ordered.push_back(found->get());
    }
    const auto raised = std::find_if(ordered.begin(), ordered.end(), [raisedPanel](auto p) { return p->window == raisedPanel; });
    if (raised != ordered.end()) std::rotate(ordered.begin(), raised, raised + 1);
    for (auto p : ordered) {
      auto &panel = *p;
      RECT bounds{};
      if (panel.dockHidden || !GetWindowRect(panel.window, &bounds)) continue;
      int strength = -1;
      if (panel.hasSurface() && panel.surface->usesDesktopGlass() && panel.backgroundBlur() > 0) {
        const int wanted = panel.backgroundBlur();
        auto image = wallpaperBackdrop(wanted);
        if (image) panel.renderedBlur = wanted;
        strength = panel.renderedBlur;
        if (!image) image = wallpaperBackdrop(strength);
        auto &frame = frames[strength];
        if (image) frame = std::move(image);
      }
      areas.push_back({bounds, panel.cornerDiameter(), strength});
    }
  }
#endif
  std::erase_if(glass, [&](const auto &layer) { return !frames.contains(layer.first); });
  for (auto &[strength, image] : frames) {
    auto &layer = glass[strength];
    if (!layer) layer = std::make_unique<DesktopGlass>();
    layer->update(host, std::move(image), areas, strength);
  }
}

void Desktop::lower(HWND raisedPanel) {
  updateGlass(raisedPanel);
  if (boxesHidden || !IsWindow(host) || panels.empty()) return;
  std::vector<HWND> boxes;
  for (auto candidate = GetTopWindow(host); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
    if (std::any_of(panels.begin(), panels.end(), [candidate](const auto &panel) { return panel->window == candidate; }))
      boxes.push_back(candidate);
  }
  if (const auto raised = std::find(boxes.begin(), boxes.end(), raisedPanel); raised != boxes.end())
    std::rotate(boxes.begin(), raised, raised + 1);
  for (const auto &panel : panels)
    if (panel->shadow && panel->shadowSize.cx) boxes.push_back(panel->shadow);
  for (const auto &[strength, layer] : glass)
    if (layer->handle()) boxes.push_back(layer->handle());
  lowerDesktopWindows(host, boxes);
}
void Desktop::manage() {
  if (menuOpen || editing || dragging) return;
  menuOpen = true;
  auto menu = std::make_shared<Branch>();
  menuItem(*menu, NewGroup, L"新建盒子", Symbol::AddFolder);
  if (!groups.empty()) {
    menuItem(*menu, ToggleHideAllBoxes, boxesHidden ? L"显示全部盒子" : L"隐藏全部盒子", Symbol::Visibility);
  }
  menuItem(*menu, ToggleDoubleClickHide, L"双击桌面空白处隐藏盒子", Symbol::Visibility, doubleClickHideEnabled());
  separator(*menu);
  menuItem(*menu, ExportBackup, L"导出备份...", Symbol::Documents);
  menuItem(*menu, ImportBackup, L"导入备份...", Symbol::Folder);
  POINT point{};
  GetCursorPos(&point);
  unsigned command = 0;
  try { command = quick::popup::show(instance, point, menu, theme.mode, GetForegroundWindow()); }
  catch (...) { menuOpen = false; throw; }
  menuOpen = false;
  PostMessageW(window, WM_NULL, 0, 0);
  if (quitting) return;
  switch (command) {
  case NewGroup: createBox(point, window); break;
  case ToggleHideAllBoxes: setAllBoxesHidden(!boxesHidden); break;
  case ToggleDoubleClickHide: toggleDoubleClickHide(); break;
  case ExportBackup: exportBackup(window); break;
  case ImportBackup: importBackup(window); break;
  }
}

void Desktop::createBox(POINT point, HWND owner) {
  if (menuOpen || editing || dragging || quitting) return;
  if (boxesHidden) setAllBoxesHidden(false);
  Group group;
  group.name = lang::text(L"新建盒子");
  const auto monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  const auto dpi = GetDpiForSystem();
  group.bounds = visibleBounds({point.x, point.y, point.x + MulDiv(360, dpi, 96),
                                point.y + MulDiv(254, dpi, 96)}, monitor);
  group.referenceWorkArea = currentWorkArea(group.bounds, monitor);
  auto changed = groups;
  changed.push_back(std::move(group));
  if (!commit(std::move(changed), owner)) return;
  if (!openPanel(groups.size() - 1)) {
    auto previous = groups;
    previous.pop_back();
    commit(std::move(previous), owner);
    report(owner, lang::text(L"无法打开桌面盒子。"));
    return;
  }
  panels.back()->beginTitleRename();
}

void Desktop::renameBox(Panel &panel, std::wstring name) {
  const auto first = name.find_first_not_of(L" \t\r\n");
  if (quitting || first == name.npos) return;
  name = name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
  const auto index = panel.index;
  if (name == groups[index].name) return;
  auto changed = groups;
  changed[index].name = std::move(name);
  if (!commit(std::move(changed), panel.window)) return;
  SetWindowTextW(panel.window, groups[index].name.c_str());
#ifdef FOLDERNOTE_WINUI
  panel.syncSurfaceHeader();
#endif
  InvalidateRect(panel.window, nullptr, FALSE);
  InvalidateRect(panel.tabs, nullptr, FALSE);
}

void Desktop::renameItem(Panel &panel, std::wstring oldPath, std::wstring originalName, std::wstring newName) {
  const auto first = newName.find_first_not_of(L" \t\r\n");
  if (quitting || editing || oldPath.empty() || first == newName.npos) return;
  newName = newName.substr(first, newName.find_last_not_of(L" \t\r\n") - first + 1);
  if (newName.empty() || newName == originalName) return;

  struct EditScope {
    bool &active;
    explicit EditScope(bool &flag) : active(flag) { active = true; }
    ~EditScope() { active = false; }
  } scope(editing);

  PIDLIST_ABSOLUTE parsed = nullptr;
  if (FAILED(SHParseDisplayName(oldPath.c_str(), nullptr, &parsed, 0, nullptr)) || !parsed) return;
  const std::unique_ptr<void, decltype(&CoTaskMemFree)> owned(static_cast<void *>(parsed), CoTaskMemFree);
  Microsoft::WRL::ComPtr<IShellFolder> folder;
  PCUITEMID_CHILD child = nullptr;
  if (FAILED(SHBindToParent(parsed, IID_PPV_ARGS(&folder), &child)) || !folder || !child) return;

  SetForegroundWindow(panel.window);
  PITEMID_CHILD renamedChild = nullptr;
  if (FAILED(folder->SetNameOf(panel.window, child, newName.c_str(), SHGDN_INFOLDER, &renamedChild))) return;

  std::wstring newPath;
  if (renamedChild) {
    if (PIDLIST_ABSOLUTE parent = ILCloneFull(parsed)) {
      ILRemoveLastID(parent);
      if (PIDLIST_ABSOLUTE full = ILCombine(parent, renamedChild)) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(full, SIGDN_DESKTOPABSOLUTEPARSING, &raw)) && raw) {
          newPath = raw;
          CoTaskMemFree(raw);
        }
        ILFree(full);
      }
      ILFree(parent);
    }
    CoTaskMemFree(renamedChild);
  }
  if (quitting) return;
  panel.shellInfoCache.erase(oldPath);
  if (!newPath.empty()) panel.shellInfoCache.erase(newPath);

  const auto selectRenamed = [&](const std::wstring &target) {
    if (target.empty()) return;
    const auto found = std::find_if(panel.orderedItems.begin(), panel.orderedItems.end(),
                                    [&](const auto &candidate) { return samePath(candidate, target); });
    if (found == panel.orderedItems.end()) return;
    const int match = static_cast<int>(found - panel.orderedItems.begin());
    if (panel.pageCount && panel.pageCapacity && match / panel.pageCapacity != panel.currentPage)
      panel.goToPage(match / panel.pageCapacity);
    const int row = match - (panel.pageCount ? panel.currentPage * panel.pageCapacity : 0);
    if (row < 0 || static_cast<size_t>(row) >= panel.displayed.size()) return;
    ListView_SetItemState(panel.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(panel.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(panel.list, row, FALSE);
#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) { panel.surface->select({row}); panel.surface->reveal(row); }
#endif
  };

  if (panel.folderView()) {
    if (!newPath.empty()) {
      for (auto &item : panel.mappedItems) {
        if (samePath(item.path, oldPath)) item.path = newPath;
      }
    }
    panel.mappedResult = {};
    panel.refresh(groups[panel.index]);
    selectRenamed(newPath);
    return;
  }

  if (!newPath.empty()) {
    auto changed = groups;
    bool updated = false;
    const auto replacePath = [&](std::vector<std::wstring> &entries) {
      for (auto &entry : entries) {
        if (samePath(entry, oldPath)) {
          entry = newPath;
          updated = true;
        }
      }
    };
    for (auto &group : changed) {
      replacePath(group.paths);
      for (auto &tab : group.tabs) replacePath(tab.paths);
    }
    if (updated && commit(std::move(changed), panel.window, false)) {
      for (size_t i = 0; i < panels.size(); ++i) panels[i]->refresh(groups[i]);
      selectRenamed(newPath);
      return;
    }
  }
  panel.refresh(groups[panel.index]);
}

void Desktop::action(Panel *panel, Command command, size_t tabIndex) {
  if (editing || dragging || quitting || (panel && panel->tabDrag)) return;
  struct EditScope {
    bool &active;
    explicit EditScope(bool &flag) : active(flag) { active = true; }
    ~EditScope() { active = false; }
  } scope(editing);
  const auto owner = panel ? panel->window : window;
  if (!panel || quitting) return;
  const auto index = panel->index;
  const int selected = panel->selection();
  switch (command) {
  case BoxIcon: case ClearBoxIcon: case TitleText: case TitleIcon: {
    auto next = groups[index];
    if (command == BoxIcon) {
      next.titleIcon = pickBoxIcon(owner);
      if (next.titleIcon.empty() || quitting) return;
      next.titleStyle = titleStyleFor(true, showsTitleText(next.titleStyle));
    } else if (command == ClearBoxIcon) {
      next.titleIcon.clear();
    } else {
      const auto icon = showsTitleIcon(next.titleStyle), text = showsTitleText(next.titleStyle);
      next.titleStyle = command == TitleIcon ? titleStyleFor(!icon, text) : titleStyleFor(icon, !text);
    }
    auto changed = groups;
    changed[index] = std::move(next);
    if (!commit(std::move(changed), owner, false)) return;
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
    if (tabIndex > count || (command == MoveTabLeft ? tabIndex == 0 : tabIndex == count)) return;
    const auto slot = command == MoveTabLeft ? tabIndex - 1 : tabIndex + 2;
    movePanelTab(*panel, tabIndex, *panel, slot, false);
    return;
  }
  case MapFolder: {
    Tab tab;
    tab.folder = pickFolder(owner);
    if (tab.folder.empty() || quitting) return;
    const fn::fs::path folder(tab.folder);
    tab.name = folder.filename().wstring();
    if (tab.name.empty()) tab.name = tab.folder;
    tab.name.resize(std::min(tab.name.size(), groupNameLimit));
    auto changed = groups;
    changed[index].tabs.push_back(std::move(tab));
    changed[index].collapsed = false;
    if (commit(std::move(changed), owner)) {
      panel->autoCollapsed = false;
      panel->showTab(groups[index].tabs.size());
      panel->setCollapsed();
    }
    return;
  }
  case RemoveTab: {
    if (groups[index].tabs.empty() || tabIndex > groups[index].tabs.size()) return;
    auto changed = groups;
    auto removed = deleteTab(changed, index, tabIndex);
    if (!removed) return;
    const bool hadItems = !removed->paths.empty();
    if (releaseItems(std::move(changed), removed->paths, owner, hadItems)) {
      std::erase_if(panel->activeTransfers, [tabIndex](const auto &e) { return e.tab == tabIndex; });
      for (auto &e : panel->activeTransfers) {
        if (e.tab > tabIndex) --e.tab;
      }
      panel->syncSurfaceTransfers();
      if (panel->activeTab == tabIndex) {
        panel->activeTab = std::min(tabIndex, groups[index].tabs.size());
        panel->currentPage = panel->pageWheelRemainder = 0;
        panel->hoveredFolderRow = -1;
        panel->suppressedHoverRow = -1;
      } else if (panel->activeTab > tabIndex) {
        --panel->activeTab;
      }
      SetWindowTextW(owner, groups[index].name.c_str());
      panel->applyBackground();
      panel->refresh(groups[index]);
      panel->resize();
    }
    return;
  }
  case RenameTab: {
    if (tabIndex == 0) {
      auto name = groups[index].name;
      if (!showBoxDialog(instance, owner, L"重命名当前标签", &name, lang::text(L"标签名称")) || quitting) return;
      renameBox(*panel, std::move(name));
      return;
    }
    if (tabIndex > groups[index].tabs.size()) return;
    auto name = groups[index].tabs[tabIndex - 1].name;
    if (!showBoxDialog(instance, owner, L"重命名当前标签", &name, lang::text(L"标签名称")) || quitting) return;
    const auto first = name.find_first_not_of(L" \t\r\n");
    if (first == name.npos) return;
    name = name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
    if (name == groups[index].tabs[tabIndex - 1].name) return;
    auto changed = groups;
    changed[index].tabs[tabIndex - 1].name = std::move(name);
    if (commit(std::move(changed), owner, false)) {
#ifdef FOLDERNOTE_WINUI
      panel->syncSurfaceHeader();
#endif
      InvalidateRect(panel->tabs, nullptr, FALSE);
    }
    return;
  }
  case DetachTabToBox: {
    if (groups[index].tabs.empty() || tabIndex > groups[index].tabs.size()) return;
    RECT base{};
    if (!GetWindowRect(owner, &base)) base = resolvedGroupBounds(groups[index]);
    const auto dpi = GetDpiForWindow(owner);
    const int offset = MulDiv(28, dpi, 96);
    const LONG height = panel->collapsed()
        ? (resolvedGroupBounds(groups[index]).bottom - resolvedGroupBounds(groups[index]).top)
        : (base.bottom - base.top);
    const RECT target = visibleBounds({base.left + offset, base.top + offset,
                                       base.right + offset, base.top + offset + height});
    editing = false;
    detachPanelTab(index, tabIndex, target);
    return;
  }
  case ScrollDisplay: case PageDisplay: {
    const auto mode = command == PageDisplay ? DisplayMode::Pages : DisplayMode::Scroll;
    if (groups[index].displayMode == mode) return;
    auto changed = groups;
    changed[index].displayMode = mode;
    if (!commit(std::move(changed), owner, false)) return;
    panel->currentPage = panel->pageWheelRemainder = 0;
    panel->resize();
    panel->renderPage(groups[index]);
    return;
  }
  case IconSize24: case IconSize32: case IconSize48: case IconSize64: case IconSize96: {
    constexpr int sizes[]{24, 32, 48, 64, 96};
    const int wanted = sizes[command - IconSize24];
    if (groups[index].view == View::Icons && groups[index].iconSize == wanted) return;
    const auto previousView = groups[index].view;
    const int previousSize = groups[index].iconSize;
    groups[index].view = View::Icons;
    groups[index].iconSize = wanted;
    panel->renderPage(groups[index]);
    for (const auto button : panel->viewButtons) InvalidateRect(button, nullptr, FALSE);
    try {
      save(groups);
    } catch (const fn::Error &error) {
      groups[index].view = previousView;
      groups[index].iconSize = previousSize;
      panel->renderPage(groups[index]);
      report(owner, error.message.c_str());
    }
    return;
  }
  case OpenFolderInExplorer: {
    std::wstring folder = panel->activeFolder();
    if (folder.empty()) {
      const auto storage = boxStorageFolder(roots);
      if (!storage.empty() && PathIsDirectoryW(storage.c_str())) folder = storage;
      else if (!roots.empty()) folder = roots.front();
    }
    if (!folder.empty()) ShellExecuteW(owner, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  case IconView: case ListViewMode: {
    const auto targetView = command == IconView ? View::Icons : View::List;
    if (groups[index].view == targetView) return;
    const auto previousView = groups[index].view;
    groups[index].view = targetView;
    panel->renderPage(groups[index]);
    for (const auto button : panel->viewButtons) InvalidateRect(button, nullptr, FALSE);
    try {
      save(groups);
    } catch (const fn::Error &error) {
      groups[index].view = previousView;
      panel->renderPage(groups[index]);
      report(owner, error.message.c_str());
    }
    return;
  }
  case ManualSort: case NameSort: case TypeSort: case ModifiedSort: {
    const auto targetSort = static_cast<Sort>(command - ManualSort);
    if (groups[index].sort == targetSort) return;
    auto changed = groups;
    changed[index].sort = targetSort;
    if (commit(std::move(changed), owner, false)) {
      panel->refresh(groups[index]);
    }
    return;
  }
  case ToggleBoxGlass: {
    auto changed = groups;
    changed[index].glass = !panel->backgroundGlass();
    if (commit(std::move(changed), owner, false)) panel->applyBackground();
    return;
  }
  case ToggleAutoCollapse: {
    auto changed = groups;
    changed[index].autoCollapse = !changed[index].autoCollapse;
    if (commit(std::move(changed), owner, false)) {
      panel->autoCollapseAt = 0;
      panel->autoCollapsed = false;
      panel->setCollapsed();
    }
    return;
  }
  case ToggleHoverMenu: {
    auto changed = groups;
    changed[index].hoverMenu = !changed[index].hoverMenu;
    panel->hoveredFolderRow = -1;
    panel->suppressedHoverRow = -1;
    commit(std::move(changed), owner, false);
    return;
  }
  case EditAutoRule: {
    auto rule = groups[index].autoRule;
    if (!showBoxDialog(instance, owner, L"自动归入规则", &rule,
                       lang::text(L"示例：.png, .jpg, 截图*, *.pdf（留空关闭）"), true, 512) || quitting) return;
    if (rule == groups[index].autoRule) return;
    auto changed = groups;
    changed[index].autoRule = std::move(rule);
    if (commit(std::move(changed), owner, false) && !groups[index].autoRule.empty()) {
      editing = false;
      applyAutoRules();
    }
    return;
  }
  case ToggleHideAllBoxes:
    setAllBoxesHidden(!boxesHidden);
    return;
  case CopyItems:
    copySelection(*panel, false);
    return;
  case CutItems:
    copySelection(*panel, true);
    return;
  case PasteIntoBox:
    editing = false;
    pasteIntoPanel(*panel);
    return;
  case DeleteItems:
    editing = false;
    deleteSelection(*panel, false);
    return;
  case PermanentDeleteItems:
    editing = false;
    deleteSelection(*panel, true);
    return;
  case ItemProperties:
    if (selected >= 0 && static_cast<size_t>(selected) < panel->displayed.size()) {
      const auto &item = panel->displayed[selected];
      const auto path = panel->folderView() ? item : boxItem(item);
      if (!path.empty()) {
        PIDLIST_ABSOLUTE id = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) && id) {
          SHELLEXECUTEINFOW exec{sizeof(exec)};
          exec.fMask = SEE_MASK_INVOKEIDLIST;
          exec.hwnd = owner;
          exec.lpVerb = L"properties";
          exec.lpIDList = id;
          exec.nShow = SW_SHOWNORMAL;
          ShellExecuteExW(&exec);
          CoTaskMemFree(id);
        }
      }
    }
    return;
  case NewBoxFolder:
    editing = false;
    createNewItemInPanel(*panel, true);
    return;
  case NewBoxTextFile:
    editing = false;
    createNewItemInPanel(*panel, false);
    return;
  case ToggleGroup: {
    auto changed = groups;
    changed[index].collapsed = !panel->collapsed();
    if (commit(std::move(changed), owner, false)) {
      panel->autoCollapsed = false;
      panel->autoCollapseAt = 0;
      panel->setCollapsed();
    }
    return;
  }
  case RenameGroup: {
    if (!groups[index].tabs.empty() && tabIndex > 0 && tabIndex <= groups[index].tabs.size()) {
      auto name = groups[index].tabs[tabIndex - 1].name;
      if (!showBoxDialog(instance, owner, L"重命名盒子", &name, lang::text(L"盒子名称")) || quitting) return;
      const auto first = name.find_first_not_of(L" \t\r\n");
      if (first == name.npos) return;
      name = name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
      if (name == groups[index].tabs[tabIndex - 1].name) return;
      auto changed = groups;
      changed[index].tabs[tabIndex - 1].name = std::move(name);
      if (commit(std::move(changed), owner, false)) {
#ifdef FOLDERNOTE_WINUI
        panel->syncSurfaceHeader();
#endif
        InvalidateRect(panel->tabs, nullptr, FALSE);
      }
      return;
    }
    auto name = groups[index].name;
    if (!showBoxDialog(instance, owner, L"重命名盒子", &name, lang::text(L"盒子名称")) || quitting) return;
    renameBox(*panel, std::move(name));
    return;
  }
  case DeleteGroup: {
    if (!groups[index].tabs.empty()) {
      const size_t target = std::min(tabIndex, groups[index].tabs.size());
      const auto tabName = target == 0 ? groups[index].name : groups[index].tabs[target - 1].name;
      const bool isMapped = !folderForTab(groups[index], target).empty();
      const auto prompt = isMapped
          ? lang::format(L"删除盒子“{0}”？仅移除该映射标签，源文件夹中的文件不会被删除。", {tabName})
          : lang::format(L"删除盒子“{0}”？已收纳的文件会移回个人桌面。", {tabName});
      if (!showBoxDialog(instance, owner, L"删除盒子", nullptr, prompt) || quitting) return;
      if (groups[index].tabs.empty() || target > groups[index].tabs.size()) return;
      auto changed = groups;
      auto removed = deleteTab(changed, index, target);
      if (!removed) return;
      const bool hadItems = !removed->paths.empty();
      if (releaseItems(std::move(changed), removed->paths, owner, hadItems)) {
        std::erase_if(panel->activeTransfers, [target](const auto &e) { return e.tab == target; });
        for (auto &e : panel->activeTransfers) {
          if (e.tab > target) --e.tab;
        }
        panel->syncSurfaceTransfers();
        if (panel->activeTab == target) {
          panel->activeTab = std::min(target, groups[index].tabs.size());
          panel->currentPage = panel->pageWheelRemainder = 0;
          panel->hoveredFolderRow = -1;
          panel->suppressedHoverRow = -1;
        } else if (panel->activeTab > target) {
          --panel->activeTab;
        }
        SetWindowTextW(owner, groups[index].name.c_str());
        panel->applyBackground();
        panel->refresh(groups[index]);
        panel->resize();
      }
      return;
    }
    if (!showBoxDialog(instance, owner, L"删除盒子", nullptr,
                       lang::format(L"删除盒子“{0}”？已收纳的文件会移回个人桌面。", {groups[index].name})) || quitting) return;
    auto paths = groups[index].paths;
    auto changed = groups;
    changed.erase(changed.begin() + index);
    if (releaseItems(std::move(changed), paths, owner)) {
      panels.erase(panels.begin() + index);
      for (size_t i = index; i < panels.size(); ++i) panels[i]->index = i;
      updateShadows();
    }
    return;
  }
  case OpenItem:
    if (selected >= 0 && static_cast<size_t>(selected) < panel->displayed.size()) {
      const auto &item = panel->displayed[selected];
      if (isTransferToken(item)) return;
      const auto path = panel->folderView() ? item : boxItem(item);
      if (path.empty() ||
          reinterpret_cast<INT_PTR>(ShellExecuteW(owner, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        report(owner, lang::text(L"无法打开此桌面项目。"));
    }
    return;
  case RemoveItem:
    if (!panel->folderView()) {
      std::vector<std::wstring> toUngroup;
      for (int r = -1; (r = ListView_GetNextItem(panel->list, r, LVNI_SELECTED)) >= 0;) {
        if (static_cast<size_t>(r) < panel->displayed.size()) toUngroup.push_back(panel->displayed[r]);
      }
      if (!toUngroup.empty()) ungroup(toUngroup, owner);
    }
    return;
  case RenameItem:
    if (selected >= 0 && static_cast<size_t>(selected) < panel->displayed.size()) {
      PostMessageW(owner, beginItemRenameMessage, static_cast<WPARAM>(selected), 0);
    }
    return;
  default:
    return;
  }
}

void Desktop::clearDropFeedback() {
  if (dropHighlight) {
    const auto previous = std::find_if(panels.begin(), panels.end(), [&](const auto &panel) {
      return panel->window == dropHighlight;
    });
    if (previous != panels.end()) {
#ifdef FOLDERNOTE_WINUI
      if ((*previous)->hasSurface()) (*previous)->surface->showItemInsertion(std::nullopt);
#endif
      InvalidateRect((*previous)->list, &dropMarker, FALSE);
    }
    highlight(nullptr);
  }
  dropMarker = {};
  lastDragScroll = 0;
}

void Desktop::drag(Panel &panel, int index) {
  const auto &items = panel.displayed;
  if (index < 0 || static_cast<size_t>(index) >= items.size() || isTransferToken(items[index]) ||
      editing || dragging || quitting) return;
  const bool fromMapped = panel.folderView();
  const auto resolvePath = [&](const std::wstring &candidate) {
    return fromMapped ? candidate : boxItem(candidate);
  };
  const auto primaryPath = resolvePath(items[index]);
  if (primaryPath.empty()) return;

  struct DragSlotInfo {
    std::wstring displayKey;
    size_t slot = 0;
    bool isDir = false;
  };
  std::vector<std::wstring> paths;
  std::vector<DragSlotInfo> slotInfos;
  const auto recordDragItem = [&](size_t rowIdx) {
    if (isTransferToken(items[rowIdx])) return;
    if (auto resolved = resolvePath(items[rowIdx]); !resolved.empty()) {
      const bool isDir = !isDesktopShellItem(resolved) && PathIsDirectoryW(resolved.c_str()) != FALSE;
      slotInfos.push_back({items[rowIdx], rowIdx, isDir});
      paths.push_back(std::move(resolved));
    }
  };
  if (ListView_GetItemState(panel.list, index, LVIS_SELECTED) & LVIS_SELECTED) {
    for (int row = -1; (row = ListView_GetNextItem(panel.list, row, LVNI_SELECTED)) >= 0;) {
      if (static_cast<size_t>(row) < items.size()) recordDragItem(static_cast<size_t>(row));
    }
  }
  if (paths.empty()) recordDragItem(static_cast<size_t>(index));
  if (paths.empty()) return;

  const auto clearLiftedOnSource = [&]() {
    if (auto *src = panelForWindow(panel.window)) {
      const bool removedAny = std::erase_if(src->activeTransfers, [](const Panel::TransferEntry &e) {
        return e.dragLifted;
      }) != 0;
      if (removedAny) src->syncSurfaceTransfers();
    }
  };

  const HWND sourceWindow = panel.window;
  auto dataObject = createDragDataObject(sourceWindow, paths);
  if (!dataObject) return;

  const bool listMode = groups[panel.index].view == View::List;
  LVITEMW item{};
  item.mask = LVIF_IMAGE;
  item.iItem = index;
  const auto images = ListView_GetImageList(panel.list, listMode ? LVSIL_SMALL : LVSIL_NORMAL);
  HICON icon = nullptr;
  if (!listMode && images && ListView_GetItem(panel.list, &item) && item.iImage >= 0) {
    icon = ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT);
  }
  if (!icon) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(primaryPath.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
      icon = info.hIcon;
    } else if (images && ListView_GetItem(panel.list, &item) && item.iImage >= 0) {
      icon = ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT);
    }
  }
  if (icon) {
    RECT preview{};
    POINT cursor{};
    bool hasPreview = false;
#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) hasPreview = panel.surface->imageBounds(index, preview);
#endif
    if (!panel.hasSurface() && ListView_GetItemRect(panel.list, index, &preview, LVIR_ICON)) {
      MapWindowPoints(panel.list, HWND_DESKTOP, reinterpret_cast<POINT *>(&preview), 2);
      hasPreview = !IsRectEmpty(&preview);
    }
    const auto dpi = GetDpiForWindow(sourceWindow);
    const LONG minSize = MulDiv(48, dpi, 96);
    SIZE size{minSize, minSize};
    POINT anchor{size.cx / 2, size.cy / 2};
    if (!listMode && hasPreview && GetCursorPos(&cursor)) {
      size = {std::clamp(preview.right - preview.left, minSize, 256L),
              std::clamp(preview.bottom - preview.top, minSize, 256L)};
      anchor = {std::clamp(cursor.x - preview.left, 0L, size.cx - 1),
                std::clamp(cursor.y - preview.top, 0L, size.cy - 1)};
    }
    wchar_t labelBuf[512]{};
    ListView_GetItemText(panel.list, index, 0, labelBuf, ARRAYSIZE(labelBuf));
    std::wstring label = labelBuf[0] ? std::wstring(labelBuf) : fn::fs::path(primaryPath).filename().wstring();
    attachDragImage(dataObject.Get(), icon, size, anchor, label, paths.size(), dpi);
    DestroyIcon(icon);
  }

  DWORD effect = DROPEFFECT_NONE;
  HRESULT dragResult = DRAGDROP_S_CANCEL;
  std::optional<InternalDrop> internalDrop;
  {
    struct DragScope {
      Desktop &owner;
      HWND srcWnd;
      DragScope(Desktop &desktop, HWND source) : owner(desktop), srcWnd(source) {}
      ~DragScope() {
#ifdef FOLDERNOTE_WINUI
        if (auto *srcPanel = owner.panelForWindow(srcWnd); srcPanel && srcPanel->hasSurface()) {
          srcPanel->surface->setDragActive(false);
        }
#endif
        owner.dragging = false;
        owner.oleDragActive = false;
        owner.oleDragFromMapped = false;
        owner.dragWindow = nullptr;
        owner.dragItems.clear();
        owner.clearDropFeedback();
      }
    } scope(*this, sourceWindow);

#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) {
      panel.surface->setDragActive(true);
      for (size_t di = 0; di < paths.size(); ++di) {
        Panel::TransferEntry lifted;
        lifted.id = nextTransferId++;
        lifted.tokenPath = slotInfos[di].displayKey;
        lifted.realPath = paths[di];
        lifted.tab = panel.activeTab;
        lifted.outgoing = true;
        lifted.dragLifted = true;
        lifted.visualProgress = -1.0;
        panel.activeTransfers.push_back(std::move(lifted));
      }
      panel.syncSurfaceTransfers();
    }
#endif
    dragging = true;
    oleDragActive = true;
    oleDragFromMapped = fromMapped;
    dragWindow = sourceWindow;
    dragItems = paths;
    pendingInternalDrop.reset();

    if (GetCapture()) ReleaseCapture();
    if (const HWND desktopRoot = GetAncestor(sourceWindow, GA_ROOT)) {
      SetForegroundWindow(desktopRoot);
    }

    // WinUI 3's InputSiteWindowClass holds a WM_POINTER contact on the UI thread,
    // starving ole32's capture window of legacy WM_MOUSEMOVE / WM_LBUTTONUP messages.
    // Pump live cursor updates directly into the thread's OLE capture HWND, while
    // passing pdsrc=nullptr to SHDoDragDrop so Shell's CDropSource updates DragWindow's
    // dynamic DropDescription (e.g. "移动到 回收站", "固定到 PowerPoint", "移动到 新建盒子").
    const DWORD dragThreadId = GetCurrentThreadId();
    std::atomic<bool> stopPump{false};
    std::thread pump([dragThreadId, &stopPump]() {
      POINT last{LONG_MIN, LONG_MIN};
      bool primed = false;
      const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
      const int primaryButton = swapped ? VK_RBUTTON : VK_LBUTTON;
      const int secondaryButton = swapped ? VK_LBUTTON : VK_RBUTTON;
      while (!stopPump.load(std::memory_order_relaxed)) {
        GUITHREADINFO gui{sizeof(gui)};
        if (GetGUIThreadInfo(dragThreadId, &gui) && gui.hwndCapture) {
          POINT pt{};
          if (GetCursorPos(&pt)) {
            POINT client = pt;
            ScreenToClient(gui.hwndCapture, &client);
            const LPARAM coords = MAKELPARAM(client.x, client.y);
            WPARAM keys = 0;
            if (GetAsyncKeyState(VK_CONTROL) < 0) keys |= MK_CONTROL;
            if (GetAsyncKeyState(VK_SHIFT) < 0) keys |= MK_SHIFT;
            if (GetAsyncKeyState(VK_MENU) < 0) keys |= 0x20; // MK_ALT
            if (GetAsyncKeyState(VK_ESCAPE) < 0) {
              PostMessageW(gui.hwndCapture, WM_KEYDOWN, VK_ESCAPE, 0);
              break;
            }
            if (GetAsyncKeyState(secondaryButton) < 0) {
              PostMessageW(gui.hwndCapture, WM_RBUTTONDOWN, keys | MK_RBUTTON, coords);
              break;
            }
            if (GetAsyncKeyState(primaryButton) < 0) {
              keys |= MK_LBUTTON;
              if (!primed || pt.x != last.x || pt.y != last.y) {
                primed = true;
                last = pt;
                PostMessageW(gui.hwndCapture, WM_MOUSEMOVE, keys, coords);
              }
            } else {
              if (!primed) {
                PostMessageW(gui.hwndCapture, WM_MOUSEMOVE, keys | MK_LBUTTON, coords);
              }
              PostMessageW(gui.hwndCapture, WM_LBUTTONUP, keys, coords);
              break;
            }
          }
        }
        Sleep(8);
      }
    });
    dragResult = SHDoDragDrop(nullptr, dataObject.Get(), nullptr,
                              DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    stopPump.store(true, std::memory_order_relaxed);
    if (pump.joinable()) pump.join();
    internalDrop = std::exchange(pendingInternalDrop, std::nullopt);
  }

  clearLiftedOnSource();
  if (quitting) return;

  const auto startOutgoingVisual = [&](const std::wstring &resolvedCandidate, bool alreadyGone) {
    auto *srcPanel = panelForWindow(sourceWindow);
    if (!srcPanel) return;
    size_t slotHint = srcPanel->orderedItems.size();
    bool isDir = false;
    std::wstring displayKey = resolvedCandidate;
    for (size_t di = 0; di < paths.size(); ++di) {
      if (samePath(paths[di], resolvedCandidate)) {
        slotHint = slotInfos[di].slot;
        isDir = slotInfos[di].isDir;
        displayKey = slotInfos[di].displayKey;
        break;
      }
    }
    if (alreadyGone) {
      addOutgoingFarewell(*srcPanel, resolvedCandidate, slotHint, isDir);
    } else if (!isDesktopShellItem(resolvedCandidate)) {
      std::erase_if(srcPanel->activeTransfers, [&](const Panel::TransferEntry &e) {
        return !e.placeholder && samePath(e.tokenPath, displayKey);
      });
      Panel::TransferEntry out;
      out.id = nextTransferId++;
      out.tokenPath = displayKey;
      out.realPath = resolvedCandidate;
      out.iconSourcePath = resolvedCandidate;
      out.displayLabel = hoverEntryTitle(resolvedCandidate, isDir);
      out.tab = srcPanel->activeTab;
      out.insertSlot = slotHint;
      out.isDirectory = isDir;
      out.outgoing = true;
      out.visualProgress = 0.12;
      out.startedAt = GetTickCount64();
      srcPanel->activeTransfers.push_back(std::move(out));
      srcPanel->syncSurfaceTransfers();
      ensureTransferTimer();
    }
  };

  if (internalDrop) {
    auto *target = panelForWindow(internalDrop->targetPanel);
    if (!internalDrop->folder.empty()) {
      if (internalDrop->effect == DROPEFFECT_MOVE || isRecycleBinShellItem(internalDrop->folder)) {
        for (const auto &p : paths) startOutgoingVisual(p, true);
      }
      moveInto(paths, internalDrop->folder, sourceWindow, internalDrop->effect);
    } else if (target) {
      if (internalDrop->effect == DROPEFFECT_COPY ||
          (internalDrop->effect == DROPEFFECT_LINK &&
           std::any_of(paths.begin(), paths.end(), [](const auto &p) { return !isDesktopShellItem(p); }))) {
        assignCopyOrLink(*target, paths, internalDrop->point, internalDrop->effect);
      } else {
        if (target->window != sourceWindow) {
          for (const auto &p : paths) startOutgoingVisual(p, true);
        }
        assign(*target, paths, internalDrop->point, fromMapped);
      }
    }
    return;
  }

  if (dragResult == DRAGDROP_S_CANCEL) return;

  POINT fallbackPoint{};
  if (effect == DROPEFFECT_NONE && GetCursorPos(&fallbackPoint)) {
    if (auto *target = panelAt(fallbackPoint)) {
      const bool virtualItems = std::any_of(paths.begin(), paths.end(), isDesktopShellItem);
      const auto folder = !target->folderView() && virtualItems ? std::wstring{} : dropFolder(target, fallbackPoint);
      if (!folder.empty() && canMoveInto(paths, folder)) {
        for (const auto &p : paths) startOutgoingVisual(p, true);
        moveInto(paths, folder, sourceWindow);
        return;
      }
      if (!target->folderView()) {
        if (target->window != sourceWindow) {
          for (const auto &p : paths) startOutgoingVisual(p, true);
        }
        assign(*target, paths, fallbackPoint, fromMapped);
        return;
      }
    }
  }

  const auto targetClsid = readTargetClsid(dataObject.Get());
  const bool droppedOnRecycleBin = targetClsid && IsEqualCLSID(*targetClsid, CLSID_RecycleBin);
  const auto logicalPerformed = readDropEffect(dataObject.Get(), CFSTR_LOGICALPERFORMEDDROPEFFECT);
  const auto physicalPerformed = readDropEffect(dataObject.Get(), CFSTR_PERFORMEDDROPEFFECT);
  const DWORD performed = logicalPerformed.value_or(physicalPerformed.value_or(effect));
  const bool isCopyOrLink = !droppedOnRecycleBin && (performed & (DROPEFFECT_COPY | DROPEFFECT_LINK)) != 0;

  const auto missingFile = [](const std::wstring &candidate) {
    return !isDesktopShellItem(candidate) &&
           GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES &&
           GetLastError() == ERROR_FILE_NOT_FOUND;
  };

  if (dragResult == DRAGDROP_S_DROP && !isCopyOrLink) {
    const bool hasRealFiles = std::any_of(paths.begin(), paths.end(), [](const auto &p) { return !isDesktopShellItem(p); });
    if (hasRealFiles) {
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (std::all_of(paths.begin(), paths.end(), [&](const std::wstring &p) {
              return isDesktopShellItem(p) || missingFile(p);
            })) {
          break;
        }
        Sleep(12);
      }
    }
  }

  if (fromMapped) {
    if (std::any_of(paths.begin(), paths.end(), missingFile)) {
      for (const auto &p : paths) {
        if (missingFile(p)) startOutgoingVisual(p, true);
      }
      if (auto *sourcePanel = panelForWindow(sourceWindow)) {
        sourcePanel->mappedResult = {};
        sourcePanel->refresh(groups[sourcePanel->index]);
      }
    } else if (dragResult == DRAGDROP_S_DROP && !isCopyOrLink) {
      for (const auto &p : paths) startOutgoingVisual(p, false);
      lastFolderRefresh = 0;
    }
    return;
  }

  std::vector<std::wstring> movedAway;
  std::vector<std::wstring> remaining;
  for (const auto &candidate : paths) {
    if (missingFile(candidate)) movedAway.push_back(candidate);
    else remaining.push_back(candidate);
  }
  if (!movedAway.empty()) {
    for (const auto &m : movedAway) startOutgoingVisual(m, true);
    auto changed = groups;
    bool removed = false;
    const auto wasMoved = [&](const std::wstring &entry) {
      return std::any_of(movedAway.begin(), movedAway.end(),
                         [&](const std::wstring &moved) { return samePath(entry, moved); });
    };
    for (auto &group : changed) {
      removed |= std::erase_if(group.paths, wasMoved) != 0;
      for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, wasMoved) != 0;
    }
    if (removed) commit(std::move(changed), sourceWindow);
  }

  if (remaining.empty() || isCopyOrLink) return;

  const auto trackPendingMove = [&](const std::wstring &candidate) {
    if (isDesktopShellItem(candidate)) return;
    const bool alreadyTracked = std::any_of(cutPaths.begin(), cutPaths.end(), [&](const std::wstring &p) {
      return samePath(p, candidate);
    });
    if (!alreadyTracked) cutPaths.push_back(candidate);
    startOutgoingVisual(candidate, false);
  };

  if (droppedOnRecycleBin) {
    std::vector<std::wstring> virtualRemaining;
    for (const auto &candidate : remaining) {
      if (isDesktopShellItem(candidate)) virtualRemaining.push_back(candidate);
      else trackPendingMove(candidate);
    }
    if (!virtualRemaining.empty()) ungroup(virtualRemaining, sourceWindow);
    return;
  }

  POINT endPoint{};
  const bool onDesktop = GetCursorPos(&endPoint) && !panelAt(endPoint) && desktopDropAt(endPoint);
  const bool onBlankDesktop = onDesktop && !desktopIconAt(host, endPoint);
  if (onBlankDesktop) {
    std::vector<std::wstring> directUngroup;
    for (const auto &candidate : remaining) {
      if (isDesktopShellItem(candidate)) {
        directUngroup.push_back(candidate);
      } else if (dragResult == DRAGDROP_S_DROP) {
        const auto parent = fn::fs::path(candidate).parent_path().native();
        const bool alreadyOnDesktop = boxStoredItem(roots, candidate).empty() &&
            std::any_of(roots.begin(), roots.end(), [&](const auto &r) { return samePath(parent, r); });
        if (alreadyOnDesktop) directUngroup.push_back(candidate);
        else trackPendingMove(candidate);
      } else {
        directUngroup.push_back(candidate);
      }
    }
    if (!directUngroup.empty()) ungroup(directUngroup, sourceWindow);
  } else if (dragResult == DRAGDROP_S_DROP) {
    for (const auto &candidate : remaining) trackPendingMove(candidate);
  }
}

std::wstring Desktop::dropFolder(Panel *panel, POINT point) const {
  std::wstring path;
  if (panel) {
    if (panel->collapsed()) return {};
    RECT bounds{};
    GetClientRect(panel->window, &bounds);
    bounds.top = panel->headerHeight();
    auto client = point;
    ScreenToClient(panel->window, &client);
    if (!PtInRect(&bounds, client)) return {};

    int row = -1;
#ifdef FOLDERNOTE_WINUI
    if (panel->hasSurface()) row = panel->surface->hit(point);
#endif
    if (!panel->hasSurface() && IsWindowVisible(panel->list)) {
      LVHITTESTINFO item{};
      item.pt = point;
      ScreenToClient(panel->list, &item.pt);
      row = ListView_HitTest(panel->list, &item);
    }
    if (row >= 0 && static_cast<size_t>(row) < panel->displayed.size()) path = panel->displayed[row];
    if (isRecycleBinShellItem(path)) return recycleBinShellPath;
    if (path.empty() || isDesktopShellItem(path) || !PathIsDirectoryW(path.c_str())) {
      return panel->activeFolder();
    }
  }
  return path;
}

bool Desktop::canMoveInto(std::span<const std::wstring> paths, const std::wstring &folder) const {
  if (isRecycleBinShellItem(folder)) {
    return !paths.empty() && std::all_of(paths.begin(), paths.end(), [](const auto &path) {
      return !path.empty() && !isDesktopShellItem(path);
    });
  }
  return !paths.empty() && std::all_of(paths.begin(), paths.end(), [&](const auto &path) {
    if (path.empty() || isDesktopShellItem(path) || samePath(path, folder)) return false;
    if (folder.size() > path.size() && folder[path.size()] == L'\\' &&
        CompareStringOrdinal(path.data(), static_cast<int>(path.size()), folder.data(),
                             static_cast<int>(path.size()), TRUE) == CSTR_EQUAL) return false;
    return !samePath(fn::fs::path(path).parent_path().native(), folder);
  });
}

void Desktop::moveInto(std::span<const std::wstring> paths, const std::wstring &folder, HWND owner, DWORD effect) {
  if (editing || quitting || paths.empty() || folder.empty()) return;
  if (effect == DROPEFFECT_MOVE && !canMoveInto(paths, folder)) return;
  struct TransferScope {
    bool &active;
    explicit TransferScope(bool &flag) : active(flag) { active = true; }
    ~TransferScope() { active = false; }
  } transfer(editing);

  if (effect == DROPEFFECT_LINK && !isRecycleBinShellItem(folder)) {
    bool anyCreated = false;
    for (const auto &path : paths) {
      if (!createShortcutInFolder(path, folder).empty()) anyCreated = true;
    }
    PostMessageW(window, refreshMessage, 0, 0);
    if (!anyCreated && !quitting) report(owner, lang::text(L"无法完成复制 / 移动操作"));
    return;
  }

  using ItemId = std::unique_ptr<void, decltype(&CoTaskMemFree)>;
  std::vector<ItemId> ownedIds;
  std::vector<PCIDLIST_ABSOLUTE> ids;
  ownedIds.reserve(paths.size());
  ids.reserve(paths.size());
  for (const auto &path : paths) {
    PIDLIST_ABSOLUTE id = nullptr;
    HRESULT result = SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr);
    ownedIds.emplace_back(static_cast<void *>(id), CoTaskMemFree);
    if (FAILED(result) || !id) return;
    ids.push_back(id);
  }
  Microsoft::WRL::ComPtr<IShellItemArray> items;
  HRESULT result = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &items);
  if (SUCCEEDED(result) && isRecycleBinShellItem(folder)) {
    Microsoft::WRL::ComPtr<IFileOperation> op;
    result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (SUCCEEDED(result) && op) {
      op->SetOwnerWindow(owner);
      op->SetOperationFlags((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? FOF_WANTNUKEWARNING : FOF_ALLOWUNDO);
      result = op->DeleteItems(items.Get());
      if (SUCCEEDED(result)) {
        SetForegroundWindow(owner);
        result = op->PerformOperations();
        if (result == COPYENGINE_E_USER_CANCELLED) result = S_OK;
      }
    }
  } else if (SUCCEEDED(result)) {
    Microsoft::WRL::ComPtr<IShellItem> destination;
    result = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&destination));
    if (SUCCEEDED(result)) {
      const auto mode = effect == DROPEFFECT_COPY ? quick::MenuMode::Copy : quick::MenuMode::Move;
      result = quick::transfer::perform(owner, items.Get(), destination.Get(), mode, [&] { return !quitting; });
    }
  }

  if (effect == DROPEFFECT_MOVE || isRecycleBinShellItem(folder)) {
    const auto moved = [&](const std::wstring &path) {
      return std::any_of(paths.begin(), paths.end(), [&](const auto &source) {
        return samePath(source, path) && GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND;
      });
    };
    auto changed = groups;
    bool removed = false;
    for (auto &group : changed) {
      removed |= std::erase_if(group.paths, moved) != 0;
      for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, moved) != 0;
    }
    // A cancelled transfer can still have completed some of the requested moves.
    if (removed) commit(std::move(changed), owner);
    else PostMessageW(window, refreshMessage, 0, 0);
  } else {
    PostMessageW(window, refreshMessage, 0, 0);
  }
  if (FAILED(result) && !quitting) report(owner, lang::text(L"无法完成复制 / 移动操作"));
}

bool Desktop::releaseItems(std::vector<Group> changed, std::span<const std::wstring> paths, HWND owner,
                           bool refreshContents) {
  BoxFileMoves moves(roots, owner, paths.size());
  for (const auto &path : paths) {
    if (!boxStoredItem(roots, path).empty() && moves.transfer(path, false).empty()) {
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        continue;
      report(owner, lang::text(L"无法将文件移回桌面。"));
      return false;
    }
  }
  if (!commit(std::move(changed), owner, refreshContents)) return false;
  moves.keep();
  return true;
}

bool Desktop::ungroup(std::span<const std::wstring> paths, HWND owner) {
  if (auto *srcPanel = panelForWindow(owner)) {
    for (const auto &p : paths) {
      auto found = std::find_if(srcPanel->orderedItems.begin(), srcPanel->orderedItems.end(),
                                [&](const auto &item) { return samePath(item, p); });
      const size_t slot = found != srcPanel->orderedItems.end()
                              ? static_cast<size_t>(found - srcPanel->orderedItems.begin())
                              : srcPanel->orderedItems.size();
      const bool isDir = !isDesktopShellItem(p) && PathIsDirectoryW(p.c_str()) != FALSE;
      addOutgoingFarewell(*srcPanel, p, slot, isDir);
    }
  }
  auto changed = groups;
  bool removed = false;
  const auto included = [&](const auto &path) {
    return std::any_of(paths.begin(), paths.end(), [&](const auto &candidate) { return samePath(candidate, path); });
  };
  for (auto &group : changed) {
    removed |= std::erase_if(group.paths, included) != 0;
    for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, included) != 0;
  }
  return removed && releaseItems(std::move(changed), paths, owner);
}

Insertion Panel::insertion(POINT point) const {
  const auto &group = desktop.groups[index];
  Insertion insertion;
#ifdef FOLDERNOTE_WINUI
  if (surface && surface->available()) {
    if (group.sort != Sort::Manual || collapsed()) return insertion;
    const bool rows = group.view == View::List;
    const auto dpi = GetDpiForWindow(window);
    const auto thickness = std::max(2, MulDiv(3, dpi, 96));
    const auto gap = std::max(2, MulDiv(2, dpi, 96));
    insertion.item = surface->hit(point);
    RECT item{};
    RECT winBounds{};
    GetWindowRect(window, &winBounds);
    RECT contentBounds = winBounds;
    contentBounds.top += headerHeight();
    if (insertion.item >= 0 && surface->itemBounds(insertion.item, item)) {
      insertion.after = rows ? point.y >= (item.top + item.bottom) / 2 :
                               point.x >= (item.left + item.right) / 2;
    } else {
      const int last = static_cast<int>(displayed.size()) - 1;
      if (!PtInRect(&contentBounds, point) || last < 0 || !surface->itemBounds(last, item))
        return insertion;
      insertion.after = true;
    }
    if (rows) {
      const auto edge = insertion.after ? item.bottom : item.top;
      insertion.marker = {item.left + gap * 2, edge - thickness / 2, item.right - gap * 2, edge - thickness / 2 + thickness};
    } else {
      const auto edge = insertion.after ? item.right + gap : item.left - gap;
      insertion.marker = {edge - thickness / 2, item.top + gap * 2, edge - thickness / 2 + thickness, item.bottom - gap * 2};
    }
    IntersectRect(&insertion.marker, &insertion.marker, &contentBounds);
    return insertion;
  }
#endif
  if (group.sort != Sort::Manual || !IsWindowVisible(list)) return insertion;
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

void Desktop::ensureTransferTimer() {
  if (!transferTimerRunning && IsWindow(window)) {
    transferTimerRunning = SetTimer(window, transferTimer, 25, nullptr) != 0;
  }
}

void Desktop::addSettlePulse(Panel &panel, const std::wstring &storedPath) {
  if (storedPath.empty()) return;
  std::erase_if(panel.activeTransfers, [&](const Panel::TransferEntry &e) {
    return samePath(e.tokenPath, storedPath);
  });
  const ULONGLONG now = GetTickCount64();
  Panel::TransferEntry pulse;
  pulse.id = nextTransferId++;
  pulse.tokenPath = storedPath;
  pulse.realPath = storedPath;
  pulse.iconSourcePath = storedPath;
  pulse.tab = panel.activeTab;
  pulse.isDirectory = !isDesktopShellItem(storedPath) && PathIsDirectoryW(storedPath.c_str()) != FALSE;
  pulse.outgoing = false;
  pulse.finished = true;
  pulse.visualProgress = 0.12;
  pulse.startedAt = now;
  pulse.finishedAt = now;
  panel.activeTransfers.push_back(std::move(pulse));
  panel.syncSurfaceTransfers();
  ensureTransferTimer();
}

void Desktop::addOutgoingFarewell(Panel &panel, const std::wstring &removedPath, size_t slotHint, bool isDir) {
  if (removedPath.empty()) return;
  std::erase_if(panel.activeTransfers, [&](const Panel::TransferEntry &e) {
    return samePath(e.tokenPath, removedPath) || samePath(e.realPath, removedPath);
  });
  const ULONGLONG now = GetTickCount64();
  const uint64_t id = nextTransferId++;
  Panel::TransferEntry farewell;
  farewell.id = id;
  farewell.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(id) + L"\\" +
                       fn::fs::path(removedPath).filename().native();
  farewell.realPath = removedPath;
  farewell.iconSourcePath = removedPath;
  farewell.displayLabel = hoverEntryTitle(removedPath, isDir);
  farewell.tab = panel.activeTab;
  farewell.insertSlot = slotHint;
  farewell.isDirectory = isDir;
  farewell.placeholder = true;
  farewell.outgoing = true;
  farewell.finished = true;
  farewell.visualProgress = 0.22;
  farewell.startedAt = now;
  farewell.finishedAt = now;
  panel.activeTransfers.push_back(std::move(farewell));
  ensureTransferTimer();
}

bool Desktop::enqueueAsyncTransferIntoPanel(Panel &panel, const std::wstring &sourcePath, size_t insertSlot, bool isMove) {
  if (quitting || sourcePath.empty() || isDesktopShellItem(sourcePath)) return false;
  const DWORD attrs = GetFileAttributesW(sourcePath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  const bool intoMapped = panel.folderView();
  const std::wstring targetFolder = intoMapped ? panel.activeFolder() : boxStorageFolder(roots);
  if (targetFolder.empty()) return false;
  if (samePath(sourcePath, targetFolder) || samePath(sourcePath, boxStorageFolder(roots)) ||
      std::any_of(roots.begin(), roots.end(), [&](const auto &r) { return samePath(sourcePath, r); })) {
    return false;
  }
  if (intoMapped && isMove && samePath(fn::fs::path(sourcePath).parent_path().native(), targetFolder)) {
    return false;
  }

  const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const uint64_t id = nextTransferId++;
  auto progress = std::make_shared<std::atomic<double>>(0.0);

  Panel::TransferEntry entry;
  entry.id = id;
  entry.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(id) + L"\\" +
                    fn::fs::path(sourcePath).filename().native();
  entry.realPath = sourcePath;
  entry.iconSourcePath = sourcePath;
  entry.displayLabel = hoverEntryTitle(sourcePath, isDir);
  entry.tab = panel.activeTab;
  entry.insertSlot = insertSlot;
  entry.isDirectory = isDir;
  entry.placeholder = true;
  entry.outgoing = false;
  entry.finished = false;
  entry.visualProgress = 0.06;
  entry.sharedProgress = progress;
  entry.startedAt = GetTickCount64();
  panel.activeTransfers.push_back(std::move(entry));
  panel.refresh(groups[panel.index]);
  ensureTransferTimer();

  const HWND desktopWnd = window;
  const HWND panelWnd = panel.window;
  const auto rootsCopy = roots;
  const auto queue = completionQueue;
  std::thread([id, desktopWnd, panelWnd, sourcePath, targetFolder, rootsCopy, isMove, intoMapped, progress, queue] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring resultPath;
    if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
      const auto reportRatio = [progress](double r) {
        double prev = progress->load(std::memory_order_relaxed);
        if (r > prev) progress->store(r, std::memory_order_relaxed);
      };
      if (intoMapped) {
        resultPath = transferItemToFolder(sourcePath, targetFolder, isMove, panelWnd, false, reportRatio);
      } else if (isMove) {
        resultPath = moveToBoxStorage(rootsCopy, sourcePath, panelWnd, reportRatio);
      } else {
        resultPath = copyToBoxStorage(rootsCopy, sourcePath, panelWnd, reportRatio);
      }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    {
      std::lock_guard lock(queue->mutex);
      queue->items.push_back({id, panelWnd, sourcePath, std::move(resultPath), isMove, intoMapped});
    }
    if (IsWindow(desktopWnd)) PostMessageW(desktopWnd, transferDoneMessage, 0, 0);
  }).detach();
  return true;
}

void Desktop::handleCompletedTransfers() {
  if (quitting) return;
  std::vector<CompletedTransfer> doneList;
  {
    std::lock_guard lock(completionQueue->mutex);
    doneList.swap(completionQueue->items);
  }
  if (doneList.empty()) return;

  const ULONGLONG now = GetTickCount64();
  for (auto &done : doneList) {
    auto *panel = panelForWindow(done.panelWindow);
    if (!panel) continue;
    auto entryIt = std::find_if(panel->activeTransfers.begin(), panel->activeTransfers.end(),
                                [&](const Panel::TransferEntry &e) { return e.id == done.id; });
    if (done.resultPath.empty()) {
      if (entryIt != panel->activeTransfers.end()) panel->activeTransfers.erase(entryIt);
      panel->refresh(groups[panel->index]);
      continue;
    }

    const size_t desiredTab = entryIt != panel->activeTransfers.end() ? entryIt->tab : panel->activeTab;
    const size_t desiredSlot = entryIt != panel->activeTransfers.end() ? entryIt->insertSlot : SIZE_MAX;

    if (done.intoMapped) {
      WIN32_FILE_ATTRIBUTE_DATA data{};
      ULONGLONG mod = 0;
      DWORD attr = FILE_ATTRIBUTE_NORMAL;
      if (GetFileAttributesExW(done.resultPath.c_str(), GetFileExInfoStandard, &data)) {
        mod = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        attr = data.dwFileAttributes;
      }
      if (std::none_of(panel->mappedItems.begin(), panel->mappedItems.end(),
                       [&](const Panel::MappedItem &m) { return samePath(m.path, done.resultPath); })) {
        panel->mappedItems.push_back({done.resultPath, mod, attr});
      }
      panel->mappedResult = {};
      if (done.isMove && GetFileAttributesW(done.sourcePath.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetLastError() == ERROR_FILE_NOT_FOUND) {
        auto changed = groups;
        bool removed = false;
        const auto matchSrc = [&](const std::wstring &p) { return samePath(p, done.sourcePath); };
        for (auto &g : changed) {
          removed |= std::erase_if(g.paths, matchSrc) != 0;
          for (auto &t : g.tabs) removed |= std::erase_if(t.paths, matchSrc) != 0;
        }
        if (removed) commit(std::move(changed), panel->window);
      }
    } else {
      auto changed = groups;
      if (done.isMove) {
        const auto removeOld = [&](std::vector<std::wstring> &entries) {
          std::erase_if(entries, [&](const auto &p) {
            return samePath(p, done.sourcePath) && !samePath(p, done.resultPath);
          });
        };
        for (auto &g : changed) {
          removeOld(g.paths);
          for (auto &t : g.tabs) removeOld(t.paths);
        }
      }
      addItem(changed, panel->index, done.resultPath, desiredTab);
      if (changed[panel->index].sort == Sort::Manual) {
        auto &tabItems = manualItems(changed[panel->index], desiredTab);
        auto found = std::find_if(tabItems.begin(), tabItems.end(),
                                  [&](const auto &p) { return samePath(p, done.resultPath); });
        if (found != tabItems.end() && desiredSlot < tabItems.size()) {
          const size_t from = static_cast<size_t>(found - tabItems.begin());
          const size_t to = std::min(desiredSlot, tabItems.size() - 1);
          if (from < to) std::rotate(tabItems.begin() + from, tabItems.begin() + from + 1, tabItems.begin() + to + 1);
          else if (to < from) std::rotate(tabItems.begin() + to, tabItems.begin() + from, tabItems.begin() + from + 1);
        }
      }
      commit(std::move(changed), panel->window, false);
    }

    if (entryIt != panel->activeTransfers.end()) {
      entryIt->tokenPath = done.resultPath;
      entryIt->realPath = done.resultPath;
      entryIt->iconSourcePath = done.resultPath;
      entryIt->placeholder = false;
      entryIt->finished = true;
      entryIt->finishedAt = now;
      entryIt->visualProgress = std::max(entryIt->visualProgress, 0.72);
    }
    for (size_t pi = 0; pi < panels.size(); ++pi) panels[pi]->refresh(groups[pi]);
  }
  ensureTransferTimer();
}

void Desktop::tickTransfers() {
  if (quitting) return;
  const ULONGLONG now = GetTickCount64();
  bool anyActive = false;
  bool anyMissingCut = false;
  const bool shellDialogActive = FindWindowW(L"OperationStatusWindow", nullptr) != nullptr ||
                                 FindWindowW(L"#32770", nullptr) != nullptr;

  for (const auto &panel : panels) {
    if (panel->activeTransfers.empty()) continue;
    bool changedVisual = false;
    bool needsRefresh = false;

    for (size_t i = 0; i < panel->activeTransfers.size();) {
      auto &entry = panel->activeTransfers[i];
      if (entry.dragLifted) {
        anyActive = true;
        ++i;
        continue;
      }
      if (entry.finished) {
        const double next = std::min(1.0, entry.visualProgress + std::max(0.11, (1.0 - entry.visualProgress) * 0.40));
        if (std::abs(next - entry.visualProgress) > 0.002) {
          entry.visualProgress = next;
          changedVisual = true;
        }
        if (entry.visualProgress >= 0.995 && now - entry.finishedAt >= 90) {
          const bool wasPlaceholder = entry.placeholder;
          panel->activeTransfers.erase(panel->activeTransfers.begin() + static_cast<ptrdiff_t>(i));
          if (wasPlaceholder) needsRefresh = true;
          changedVisual = true;
          continue;
        }
        anyActive = true;
        ++i;
        continue;
      }

      if (!entry.outgoing) {
        const double real = entry.sharedProgress ? entry.sharedProgress->load(std::memory_order_relaxed) : 0.0;
        const double target = real > 0.01
            ? std::clamp(0.08 + real * 0.88, entry.visualProgress, 0.96)
            : std::min(0.26, 0.06 + static_cast<double>(now - entry.startedAt) / 5500.0);
        if (target > entry.visualProgress) {
          entry.visualProgress = std::min(target, entry.visualProgress + std::max(0.008, (target - entry.visualProgress) * 0.28));
          changedVisual = true;
        }
        anyActive = true;
        ++i;
        continue;
      }

      // Active outgoing transfer (async Shell move / Recycle Bin)
      const bool fileGone = !entry.realPath.empty() &&
          GetFileAttributesW(entry.realPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetLastError() == ERROR_FILE_NOT_FOUND;
      if (fileGone) {
        if (panel->folderView()) {
          entry.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(entry.id) + L"\\" +
                            fn::fs::path(entry.realPath).filename().native();
          entry.placeholder = true;
          panel->mappedResult = {};
          needsRefresh = true;
        } else {
          const auto realTarget = entry.realPath;
          auto found = std::find_if(panel->orderedItems.begin(), panel->orderedItems.end(),
                                    [&](const auto &p) { return samePath(p, entry.tokenPath); });
          if (found != panel->orderedItems.end()) {
            entry.insertSlot = static_cast<size_t>(found - panel->orderedItems.begin());
          }
          entry.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(entry.id) + L"\\" +
                            fn::fs::path(realTarget).filename().native();
          entry.placeholder = true;
          anyMissingCut = true;
          needsRefresh = true;
        }
        entry.finished = true;
        entry.finishedAt = now;
        entry.visualProgress = std::max(entry.visualProgress, 0.68);
        changedVisual = true;
        anyActive = true;
        ++i;
        continue;
      }

      if (now - entry.startedAt > 1800 && !shellDialogActive) {
        panel->activeTransfers.erase(panel->activeTransfers.begin() + static_cast<ptrdiff_t>(i));
        changedVisual = true;
        continue;
      }

      const double target = std::min(0.88, 0.12 + static_cast<double>(now - entry.startedAt) / 7000.0);
      if (target > entry.visualProgress) {
        entry.visualProgress = std::min(target, entry.visualProgress + 0.015);
        changedVisual = true;
      }
      anyActive = true;
      ++i;
    }

    if (needsRefresh && !anyMissingCut) {
      panel->refresh(groups[panel->index]);
    } else if (changedVisual) {
      panel->syncSurfaceTransfers();
    }
  }

  if (anyMissingCut) cleanupMovedCutItems();

  if (!anyActive && transferTimerRunning) {
    KillTimer(window, transferTimer);
    transferTimerRunning = false;
  }
}

bool Desktop::assign(Panel &panel, std::span<const std::wstring> paths, POINT point, bool fromExplorer) {
  if (editing || quitting || panel.folderView()) return false;
  auto changed = groups;
  BoxFileMoves moves(roots, panel.window, paths.size());
  bool rejected = false, moveFailed = false, accepted = false, updated = false;
  const auto insertion = panel.insertion(point);
  auto &items = manualItems(changed[panel.index], panel.activeTab);
  const auto anchor = insertion.item >= 0 && static_cast<size_t>(insertion.item) < panel.displayed.size() &&
                      !isTransferToken(panel.displayed[insertion.item])
      ? std::find(items.begin(), items.end(), panel.displayed[insertion.item]) : items.end();
  size_t insertAt = static_cast<size_t>(anchor - items.begin()) + (anchor != items.end() && insertion.after);
  std::vector<std::wstring> seen;
  std::vector<std::wstring> pulseAdded;
  for (const auto &candidate : paths) {
    if (std::any_of(seen.begin(), seen.end(), [&](const auto &path) { return samePath(path, candidate); })) continue;
    seen.push_back(candidate);
    auto path = boxItem(candidate);
    if (path.empty()) {
      if (fromExplorer && enqueueAsyncTransferIntoPanel(panel, candidate, insertAt, true)) {
        ++insertAt;
        accepted = true;
        continue;
      }
      rejected = true;
      continue;
    }
    bool movedFromDesktop = false;
    if (fromExplorer && !isDesktopShellItem(path) && boxStoredItem(roots, path).empty()) {
      const auto &stored = moves.transfer(path, true);
      if (stored.empty()) {
        if (enqueueAsyncTransferIntoPanel(panel, path, insertAt, true)) {
          ++insertAt;
          accepted = true;
          continue;
        }
        moveFailed = true;
        continue;
      }
      const auto updatePath = [&](std::vector<std::wstring> &entries) {
        std::erase_if(entries, [&](const auto &entry) { return samePath(entry, stored); });
        for (auto &entry : entries)
          if (samePath(entry, path)) entry = stored;
      };
      for (auto &group : changed) {
        updatePath(group.paths);
        for (auto &tab : group.tabs) updatePath(tab.paths);
      }
      path = stored;
      updated = true;
      movedFromDesktop = true;
    }
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) { return samePath(item, path); });
    const bool exists = found != items.end();
    const size_t from = exists ? static_cast<size_t>(found - items.begin()) : items.size();
    if (!exists && !addItem(changed, panel.index, path, panel.activeTab)) return false;
    accepted = true;
    updated |= !exists;
    if (!exists || movedFromDesktop) pulseAdded.push_back(path);
    if (groups[panel.index].sort != Sort::Manual) continue;
    const auto slot = std::min(insertAt, items.size() - (exists ? 0 : 1));
    const auto to = slot - (from < slot ? 1 : 0);
    if (from < to) std::rotate(items.begin() + from, items.begin() + from + 1, items.begin() + to + 1);
    if (to < from) std::rotate(items.begin() + to, items.begin() + from, items.begin() + from + 1);
    updated |= to != from;
    insertAt = to + 1;
  }
  if (rejected || moveFailed) PostMessageW(panel.window, rejectedMessage, moveFailed, 0);
  if (updated && !commit(std::move(changed), panel.window)) return false;
  moves.keep();
  if (!pulseAdded.empty()) {
    panel.refresh(groups[panel.index]);
    for (const auto &added : pulseAdded) addSettlePulse(panel, added);
  }
  return accepted;
}

bool Desktop::assignCopyOrLink(Panel &panel, std::span<const std::wstring> paths, POINT point, DWORD effect) {
  if (editing || quitting || panel.folderView() || paths.empty()) return false;
  if (effect != DROPEFFECT_LINK) {
    const auto insertion = panel.insertion(point);
    const auto &existing = manualItems(groups[panel.index], panel.activeTab);
    const auto anchor = insertion.item >= 0 && static_cast<size_t>(insertion.item) < panel.displayed.size() &&
                        !isTransferToken(panel.displayed[insertion.item])
        ? std::find(existing.begin(), existing.end(), panel.displayed[insertion.item]) : existing.end();
    size_t insertAt = static_cast<size_t>(anchor - existing.begin()) + (anchor != existing.end() && insertion.after);
    std::vector<std::wstring> immediatePaths;
    bool anyQueued = false;
    for (const auto &candidate : paths) {
      if (candidate.empty()) continue;
      if (isDesktopShellItem(candidate)) {
        immediatePaths.push_back(candidate);
      } else if (enqueueAsyncTransferIntoPanel(panel, candidate, insertAt, false)) {
        ++insertAt;
        anyQueued = true;
      }
    }
    if (!immediatePaths.empty()) anyQueued |= assign(panel, immediatePaths, point, false);
    return anyQueued;
  }

  struct TransferScope {
    bool &active;
    explicit TransferScope(bool &flag) : active(flag) { active = true; }
    ~TransferScope() { active = false; }
  } transfer(editing);

  std::vector<std::wstring> createdPaths;
  createdPaths.reserve(paths.size());
  for (const auto &candidate : paths) {
    if (candidate.empty()) continue;
    if (isDesktopShellItem(candidate)) {
      createdPaths.push_back(candidate);
      continue;
    }
    std::wstring stored = createBoxShortcut(roots, candidate);
    if (!stored.empty()) createdPaths.push_back(std::move(stored));
  }
  if (createdPaths.empty()) return false;
  return assign(panel, createdPaths, point, false);
}

void Desktop::copySelection(Panel &panel, bool cut) {
  if (quitting) return;
  const auto paths = panel.selectedResolvedPaths();
  if (paths.empty()) return;
  auto dataObject = createDragDataObject(panel.window, paths);
  if (!dataObject) return;
  const bool allVirtual = std::all_of(paths.begin(), paths.end(), isDesktopShellItem);
  const DWORD effect = (cut && !allVirtual) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
  setDropEffect(dataObject.Get(), CFSTR_PREFERREDDROPEFFECT, effect);
  if (SUCCEEDED(OleSetClipboard(dataObject.Get()))) {
    cutPaths.clear();
    if (cut && !panel.folderView()) {
      for (const auto &p : paths) {
        if (!isDesktopShellItem(p)) cutPaths.push_back(p);
      }
    }
  }
}

void Desktop::pasteIntoPanel(Panel &panel) {
  if (editing || dragging || quitting) return;
  Microsoft::WRL::ComPtr<IDataObject> clipData;
  std::vector<std::wstring> paths;
  bool isMove = false;
  if (SUCCEEDED(OleGetClipboard(&clipData)) && clipData) {
    paths = desktopDropPaths(clipData.Get());
    if (paths.empty()) paths = desktopShellDropItems(clipData.Get());
    if (!paths.empty()) {
      const auto preferred = readDropEffect(clipData.Get(), CFSTR_PREFERREDDROPEFFECT);
      isMove = preferred && (*preferred & DROPEFFECT_MOVE) != 0 && (*preferred & DROPEFFECT_COPY) == 0;
    }
  }

  if (!paths.empty()) {
    clipData.Reset();
    if (panel.folderView()) {
      const auto targetFolder = panel.activeFolder();
      if (targetFolder.empty()) return;
      size_t slot = panel.orderedItems.size();
      bool anyQueued = false;
      for (const auto &p : paths) {
        if (enqueueAsyncTransferIntoPanel(panel, p, slot, isMove)) {
          ++slot;
          anyQueued = true;
        }
      }
      if (anyQueued && isMove) OleSetClipboard(nullptr);
      return;
    }

    if (!isMove) {
      assignCopyOrLink(panel, paths, {-1, -1}, DROPEFFECT_COPY);
      return;
    }

    if (assign(panel, paths, {-1, -1}, true)) {
      cutPaths.clear();
      OleSetClipboard(nullptr);
    }
    return;
  }
  clipData.Reset();

  if (!clipboardHasBitmap()) return;
  struct TransferScope {
    bool &active;
    explicit TransferScope(bool &flag) : active(flag) { active = true; }
    ~TransferScope() { active = false; }
  } scope(editing);

  const std::wstring prefix = lang::text(L"截图");
  if (panel.folderView()) {
    const auto saved = saveClipboardImageToFolder(panel.activeFolder(), prefix);
    if (saved.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    ULONGLONG mod = 0;
    DWORD attr = FILE_ATTRIBUTE_NORMAL;
    if (GetFileAttributesExW(saved.c_str(), GetFileExInfoStandard, &data)) {
      mod = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
      attr = data.dwFileAttributes;
    }
    panel.mappedItems.push_back({saved, mod, attr});
    panel.mappedResult = {};
    panel.refresh(groups[panel.index]);
    panel.selectAndRevealPath(saved, false);
    return;
  }

  const auto saved = saveClipboardImageToBox(roots, prefix);
  if (saved.empty()) return;
  auto changed = groups;
  if (addItem(changed, panel.index, saved, panel.activeTab) && commit(std::move(changed), panel.window, false)) {
    panel.refresh(groups[panel.index]);
    panel.selectAndRevealPath(saved, false);
  }
}

void Desktop::deleteSelection(Panel &panel, bool permanent) {
  if (editing || dragging || quitting) return;
  const auto paths = panel.selectedResolvedPaths();
  if (paths.empty()) return;

  std::vector<std::wstring> virtualItems;
  std::vector<std::wstring> fileItems;
  for (const auto &path : paths) {
    if (isDesktopShellItem(path)) virtualItems.push_back(path);
    else fileItems.push_back(path);
  }
  if (!virtualItems.empty() && !panel.folderView()) {
    ungroup(virtualItems, panel.window);
  }
  if (fileItems.empty() || quitting) return;

  struct PreDeleteSlot {
    std::wstring path;
    size_t slot = 0;
    bool isDir = false;
  };
  std::vector<PreDeleteSlot> preSlots;
  for (const auto &path : fileItems) {
    auto found = std::find_if(panel.orderedItems.begin(), panel.orderedItems.end(),
                              [&](const auto &item) { return samePath(item, path); });
    const size_t slot = found != panel.orderedItems.end()
                            ? static_cast<size_t>(found - panel.orderedItems.begin())
                            : panel.orderedItems.size();
    preSlots.push_back({path, slot, PathIsDirectoryW(path.c_str()) != FALSE});
  }

  {
    struct DeleteScope {
      bool &active;
      explicit DeleteScope(bool &flag) : active(flag) { active = true; }
      ~DeleteScope() { active = false; }
    } scope(editing);

    using ItemId = std::unique_ptr<void, decltype(&CoTaskMemFree)>;
    std::vector<ItemId> ownedIds;
    std::vector<PCIDLIST_ABSOLUTE> ids;
    ownedIds.reserve(fileItems.size());
    ids.reserve(fileItems.size());
    for (const auto &path : fileItems) {
      PIDLIST_ABSOLUTE id = nullptr;
      if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) && id) {
        ownedIds.emplace_back(static_cast<void *>(id), CoTaskMemFree);
        ids.push_back(id);
      }
    }
    if (!ids.empty()) {
      Microsoft::WRL::ComPtr<IShellItemArray> shellItems;
      Microsoft::WRL::ComPtr<IFileOperation> op;
      if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &shellItems)) &&
          shellItems && SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) && op) {
        op->SetOwnerWindow(panel.window);
        op->SetOperationFlags(permanent ? FOF_WANTNUKEWARNING : FOF_ALLOWUNDO);
        if (SUCCEEDED(op->DeleteItems(shellItems.Get()))) {
          SetForegroundWindow(panel.window);
          op->PerformOperations();
        }
      }
    }
  }
  if (quitting) return;

  const auto wasDeleted = [&](const std::wstring &candidate) {
    return std::any_of(fileItems.begin(), fileItems.end(), [&](const std::wstring &deleted) {
      return samePath(candidate, deleted) &&
             GetFileAttributesW(deleted.c_str()) == INVALID_FILE_ATTRIBUTES &&
             GetLastError() == ERROR_FILE_NOT_FOUND;
    });
  };
  for (const auto &slotInfo : preSlots) {
    if (wasDeleted(slotInfo.path)) {
      addOutgoingFarewell(panel, slotInfo.path, slotInfo.slot, slotInfo.isDir);
    }
  }
  if (panel.folderView()) {
    std::erase_if(panel.mappedItems, [&](const Panel::MappedItem &entry) { return wasDeleted(entry.path); });
    panel.mappedResult = {};
    panel.refresh(groups[panel.index]);
    return;
  }
  auto changed = groups;
  bool removed = false;
  for (auto &group : changed) {
    removed |= std::erase_if(group.paths, wasDeleted) != 0;
    for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, wasDeleted) != 0;
  }
  if (removed) commit(std::move(changed), panel.window);
  else panel.refresh(groups[panel.index]);
}

void Desktop::createNewItemInPanel(Panel &panel, bool isFolder) {
  if (editing || dragging || quitting) return;
  struct CreateScope {
    bool &active;
    explicit CreateScope(bool &flag) : active(flag) { active = true; }
    ~CreateScope() { active = false; }
  } scope(editing);

  const std::wstring baseName = lang::text(isFolder ? L"新建文件夹" : L"新建文本文档");
  if (panel.folderView()) {
    const auto parent = panel.activeFolder();
    const auto created = isFolder ? createNewFolderIn(parent, baseName)
                                  : createNewTextFileIn(parent, baseName + L".txt");
    if (created.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    ULONGLONG mod = 0;
    DWORD attr = isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (GetFileAttributesExW(created.c_str(), GetFileExInfoStandard, &data)) {
      mod = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
      attr = data.dwFileAttributes;
    }
    panel.mappedItems.push_back({created, mod, attr});
    panel.mappedResult = {};
    panel.refresh(groups[panel.index]);
    editing = false;
    panel.selectAndRevealPath(created, true);
    return;
  }

  const auto created = isFolder ? createNewFolderInBox(roots, baseName)
                                : createNewTextFileInBox(roots, baseName + L".txt");
  if (created.empty()) return;
  auto changed = groups;
  if (addItem(changed, panel.index, created, panel.activeTab) && commit(std::move(changed), panel.window, false)) {
    panel.refresh(groups[panel.index]);
    editing = false;
    panel.selectAndRevealPath(created, true);
  }
}

void Desktop::cleanupMovedCutItems() {
  if (editing || dragging || quitting) return;
  std::vector<std::wstring> gone;
  for (const auto &path : cutPaths) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
      gone.push_back(path);
  }
  if (!gone.empty()) {
    std::erase_if(cutPaths, [&](const std::wstring &p) {
      return std::any_of(gone.begin(), gone.end(), [&](const std::wstring &g) { return samePath(p, g); });
    });
  }

  const auto storageDir = boxStorageFolder(roots);
  const bool storageOnline = !storageDir.empty() && PathIsDirectoryW(storageDir.c_str());
  const bool personalOnline = !roots.empty() && !roots.front().empty() && PathIsDirectoryW(roots.front().c_str());

  const auto isDeletedLocalEntry = [&](const std::wstring &entry) {
    if (entry.empty() || isDesktopShellItem(entry)) return false;
    if (std::any_of(gone.begin(), gone.end(), [&](const std::wstring &g) { return samePath(entry, g); }))
      return true;
    const auto parent = fn::fs::path(entry).parent_path().native();
    const bool inLocalManagedDir = (storageOnline && samePath(parent, storageDir)) ||
                                   (personalOnline && samePath(parent, roots.front()));
    if (!inLocalManagedDir) return false;
    return GetFileAttributesW(entry.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
  };

  bool anyMissing = !gone.empty();
  if (!anyMissing) {
    for (const auto &group : groups) {
      if (std::any_of(group.paths.begin(), group.paths.end(), isDeletedLocalEntry)) { anyMissing = true; break; }
      for (const auto &tab : group.tabs) {
        if (std::any_of(tab.paths.begin(), tab.paths.end(), isDeletedLocalEntry)) { anyMissing = true; break; }
      }
      if (anyMissing) break;
    }
  }
  if (!anyMissing) return;

  auto changed = groups;
  bool removed = false;
  for (auto &group : changed) {
    removed |= std::erase_if(group.paths, isDeletedLocalEntry) != 0;
    for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, isDeletedLocalEntry) != 0;
  }
  if (removed) commit(std::move(changed), window);
}

void Desktop::applyAutoRules() {
  if (editing || dragging || menuOpen || quitting || roots.empty()) return;
  const bool hasAnyRule = std::any_of(groups.begin(), groups.end(), [](const Group &g) {
    return !g.autoRule.empty() && g.folder.empty();
  });
  if (!hasAnyRule) return;

  const auto personal = quick::normalize(roots.front());
  if (personal.empty()) return;
  const auto storageDir = boxStorageFolder(roots);

  FILETIME nowFt{};
  GetSystemTimeAsFileTime(&nowFt);
  const ULONGLONG nowTicks = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;

  const auto isGrouped = [&](const std::wstring &candidate) {
    for (const auto &g : groups) {
      for (const auto &p : g.paths) if (samePath(p, candidate)) return true;
      for (const auto &t : g.tabs)
        for (const auto &p : t.paths) if (samePath(p, candidate)) return true;
    }
    return false;
  };

  struct Match { size_t groupIndex; std::wstring path; };
  std::vector<Match> matches;
  std::error_code ec;
  const fn::fs::directory_iterator end;
  for (fn::fs::directory_iterator it(personal, ec); !ec && it != end; it.increment(ec)) {
    const auto fullPath = quick::normalize(it->path().wstring());
    if (fullPath.empty() || samePath(fullPath, storageDir)) continue;
    const auto fileName = it->path().filename().wstring();
    if (fileName.empty() || samePath(fileName, L"desktop.ini") || fileName.starts_with(L"~$")) continue;
    const auto ext = it->path().extension().wstring();
    if (!_wcsicmp(ext.c_str(), L".crdownload") || !_wcsicmp(ext.c_str(), L".part") ||
        !_wcsicmp(ext.c_str(), L".partial") || !_wcsicmp(ext.c_str(), L".tmp") ||
        !_wcsicmp(ext.c_str(), L".opdownload") || !_wcsicmp(ext.c_str(), L".download")) continue;

    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &info)) continue;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
    const ULONGLONG writeTicks = (static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                                 info.ftLastWriteTime.dwLowDateTime;
    // Wait at least 1.5s after last write so active downloads/screenshots finish closing.
    if (nowTicks > writeTicks && nowTicks - writeTicks < 15'000'000ULL) continue;
    if (isGrouped(fullPath)) continue;

    const bool isShortcut = !_wcsicmp(ext.c_str(), L".lnk") || !_wcsicmp(ext.c_str(), L".url");
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      const auto &g = groups[gi];
      if (g.autoRule.empty() || !g.folder.empty()) continue;
      if (isShortcut && !matchesAutoRule(L"x" + ext, g.autoRule)) {
        // Avoid sweeping desktop app shortcuts into keyword rules unless .lnk/.url is matched.
        std::wstring lowerRule = g.autoRule;
        std::transform(lowerRule.begin(), lowerRule.end(), lowerRule.begin(), ::towlower);
        if (lowerRule.find(L"lnk") == std::wstring::npos && lowerRule.find(L"url") == std::wstring::npos) continue;
      }
      if (matchesAutoRule(fileName, g.autoRule)) {
        matches.push_back({gi, fullPath});
        break;
      }
    }
  }

  if (matches.empty()) return;
  auto changed = groups;
  BoxFileMoves moves(roots, window, matches.size());
  bool anyAdded = false;
  for (const auto &m : matches) {
    const auto &stored = moves.transfer(m.path, true);
    if (stored.empty()) continue;
    if (addItem(changed, m.groupIndex, stored, 0)) anyAdded = true;
  }
  if (anyAdded && commit(std::move(changed), window)) {
    moves.keep();
  }
}

void Desktop::setAllBoxesHidden(bool hidden) {
  if (boxesHidden == hidden) return;
  boxesHidden = hidden;
  const DWORD value = hidden ? 1 : 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, L"DesktopGroupsHidden", REG_DWORD, &value, sizeof(value));
  for (const auto &panel : panels) {
    if (!IsWindow(panel->window)) continue;
    if (hidden) {
      panel->finishTitleEdit(true);
      panel->finishItemEdit(true);
      if (panel->gestureEdge) panel->finishGesture(false);
#ifdef FOLDERNOTE_WINUI
      if (panel->hasSurface()) panel->surface->show(false);
#endif
      ShowWindow(panel->window, SW_HIDE);
      panel->updateShadow();
    } else {
#ifdef FOLDERNOTE_WINUI
      if (!panel->dockHidden && panel->hasSurface()) panel->surface->show(true);
#endif
      panel->updateShadow();
    }
  }
  if (hidden) updateGlass();
  else lower();
}

bool Desktop::doubleClickHideEnabled() const {
  return appearanceValue(L"DesktopGroupsDoubleClickHide", 1) != 0;
}

void Desktop::toggleDoubleClickHide() {
  const DWORD next = doubleClickHideEnabled() ? 0 : 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, desktopSettings, L"DesktopGroupsDoubleClickHide", REG_DWORD, &next, sizeof(next));
}

void Desktop::exportBackup(HWND owner) {
  if (editing || dragging || quitting) return;
  struct ExportScope {
    bool &active;
    explicit ExportScope(bool &flag) : active(flag) { active = true; }
    ~ExportScope() { active = false; }
  } scope(editing);

  Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog)
    return;
  FILEOPENDIALOGOPTIONS options{};
  if (SUCCEEDED(dialog->GetOptions(&options)))
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
  const COMDLG_FILTERSPEC filters[]{{L"JSON (*.json)", L"*.json"}};
  dialog->SetFileTypes(ARRAYSIZE(filters), filters);
  dialog->SetDefaultExtension(L"json");
  dialog->SetFileName(L"FolderNote-Backup.json");
  dialog->SetTitle(lang::text(L"导出备份"));
  Microsoft::WRL::ComPtr<IShellItem> result;
  PWSTR raw = nullptr;
  if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&result)) || !result ||
      FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(raw, CoTaskMemFree);
  try {
    exportDesktopBackup(raw);
    ui::message(owner, lang::text(L"备份已导出。"), lang::text(L"桌面盒子"), MB_OK | MB_ICONINFORMATION);
  } catch (const fn::Error &err) {
    report(owner, err.message.c_str());
  } catch (...) {
    report(owner, lang::text(L"无法导出备份。"));
  }
}

void Desktop::importBackup(HWND owner) {
  if (editing || dragging || quitting) return;
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  PWSTR raw = nullptr;
  {
    struct ImportScope {
      bool &active;
      explicit ImportScope(bool &flag) : active(flag) { active = true; }
      ~ImportScope() { active = false; }
    } scope(editing);
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog)
      return;
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(dialog->GetOptions(&options)))
      dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const COMDLG_FILTERSPEC filters[]{{L"JSON (*.json)", L"*.json"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(lang::text(L"导入备份"));
    Microsoft::WRL::ComPtr<IShellItem> result;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&result)) || !result ||
        FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
  }
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(raw, CoTaskMemFree);
  if (quitting) return;
  try {
    auto layout = importDesktopBackup(raw);
    groups = std::move(layout.groups);
    invalidateWallpaperBackdrop();
    theme.reset(ui::configuredTheme());
    if (!previewingAppearance) style = configuredAppearance();
    panels.clear();
    glass.clear();
    host = nullptr;
    if (boxesHidden) setAllBoxesHidden(false);
    ensureHost();
    lower();
    ui::message(owner, lang::text(L"备份已导入。"), lang::text(L"桌面盒子"), MB_OK | MB_ICONINFORMATION);
  } catch (const fn::Error &err) {
    report(owner, err.message.c_str());
  } catch (...) {
    report(owner, lang::text(L"无法导入备份。"));
  }
}

class DesktopBlankClickWatcher {
  static inline std::atomic<HWND> activeReceiver{nullptr};
  DWORD threadId = 0;
  std::thread worker;

  static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && lParam) {
      const auto &info = *reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
      if ((info.flags & LLMHF_INJECTED) == 0) {
        thread_local DWORD lastTime = 0;
        thread_local POINT lastPt{LONG_MIN, LONG_MIN};
        const DWORD maxDt = GetDoubleClickTime();
        const int maxDx = std::max(2, GetSystemMetrics(SM_CXDOUBLECLK) / 2);
        const int maxDy = std::max(2, GetSystemMetrics(SM_CYDOUBLECLK) / 2);
        if (lastTime != 0 && info.time - lastTime <= maxDt &&
            std::abs(info.pt.x - lastPt.x) <= maxDx &&
            std::abs(info.pt.y - lastPt.y) <= maxDy) {
          lastTime = 0;
          if (const HWND target = activeReceiver.load(std::memory_order_relaxed)) {
            PostMessageW(target, desktopDblClickMessage,
                         static_cast<WPARAM>(static_cast<INT_PTR>(info.pt.x)),
                         static_cast<LPARAM>(info.pt.y));
          }
        } else {
          lastTime = info.time ? info.time : 1;
          lastPt = info.pt;
        }
      }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

public:
  explicit DesktopBlankClickWatcher(HWND receiverWnd) {
    activeReceiver.store(receiverWnd, std::memory_order_relaxed);
    std::promise<DWORD> ready;
    auto future = ready.get_future();
    worker = std::thread([p = std::move(ready)]() mutable {
      MSG msg{};
      PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      const HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
      p.set_value(GetCurrentThreadId());
      if (hook) {
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        UnhookWindowsHookEx(hook);
      }
    });
    threadId = future.get();
  }

  ~DesktopBlankClickWatcher() {
    activeReceiver.store(nullptr, std::memory_order_relaxed);
    if (threadId) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
    if (worker.joinable()) worker.join();
  }
};

LRESULT CALLBACK Desktop::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto desktop = reinterpret_cast<Desktop *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    desktop = static_cast<Desktop *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    desktop->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(desktop));
  }
  if (!desktop) return DefWindowProcW(window, message, w, l);
  try {
    static const UINT seerReady = RegisterWindowMessageW(L"SEER_CORE_READY");
    if (seerReady && message == seerReady) {
      if (desktop->previewKeys) desktop->previewKeys->refresh();
      return 0;
    }
    if (message == desktop->taskbarCreated) {
      if (desktop->ensureHost()) desktop->lower();
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
      invalidateWallpaperBackdrop();
      desktop->theme.reset(ui::configuredTheme());
      if (!desktop->previewingAppearance) desktop->style = configuredAppearance();
      for (const auto &panel : desktop->panels) {
        panel->applyTheme(desktop->theme.colors);
        panel->resize();
      }
      desktop->updateShadows();
    }
    switch (message) {
    case WM_COPYDATA: {
      if (!l || desktop->quitting || !search::sameApplication(reinterpret_cast<HWND>(w))) return FALSE;
      const auto &data = *reinterpret_cast<const COPYDATASTRUCT *>(l);
      if (data.dwData != search::copyDataCommand || !data.lpData || !data.cbData ||
          data.cbData % sizeof(wchar_t) || data.cbData > search::requestLimit * sizeof(wchar_t)) return FALSE;
      auto entry = search::decode({static_cast<const wchar_t *>(data.lpData), data.cbData / sizeof(wchar_t)});
      if (!entry || !search::matches(desktop->groups, *entry)) return FALSE;
      desktop->pendingSearch = std::move(entry);
      desktop->searchDeadline = GetTickCount64() + 5000;
      if (!PostMessageW(window, locateSearchMessage, 0, 0)) { desktop->pendingSearch.reset(); return FALSE; }
      return TRUE;
    }
    case locateSearchMessage:
      desktop->locateSearch();
      return 0;
    case transferDoneMessage:
      desktop->handleCompletedTransfers();
      return 0;
    case previewSelectedMessage:
      if (desktop->previewKeys) {
        if (const auto path = desktop->previewKeys->take()) previewDesktopItem(*path);
      }
      return 0;
    case desktopDblClickMessage: {
      if (desktop->quitting || desktop->editing || desktop->dragging || desktop->menuOpen ||
          !desktop->doubleClickHideEnabled() || !IsWindow(desktop->host)) return 0;
      const POINT pt{static_cast<LONG>(static_cast<INT_PTR>(w)), static_cast<LONG>(l)};
      if (!desktop->panelAt(pt) && quick::detail::blankSurface(desktop->host, pt)) {
        desktop->setAllBoxesHidden(!desktop->boxesHidden);
      }
      return 0;
    }
    case WM_TIMER:
      if (desktop->rebuilding) return 0;
      if (w == transferTimer) {
        desktop->tickTransfers();
        return 0;
      }
      if (w == headerTimer) {
        if (!IsWindow(desktop->host)) return 0;
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
          const auto now = GetTickCount64();
          const auto underPointer = WindowFromPoint(cursor);
          for (const auto &panel : desktop->panels) {
            if (!IsWindow(panel->window)) continue;
            panel->tickDock(cursor, now);
            if (!panel->dockHidden) {
              const bool inside = insideDesktopWindow(panel->window, underPointer);
              panel->tickAutoCollapse(inside, now);
              panel->updateHeaderActions(cursor);
              panel->tickFolderHover(cursor, inside, now);
            }
          }
        }
        return 0;
      }
      if (!enabled()) PostMessageW(window, WM_CLOSE, 0, 0);
      else {
        if (!desktop->ensureHost()) return 0;
        desktop->lower();
        if (std::any_of(desktop->panels.begin(), desktop->panels.end(),
                        [](const auto &panel) { return panel->movingOrSizing; })) return 0;
        for (const auto &panel : desktop->panels) panel->applyThumbnails();
        desktop->locateSearch();
        const auto now = GetTickCount64();
        if (!desktop->editing && !desktop->dragging && !desktop->menuOpen &&
            !desktop->cutPaths.empty()) {
          desktop->cleanupMovedCutItems();
        }
        if (!desktop->editing && !desktop->dragging && !desktop->menuOpen &&
            now - desktop->lastFolderRefresh > 2000) {
          desktop->lastFolderRefresh = now;
          for (const auto &panel : desktop->panels)
            if (!panel->movingOrSizing && panel->folderView() && !panel->collapsed())
              panel->refresh(desktop->groups[panel->index]);
          desktop->cleanupMovedCutItems();
          desktop->applyAutoRules();
        }
      }
      return 0;
    case WM_DISPLAYCHANGE:
      if (desktop->rebuilding) return 0;
      invalidateWallpaperBackdrop();
      for (const auto &panel : desktop->panels)
        if (panel->gestureEdge) panel->finishGesture(false);
      for (const auto &panel : desktop->panels) {
        panel->setDockSide(DockSide::None);
        panel->place(resolvedGroupBounds(desktop->groups[panel->index]));
        panel->setCollapsed();
        RedrawWindow(panel->window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      }
      return 0;
    case refreshMessage:
      if (desktop->rebuilding) return 0;
      for (size_t i = 0; i < desktop->panels.size(); ++i) desktop->panels[i]->refresh(desktop->groups[i]);
      return 0;
    case manageMessage:
      if (!desktop->quitting && IsWindowEnabled(window) && desktop->ensureHost()) desktop->manage();
      return 0;
    case WM_COMMAND:
      if (!desktop->quitting) {
        switch (static_cast<Command>(LOWORD(w))) {
        case ToggleHideAllBoxes: desktop->setAllBoxesHidden(!desktop->boxesHidden); break;
        case ToggleDoubleClickHide: desktop->toggleDoubleClickHide(); break;
        case ExportBackup: desktop->exportBackup(window); break;
        case ImportBackup: desktop->importBackup(window); break;
        default: break;
        }
      }
      return 0;
    case createMessage: {
      if (!desktop->ensureHost()) return 0;
      POINT point{static_cast<LONG>(static_cast<INT_PTR>(w)), static_cast<LONG>(l)};
      desktop->createBox(point, window);
      return 0;
    }
    case WM_CLOSE:
      desktop->quitting = true;
      EndMenu();
      EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM) -> BOOL {
        wchar_t name[16]{};
        GetClassNameW(candidate, name, ARRAYSIZE(name));
        if (wcscmp(name, L"#32770") == 0) PostMessageW(candidate, WM_CLOSE, 0, 0);
        return TRUE;
      }, 0);
      PostQuitMessage(0);
      return 0;
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
    const auto context = desktopLaunchContext();
    if (context.mode == DesktopLaunchMode::Unavailable) return false;
    const auto exe = installedExecutable.empty() ? quick::executable() : installedExecutable;
    if (!startup(exe) || !configureDesktopMenu(exe)) return false;
    const auto running = receiver();
    if (running && (!installedExecutable.empty() || GetPropW(running, legacyManagedIconsProperty) ||
                    (context.mode == DesktopLaunchMode::Explorer &&
                     desktopReceiverStatus(running, context) == DesktopReceiverStatus::Elevated)) &&
        !stopProcess(running)) return false;
    const auto window = receiver();
    return window ? receiverReady(window, context) : launch(exe, L"--desktop-groups", context, true);
  } catch (...) {
    return false;
  }
}

bool showManager() {
  if (!enabled()) return false;
  try {
    const auto context = desktopLaunchContext();
    if (auto window = receiver()) {
      const auto status = desktopReceiverStatus(window, context);
      if (status == DesktopReceiverStatus::Compatible) return notifyManager(window);
      if (status != DesktopReceiverStatus::Elevated || context.mode != DesktopLaunchMode::Explorer ||
          !stopProcess(window)) return false;
    }
    return launch(quick::executable(), L"--desktop-groups-manage", context, false);
  } catch (...) {
    return false;
  }
}

int run(HINSTANCE instance, bool manage, bool create, bool delegated) {
  POINT creationPoint{};
  if (create) GetCursorPos(&creationPoint);
  if (!enabled()) return 0;
  // A box must run at the desktop's integrity level to receive its OLE drags.
  const auto context = desktopLaunchContext();
  if (context.mode != DesktopLaunchMode::Direct) {
    try {
      if (context.mode == DesktopLaunchMode::Explorer && !delegated) {
        const auto window = receiver();
        if (desktopReceiverStatus(window, context) != DesktopReceiverStatus::Elevated || stopProcess(window)) {
          const auto arguments = create ? L"--desktop-groups-create" :
                                manage ? L"--desktop-groups-manage" : L"--desktop-groups";
          if (launch(fn::modulePath().native(), arguments, context, !manage && !create)) return 0;
        }
      }
    } catch (...) {}
    if (manage || create) report(nullptr, lang::text(L"无法打开桌面盒子。"));
    return 1;
  }
  const auto mutex = CreateMutexW(nullptr, FALSE, mutexName);
  if (!mutex) return 1;
  DWORD status = WaitForSingleObject(mutex, 0);
  if (status == WAIT_TIMEOUT && (manage || create)) {
    for (int i = 0; i < 40 && status == WAIT_TIMEOUT; ++i) {
      if (auto window = receiver(); window && notifyManager(window, create ? createMessage : manageMessage, creationPoint)) break;
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
        if (!RegisterClassW(&panel) || !RegisterClassW(&manager)) throw 0;
#ifdef FOLDERNOTE_WINUI
        winui::Runtime xaml;
#endif
        Desktop desktop(instance);
#ifdef FOLDERNOTE_WINUI
        desktop.useWinui = xaml.available();
#endif
        if (!CreateWindowExW(WS_EX_TOOLWINDOW, receiverClass, L"", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, instance, &desktop)) throw 0;
        // The Shell owns clipping and global stacking. Recover child windows
        // only when Explorer recreates its desktop view.
        restoreDesktopIcons();
        desktop.ensureHost();
        desktop.previewKeys = std::make_unique<PreviewKeys>(desktop.window, previewSelectedMessage,
            [&desktop] { return desktop.previewTarget(); });
        DesktopBlankClickWatcher blankClickWatcher(desktop.window);
        if (!SetTimer(desktop.window, 1, 400, nullptr)) throw 0;
        if (!SetTimer(desktop.window, headerTimer, 70, nullptr)) throw 0;
        if (!SetPropW(desktop.window, readyProperty, reinterpret_cast<HANDLE>(1))) throw 0;
        if (manage) PostMessageW(desktop.window, manageMessage, 0, 0);
        if (create) PostMessageW(desktop.window, createMessage,
                                 static_cast<WPARAM>(static_cast<INT_PTR>(creationPoint.x)), creationPoint.y);
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
      OleFlushClipboard();
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
