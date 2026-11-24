// Desktop panel lifecycle, box and tab management and menu command handling.
#include "desktop_groups_internal.h"

namespace desktop_groups::detail {
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

static INT_PTR CALLBACK boxDialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
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
                   bool allowEmpty, size_t maxLength) {
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


bool Desktop::commit(std::vector<Group> changed, HWND owner, bool refreshContents) {
  try {
    save(changed, layoutFile);
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
      if (panel.hasSurface() && panel.surface->usesDesktopGlass() && panel.backgroundBlur() >= 0) {
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
  if (!boxesHidden && !theme.colors.highContrast) {
    for (const auto &p : panels) {
      auto &panel = *p;
      if (panel.hasSurface() || panel.dockHidden || !IsWindowVisible(panel.window)) continue;
      if (panel.backgroundOpacity() >= 100) continue;
      const int wanted = panel.backgroundGlass() ? std::clamp(panel.backgroundBlur(), 0, 100) : 0;
      auto image = wallpaperBackdrop(wanted);
      if (image && (panel.renderedBlur != wanted || panel.tintedWallpaperSource != image.get())) {
        panel.renderedBlur = wanted;
        RedrawWindow(panel.window, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
      }
    }
  }
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
      save(groups, layoutFile);
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
      save(groups, layoutFile);
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

}  // namespace desktop_groups::detail
