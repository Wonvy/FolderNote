// Desktop box clipboard, deletion, automation, backup and the public interface.
#include "desktop_groups_internal.h"

namespace desktop_groups::detail {
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
          quick::ensureHelperRunning();
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
}  // namespace desktop_groups::detail

namespace desktop_groups {
using namespace detail;


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
