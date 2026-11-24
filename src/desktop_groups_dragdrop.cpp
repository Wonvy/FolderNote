// Drag and drop between boxes, the desktop and Explorer, plus transfer progress.
#include "desktop_groups_internal.h"

namespace desktop_groups::detail {
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

}  // namespace desktop_groups::detail
