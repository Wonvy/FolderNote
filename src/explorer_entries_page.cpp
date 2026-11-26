#include "explorer_entries_page.h"
#include "core.h"
#include "custom_explorer_entries.h"
#include "desktop_folder_links.h"
#include "explorer_entries.h"
#include "quick_find_controls.h"
#include "language.h"
#include "quick_popup.h"
#include "removed_entries.h"
#include "theme_scroll.h"
#include "this_pc_view.h"
#include "window_position.h"
#include <algorithm>
#include <array>
#include <commctrl.h>
#include <commoncontrols.h>
#include <memory>
#include <shellapi.h>
#include <shobjidl.h>
#include <type_traits>
#include <variant>
#include <wrl/client.h>
#include <windowsx.h>

namespace explorer_entries_page {
namespace {
using namespace explorer_entries;
using Microsoft::WRL::ComPtr;
using Link = desktop_folder_links::Entry;
using NativeItem = this_pc_view::Item;
using Group = this_pc_view::Group;
using Row = std::variant<Entry, Link, CustomEntry, NativeItem>;
struct DisplayRow {
  Row row;
  Group group = Group::Folders;
  int iconIndex = -1;
};
constexpr wchar_t className[] = L"FolderNote.Settings.ExplorerEntries";
constexpr wchar_t title[] = L"文件夹入口管理";
constexpr wchar_t settingsKey[] = L"Software\\FolderNote";
constexpr wchar_t viewPreference[] = L"EntryIconView";
constexpr std::array surfaces{Surface::ThisPc, Surface::Navigation, Surface::Desktop};
constexpr std::array locations{L"此电脑", L"左侧导航栏", L"桌面"};
constexpr std::array groupLabels{L"文件夹 ({0})", L"设备和驱动器 ({0})", L"网络位置 ({0})"};
enum Control { LocationLabel = 100, Location, List, Details, Hint, Add, Toggle, Restore, Remove, Refresh,
               StatusLine, StatusIcon, ListMode, Scroll, IconMode, Removed, Recover, StatusMetadata };
constexpr int minimumStatusHeight = 58;
constexpr int maximumStatusHeight = 150;
constexpr wchar_t statusHeightPreference[] = L"EntryStatusHeight";

template<class T> constexpr bool hasVisibility = std::is_same_v<T, Entry> || std::is_same_v<T, Link>;

template<class T> const std::wstring &identity(const T &entry) {
  if constexpr (std::is_same_v<T, Link>) return entry.path;
  else return entry.id;
}

const wchar_t *status(const Row &row) {
  return std::visit([](const auto &entry) {
    using T = std::decay_t<decltype(entry)>;
    if constexpr (hasVisibility<T>) return lang::text(entry.hidden ? L"已隐藏" : L"显示中");
    else if constexpr (std::is_same_v<T, CustomEntry>) return lang::text(L"已添加");
    else return lang::text(L"只读");
  }, row);
}

const wchar_t *source(const Row &row) {
  const auto *link = std::get_if<Link>(&row);
  return lang::text(std::holds_alternative<CustomEntry>(row) ? L"FolderNote" :
      link ? (link->shared ? L"共享桌面快捷方式" : L"桌面快捷方式") : L"系统或其他软件");
}

bool changeable(const Row &row) {
  return std::visit([](const auto &entry) {
    if constexpr (hasVisibility<std::decay_t<decltype(entry)>>) return entry.canChange;
    else return false;
  }, row);
}

bool isHidden(const Row &row) {
  return std::visit([](const auto &entry) {
    if constexpr (hasVisibility<std::decay_t<decltype(entry)>>) return entry.hidden;
    else return false;
  }, row);
}

bool restorable(const Row &row) {
  return std::visit([](const auto &entry) {
    if constexpr (hasVisibility<std::decay_t<decltype(entry)>>) return entry.canChange && entry.changed;
    else return false;
  }, row);
}

HICON entryIcon(std::wstring specification, int size) {
  int index = 0;
  if (const auto comma = specification.rfind(L','); comma != std::wstring::npos) {
    wchar_t *end = nullptr;
    index = static_cast<int>(wcstol(specification.c_str() + comma + 1, &end, 10));
    if (*end == 0) specification.resize(comma);
  }
  if (specification.size() > 1 && specification.front() == L'"' && specification.back() == L'"')
    specification = specification.substr(1, specification.size() - 2);
  wchar_t path[MAX_PATH]{};
  const auto count = ExpandEnvironmentStringsW(specification.c_str(), path, ARRAYSIZE(path));
  if (count < 4 || count > ARRAYSIZE(path) || path[1] != L':' || path[2] != L'\\') return nullptr;
  wchar_t root[]{path[0], L':', L'\\', L'\0'};
  if (GetDriveTypeW(root) != DRIVE_FIXED) return nullptr;
  HICON icon = nullptr;
  PrivateExtractIconsW(path, index, size, size, &icon, nullptr, 1, 0);
  return icon;
}

int addIcon(HIMAGELIST images, HICON icon, int size, bool hidden) {
  if (!hidden) return ImageList_AddIcon(images, icon);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size;
  info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  auto dc = CreateCompatibleDC(nullptr);
  auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  int index = -1;
  if (dc && bitmap && bits) {
    auto previous = SelectObject(dc, bitmap);
    auto pixels = static_cast<BYTE *>(bits);
    std::fill_n(pixels, size * size * 4, BYTE{});
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    GdiFlush();
    for (int i = 0; i < size * size * 4; ++i) pixels[i] = static_cast<BYTE>(pixels[i] / 2);
    SelectObject(dc, previous);
    index = ImageList_Add(images, bitmap, nullptr);
  }
  if (bitmap) DeleteObject(bitmap);
  if (dc) DeleteDC(dc);
  return index >= 0 ? index : ImageList_AddIcon(images, icon);
}

std::wstring pickFolder(HWND owner) {
  ComPtr<IFileOpenDialog> dialog;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  FILEOPENDIALOGOPTIONS options{};
  if (SUCCEEDED(result)) result = dialog->GetOptions(&options);
  if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  if (SUCCEEDED(result)) result = dialog->SetTitle(lang::text(L"选择要添加的文件夹"));
  if (SUCCEEDED(result)) result = dialog->Show(GetAncestor(owner, GA_ROOT));
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
  ComPtr<IShellItem> item;
  if (SUCCEEDED(result)) result = dialog->GetResult(&item);
  PWSTR rawPath = nullptr;
  if (SUCCEEDED(result)) result = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath) : E_FAIL;
  std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, CoTaskMemFree);
  if (FAILED(result) || !path) throw fn::Error{lang::text(L"无法打开文件夹选择窗口。")};
  return path.get();
}

