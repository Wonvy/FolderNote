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

std::wstring pickFolder(HWND owner);
bool showBoxDialog(HINSTANCE instance, HWND owner, const wchar_t *title, std::wstring *name, std::wstring prompt,
                   bool allowEmpty = false, size_t maxLength = groupNameLimit);
std::wstring pickBoxIcon(HWND owner);

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
  HANDLE mappedWatch = INVALID_HANDLE_VALUE;
  ULONGLONG mappedLastScan = 0;
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
    if (mappedWatch != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(mappedWatch); mappedWatch = INVALID_HANDLE_VALUE; }
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
  fn::fs::path layoutFile = layoutPath();
  std::vector<Group> groups;
  std::vector<std::unique_ptr<Panel>> panels;
  std::vector<std::wstring> roots;
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
  bool headerTimerRunning = false;
  void ensureHeaderTimer();
  void tickHeader();
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
  std::shared_ptr<std::atomic<int>> inFlightTransfers = std::make_shared<std::atomic<int>>(0);
  void recoverOrphanedBoxStorage();
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

  explicit Desktop(HINSTANCE module, fn::fs::path layout = layoutPath(),
                   std::optional<std::vector<std::wstring>> customRoots = std::nullopt)
      : instance(module), layoutFile(std::move(layout)), groups(load(layoutFile)),
        roots(customRoots ? std::move(*customRoots) : desktopRoots()) {
    ensureHiddenBoxStorage(roots);
    recoverOrphanedBoxStorage();
  }
  ~Desktop() {
    quitting = true;
    previewKeys.reset();
    panels.clear();
    if (window) DestroyWindow(window);
    for (int i = 0; i < 100 && inFlightTransfers->load() > 0; ++i) Sleep(50);
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

constexpr auto recycleBinShellPath = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
inline bool isRecycleBinShellItem(const std::wstring &path) {
  return samePath(path, recycleBinShellPath);
}

std::vector<std::wstring> desktopDropPaths(IDataObject *data);
void setDropEffect(IDataObject *data, const wchar_t *name, DWORD effect);
std::optional<DWORD> readDropEffect(IDataObject *data, const wchar_t *name);
std::wstring recycleBinDisplayName();
std::optional<CLSID> readTargetClsid(IDataObject *data);
bool desktopIconAt(HWND view, POINT point);
Microsoft::WRL::ComPtr<IDataObject> createDragDataObject(HWND owner, std::span<const std::wstring> paths);
void attachDragImage(IDataObject *data, HICON icon, SIZE iconSize, POINT iconAnchor,
                     const std::wstring &label, size_t itemCount, UINT dpi);
void setDropDescription(IDataObject *data, DROPIMAGETYPE type, const wchar_t *message = L"",
                        const wchar_t *insert = L"");
bool itemEditInfo(const std::wstring &path, std::wstring &editText, int &selectLength);

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

  void clearMarker();
  void updateDescription(IDataObject *data, DWORD allowed, const std::wstring &folder, bool groupDrop);
  static DWORD applyModifierEffect(DWORD keyState, DWORD allowed, DWORD preferred);
  DWORD externalEffect(DWORD keyState, DWORD allowed) const;
  DWORD evaluate(POINT screen, DWORD keyState, DWORD allowed, std::wstring &folder, bool &groupDrop) const;
  void updateFeedback(POINT screen, DWORD allowed, const std::wstring &folder, bool groupDrop);
  void checkSpringLoadedTab(POINT screen);

public:
  explicit DropTarget(Panel &value);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD *effects) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD *effects) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keyState, POINTL point, DWORD *effects) override;
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
  BoxFileMoves(const std::vector<std::wstring> &desktopRoots, HWND window, size_t count);
  ~BoxFileMoves();
  const std::wstring &transfer(const std::wstring &path, bool intoBox);
  void keep() { moves.clear(); }
};

// Native control subclasses installed by Panel::proc.
LRESULT CALLBACK tabInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
LRESULT CALLBACK iconInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
LRESULT CALLBACK toggleInput(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data);
}  // namespace desktop_groups::detail
