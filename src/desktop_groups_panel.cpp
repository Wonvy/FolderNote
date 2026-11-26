// Panel appearance, tabs, hover menus, thumbnails, docking and resize gestures.
#include "desktop_groups_internal.h"
#include <shlwapi.h>

namespace desktop_groups::detail {
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

bool isLocalPath(const std::wstring &path) {
  if (path.empty() || PathIsNetworkPathW(path.c_str())) return false;
  if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
    const wchar_t root[4]{path[0], L':', L'\\', L'\0'};
    if (GetDriveTypeW(root) == DRIVE_REMOTE) return false;
  }
  return true;
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
      isLocalPath(rawTarget) && PathIsDirectoryW(rawTarget)) {
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
    } else if (isLocalPath(entryPath) && PathIsDirectoryW(entryPath.c_str())) {
      resolvedFolder = entryPath;
    }
  } else {
    const auto actual = desktop.boxItem(entryPath);
    if (!actual.empty() && !isDesktopShellItem(actual)) {
      if (isLocalPath(actual) && PathIsDirectoryW(actual.c_str())) resolvedFolder = actual;
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
  if (rootFolder.empty() || !isLocalPath(rootFolder) || !PathIsDirectoryW(rootFolder.c_str())) return;

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
    if (previous.opacity != value.opacity || previous.glass != value.glass) {
      panel->applyBackground();
    }
#ifdef FOLDERNOTE_WINUI
    if (panel->hasSurface()) {
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
  mappedLastScan = GetTickCount64();
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
  const int savedClip = !buffered ? SaveDC(dc) : 0;
  if (savedClip) IntersectClipRect(dc, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
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
  if (savedClip) RestoreDC(dc, savedClip);
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
  if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) {
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ClientToScreen(control, &pt);
    if (!GetCursorPos(&panel.middleDownPoint)) panel.middleDownPoint = pt;
    panel.middleDown = true;
    return 0;
  }
  if (message == WM_MBUTTONUP && std::exchange(panel.middleDown, false)) {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
      cursor = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ClientToScreen(control, &cursor);
    }
    if (quick::middleClickWithin(panel.middleDownPoint, cursor)) {
      panel.handleMiddleClick(cursor);
    }
    return 0;
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
    panel.desktop.ensureHeaderTimer();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return TRUE;
  }
  if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
    panel.desktop.ensureHeaderTimer();
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
  const auto tint = backgroundColor();
  const int opacityPct = desktop.theme.colors.highContrast ? 100 : std::clamp(backgroundOpacity(), 0, 100);
  if (opacityPct < 100) {
    const int wantedBlur = backgroundGlass() ? std::clamp(backgroundBlur(), 0, 100) : 0;
    auto image = wallpaperBackdrop(wantedBlur);
    if (image) renderedBlur = wantedBlur;
    else {
      image = wallpaperBackdrop(renderedBlur);
      if (!image) image = wallpaperBackdrop(0);
    }
    RECT boxScreen{};
    if (image && image->width > 0 && image->height > 0 &&
        image->screen.right > image->screen.left && image->screen.bottom > image->screen.top &&
        image->pixels.size() == static_cast<size_t>(image->width) * image->height &&
        GetWindowRect(window, &boxScreen)) {
      if (tintedWallpaperSource != image.get() || tintedWallpaperColor != tint ||
          tintedWallpaperOpacity != opacityPct || tintedWallpaperPixels.size() != image->pixels.size()) {
        tintedWallpaperSource = image.get();
        tintedWallpaperColor = tint;
        tintedWallpaperOpacity = opacityPct;
        tintedWallpaperPixels.resize(image->pixels.size());
        const int alpha = MulDiv(opacityPct, 255, 100);
        const int inv = 255 - alpha;
        const int tr = GetRValue(tint) * alpha;
        const int tg = GetGValue(tint) * alpha;
        const int tb = GetBValue(tint) * alpha;
        const auto *src = image->pixels.data();
        auto *dst = tintedWallpaperPixels.data();
        const size_t count = image->pixels.size();
        for (size_t i = 0; i < count; ++i) {
          const DWORD px = src[i];
          const DWORD r = static_cast<DWORD>((tr + int((px >> 16) & 255) * inv + 127) / 255);
          const DWORD g = static_cast<DWORD>((tg + int((px >> 8) & 255) * inv + 127) / 255);
          const DWORD b = static_cast<DWORD>((tb + int(px & 255) * inv + 127) / 255);
          dst[i] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
      }
      if (boxScreen.left < image->screen.left || boxScreen.top < image->screen.top ||
          boxScreen.right > image->screen.right || boxScreen.bottom > image->screen.bottom) {
        const auto prev = SetDCBrushColor(dc, tint);
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc, prev);
      }
      const int screenW = image->screen.right - image->screen.left;
      const int screenH = image->screen.bottom - image->screen.top;
      const int destX = offset.x + (image->screen.left - boxScreen.left);
      const int destY = offset.y + (image->screen.top - boxScreen.top);
      BITMAPINFO bmi{};
      bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), image->width, -image->height, 1, 32, BI_RGB};
      const int oldMode = SetStretchBltMode(dc, HALFTONE);
      POINT oldOrg{};
      SetBrushOrgEx(dc, offset.x, offset.y, &oldOrg);
      const auto copied = StretchDIBits(dc, destX, destY, screenW, screenH,
                                        0, 0, image->width, image->height,
                                        tintedWallpaperPixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
      SetBrushOrgEx(dc, oldOrg.x, oldOrg.y, nullptr);
      SetStretchBltMode(dc, oldMode);
      if (copied && copied != GDI_ERROR) return;
    }
  }
  const auto previous = SetDCBrushColor(dc, tint);
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
  const int buttonWidth = px(30), buttonPad = px(6);
  const int buttonsShown = IsWindowVisible(viewButtons[0]) ? 3 : 1;
  const LONG buttonLeft = std::max(0L, area.right - buttonPad - buttonWidth * buttonsShown);
  RECT header{pad, 0, std::max(static_cast<LONG>(pad), buttonLeft - px(5)), headerBottom};
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

}  // namespace desktop_groups::detail
