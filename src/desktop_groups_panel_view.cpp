// Panel content refresh, paging, rendering, context menu and window procedure.
#include "desktop_groups_internal.h"

namespace desktop_groups::detail {
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
  bool changed = false;
  for (auto button : viewButtons) {
    if (button && (IsWindowVisible(button) != FALSE) != show) {
      ShowWindow(button, show ? SW_SHOWNA : SW_HIDE);
      changed = true;
    }
  }
  if (changed) {
    RECT header{};
    GetClientRect(window, &header);
    header.bottom = headerHeight();
    RedrawWindow(window, &header, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
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

void Panel::handleMiddleClick(POINT screen) {
  if (desktop.dragging || desktop.quitting || desktop.menuOpen || desktop.editing || !navigation::quickEnabled()) return;
  if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU) |
       GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
    return;
  quick::ensureHelperRunning();
  if (const HWND shell = GetShellWindow()) {
    try { quick::launchMenu(shell, nullptr, screen); } catch (...) {}
  }
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
    if (!desktop.theme.colors.highContrast) {
      menuItem(*sub, ToggleBoxGlass, L"毛玻璃效果", Symbol::Visibility, backgroundGlass());
      if (backgroundGlass()) {
        quick::popup::Item blur;
        blur.title = lang::text(L"模糊强度");
        blur.slider = quick::popup::Slider{0, 100, backgroundBlur(),
          [this](int value) {
            blurPreview = value;
            SetTimer(window, blurPreviewTimer, 33, nullptr);
            desktop.lower();
            if (!hasSurface()) applyBackground();
          },
          [this](int value) {
            if (desktop.quitting) return false;
            auto changed = desktop.groups;
            changed[index].blurStrength = value;
            const bool saved = desktop.commit(std::move(changed), window, false);
            blurPreview.reset();
            KillTimer(window, blurPreviewTimer);
            desktop.lower();
            if (!hasSurface()) applyBackground();
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
    if (std::exchange(blurPreview, std::nullopt)) {
      desktop.lower();
      if (!hasSurface()) applyBackground();
    }
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
        RECT area{};
        GetClientRect(child, &area);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
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
        if (panel->blurPreview && desktop.menuOpen) {
          desktop.lower();
          if (!panel->hasSurface()) panel->applyBackground();
        } else KillTimer(window, blurPreviewTimer);
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
      if (!panel->dockHidden) {
        RECT area{};
        GetClientRect(window, &area);
        const auto buffer = !panel->hasSurface() && area.right > 0 && area.bottom > 0 ? CreateCompatibleDC(dc) : nullptr;
        const auto bitmap = buffer ? CreateCompatibleBitmap(dc, area.right, area.bottom) : nullptr;
        const auto oldBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
        if (oldBitmap && oldBitmap != HGDI_ERROR) {
          panel->paint(buffer, false);
          BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
          SelectObject(buffer, oldBitmap);
        } else {
          panel->paint(dc, false);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
      }
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DRAWITEM: {
      const auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item.hwndItem == panel->scrollCorner) {
        const auto saved = SaveDC(item.hDC);
        if (saved) {
          IntersectClipRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
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
      const auto saved = SaveDC(item.hDC);
      if (saved) IntersectClipRect(item.hDC, box.left, box.top, box.right, box.bottom);
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
        if (saved) RestoreDC(item.hDC, saved);
        return TRUE;
      }
      if (panel->collapsePress && panel->collapsePress->armed) {
        if (const auto move = LoadCursorW(nullptr, IDC_SIZEALL))
          DrawIconEx(item.hDC, centerX - px(8), centerY - px(8), move, px(16), px(16), 0, nullptr, DI_NORMAL);
        if (saved) RestoreDC(item.hDC, saved);
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
      if (saved) RestoreDC(item.hDC, saved);
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
      if (panel->windowPositionReady) {
        desktop.updateGlass();
        if (!panel->hasSurface() && !desktop.theme.colors.highContrast && panel->backgroundOpacity() < 100) {
          RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
        }
      }
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
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_NCMBUTTONDOWN:
    case WM_NCMBUTTONDBLCLK:
      panel->middleDown = GetCursorPos(&panel->middleDownPoint) != FALSE;
      return 0;
    case WM_MBUTTONUP:
    case WM_NCMBUTTONUP:
      if (std::exchange(panel->middleDown, false)) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && quick::middleClickWithin(panel->middleDownPoint, cursor)) {
          panel->handleMiddleClick(cursor);
        }
      }
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
      report(window, lang::text(w ? L"无法移动部分桌面项目。" : L"只能添加桌面或 .desktop 中的文件和文件夹。"));
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
  events.middleClick = [this](POINT point) { handleMiddleClick(point); };
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

}  // namespace desktop_groups::detail