struct Page {
  HWND window{}, list{};
  ui::Theme &theme;
  HFONT font;
  std::vector<DisplayRow> rows;
  std::array<std::wstring, groupLabels.size()> headings;
  std::array<bool, groupLabels.size()> collapsed{};
  Surface surface = surfaces.front();
  bool reloading = false;
  bool iconView = false;
  bool removedView = false;
  int statusHeight = minimumStatusHeight;
  int resizeStart = 0, resizeHeight = 0;
  bool resizingStatus = false;
  bool hasSecondaryStatus = false;
  HIMAGELIST images{};
  HIMAGELIST largeImages{};
  int imageSize{};
  HFONT symbols{};
  HICON selectedIcon{};
  HWND tooltips{};
  ui::ScrollBar scroll;
  std::array<int, 7> scrollLayout{};
  int hovered = -1;
  bool hoverEye = false;

  bool statusDividerAt(int y) const {
    RECT bounds{};
    GetClientRect(window, &bounds);
    return std::abs(y - (bounds.bottom - px(statusHeight))) <= px(4);
  }

  int groupAt(POINT point) const {
    if (surface != Surface::ThisPc) return -1;
    for (int i = 0; i < static_cast<int>(headings.size()); ++i) {
      RECT header{};
      if (ListView_GetGroupRect(list, i, LVGGR_HEADER, &header) && PtInRect(&header, point)) return i;
    }
    return -1;
  }

  void hover(POINT point) {
    LVHITTESTINFO hit{};
    hit.pt = point;
    const int index = ListView_HitTest(list, &hit);
    RECT bounds{};
    const bool eye = !removedView && !iconView && index >= 0 && size_t(index) < rows.size() &&
        changeable(rows[size_t(index)].row) && ListView_GetItemRect(list, index, &bounds, LVIR_BOUNDS) &&
        PtInRect(&bounds, point) && point.x >= bounds.right - px(41) && point.x < bounds.right - px(11);
    if (hovered == index && hoverEye == eye) return;
    hovered = index;
    hoverEye = eye;
    InvalidateRect(list, nullptr, FALSE);
  }

  void syncScroll() {
    if (!list || !scroll.window()) return;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    RECT bounds{};
    GetWindowRect(list, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
    const auto dpi = GetDpiForWindow(window);
    const int width = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    const bool visible = (GetWindowLongPtrW(list, GWL_STYLE) & WS_VSCROLL) != 0 &&
        GetScrollInfo(list, SB_VERT, &info);
    const std::array layout{int(bounds.right) - width, int(bounds.top), width, int(bounds.bottom - bounds.top),
        visible ? info.nMax - info.nMin + 1 : 0, int(info.nPage), info.nPos - info.nMin};
    if (scrollLayout == layout) return;
    scrollLayout = layout;
    scroll.move(layout[0], layout[1], layout[2], layout[3]);
    scroll.configure(layout[4], layout[5], layout[6], dpi);
  }
  static LRESULT CALLBACK listProc(HWND field, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &page = *reinterpret_cast<Page *>(data);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(field, listProc, id);
    if (message == WM_MOUSEMOVE) {
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, field, 0};
      TrackMouseEvent(&track);
      page.hover({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
    }
    if (message == WM_MOUSELEAVE) page.hover({-1, -1});
    if (message == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
      POINT point{};
      GetCursorPos(&point);
      ScreenToClient(field, &point);
      if (page.hoverEye || page.groupAt(point) >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
      const int group = page.groupAt({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
      if (group >= 0) {
        if (message == WM_LBUTTONDOWN) { SetFocus(field); return 0; }
        const bool fold = !(ListView_GetGroupState(field, group, LVGS_COLLAPSED) & LVGS_COLLAPSED);
        ListView_SetGroupState(field, group, LVGS_COLLAPSED, fold ? LVGS_COLLAPSED : 0);
        page.collapsed[size_t(group)] = fold;
        InvalidateRect(field, nullptr, FALSE);
        page.syncScroll();
        return 0;
      }
    }
    const auto result = DefSubclassProc(field, message, w, l);
    if (message == WM_PAINT || message == WM_VSCROLL || message == WM_MOUSEWHEEL ||
        message == WM_KEYDOWN || message == LVM_SCROLL) page.syncScroll();
    return result;
  }

  ~Page() {
    if (images) ImageList_Destroy(images);
    if (largeImages) ImageList_Destroy(largeImages);
    if (symbols) DeleteObject(symbols);
    if (selectedIcon) DestroyIcon(selectedIcon);
  }

  HWND control(int id) const { return GetDlgItem(window, id); }
  int px(int value) const { return MulDiv(value, GetDpiForWindow(window), 96); }
  HWND add(DWORD ex, const wchar_t *type, const wchar_t *label, DWORD style, int id) const {
    auto child = CreateWindowExW(ex, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return child;
  }
  bool create() {
    add(0, L"STATIC", lang::text(L"位置"), SS_LEFT | SS_NOPREFIX, LocationLabel);
    auto combo = add(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, Location);
    for (const auto label : locations)
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lang::text(label)));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    list = add(0, WC_LISTVIEWW, L"", WS_CLIPSIBLINGS | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_OWNERDRAWFIXED | LVS_AUTOARRANGE |
        LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_TABSTOP, List);
    if (!combo || !list) return false;
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    ListView_InsertColumn(list, 0, &column);
    scroll.create(window, Scroll, theme, [this](int position) {
      SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_POS};
      if (!GetScrollInfo(list, SB_VERT, &info)) return;
      int delta = position + info.nMin - info.nPos;
      if (!iconView && surface != Surface::ThisPc) {
        RECT row{};
        if (ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS)) delta *= row.bottom - row.top;
      }
      ListView_Scroll(list, 0, delta);
    }, true);
    SetWindowPos(scroll.window(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowSubclass(list, listProc, 1, reinterpret_cast<DWORD_PTR>(this));
    quick_find::attach(list, [this](int row) {
      if (row < 0 || size_t(row) >= rows.size()) return;
      const auto path = std::visit([](const auto &entry) { return entry.target; }, rows[row].row);
      const auto attributes = GetFileAttributesW(path.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES && quick_find::settings().opens((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
        ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    add(0, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, StatusLine);
    auto metadata = add(0, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, StatusMetadata);
    ui::muted(metadata);
    add(0, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, StatusIcon);
    add(0, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS | SS_NOTIFY, Details);
    auto hint = add(0, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, Hint);
    ui::muted(hint);
    add(0, L"BUTTON", lang::text(L"添加文件夹…"), WS_TABSTOP, Add);
    add(0, L"BUTTON", lang::text(L"隐藏"), WS_TABSTOP | BS_OWNERDRAW, Toggle);
    add(0, L"BUTTON", lang::text(L"刷新"), WS_TABSTOP | BS_OWNERDRAW, Refresh);
    add(0, L"BUTTON", lang::text(L"已移除"), WS_TABSTOP, Removed);
    add(0, L"BUTTON", lang::text(L"列表视图"), WS_TABSTOP | BS_OWNERDRAW, ListMode);
    add(0, L"BUTTON", lang::text(L"图标视图"), WS_TABSTOP | BS_OWNERDRAW, IconMode);
    DWORD viewMode = 0;
    DWORD bytes = sizeof(viewMode);
    RegGetValueW(HKEY_CURRENT_USER, settingsKey, viewPreference, RRF_RT_REG_DWORD, nullptr, &viewMode, &bytes);
    DWORD savedHeight = minimumStatusHeight;
    bytes = sizeof(savedHeight);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, statusHeightPreference, RRF_RT_REG_DWORD,
                     nullptr, &savedHeight, &bytes) == ERROR_SUCCESS)
      statusHeight = static_cast<int>(std::clamp(savedHeight, DWORD(minimumStatusHeight), DWORD(maximumStatusHeight)));
    iconView = viewMode == 1;
    setView();
    tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    TOOLINFOW tip{sizeof(tip)};
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window;
    tip.uId = reinterpret_cast<UINT_PTR>(control(Toggle));
    tip.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    for (const auto id : {ListMode, IconMode, Refresh}) {
      tip.uId = reinterpret_cast<UINT_PTR>(control(id));
      tip.lpszText = const_cast<wchar_t *>(lang::text(id == Refresh ? L"刷新" : id == IconMode ? L"图标视图" : L"列表视图"));
      SendMessageW(tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    updateSelection();
    return true;
  }
  void setView() {
    auto style = GetWindowLongPtrW(list, GWL_STYLE);
    SetWindowLongPtrW(list, GWL_STYLE, iconView ? style & ~LVS_OWNERDRAWFIXED : style | LVS_OWNERDRAWFIXED);
    ListView_SetView(list, iconView ? LV_VIEW_ICON : LV_VIEW_DETAILS);
  }
  void layout() {
    RECT bounds{}; GetClientRect(window, &bounds);
    const int left = px(19);
    const int width = std::max(0, int(bounds.right) - px(39));
    const int bottom = bounds.bottom;
    MoveWindow(control(LocationLabel), left, px(6), px(46), px(24), TRUE);
    SendMessageW(control(Location), CB_SETITEMHEIGHT, WPARAM(-1), px(24));
    SendMessageW(control(Location), CB_SETITEMHEIGHT, 0, px(37));
    MoveWindow(control(Location), left + px(48), 0, px(143), px(162), TRUE);
    MoveWindow(control(ListMode), left + px(201), 0, px(34), px(30), TRUE);
    MoveWindow(control(IconMode), left + px(239), 0, px(34), px(30), TRUE);
    MoveWindow(control(Removed), left + px(282), 0, px(104), px(30), TRUE);
    ShowWindow(control(Removed), surface == Surface::ThisPc ? SW_SHOWNA : SW_HIDE);
    MoveWindow(control(Add), left + width - px(182), 0, px(138), px(30), TRUE);
    MoveWindow(control(Refresh), left + width - px(34), 0, px(34), px(30), TRUE);
    const int listBottom = bottom - px(statusHeight);
    MoveWindow(list, left + 1, px(43), std::max(0, width - 2), std::max(0, listBottom - px(43)), TRUE);
    const int textLeft = left + px(52), textWidth = std::max(0, width - px(104));
    const int mainWidth = textWidth * 3 / 5, gap = px(12);
    const int textTop = listBottom + px((statusHeight - minimumStatusHeight) / 2);
    MoveWindow(control(StatusIcon), left + px(12), listBottom + 1, px(30), px(statusHeight) - 1, TRUE);
    MoveWindow(control(StatusLine), textLeft, textTop + px(8), mainWidth, px(20), TRUE);
    MoveWindow(control(StatusMetadata), textLeft + mainWidth + gap, textTop + px(8),
        std::max(0, textWidth - mainWidth - gap), px(20), TRUE);
    MoveWindow(control(Details), textLeft, textTop + px(30), hasSecondaryStatus ? mainWidth : textWidth, px(20), TRUE);
    MoveWindow(control(Hint), textLeft + mainWidth + gap, textTop + px(30),
        std::max(0, textWidth - mainWidth - gap), px(20), TRUE);
    MoveWindow(control(Toggle), left + width - px(40), textTop + px(15), px(34), px(28), TRUE);
    RECT content{};
    GetClientRect(list, &content);
    ListView_SetColumnWidth(list, 0, std::max(0, int(content.right) - px(20)));
    ListView_SetIconSpacing(list, px(118), px(83));
    LVGROUPMETRICS spacing{};
    spacing.cbSize = sizeof(spacing);
    spacing.mask = LVGMF_BORDERSIZE;
    spacing.Top = px(10);
    spacing.Bottom = px(5);
    ListView_SetGroupMetrics(list, &spacing);
    if (px(16) != imageSize) {
      imageSize = px(16);
      auto previous = symbols;
      symbols = ui::makeSymbolFont(GetDpiForWindow(window), 15);
      if (previous) DeleteObject(previous);
      loadImages();
    }
    if (iconView) ListView_Arrange(list, LVA_DEFAULT);
    syncScroll();
  }
  void applyTheme() const {
    ListView_SetBkColor(list, theme.colors.background);
    ListView_SetTextBkColor(list, theme.colors.background);
    ListView_SetTextColor(list, theme.colors.text);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
  }
  LRESULT drawGroup(NMLVCUSTOMDRAW &draw) const {
    auto &item = draw.nmcd;
    if (item.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    if (iconView && draw.dwItemType == LVCDI_ITEM && item.dwDrawStage == CDDS_ITEMPREPAINT &&
        item.dwItemSpec < rows.size()) {
      const int index = static_cast<int>(item.dwItemSpec);
      RECT icon{};
      if (!ListView_GetItemRect(list, index, &icon, LVIR_ICON)) return CDRF_DODEFAULT;
      const auto &colors = theme.colors;
      const bool selected = (ListView_GetItemState(list, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
      const bool hot = index == hovered;
      const int center = (icon.left + icon.right) / 2;
      const int iconTop = icon.top + (surface == Surface::ThisPc ? px(10) : 0);
      RECT tile{center - px(55), iconTop - px(4), center + px(55), icon.top + px(73)};
      if (selected || hot) {
        const auto fill = selected ? (colors.highContrast ? colors.accent : colors.pressed) : colors.hover;
        ui::rounded(item.hdc, tile, fill, fill, px(5));
      }
      LVITEMW image{};
      image.mask = LVIF_IMAGE;
      image.iItem = index;
      if (largeImages && ListView_GetItem(list, &image))
        ImageList_Draw(largeImages, image.iImage, item.hdc, center - px(40) / 2, iconTop, ILD_TRANSPARENT);
      RECT label{tile.left + px(4), iconTop + px(43), tile.right - px(4), tile.bottom};
      const auto previousFont = SelectObject(item.hdc, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
      const auto ink = selected && colors.highContrast ? colors.onAccent : colors.text;
      std::visit([&](const auto &entry) {
        ui::text(item.hdc, entry.name, label, ink,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      }, rows[size_t(index)].row);
      SelectObject(item.hdc, previousFont);
      return CDRF_SKIPDEFAULT;
    }
    if (draw.dwItemType != LVCDI_GROUP || item.dwDrawStage != CDDS_POSTPAINT ||
        item.dwItemSpec >= headings.size()) return CDRF_DODEFAULT;
    const auto &caption = headings[item.dwItemSpec];
    auto bounds = draw.rcText;
    FillRect(item.hdc, &bounds, theme.background);
    const int x = bounds.left + px(9);
    const int y = (bounds.top + bounds.bottom) / 2;
    const bool folded = (ListView_GetGroupState(list, item.dwItemSpec, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0;
    const POINT arrow[] = {
        {x, y - px(folded ? 4 : 2)}, {x + px(4), y + px(folded ? 0 : 2)},
        {x + px(folded ? 0 : 8), y + px(folded ? 4 : -2)}};
    auto pen = CreatePen(PS_SOLID, 1, theme.colors.muted);
    if (pen) {
      auto old = SelectObject(item.hdc, pen);
      Polyline(item.hdc, arrow, ARRAYSIZE(arrow));
      SelectObject(item.hdc, old);
      DeleteObject(pen);
    }
    bounds.left += px(23);
    const auto previous = SelectObject(item.hdc, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
    SIZE extent{};
    GetTextExtentPoint32W(item.hdc, caption.c_str(), static_cast<int>(caption.size()), &extent);
    ui::text(item.hdc, caption, bounds, theme.colors.text);
    SelectObject(item.hdc, previous);
    const int middle = (bounds.top + bounds.bottom) / 2;
    RECT divider{bounds.left + extent.cx + px(10), middle, bounds.right - px(8), middle + 1};
    if (divider.left < divider.right) {
      SetDCBrushColor(item.hdc, theme.colors.border);
      FillRect(item.hdc, &divider, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    return CDRF_DODEFAULT;
  }
  void paintStatus() const {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const int top = bounds.bottom - px(statusHeight);
    RECT divider{0, top, bounds.right, top + 1};
    SetDCBrushColor(dc, theme.colors.border);
    FillRect(dc, &divider, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    EndPaint(window, &paint);
  }
  RECT eyeBounds(RECT row) const {
    row.left = row.right - px(38);
    row.right -= px(8);
    return row;
  }
  void drawEye(HDC dc, RECT bounds, bool hidden, COLORREF color) const {
    auto previous = SelectObject(dc, symbols);
    ui::text(dc, L"\xE890", bounds, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, previous);
    if (hidden) {
      auto pen = CreatePen(PS_SOLID, std::max(1, px(1)), color);
      if (!pen) return;
      auto old = SelectObject(dc, pen);
      const int x = (bounds.left + bounds.right) / 2, y = (bounds.top + bounds.bottom) / 2;
      MoveToEx(dc, x - px(7), y + px(7), nullptr);
      LineTo(dc, x + px(8), y - px(7));
      SelectObject(dc, old);
      DeleteObject(pen);
    }
  }
  void drawItem(const DRAWITEMSTRUCT &item) const {
    const auto &p = theme.colors;
    if (item.CtlID == ListMode || item.CtlID == IconMode || item.CtlID == Refresh) {
      const bool active = item.CtlID != Refresh && (item.CtlID == IconMode) == iconView;
      POINT pointer{};
      GetCursorPos(&pointer);
      ScreenToClient(item.hwndItem, &pointer);
      const auto fill = active ? p.accent : item.itemState & ODS_SELECTED ? p.pressed :
          (item.itemState & ODS_FOCUS) || PtInRect(&item.rcItem, pointer) ? p.hover : p.field;
      FillRect(item.hDC, &item.rcItem, theme.background);
      ui::rounded(item.hDC, item.rcItem, fill, fill, px(5));
      const auto previous = SelectObject(item.hDC, symbols);
      ui::text(item.hDC, item.CtlID == Refresh ? L"\xE72C" : item.CtlID == IconMode ? L"\xE80A" : L"\xEA37",
          item.rcItem, active ? p.onAccent : p.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(item.hDC, previous);
      return;
    }
    if (item.CtlID == Toggle) {
      FillRect(item.hDC, &item.rcItem, theme.background);
      const auto *row = selected();
      if (row && changeable(*row)) {
        POINT pointer{};
        GetCursorPos(&pointer);
        ScreenToClient(item.hwndItem, &pointer);
        if (item.itemState & (ODS_SELECTED | ODS_FOCUS) || PtInRect(&item.rcItem, pointer))
          ui::rounded(item.hDC, item.rcItem, p.hover, p.border, px(4));
        drawEye(item.hDC, item.rcItem, isHidden(*row), p.text);
      }
      return;
    }
    FillRect(item.hDC, &item.rcItem, theme.background);
    if (item.itemID >= rows.size()) return;
    const auto &entry = rows[item.itemID].row;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool contrast = selected && p.highContrast;
    const bool hidden = isHidden(entry);
    auto row = item.rcItem;
    InflateRect(&row, -px(3), -px(2));
    if (selected || static_cast<int>(item.itemID) == hovered) {
      const auto fill = contrast ? p.accent : p.hover;
      ui::rounded(item.hDC, row, fill, fill, px(4));
    }
    LVITEMW image{};
    image.mask = LVIF_IMAGE;
    image.iItem = static_cast<int>(item.itemID);
    if (images && ListView_GetItem(list, &image)) {
      auto icon = ImageList_GetIcon(images, image.iImage, ILD_NORMAL);
      if (icon) {
        const int x = row.left + px(6), y = (row.top + row.bottom - px(16)) / 2;
        DrawIconEx(item.hDC, x, y, icon, px(16), px(16), 0, nullptr, DI_NORMAL);
        DestroyIcon(icon);
      }
    }
    const auto eye = eyeBounds(row);
    RECT name{row.left + px(29), row.top, eye.left - px(5), row.bottom};
    auto previous = SelectObject(item.hDC, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
    const auto ink = contrast ? p.onAccent : hidden ? p.muted : p.text;
    std::visit([&](const auto &value) { ui::text(item.hDC, value.name, name, ink); }, entry);
    SelectObject(item.hDC, previous);
    if (!removedView && changeable(entry))
      drawEye(item.hDC, eye, hidden, contrast ? p.onAccent :
          static_cast<int>(item.itemID) == hovered && hoverEye ? p.text : p.muted);
  }
  void clickRow(const NMITEMACTIVATE &click) {
    if (removedView || iconView || click.iItem < 0 || size_t(click.iItem) >= rows.size()) return;
    RECT row{};
    if (!ListView_GetItemRect(list, click.iItem, &row, LVIR_BOUNDS)) return;
    InflateRect(&row, -px(3), -px(2));
    const auto eye = eyeBounds(row);
    if (PtInRect(&eye, click.ptAction) && changeable(rows[size_t(click.iItem)].row)) {
      ListView_SetItemState(list, click.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      command(Toggle, BN_CLICKED);
    }
  }
  void loadImages() {
    if (!imageSize || !list) return;
    auto replacement = ImageList_Create(imageSize, imageSize, ILC_COLOR32, 1, 8);
    if (!replacement) return;
    const int largeSize = px(40);
    auto large = ImageList_Create(largeSize, largeSize, ILC_COLOR32, 1, 8);
    if (!large) { ImageList_Destroy(replacement); return; }
    SHSTOCKICONINFO folder{sizeof(folder)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &folder))) {
      ImageList_AddIcon(replacement, folder.hIcon);
      DestroyIcon(folder.hIcon);
    }
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_LARGEICON, &folder))) {
      ImageList_AddIcon(large, folder.hIcon);
      DestroyIcon(folder.hIcon);
    }
    ListView_SetImageList(list, replacement, LVSIL_SMALL);
    ListView_SetImageList(list, large, LVSIL_NORMAL);
    if (images) ImageList_Destroy(images);
    if (largeImages) ImageList_Destroy(largeImages);
    images = replacement;
    largeImages = large;
    ComPtr<IImageList> systemSmall, systemLarge;
    SHGetImageList(SHIL_SMALL, IID_PPV_ARGS(&systemSmall));
    SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&systemLarge));
    for (int i = 0; i < ListView_GetItemCount(list); ++i) {
      const auto &itemRow = rows[size_t(i)];
      int index = 0;
      const auto load = [&](IImageList *system, int size) {
        auto icon = std::visit([&](const auto &entry) -> HICON {
          if constexpr (hasVisibility<std::decay_t<decltype(entry)>>) return entryIcon(entry.iconLocation, size);
          else return nullptr;
        }, itemRow.row);
        if (!icon && system && itemRow.iconIndex >= 0) system->GetIcon(itemRow.iconIndex, ILD_NORMAL, &icon);
        return icon;
      };
      auto smallIcon = load(systemSmall.Get(), imageSize);
      auto largeIcon = load(systemLarge.Get(), largeSize);
      const bool hidden = isHidden(itemRow.row);
      if (hidden && !smallIcon && !largeIcon) {
        smallIcon = ImageList_GetIcon(images, 0, ILD_NORMAL);
        largeIcon = ImageList_GetIcon(large, 0, ILD_NORMAL);
      }
      if (smallIcon || largeIcon) {
        const int added = addIcon(images, smallIcon ? smallIcon : largeIcon, imageSize, hidden);
        const int addedLarge = addIcon(large, largeIcon ? largeIcon : smallIcon, largeSize, hidden);
        if (added == addedLarge && added >= 0) index = added;
        if (smallIcon) DestroyIcon(smallIcon);
        if (largeIcon) DestroyIcon(largeIcon);
      }
      LVITEMW item{};
      item.mask = LVIF_IMAGE;
      item.iItem = i;
      item.iImage = index;
      ListView_SetItem(list, &item);
    }
    updateSelection();
  }
  const Row *selected() const {
    const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    return index >= 0 && size_t(index) < rows.size() ? &rows[size_t(index)].row : nullptr;
  }
  const wchar_t *toggleLabel() const {
    const auto *row = selected();
    return lang::text(row && isHidden(*row) ? L"显示" : L"隐藏");
  }
  void updateSelection() {
    auto line = std::wstring(lang::text(rows.empty() ? L"此位置没有可管理的入口。" : L"请选择一个入口。"));
    std::wstring metadata, details, hint;
    const auto *row = selected();
    if (row) std::visit([&](const auto &entry) {
      using T = std::decay_t<decltype(entry)>;
      line = entry.name;
      metadata = std::wstring(removedView ? lang::text(L"已移除") : status(*row)) + L"  ·  " + source(*row);
      if (!entry.target.empty()) details = lang::format(L"目标路径：{0}", {entry.target});
      if constexpr (std::is_same_v<T, CustomEntry>) {
        if (!entry.shortcut.empty()) hint = lang::format(L"快捷方式：{0}", {entry.shortcut});
      } else {
        if constexpr (std::is_same_v<T, Link>) hint = lang::format(L"快捷方式：{0}", {entry.path});
        else if constexpr (std::is_same_v<T, Entry>) {
          if (entry.target.empty()) hint = lang::format(L"系统入口标识：{0}", {entry.id});
        }
      }
      if (details.empty()) {
        details = std::move(hint);
        hint.clear();
      }
    }, *row);
    EnableWindow(control(Toggle), !removedView && row && changeable(*row));
    SetWindowTextW(control(Toggle), toggleLabel());
    InvalidateRect(control(Toggle), nullptr, FALSE);
    SetWindowTextW(control(StatusLine), line.c_str());
    SetWindowTextW(control(StatusMetadata), metadata.c_str());
    if (removedView) EnableWindow(control(Add), row && changeable(*row));
    SetWindowTextW(control(Details), details.c_str());
    SetWindowTextW(control(Hint), hint.c_str());
    if (hasSecondaryStatus != !hint.empty()) {
      hasSecondaryStatus = !hint.empty();
      layout();
    }
    LVITEMW item{};
    item.mask = LVIF_IMAGE;
    item.iItem = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    auto icon = images && item.iItem >= 0 && ListView_GetItem(list, &item)
        ? ImageList_GetIcon(images, item.iImage, ILD_NORMAL) : nullptr;
    SendMessageW(control(StatusIcon), STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
    if (selectedIcon) DestroyIcon(selectedIcon);
    selectedIcon = icon;
  }
  void load() {
    const auto *selection = selected();
    const size_t kind = selection ? selection->index() : std::variant_npos;
    const auto key = selection ? std::visit([](const auto &entry) { return identity(entry); }, *selection) : std::wstring{};
    auto custom = customEntries(surface);
    auto owned = custom;
    for (const auto other : surfaces) {
      if (other == surface || other == Surface::Desktop) continue;
      const auto entries = customEntries(other);
      owned.insert(owned.end(), entries.begin(), entries.end());
    }
    std::vector<DisplayRow> next;
    for (auto &entry : inventory(surface)) {
      const auto sameId = [&](const auto &own) { return !_wcsicmp(entry.id.c_str(), own.id.c_str()); };
      if (std::none_of(owned.begin(), owned.end(), sameId))
        next.push_back({std::move(entry)});
    }
    if (surface == Surface::Desktop) for (auto &entry : desktop_folder_links::inventory()) {
      if (std::none_of(custom.begin(), custom.end(), [&](const auto &own) { return !_wcsicmp(entry.path.c_str(), own.shortcut.c_str()); }))
        next.push_back({std::move(entry)});
    }
    for (auto &entry : custom) {
      if (!entry.shortcut.empty()) entry.name = fn::fs::path(entry.shortcut).stem().native();
      next.push_back({std::move(entry)});
    }
    const auto nameOf = [](const Row &row) -> const std::wstring & {
      return std::visit([](const auto &entry) -> const std::wstring & { return entry.name; }, row);
    };
    if (surface == Surface::ThisPc) {
      std::vector<DisplayRow> ordered;
      for (auto &item : this_pc_view::items()) {
        auto match = std::find_if(next.begin(), next.end(), [&](const auto &row) {
          return !item.id.empty() && std::visit([&](const auto &entry) {
            return !_wcsicmp(identity(entry).c_str(), item.id.c_str());
          }, row.row);
        });
        if (match == next.end()) {
          const auto group = item.group;
          const auto icon = item.iconIndex;
          ordered.push_back({std::move(item), group, icon});
        } else {
          std::visit([&](auto &entry) {
            entry.name = item.name;
            if (entry.target.empty()) entry.target = item.target;
          }, match->row);
          ordered.push_back({std::move(match->row), item.group, item.iconIndex});
          next.erase(match);
        }
      }
      for (auto &row : next) {
        row.group = std::holds_alternative<CustomEntry>(row.row) ? Group::Folders :
            this_pc_view::classify(std::get<Entry>(row.row).id);
        const auto firstDrive = std::find_if(ordered.begin(), ordered.end(), [](const auto &existing) {
          const auto *native = std::get_if<NativeItem>(&existing.row);
          return existing.group == Group::Drives && native && native->target.size() == 3 &&
                 native->target[1] == L':' && native->target[2] == L'\\';
        });
        if (row.group == Group::Drives) ordered.insert(firstDrive, std::move(row));
        else ordered.push_back(std::move(row));
      }
      next = std::move(ordered);
    } else {
      std::stable_sort(next.begin(), next.end(), [&](const auto &a, const auto &b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, nameOf(a.row).c_str(), -1,
                               nameOf(b.row).c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
      });
    }
    if (surface == Surface::ThisPc) {
      std::erase_if(next, [&](const auto &display) {
        if (const auto *entry = std::get_if<Entry>(&display.row))
          return isRemoved(surface, entry->id) != removedView;
        if (const auto *native = std::get_if<NativeItem>(&display.row))
          return removedView || isRemoved(surface, native->id);
        return removedView;
      });
    }
    reloading = true;
    hovered = -1;
    hoverEye = false;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    rows = std::move(next);
    ListView_RemoveAllGroups(list);
    setView();
    ListView_EnableGroupView(list, surface == Surface::ThisPc);
    if (surface == Surface::ThisPc) {
      for (int i = 0; i < int(groupLabels.size()); ++i) {
        LVGROUP group{};
        group.cbSize = sizeof(group);
        group.mask = LVGF_GROUPID | LVGF_HEADER | LVGF_STATE;
        group.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
        group.state = LVGS_COLLAPSIBLE | (collapsed[size_t(i)] ? LVGS_COLLAPSED : 0);
        group.iGroupId = i;
        const auto count = std::count_if(rows.begin(), rows.end(), [&](const auto &row) {
          return static_cast<int>(row.group) == i;
        });
        headings[size_t(i)] = lang::format(groupLabels[size_t(i)], {std::to_wstring(count)});
        group.pszHeader = headings[size_t(i)].data();
        ListView_InsertGroup(list, -1, &group);
      }
    }
    for (int i = 0; i < int(rows.size()); ++i) {
      const auto &display = rows[size_t(i)];
      const auto &row = display.row;
      LVITEMW item{};
      item.mask = LVIF_TEXT;
      item.iItem = i;
      item.pszText = const_cast<wchar_t *>(nameOf(row).c_str());
      if (surface == Surface::ThisPc) {
        item.mask |= LVIF_GROUPID;
        item.iGroupId = static_cast<int>(display.group);
      }
      if (ListView_InsertItem(list, &item) < 0) break;
      if (row.index() == kind && std::visit([](const auto &entry) { return identity(entry); }, row) == key)
        ListView_SetItemState(list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    loadImages();
    if (iconView) ListView_Arrange(list, LVA_DEFAULT);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    reloading = false;
    InvalidateRect(list, nullptr, TRUE);
    updateSelection();
    syncScroll();
  }
  void contextMenu(HWND from, POINT point) {
    if (from == list && point.x != -1) {
      LVHITTESTINFO hit{};
      hit.pt = point;
      ScreenToClient(list, &hit.pt);
      const int index = ListView_HitTest(list, &hit);
      ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
      if (index >= 0) ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (point.x == -1 && point.y == -1) {
      RECT bounds{};
      const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
      if (index >= 0) ListView_GetItemRect(list, index, &bounds, LVIR_BOUNDS);
      point = {bounds.left + px(12), bounds.bottom + px(8)};
      ClientToScreen(list, &point);
    }
    using namespace quick::popup;
    auto menu = std::make_shared<Branch>();
    const auto append = [&](unsigned command, const wchar_t *caption, Symbol symbol) {
      Item item;
      item.title = caption;
      item.command = command;
      item.symbol = symbol;
      menu->items.push_back(std::move(item));
    };
    if (removedView) {
      if (const auto *row = selected(); row && changeable(*row))
        append(Recover, lang::text(L"恢复入口"), Symbol::History);
    } else {
      append(Add, lang::text(L"添加文件夹…"), Symbol::AddFolder);
      if (const auto *row = selected()) {
        if (changeable(*row)) append(Toggle, toggleLabel(), Symbol::Visibility);
        if (restorable(*row)) append(Restore, lang::text(L"还原"), Symbol::History);
        if (std::holds_alternative<CustomEntry>(*row) ||
            (surface == Surface::ThisPc && std::holds_alternative<Entry>(*row) && changeable(*row)))
          append(Remove, lang::text(L"移除入口"), Symbol::Trash);
      }
    }
    if (menu->items.empty()) return;
    const auto action = show(GetModuleHandleW(nullptr), point, std::move(menu), theme.mode, GetAncestor(window, GA_ROOT));
    if (action) command(action, BN_CLICKED);
  }
  void command(int id, int code) {
    if (id == Add && removedView) id = Recover;
    if (id == Removed && code == BN_CLICKED) {
      removedView = !removedView;
      SetWindowTextW(control(Removed), lang::text(removedView ? L"返回入口列表" : L"已移除"));
      SetWindowTextW(control(Add), lang::text(removedView ? L"恢复入口" : L"添加文件夹…"));
      EnableWindow(control(Add), !removedView);
      load();
      return;
    }
    if ((id == ListMode || id == IconMode) && code == BN_CLICKED) {
      DWORD value = id == IconMode;
      if (iconView == (value != 0)) return;
      if (const auto error = RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, viewPreference, REG_DWORD, &value, sizeof(value)))
        fn::fail(L"文件夹入口操作失败。", error);
      iconView = value != 0;
      InvalidateRect(control(ListMode), nullptr, FALSE);
      InvalidateRect(control(IconMode), nullptr, FALSE);
      load();
      layout();
      InvalidateRect(list, nullptr, TRUE);
      return;
    }
    if (id == Location && code == CBN_SELCHANGE) {
      const auto index = SendMessageW(control(Location), CB_GETCURSEL, 0, 0);
      if (index < 0 || size_t(index) >= surfaces.size()) return;
      surface = surfaces[size_t(index)];
      removedView = false;
      SetWindowTextW(control(Removed), lang::text(L"已移除"));
      SetWindowTextW(control(Add), lang::text(L"添加文件夹…"));
      EnableWindow(control(Add), TRUE);
      layout();
      ListView_DeleteAllItems(list);
      rows.clear();
      updateSelection();
    } else if (id == Add && code == BN_CLICKED) {
      const auto destination = surface;
      const auto target = pickFolder(window);
      if (target.empty()) return;
      const fn::fs::path folder(target);
      auto name = folder.filename();
      if (name.empty()) name = folder.parent_path().filename();
      if (name.empty()) name = folder.root_name();
      auto label = name.native();
      if (!label.empty() && label.back() == L':') label.pop_back();
      addCustom(destination, label, target);
      refreshOpen(destination);
    } else if ((id == Toggle || id == Restore || id == Remove || id == Recover) && code == BN_CLICKED) {
      const auto *row = selected();
      if (!row) return;
      bool changed = false;
      std::visit([&](const auto &entry) {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T, CustomEntry>) {
          if (id == Remove) { removeCustom(entry); changed = true; }
        } else if constexpr (std::is_same_v<T, Link>) {
          if (id == Toggle && entry.canChange) { desktop_folder_links::setHidden(entry, !entry.hidden); changed = true; }
          if (id == Restore && restorable(*row)) { desktop_folder_links::restore(entry); changed = true; }
        } else if constexpr (std::is_same_v<T, Entry>) {
          if (surface == Surface::ThisPc && entry.canChange && id == Remove && !removedView) {
            setHidden(entry, true);
            markRemoved(surface, entry.id, true);
            changed = true;
          }
          if (removedView && id == Recover && entry.canChange) {
            setHidden(entry, false);
            markRemoved(surface, entry.id, false);
            changed = true;
          }
          if (id == Toggle && entry.canChange && !removedView) { setHidden(entry, !entry.hidden); changed = true; }
          if (id == Restore && restorable(*row)) { restore(entry); changed = true; }
          if (changed) refreshOpen(entry);
        }
      }, *row);
      if (!changed) return;
      if (!std::holds_alternative<Entry>(*row)) refreshOpen(surface);
    } else if (id != Refresh || code != BN_CLICKED) return;
    load();
  }
};

constexpr UINT reloadMessage = WM_APP + 1;
LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) {
    auto &state = *static_cast<std::unique_ptr<Page> *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.release()));
    return TRUE;
  }
  auto *state = reinterpret_cast<Page *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return DefWindowProcW(window, message, w, l);
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    delete state;
    return DefWindowProcW(window, message, w, l);
  }
  LRESULT result = 0;
  if (state->theme.handle(window, message, w, l, result)) return result;
  try {
    switch (message) {
    case WM_CREATE: return state->create() ? 0 : -1;
    case WM_PAINT: state->paintStatus(); return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        if (state->resizingStatus || state->statusDividerAt(point.y)) {
          SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
          return TRUE;
        }
      }
      break;
    case WM_LBUTTONDOWN:
      if (state->statusDividerAt(GET_Y_LPARAM(l))) {
        state->resizeStart = GET_Y_LPARAM(l);
        state->resizeHeight = state->statusHeight;
        state->resizingStatus = true;
        SetCapture(window);
        return 0;
      }
      break;
    case WM_MOUSEMOVE:
      if (state->resizingStatus) {
        const auto dpi = GetDpiForWindow(window);
        const int delta = MulDiv(state->resizeStart - GET_Y_LPARAM(l), 96, dpi);
        const int height = std::clamp(state->resizeHeight + delta, minimumStatusHeight, maximumStatusHeight);
        if (height != state->statusHeight) {
          state->statusHeight = height;
          state->layout();
          InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (state->resizingStatus) {
        ReleaseCapture();
        const DWORD height = state->statusHeight;
        RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, statusHeightPreference, REG_DWORD, &height, sizeof(height));
        return 0;
      }
      break;
    case WM_CAPTURECHANGED: state->resizingStatus = false; break;
    case WM_MEASUREITEM:
      if (w == List) {
        reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = state->px(28);
        return TRUE;
      }
      break;
    case WM_DRAWITEM:
      if (w == List || w == Toggle || w == ListMode || w == IconMode || w == Refresh) {
        state->drawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
        return TRUE;
      }
      break;
    case WM_SIZE:
    case WM_DPICHANGED_AFTERPARENT: state->layout(); return 0;
    case WM_COMMAND: state->command(LOWORD(w), HIWORD(w)); return 0;
    case WM_VSCROLL:
      if (reinterpret_cast<HWND>(l) == state->scroll.window()) { state->scroll.command(w); return 0; }
      break;
    case WM_CONTEXTMENU:
      if (reinterpret_cast<HWND>(w) != state->control(Details)) {
        state->contextMenu(reinterpret_cast<HWND>(w), POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        return 0;
      }
      break;
    case WM_NOTIFY:
      if (auto *notice = reinterpret_cast<NMHDR *>(l); notice->idFrom == List && notice->code == NM_CUSTOMDRAW) {
        return state->drawGroup(*reinterpret_cast<NMLVCUSTOMDRAW *>(l));
      } else if (notice->idFrom == List && notice->code == LVN_ITEMCHANGED &&
          !state->reloading && (reinterpret_cast<NMLISTVIEW *>(l)->uChanged & LVIF_STATE)) state->updateSelection();
      else if (notice->hwndFrom == state->tooltips && notice->code == TTN_GETDISPINFOW &&
          notice->idFrom == reinterpret_cast<UINT_PTR>(state->control(Toggle)))
        reinterpret_cast<NMTTDISPINFOW *>(l)->lpszText = const_cast<wchar_t *>(state->toggleLabel());
      else if (notice->idFrom == List && notice->code == NM_CLICK)
        state->clickRow(*reinterpret_cast<NMITEMACTIVATE *>(l));
      break;
    case reloadMessage: state->applyTheme(); state->load(); return 0;
    case WM_THEMECHANGED: state->applyTheme(); return 0;
    }
  } catch (const fn::Error &error) {
    if (message == WM_CREATE) return -1;
    ui::message(window, error.message.c_str(), lang::text(title), MB_OK | MB_ICONERROR);
  } catch (...) {
    if (message == WM_CREATE) return -1;
    ui::message(window, lang::text(L"文件夹入口操作失败。"), lang::text(title), MB_OK | MB_ICONERROR);
  }
  return DefWindowProcW(window, message, w, l);
}
}

HWND create(HWND owner, ui::Theme &theme, HFONT font) {
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
  if (!InitCommonControlsEx(&controls)) return nullptr;
  auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW cls{};
  cls.lpfnWndProc = proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.lpszClassName = className;
  RegisterClassW(&cls);
  auto state = std::unique_ptr<Page>(new Page{.theme = theme, .font = font});
  return CreateWindowExW(WS_EX_CONTROLPARENT, className, lang::text(title), WS_CHILD | WS_CLIPCHILDREN,
      0, 0, 0, 0, owner, nullptr, instance, &state);
}
void refresh(HWND page) { SendMessageW(page, reloadMessage, 0, 0); }
HWND locationControl(HWND page) { return GetDlgItem(page, Location); }
HWND viewControl(HWND page, bool icons) { return GetDlgItem(page, icons ? IconMode : ListMode); }
}
