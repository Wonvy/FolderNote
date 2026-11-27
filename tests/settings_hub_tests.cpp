// Only hidden, test-owned windows and an isolated registry are used here.
// Compile the actual settings controller so page routing and layout are tested
// without starting Explorer, helpers, or a second interactive settings process.
#include "../src/navigation.cpp"
#include "../src/context_inventory.h"
#include "../src/context_commands.h"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <shellapi.h>

namespace {
int checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
struct Registry {
  HKEY real = nullptr, isolated = nullptr;
  std::wstring path;
  Registry() {
    GUID token{}; CoCreateGuid(&token); wchar_t text[40]{}; StringFromGUID2(token, text, ARRAYSIZE(text));
    path = std::wstring(L"Software\\FolderNoteSettingsHubTests\\") + text;
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &real) == ERROR_SUCCESS &&
          RegCreateKeyExW(real, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &isolated, nullptr) == ERROR_SUCCESS &&
          RegOverridePredefKey(HKEY_CURRENT_USER, isolated) == ERROR_SUCCESS, "isolate settings registry");
  }
  ~Registry() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (isolated) RegCloseKey(isolated);
    if (real) { RegDeleteTreeW(real, path.c_str()); RegCloseKey(real); }
  }
};
bool visibleStyle(HWND window) { return window && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0; }
std::wstring windowText(HWND window) {
  std::wstring text(size_t(GetWindowTextLengthW(window)) + 1, L'\0');
  int copied = GetWindowTextW(window, text.data(), int(text.size())); text.resize(size_t(copied)); return text;
}
RECT childBounds(HWND child, HWND parent) {
  RECT rect{}; GetWindowRect(child, &rect);
  MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&rect), 2); return rect;
}
struct Hub {
  HWND window = nullptr;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  Hub() {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = navigation::contentProc;
    cls.lpszClassName = L"FolderNote.Settings.Content"; RegisterClassW(&cls);
    cls.lpfnWndProc = navigation::settingsProc; cls.lpszClassName = L"FolderNote.Settings.HiddenTest"; RegisterClassW(&cls);
    navigation::appearance = std::make_unique<ui::Theme>();
    navigation::dpi = 96;
    window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"Hidden settings layout test",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        0, 0, 980, 660, nullptr, nullptr, instance, nullptr);
    check(window != nullptr, "create hidden settings controller");
  }
  ~Hub() {
    if (IsWindow(window)) DestroyWindow(window);
    navigation::appearance.reset();
    MSG message{}; while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    UnregisterClassW(L"FolderNote.Settings.HiddenTest", instance);
    UnregisterClassW(L"FolderNote.Settings.Content", instance);
  }
};
void inspectVersion(HWND window) {
  HWND version = GetDlgItem(window, 31);
  check(visibleStyle(version), "version remains visible outside page scroller");
  check(windowText(version).find(app::version) != std::wstring::npos, "visible version identifies this build");
  check(GetParent(version) == window, "version belongs to fixed shell instead of a scrolling page");
  RECT client{}; GetClientRect(window, &client);
  auto bounds = childBounds(version, window);
  HWND update = GetDlgItem(window, navigation::updateButton);
  auto updateBounds = childBounds(update, window);
  check(visibleStyle(update) && windowText(update) == L"检查更新" &&
        updateBounds.left >= bounds.right && updateBounds.top <= bounds.top && updateBounds.bottom >= bounds.bottom &&
        updateBounds.bottom <= client.bottom && updateBounds.right <= navigation::scale(navigation::settingsSidebarWidth),
        "update icon and version share one row");
  check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
        "version stays inside resized settings window");
  auto lastTab = childBounds(GetDlgItem(window, navigation::sidebarItems.back().id), window);
  check(bounds.top >= lastTab.bottom, "version does not overlap navigation tabs");
  auto page = childBounds(navigation::managerPage ? navigation::managerPage : navigation::body, window);
  check(bounds.right <= page.left, "version stays clear of the right-hand page");
  HDC dc = GetDC(version); auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(version, WM_GETFONT, 0, 0)));
  SIZE extent{}; auto text = windowText(version); GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
  SelectObject(dc, oldFont); ReleaseDC(version, dc);
  check(extent.cx <= bounds.right - bounds.left && extent.cy <= bounds.bottom - bounds.top,
        "version text is not clipped at current scale");
}
void quickSearchSettings() {
  using namespace navigation;
  {
    Hub hub;
    check(!GetDlgItem(hub.window, IDOK) && !GetDlgItem(hub.window, IDCANCEL) && !GetDlgItem(hub.window, 33) &&
          !GetDlgItem(hub.window, 96) && !GetDlgItem(hub.window, 97),
          "classic two-column settings omit footer Save/Cancel and detail-header back/toggle controls");
    check(visibleStyle(GetDlgItem(hub.window, 75)) && IsWindowEnabled(GetDlgItem(body, cardEditQuickFind)),
          "quick search sidebar tab and Edit button start active because default scopes are enabled");
    SendDlgItemMessageW(body, cardEditQuickFind, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::QuickFind && windowText(GetDlgItem(hub.window, 30)) == L"快捷搜索",
          "quick search card Edit button routes to QuickFind page");
    check(!visibleStyle(body), "overview cards hide on QuickFind page");
    check(SendDlgItemMessageW(hub.window, findFolderAction, CB_GETCURSEL, 0, 0) == 1 &&
          SendDlgItemMessageW(hub.window, findFileAction, CB_GETCURSEL, 0, 0) == 0, "existing folder-open file-select defaults appear");
    const auto choose = [&](int id, int row) {
      SendDlgItemMessageW(hub.window, id, CB_SETCURSEL, row, 0);
      SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(hub.window, id)));
    };
    choose(findFolderAction, 0); choose(findFileAction, 1); choose(findPosition, 1); choose(findRows, 0);
    SendDlgItemMessageW(hub.window, findIcons, BM_CLICK, 0, 0);
    SendDlgItemMessageW(hub.window, findBoxes, BM_CLICK, 0, 0);
    const auto saved = quick_find::settings();
    check(!saved.openFolders && saved.openFiles && !saved.atPointer && saved.visibleRows == 5 && !saved.showIcons && !saved.boxes,
          "settings controls persist independent actions, position, rows, icons and box scope immediately");
    for (UINT scaleDpi : {96u, 144u, 192u}) {
      RECT target{0, 0, MulDiv(900, scaleDpi, 96), MulDiv(500, scaleDpi, 96)};
      SendMessageW(hub.window, WM_DPICHANGED, MAKEWPARAM(scaleDpi, scaleDpi), reinterpret_cast<LPARAM>(&target));
      RECT client{}; GetClientRect(hub.window, &client);
      for (int id = findDescription; id <= findHint; ++id) {
        const auto control = GetDlgItem(hub.window, id);
        const auto bounds = childBounds(control, hub.window);
        check(visibleStyle(control) && bounds.left >= scale(settingsSidebarWidth) && bounds.right <= client.right &&
              bounds.top >= scale(68) && bounds.bottom <= client.bottom, "search controls fit at minimum size and every DPI");
      }
      const auto lastTab = childBounds(GetDlgItem(hub.window, sidebarItems.back().id), hub.window);
      check(lastTab.bottom < childBounds(GetDlgItem(hub.window, updateButton), hub.window).top, "bottom sidebar tab fits above version and update");
      check(SendDlgItemMessageW(hub.window, findRows, CB_GETITEMHEIGHT, WPARAM(-1), 0) == scale(26), "search combo scales on DPI change");
    }
    SendDlgItemMessageW(hub.window, sidebarItems[0].id, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::Features && !visibleStyle(GetDlgItem(hub.window, findFolderAction)) && visibleStyle(body),
          "sidebar overview button returns to feature cards and hides search controls");
    SendDlgItemMessageW(body, quickFindToggle, BM_CLICK, 0, 0);
    const auto disabled = quick_find::settings();
    check(!disabled.boxes && !disabled.lists && !disabled.explorer && !disabled.dialogs &&
          SendMessageW(quickFindCheck, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
          !visibleStyle(GetDlgItem(hub.window, 75)) && !IsWindowEnabled(GetDlgItem(body, cardEditQuickFind)),
          "disabling quick search turns off scopes, hides its sidebar tab, and disables Edit");
    SendDlgItemMessageW(body, quickFindToggle, BM_CLICK, 0, 0);
    const auto restored = quick_find::settings();
    check(!restored.boxes && restored.lists && !restored.explorer && !restored.dialogs &&
          SendMessageW(quickFindCheck, BM_GETCHECK, 0, 0) == BST_CHECKED &&
          visibleStyle(GetDlgItem(hub.window, 75)) && IsWindowEnabled(GetDlgItem(body, cardEditQuickFind)),
          "re-enabling quick search restores scopes, reveals its sidebar tab, and enables Edit");
  }
  {
    Hub reopened; showPage(SettingsPage::QuickFind);
    check(SendDlgItemMessageW(reopened.window, findFolderAction, CB_GETCURSEL, 0, 0) == 0 &&
          SendDlgItemMessageW(reopened.window, findFileAction, CB_GETCURSEL, 0, 0) == 1 &&
          SendDlgItemMessageW(reopened.window, findRows, CB_GETCURSEL, 0, 0) == 0, "reopening settings restores selections");
  }
  quick_find::saveSettings({});
  RegDeleteKeyValueW(HKEY_CURRENT_USER, configKey, quickFindSavedScopesValue);
  activePage = SettingsPage::Features;
}
void automaticUpdates() {
  using namespace navigation;
  Hub hub;
  showPage(SettingsPage::Preferences);
  const auto toggle = GetDlgItem(hub.window, automaticUpdateCheck);
  const auto explanation = GetDlgItem(hub.window, automaticUpdateDescription);
  check(visibleStyle(toggle) && visibleStyle(explanation) && windowText(toggle) == L"自动更新" &&
        SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED && !automaticUpdatesEnabled(),
        "automatic updates are opt-in and available in preferences");
  check(IsWindowEnabled(toggle) == (*app::updateRepository != L'\0'), "automatic updates require a configured source");
  if (*app::updateRepository) {
    SendMessageW(toggle, BM_CLICK, 0, 0);
    check(automaticUpdatesEnabled() && !updateWork.valid(), "save the enabled preference without starting network in a hidden fixture");
    check(!canInstallAutomatically(), "a development executable without an active installation can only offer a manual update");
    SendMessageW(toggle, BM_CLICK, 0, 0);
    check(!automaticUpdatesEnabled(), "save the disabled preference immediately");
  }
  const auto now = updateClock();
  constexpr ULONGLONG day = 24ULL * 60 * 60 * 10000000;
  check(automaticCheckDue(now), "first background check is due");
  check(RegSetKeyValueW(HKEY_CURRENT_USER, configKey, lastAutomaticCheckValue, REG_QWORD, &now, sizeof(now)) == ERROR_SUCCESS &&
        !automaticCheckDue(now) && !automaticCheckDue(now + day - 1) && automaticCheckDue(now + day) &&
        automaticCheckDue(now - 1), "background checks are limited to once a day and recover when the clock moves back");
  std::promise<UpdateResult> completed;
  updateWork = completed.get_future();
  updateOperation = UpdateOperation::Automatic;
  completed.set_value({updater::Release{L"9.0.1", {}, {}, true}, {}, false});
  finishUpdateWork(hub.window);
  check(availableUpdate && availableUpdate->win11Package && IsWindowEnabled(GetDlgItem(hub.window, updateButton)) &&
        windowText(GetDlgItem(hub.window, updateStatusLabel)).find(L"9.0.1") != std::wstring::npos,
        "a background check reports an available Win11 update for manual confirmation");
  SetWindowPos(hub.window, nullptr, 0, 0, scale(900), scale(500), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  const auto status = childBounds(GetDlgItem(hub.window, updateStatusLabel), hub.window);
  const auto diag = childBounds(GetDlgItem(hub.window, diagnosticsLabel), hub.window);
  check(status.bottom > status.top && status.bottom <= diag.top, "update status fits above troubleshooting at minimum size");
  availableUpdate.reset();
  updateStatus.clear();
  updateControls(hub.window);
  showPage(SettingsPage::Features);
  check(!visibleStyle(toggle), "automatic update controls stay on the preferences page");
}
void inspectCardGrid(HWND window) {
  using namespace navigation;
  showPage(SettingsPage::Features);
  RECT bodyClient{}; GetClientRect(body, &bodyClient);
  const auto subtitle = childBounds(GetDlgItem(window, featuresSubtitle), window);
  const auto bodyBounds = childBounds(body, window);
  check(visibleStyle(GetDlgItem(window, featuresSubtitle)) && subtitle.bottom <= bodyBounds.top,
        "overview subtitle sits cleanly above card grid");
  std::vector<RECT> cardRects;
  for (const auto &card : cards) {
    if (!card.visible) continue;
    check(card.rect.right > card.rect.left && card.rect.bottom > card.rect.top, "feature card has valid bounds");
    check(card.rect.left >= 0 && card.rect.right <= bodyClient.right, "feature card fits horizontally in grid");
    for (HWND child : {card.toggle, card.description, card.action}) {
      check(visibleStyle(child), "card child control is visible");
      const auto b = childBounds(child, body);
      check(b.left >= card.rect.left && b.right <= card.rect.right &&
            b.top >= card.rect.top && b.bottom <= card.rect.bottom,
            "card child control stays inside its card");
    }
    for (const auto &other : cardRects) {
      RECT intersection{};
      check(!IntersectRect(&intersection, &card.rect, &other), "feature cards do not overlap");
    }
    cardRects.push_back(card.rect);
  }
  const int expectedCards = tabsSupported() ? 6 : 5;
  check(int(cardRects.size()) == expectedCards, "expected number of feature cards are visible");
}

void inspectSidebarTabs(HWND window) {
  using namespace navigation;
  int previousBottom = scale(88);
  const int updateTop = childBounds(GetDlgItem(window, updateButton), window).top;
  const int sidebarRight = scale(settingsSidebarWidth);
  for (const auto &item : sidebarItems) {
    HWND ctrl = GetDlgItem(window, item.id);
    if (!visibleStyle(ctrl)) continue;
    const auto b = childBounds(ctrl, window);
    check(b.left >= 0 && b.right <= sidebarRight && b.top >= previousBottom && b.bottom <= updateTop,
          "visible sidebar tab fits without overlapping neighbors or version row");
    previousBottom = b.bottom;
  }
}

void scenarios() {
  using namespace navigation;
  {
    Hub hub;
    inspectCardGrid(hub.window);
    inspectSidebarTabs(hub.window);
    // Initially double-click (59) and quick-find (75) are on; notes (77), quick (41), boxes (50), tabs (76) are off
    check(visibleStyle(GetDlgItem(hub.window, 59)) && visibleStyle(GetDlgItem(hub.window, 75)) &&
          !visibleStyle(GetDlgItem(hub.window, 77)) && !visibleStyle(GetDlgItem(hub.window, 41)) &&
          !visibleStyle(GetDlgItem(hub.window, 50)) && !visibleStyle(GetDlgItem(hub.window, 76)),
          "sidebar displays parameter tabs only for enabled features");
    check(!IsWindowEnabled(GetDlgItem(body, cardEditNotes)) &&
          !IsWindowEnabled(GetDlgItem(body, cardEditQuick)) &&
          !IsWindowEnabled(GetDlgItem(body, cardEditDesktopGroups)),
          "disabled feature cards disable their Edit buttons");

    // Enable Desktop boxes -> sidebar tab 50 appears and Edit button enables
    SendDlgItemMessageW(body, desktopGroupsToggle, BM_CLICK, 0, 0);
    check(desktop_groups::enabled() && visibleStyle(GetDlgItem(hub.window, 50)) &&
          IsWindowEnabled(GetDlgItem(body, cardEditDesktopGroups)),
          "enabling desktop boxes persists immediately and shows its sidebar tab");
    SendDlgItemMessageW(body, cardEditDesktopGroups, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::BoxAppearance, "desktop box card Edit button opens BoxAppearance");
    const auto radius = GetDlgItem(hub.window, boxRadius);
    const auto shadow = GetDlgItem(hub.window, boxShadow);
    const auto shadowOpacity = GetDlgItem(hub.window, boxShadowOpacity);
    const auto shadowBlur = GetDlgItem(hub.window, boxShadowBlur);
    const auto shadowOffset = GetDlgItem(hub.window, boxShadowOffset);
    check(visibleStyle(radius) && visibleStyle(shadowOpacity) && visibleStyle(shadowBlur) && visibleStyle(shadowOffset) &&
          visibleStyle(GetDlgItem(hub.window, boxPreview)) && visibleStyle(desktopGroupsManager) &&
          IsWindowEnabled(desktopGroupsManager),
          "box appearance page shows its controls and enabled manager button");
    const auto savedAppearance = desktop_groups::configuredAppearance();
    check(!IsWindowEnabled(shadowOpacity) && !IsWindowEnabled(shadowBlur) && !IsWindowEnabled(shadowOffset),
          "shadow sliders start disabled when shadow is off");
    SendMessageW(shadow, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(boxShadow, BN_CLICKED), reinterpret_cast<LPARAM>(shadow));
    check(desktop_groups::configuredAppearance().shadow, "toggling shadow persists immediately");
    SendMessageW(radius, TBM_SETPOS, TRUE, 4);
    SendMessageW(shadowOpacity, TBM_SETPOS, TRUE, 78);
    SendMessageW(shadowBlur, TBM_SETPOS, TRUE, 24);
    SendMessageW(shadowOffset, TBM_SETPOS, TRUE, 16);
    SendMessageW(hub.window, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(shadowOffset));
    check(IsWindowEnabled(shadowOffset) && boxStyle.radius == 4 &&
          boxStyle.shadowOpacity == 78 && boxStyle.shadowBlur == 24 && boxStyle.shadowOffset == 16 &&
          desktop_groups::configuredAppearance().radius == savedAppearance.radius,
          "slider drag previews values before release");
    SendMessageW(hub.window, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(shadowOffset));
    const auto persisted = desktop_groups::configuredAppearance();
    check(persisted.radius == 4 && persisted.shadowOpacity == 78 && persisted.shadowBlur == 24 && persisted.shadowOffset == 16,
          "releasing slider commits box appearance immediately");
    SendMessageW(shadow, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(boxShadow, BN_CLICKED), reinterpret_cast<LPARAM>(shadow));
    check(!IsWindowEnabled(shadowOpacity) && SendMessageW(shadowOpacity, TBM_GETPOS, 0, 0) == 78 &&
          !desktop_groups::configuredAppearance().shadow,
          "turning off shadow persists immediately, disables sliders, and preserves values");
    SendMessageW(radius, TBM_SETPOS, TRUE, savedAppearance.radius);
    SendMessageW(shadowOpacity, TBM_SETPOS, TRUE, savedAppearance.shadowOpacity);
    SendMessageW(shadowBlur, TBM_SETPOS, TRUE, savedAppearance.shadowBlur);
    SendMessageW(shadowOffset, TBM_SETPOS, TRUE, savedAppearance.shadowOffset);
    SendMessageW(hub.window, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(radius));
    SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
    SendDlgItemMessageW(body, desktopGroupsToggle, BM_CLICK, 0, 0);
    check(!desktop_groups::enabled() && !visibleStyle(GetDlgItem(hub.window, 50)) &&
          !IsWindowEnabled(GetDlgItem(body, cardEditDesktopGroups)),
          "disabling desktop boxes hides its sidebar tab and disables Edit");

    // Preferences (General) page
    SendDlgItemMessageW(hub.window, 48, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::Preferences && visibleStyle(GetDlgItem(hub.window, languageChoice)) &&
          visibleStyle(themeChoice) && visibleStyle(GetDlgItem(hub.window, 13)),
          "language, theme, and diagnostics controls are visible on General");
    SendMessageW(themeChoice, CB_SETCURSEL, DWORD(ui::ThemeMode::Dark), 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(17, CBN_SELCHANGE), reinterpret_cast<LPARAM>(themeChoice));
    check(appearance->mode == ui::ThemeMode::Dark && ui::configuredTheme() == ui::ThemeMode::Dark,
          "theme selection persists immediately");
    SendMessageW(themeChoice, CB_SETCURSEL, DWORD(ui::ThemeMode::System), 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(17, CBN_SELCHANGE), reinterpret_cast<LPARAM>(themeChoice));
    check(ui::configuredTheme() == ui::ThemeMode::System, "theme resets cleanly");

    // Context menu page
    SendDlgItemMessageW(hub.window, 47, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::ContextMenus && visibleStyle(contextPage) &&
          GetParent(contextPage) == hub.window && !visibleStyle(body) && !visibleStyle(managerPage),
          "context manager is an embedded settings page");
    check(!visibleStyle(GetDlgItem(hub.window, languageChoice)) && !visibleStyle(themeChoice),
          "preferences are hidden on other pages");

    // Explorer entries page
    SendDlgItemMessageW(hub.window, 49, BM_CLICK, 0, 0);
    check(visibleStyle(entriesPage) && !visibleStyle(contextPage), "folder entry manager is embedded");
    auto location = explorer_entries_page::locationControl(entriesPage);
    check(SendMessageW(location, CB_GETCOUNT, 0, 0) == 3 &&
          windowText(GetDlgItem(hub.window, 30)) == L"文件夹入口管理", "folder manager exposes three locations");
    RECT pageBounds{};
    GetClientRect(entriesPage, &pageBounds);
    const auto locationBounds = childBounds(location, entriesPage);
    check(locationBounds.left >= 0 && locationBounds.right <= pageBounds.right &&
          locationBounds.top >= 0 && locationBounds.bottom <= pageBounds.bottom,
          "folder location picker fits the page");
    const auto entries = FindWindowExW(entriesPage, nullptr, WC_LISTVIEWW, nullptr);
    check(entries && (GetWindowLongPtrW(entries, GWL_STYLE) & (LVS_OWNERDRAWFIXED | LVS_NOCOLUMNHEADER)) ==
          (LVS_OWNERDRAWFIXED | LVS_NOCOLUMNHEADER) && Header_GetItemCount(ListView_GetHeader(entries)) == 1,
          "folder entries use a compact owner-drawn list without metadata columns");
    check(ListView_IsGroupViewEnabled(entries) && ListView_GetGroupCount(entries) == 3,
          "This PC uses the three Explorer groups");
    SendMessageW(explorer_entries_page::viewControl(entriesPage, true), BM_CLICK, 0, 0);
    check(ListView_GetView(entries) == LV_VIEW_ICON && ListView_GetGroupCount(entries) == 3,
          "icon view retains the This PC groups");
    check(!(GetWindowLongPtrW(entries, GWL_STYLE) & LVS_OWNERDRAWFIXED),
          "icon view uses native layout without the report row style");
    for (int i = 0; i < ListView_GetItemCount(entries); ++i) {
      LVITEMW item{};
      item.mask = LVIF_GROUPID;
      item.iItem = i;
      RECT group{}, icon{};
      if (ListView_GetItem(entries, &item) && ListView_GetGroupRect(entries, item.iGroupId, LVGGR_GROUP, &group) &&
          ListView_GetItemRect(entries, i, &icon, LVIR_ICON))
        check(icon.top >= group.top && icon.bottom <= group.bottom, "icon is positioned inside its own group");
    }
    SendMessageW(explorer_entries_page::viewControl(entriesPage, false), BM_CLICK, 0, 0);
    check(ListView_GetView(entries) == LV_VIEW_DETAILS && (GetWindowLongPtrW(entries, GWL_STYLE) & LVS_OWNERDRAWFIXED),
          "switching back restores the owner-drawn list rows");

    // Back to Features cards
    SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
    check(visibleStyle(body) && !visibleStyle(managerPage), "overview cards visible on Features");
    check(!notesEnabled(), "isolated notes default remains off");
    check(!preferNewTabEnabled(), "new-tab preference defaults off");
    check(!desktop_groups::enabled() && SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
          "desktop groups are off by default");

    // Card hover & body click tests
    {
      const auto &card1 = cards[1];
      const LPARAM center = MAKELPARAM((card1.rect.left + card1.rect.right) / 2,
                                       (card1.rect.top + card1.rect.bottom) / 2);
      SendMessageW(body, WM_MOUSEMOVE, 0, center);
      check(hoveredCard == 1, "moving mouse over a card sets hoveredCard");
      SendMessageW(body, WM_LBUTTONUP, 0, center);
      check(activePage == SettingsPage::DoubleClick, "clicking an enabled card body opens its page");
      SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
      check(hoveredCard == -1, "switching pages clears hoveredCard");
    }

    // Notes card toggle & dedicated Notes tab (77)
    SendDlgItemMessageW(body, 11, BM_CLICK, 0, 0);
    check(notesEnabled() && visibleStyle(GetDlgItem(hub.window, 77)) &&
          IsWindowEnabled(GetDlgItem(body, cardEditNotes)),
          "notes card toggle persists immediately and shows its sidebar tab");
    SendDlgItemMessageW(body, cardEditNotes, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::Notes && windowText(GetDlgItem(hub.window, 30)) == L"文件夹备注" &&
          visibleStyle(GetDlgItem(hub.window, notesCmdEdit)) &&
          visibleStyle(GetDlgItem(hub.window, notesCmdClear)) &&
          visibleStyle(GetDlgItem(hub.window, notesCmdRepair)),
          "notes Edit button opens dedicated Folder Notes page");
    SendDlgItemMessageW(hub.window, notesCmdClear, BM_CLICK, 0, 0);
    check(noteCommandHidden(1), "unchecking a note command hides it immediately");
    SendDlgItemMessageW(hub.window, notesCmdClear, BM_CLICK, 0, 0);
    check(!noteCommandHidden(1), "rechecking a note command restores it immediately");
    SendDlgItemMessageW(hub.window, notesOpenContextMenu, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::ContextMenus, "notes page shortcut opens ContextMenus");
    SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
    SendDlgItemMessageW(body, 11, BM_CLICK, 0, 0);
    check(!notesEnabled() && !visibleStyle(GetDlgItem(hub.window, 77)) &&
          !IsWindowEnabled(GetDlgItem(body, cardEditNotes)),
          "notes card toggle off hides its sidebar tab and disables Edit");

    // Double-click page via sidebar or card Edit
    SendDlgItemMessageW(body, cardEditDoubleClick, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::DoubleClick && visibleStyle(choice), "double-click card routes to DoubleClick page");
    SendMessageW(choice, CB_SETCURSEL, 1, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(10, CBN_SELCHANGE), reinterpret_cast<LPARAM>(choice));
    check(preferredAction() == Action::Back && configuredAction() == Action::Back,
          "double-click action persists immediately");
    SendMessageW(choice, CB_SETCURSEL, 0, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(10, CBN_SELCHANGE), reinterpret_cast<LPARAM>(choice));
    SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
    SendDlgItemMessageW(body, 12, BM_CLICK, 0, 0);
    check(configuredAction() == Action::Off && !visibleStyle(GetDlgItem(hub.window, 59)) &&
          !IsWindowEnabled(GetDlgItem(body, cardEditDoubleClick)),
          "disabling double-click hides its sidebar tab");
    SendDlgItemMessageW(body, 12, BM_CLICK, 0, 0);
    check(configuredAction() == Action::Parent && visibleStyle(GetDlgItem(hub.window, 59)),
          "re-enabling double-click restores its sidebar tab");

    // QuickFolders page (middle-click)
    SendDlgItemMessageW(body, 14, BM_CLICK, 0, 0);
    check(quickEnabled() && visibleStyle(GetDlgItem(hub.window, 41)) && IsWindowEnabled(GetDlgItem(body, cardEditQuick)),
          "enabling quick folders shows its sidebar tab and enables Edit");
    SendDlgItemMessageW(hub.window, 41, BM_CLICK, 0, 0);
    check(activePage == SettingsPage::QuickFolders && visibleStyle(managerPage) && visibleStyle(dialogCheck) &&
          IsWindowEnabled(dialogCheck),
          "clicking sidebar tab 41 opens embedded manager and enabled dialog checkbox");
    SendDlgItemMessageW(hub.window, 15, BM_CLICK, 0, 0);
    check(!dialogQuickEnabled(), "dialog checkbox persists immediately");
    SendDlgItemMessageW(hub.window, 15, BM_CLICK, 0, 0);

    // Tabs page (Win10 only)
    if (tabsSupported()) {
      SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
      check(!visibleStyle(GetDlgItem(hub.window, 76)) && !IsWindowEnabled(GetDlgItem(body, cardEditTabs)),
            "tabs sidebar tab starts hidden while off");
      SendDlgItemMessageW(body, 18, BM_CLICK, 0, 0);
      check(tabsEnabled() && visibleStyle(GetDlgItem(hub.window, 76)) && IsWindowEnabled(GetDlgItem(body, cardEditTabs)),
            "enabling tabs shows its sidebar tab immediately");
      SendDlgItemMessageW(hub.window, 76, BM_CLICK, 0, 0);
      check(activePage == SettingsPage::Tabs && visibleStyle(preferNewTabCheck) && IsWindowEnabled(preferNewTabCheck),
            "tabs sidebar tab opens Tabs page with new-tab checkbox enabled");
      SendDlgItemMessageW(hub.window, 21, BM_CLICK, 0, 0);
      check(preferNewTabEnabled(), "prefer-new-tab checkbox persists immediately");
      SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
      SendDlgItemMessageW(body, 18, BM_CLICK, 0, 0);
      check(!tabsEnabled() && !visibleStyle(GetDlgItem(hub.window, 76)) &&
            SendMessageW(preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
            "disabling tabs hides its sidebar tab while retaining subordinate choice");
      SendDlgItemMessageW(body, 18, BM_CLICK, 0, 0);
      check(tabsEnabled() && visibleStyle(GetDlgItem(hub.window, 76)) && preferNewTabEnabled(),
            "reenabling tabs restores sidebar tab and checked subordinate option");
    } else {
      check(!tabsCheck && !preferNewTabCheck && !visibleStyle(GetDlgItem(hub.window, 76)),
            "unsupported systems do not show Win10-only options");
      SendDlgItemMessageW(hub.window, 40, BM_CLICK, 0, 0);
    }

    // Turn on notes & desktop boxes as well so ALL sidebar items are visible simultaneously for DPI/size layout check
    SendDlgItemMessageW(body, 11, BM_CLICK, 0, 0);
    SendDlgItemMessageW(body, desktopGroupsToggle, BM_CLICK, 0, 0);
    for (UINT scaleDpi : {96u, 144u, 192u}) {
      RECT target{0, 0, MulDiv(980, scaleDpi, 96), MulDiv(660, scaleDpi, 96)};
      SendMessageW(hub.window, WM_DPICHANGED, MAKEWPARAM(scaleDpi, scaleDpi), reinterpret_cast<LPARAM>(&target));
      inspectVersion(hub.window);
      inspectCardGrid(hub.window);
      inspectSidebarTabs(hub.window);
      MoveWindow(hub.window, 0, 0, MulDiv(900, scaleDpi, 96), MulDiv(500, scaleDpi, 96), FALSE);
      inspectVersion(hub.window);
      inspectCardGrid(hub.window);
      inspectSidebarTabs(hub.window);
      showPage(SettingsPage::BoxAppearance);
      SendMessageW(hub.window, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
      check(!visibleStyle(settingsScroll.window()), "wheel over parameter page does not show feature scrollbar");
      showPage(SettingsPage::Features);
    }
  }

  // Verify persistence across close/reopen
  applySettings(false, Action::Parent, Action::Parent, false, true, ui::ThemeMode::System, false, false, true,
                DesktopGroupsSettings{true, false, 8, 55, true, 72, 18, 9});
  const auto savedShadow = desktop_groups::configuredAppearance();
  check(desktop_groups::enabled() && !savedShadow.glass && savedShadow.shadow &&
        savedShadow.shadowOpacity == 72 && savedShadow.shadowBlur == 18 && savedShadow.shadowOffset == 9,
        "settings save persists the desktop groups and appearance choices");
  {
    Hub reopened;
    check(desktop_groups::enabled() && SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED &&
          visibleStyle(GetDlgItem(reopened.window, 50)) &&
          SendDlgItemMessageW(reopened.window, boxGlass, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
          IsWindowEnabled(desktopGroupsManager), "reopening settings restores desktop groups, sidebar tab, and appearance choices");
  }
  applySettings(false, Action::Parent, Action::Parent, false, true, ui::ThemeMode::System, false, false, true,
                DesktopGroupsSettings{false, true});
  check(!desktop_groups::enabled() && desktop_groups::configuredAppearance().glass,
        "settings save can turn off desktop groups without losing the appearance choice");
}
void languages() {
  lang::select(lang::Choice::English);
  {
    Hub hub;
    check(windowText(GetDlgItem(hub.window, navigation::languageExport)) == L"Export language template" &&
          windowText(GetDlgItem(hub.window, 30)) == L"Features" &&
          windowText(GetDlgItem(navigation::body, navigation::cardEditNotes)) == L"Edit →" &&
          windowText(GetDlgItem(navigation::body, navigation::cardEditDesktopGroups)) == L"Edit →" &&
          windowText(GetDlgItem(hub.window, 77)) == L"Notes" &&
          windowText(GetDlgItem(hub.window, 59)) == L"Double-click" &&
          windowText(GetDlgItem(hub.window, 47)) == L"Context menu" &&
          windowText(GetDlgItem(hub.window, 48)) == L"General" &&
          windowText(navigation::desktopGroupsCheck) == L"Desktop boxes" &&
          windowText(navigation::desktopGroupsManager) == L"Manage desktop boxes", "English settings labels load");
    if (navigation::tabsSupported())
      check(windowText(GetDlgItem(hub.window, 76)) == L"Explorer tabs", "English tabs sidebar label loads");
  }
  wchar_t directory[MAX_PATH]{}, file[MAX_PATH]{};
  check(GetTempPathW(ARRAYSIZE(directory), directory) && GetTempFileNameW(directory, L"fnl", 0, file),
        "create temporary XML");
  struct Temp { const wchar_t *path; ~Temp() { DeleteFileW(path); } } cleanup{file};
  lang::exportFile(file);
  std::string xml;
  {
    std::ifstream input(std::filesystem::path(file), std::ios::binary);
    xml.assign(std::istreambuf_iterator<char>(input), {});
  }
  auto label = xml.find(">Export language template</message>");
  check(label != xml.npos, "template contains an editable UI message");
  xml.replace(label + 1, std::string_view("Export language template").size(), "Translator &amp; tools");
  std::ofstream(std::filesystem::path(file), std::ios::binary) << xml;
  lang::importFile(file);
  check(lang::settings().choice == lang::Choice::Custom &&
        std::wstring_view(lang::text(L"导出语言模板")) == L"Translator & tools", "import activates an escaped translation");
  std::ofstream(std::filesystem::path(file), std::ios::binary) << "<!DOCTYPE language><language/>";
  bool rejected = false;
  try { lang::importFile(file); } catch (const std::exception &) { rejected = true; }
  check(rejected && std::wstring_view(lang::text(L"导出语言模板")) == L"Translator & tools",
        "invalid XML leaves the active pack intact");
  lang::select(lang::Choice::Chinese);
  check(std::wstring_view(lang::text(L"导出语言模板")) == L"导出语言模板", "built-in Chinese can be restored");
}
void desktopFenceMenu() {
  constexpr auto verb = L"Software\\Classes\\DesktopBackground\\shell\\FolderNote.DesktopGroups.Create";
  constexpr auto sibling = L"Software\\Classes\\DesktopBackground\\shell\\Test.Fixture";
  const std::wstring exe = L"C:\\Program Files\\FolderNote\\FolderNote.exe";
  const wchar_t fixture[] = L"Fixture";
  auto read = [](const wchar_t *subkey, const wchar_t *name) {
    wchar_t value[MAX_PATH]{};
    DWORD bytes = sizeof(value);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, subkey, name, RRF_RT_REG_SZ, nullptr, value, &bytes);
    return status == ERROR_SUCCESS ? std::wstring(value) : std::wstring{};
  };
  check(desktop_groups::configureDesktopMenu({}), "removing a missing desktop fence verb succeeds");
  check(RegSetKeyValueW(HKEY_CURRENT_USER, sibling, nullptr, REG_SZ, fixture, sizeof(fixture)) == ERROR_SUCCESS,
        "register a separate desktop verb");
  check(desktop_groups::configureDesktopMenu(exe) && read(verb, L"MUIVerb") == L"新建盒子" &&
        read((std::wstring(verb) + L"\\command").c_str(), nullptr) == L"\"" + exe + L"\" --desktop-groups-create",
        "desktop fence verb invokes only FolderNote with the create command");
  lang::select(lang::Choice::English);
  check(desktop_groups::configureDesktopMenu(exe) && read(verb, L"MUIVerb") == L"New box", "desktop verb follows the language");
  lang::select(lang::Choice::Chinese);
  check(desktop_groups::configureDesktopMenu({}) && read(verb, L"MUIVerb").empty() && read(sibling, nullptr) == fixture,
        "desktop fence verb removal preserves sibling commands");
}
void contextInventory() {
  constexpr auto verb = L"Software\\Classes\\Directory\\shell\\ExampleFixture";
  constexpr auto command = L"Software\\Classes\\Directory\\shell\\ExampleFixture\\command";
  const wchar_t name[] = L"Fixture command";
  const wchar_t commandLine[] = L"\"C:\\Windows\\System32\\notepad.exe\" \"%1\"";
  check(RegSetKeyValueW(HKEY_CURRENT_USER, verb, nullptr, REG_SZ, name, sizeof(name)) == ERROR_SUCCESS &&
        RegSetKeyValueW(HKEY_CURRENT_USER, command, nullptr, REG_SZ, commandLine, sizeof(commandLine)) == ERROR_SUCCESS,
        "register an isolated folder command");
  auto read = [&] {
    auto entries = context::inventory(context::Scene::Folder);
    auto item = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.registryKey == verb; });
    check(item != entries.end(), "inventory finds the isolated folder command");
    return *item;
  };
  auto item = read();
  check(item.canHide && item.canEdit && !item.hidden, "a user static command supports reversible changes");
  context::setHidden(item, true);
  item = read();
  check(item.hidden && item.managed, "hiding an existing command saves its restore state");
  context::setAppearance(item, L"Renamed fixture", L"");
  check(read().label == L"Renamed fixture", "edited command label is visible");
  check(context::restoreAll() == 0, "restore all resolves the managed record");
  item = read();
  check(!item.hidden && item.label == name, "restoring recovers label and visibility");
  constexpr auto cascade = L"Software\\Classes\\Directory\\shell\\ExampleCascade";
  const wchar_t subCommands[] = L"Fixture.First;Fixture.Second";
  check(RegSetKeyValueW(HKEY_CURRENT_USER, cascade, L"SubCommands", REG_SZ, subCommands, sizeof(subCommands)) == ERROR_SUCCESS,
        "register an isolated static cascade without a default command");
  auto readCascade = [&] {
    auto entries = context::inventory(context::Scene::Folder);
    auto result = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.registryKey == cascade; });
    check(result != entries.end(), "inventory finds the isolated cascade");
    return *result;
  };
  auto group = readCascade();
  check(group.canHide && group.command.empty() && group.cascadeChildren == std::vector<std::wstring>{L"Fixture.First", L"Fixture.Second"},
        "a static cascade is recognized from its child list without a command");
  context::setHidden(group, true);
  group = readCascade();
  check(group.hidden && group.managed, "static cascade visibility is reversible");
  context::restore(group);
  check(!readCascade().hidden, "restoring a cascade removes only its visibility marker");
  constexpr auto extended = L"Software\\Classes\\Directory\\shell\\ExampleExtendedCascade";
  constexpr auto extendedChild = L"Software\\Classes\\Directory\\shell\\ExampleExtendedCascade\\shell\\ExampleAction\\command";
  const wchar_t extendedTarget[] = L"Directory\\shell\\ExampleExtendedCascade";
  check(RegSetKeyValueW(HKEY_CURRENT_USER, extended, L"ExtendedSubCommandsKey", REG_SZ,
                        extendedTarget, sizeof(extendedTarget)) == ERROR_SUCCESS &&
        RegSetKeyValueW(HKEY_CURRENT_USER, extendedChild, nullptr, REG_SZ, commandLine, sizeof(commandLine)) == ERROR_SUCCESS,
        "register an isolated cascade with an inline subcommand");
  auto readExtended = [&] {
    auto entries = context::inventory(context::Scene::Folder);
    auto result = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.registryKey == extended; });
    check(result != entries.end(), "inventory finds the extended cascade");
    return *result;
  };
  auto extendedGroup = readExtended();
  check(extendedGroup.canHide && extendedGroup.children.size() == 1 && extendedGroup.children[0].label == L"ExampleAction",
        "inline cascade is manageable and its leaf command is discoverable");
  context::setHidden(extendedGroup, true);
  extendedGroup = readExtended();
  check(extendedGroup.hidden && extendedGroup.children[0].hidden && extendedGroup.managed,
        "hiding the cascade marks its child as hidden and records ownership");
  context::setHidden(extendedGroup, false);
  check(!readExtended().hidden, "unhiding removes the app-owned visibility markers");
  const DWORD externalVelocity = 0x639bc8;
  check(RegSetKeyValueW(HKEY_CURRENT_USER, extended, L"HideBasedOnVelocityId", REG_DWORD,
                        &externalVelocity, sizeof(externalVelocity)) == ERROR_SUCCESS,
        "register a visibility marker from an external menu manager");
  extendedGroup = readExtended();
  check(extendedGroup.hidden && !extendedGroup.managed && !extendedGroup.canHide,
        "externally disabled cascades remain untouched");
  constexpr auto activeHandler = L"Software\\Classes\\Folder\\shellex\\ContextMenuHandlers\\ExampleHandler";
  constexpr auto disabledHandler = L"Software\\Classes\\Directory\\shellex\\-ContextMenuHandlers\\ExampleHandler";
  const wchar_t handlerId[] = L"{AFF13484-F8C8-4E3A-A537-EC993BB16604}";
  check(RegSetKeyValueW(HKEY_CURRENT_USER, activeHandler, nullptr, REG_SZ, handlerId, sizeof(handlerId)) == ERROR_SUCCESS &&
        RegSetKeyValueW(HKEY_CURRENT_USER, disabledHandler, nullptr, REG_SZ, handlerId, sizeof(handlerId)) == ERROR_SUCCESS,
        "register isolated active and disabled handlers sharing a CLSID");
  const auto registrations = context::inventory(context::Scene::Folder);
  auto enabled = std::find_if(registrations.begin(), registrations.end(),
      [&](const auto &entry) { return entry.registryKey == activeHandler; });
  auto disabled = std::find_if(registrations.begin(), registrations.end(),
      [&](const auto &entry) { return entry.registryKey == disabledHandler; });
  check(enabled != registrations.end() && !enabled->hidden, "active handler remains visible in the same scene");
  check(disabled != registrations.end() && disabled->hidden && !disabled->canHide && !disabled->managed &&
        !disabled->shellBlock && disabled->source == std::wstring(L"HKCU\\") + disabledHandler,
        "externally disabled handler is discovered at its physical source and remains read-only");
  context::setHidden(item, true);
  const wchar_t replacement[] = L"External change";
  check(RegSetKeyValueW(HKEY_CURRENT_USER, verb, L"LegacyDisable", REG_SZ, replacement, sizeof(replacement)) == ERROR_SUCCESS,
        "simulate a third-party update to a managed value");
  check(context::restoreAll() == 1 && read().hidden, "restore retains an externally modified value");
}
void contextCommands() {
  check(!context::commandsEnabled() && context::loadCommands().empty(), "context command feature is off initially");
  context::Command first;
  first.name = L"First";
  first.mode = context::Mode::Url;
  first.program = L"https://example.com/";
  first.scene = context::Scene::Folder;
  auto second = first;
  second.name = L"Second";
  const auto exe = fn::modulePath();
  context::saveCommands(true, {second, first}, exe);
  auto loaded = context::loadCommands();
  check(context::commandsEnabled() && loaded.size() == 2 &&
        loaded[0].name == L"Second" && loaded[1].name == L"First", "custom command ordering persists");
  constexpr auto registration = L"Software\\Classes\\Directory\\shell\\FolderNote.ContextCommands\\ExtendedSubCommandsKey\\shell\\1000";
  wchar_t name[64]{};
  DWORD bytes = sizeof(name);
  check(RegGetValueW(HKEY_CURRENT_USER, registration, L"MUIVerb", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_SUCCESS &&
        std::wstring_view(name) == L"Second", "the first Shell submenu entry matches the saved order");
  wchar_t invocation[2048]{};
  bytes = sizeof(invocation);
  check(RegGetValueW(HKEY_CURRENT_USER, (std::wstring(registration) + L"\\command").c_str(), nullptr,
        RRF_RT_REG_SZ, nullptr, invocation, &bytes) == ERROR_SUCCESS, "Shell menu invokes a versioned command token");
  int argc = 0;
  auto argv = CommandLineToArgvW(invocation, &argc);
  const auto token = argv && argc == 4 ? std::wstring(argv[2]) : std::wstring{};
  LocalFree(argv);
  check(!token.empty(), "the command token survives Windows argument parsing");
  context::saveCommands(true, {first, second}, exe);
  bool rejected = false;
  try { context::runCommand(token, L"C:\\|"); } catch (const fn::Error &) { rejected = true; }
  check(rejected, "a stale command token cannot invoke a different command after reordering");
  context::saveCommands(false, loaded, exe);
  check(!context::commandsEnabled() && context::loadCommands().size() == 2,
        "disabling retains the ordered command configuration");
  bytes = sizeof(name);
  check(RegGetValueW(HKEY_CURRENT_USER, registration, L"MUIVerb", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_FILE_NOT_FOUND,
        "disabling removes Shell menu entries");
}
void updateConfirmationDialog() {
  using namespace navigation;
  Hub hub;
  updater::Release release{
      L"9.9.9",
      L"https://github.com/Wonvy/FolderNote/releases/download/v9.9.9/FolderNote-Setup-x64.exe",
      std::wstring(64, L'a'),
      false,
      true,
      L"修复与优化\r\n• 修复纯色桌面下盒子显示旧壁纸\r\n• 新增更新内容确认弹窗"};
  const auto prompt = formatUpdatePrompt(release);
  check(prompt.find(L"9.9.9") != std::wstring::npos &&
        prompt.find(L"修复纯色桌面下盒子显示旧壁纸") != std::wstring::npos,
        "formatted update prompt includes target version and release notes");

  UpdateConfirmDialogState acceptState;
  HWND acceptDlg = createUpdateConfirmDialog(hub.window, release, acceptState);
  check(acceptDlg != nullptr, "create themed update confirmation dialog");
  check(windowText(GetDlgItem(acceptDlg, updateDialogHeading)).find(L"9.9.9") != std::wstring::npos,
        "update confirmation dialog heading displays new version");
  check(windowText(GetDlgItem(acceptDlg, updateDialogNotesEdit)).find(L"修复纯色桌面下盒子显示旧壁纸") != std::wstring::npos,
        "update confirmation dialog displays release notes inside scrollable edit box");
  SendMessageW(acceptDlg, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
  check(acceptState.closed && acceptState.confirmed && !IsWindow(acceptDlg),
        "clicking confirm button accepts the update and closes dialog");

  updater::Release emptyNotesRelease = release;
  emptyNotesRelease.notes.clear();
  UpdateConfirmDialogState cancelState;
  HWND cancelDlg = createUpdateConfirmDialog(hub.window, emptyNotesRelease, cancelState);
  check(cancelDlg != nullptr, "create update confirmation dialog for release without body");
  check(windowText(GetDlgItem(cancelDlg, updateDialogNotesEdit)) == L"暂无详细更新说明。",
        "empty release notes fall back to default placeholder text");
  SendMessageW(cancelDlg, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
  check(cancelState.closed && !cancelState.confirmed && !IsWindow(cancelDlg),
        "clicking cancel button declines the update and closes dialog");
}
}
int main(int argc, char **argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try {
    Registry registry;
    if (argc == 2 && std::string_view(argv[1]) == "--preview-search") {
      Hub preview;
      navigation::showPage(navigation::SettingsPage::QuickFind);
      SetWindowTextW(preview.window, L"FolderNote — 快捷搜索预览（隔离设置）");
      ShowWindow(preview.window, SW_SHOWNORMAL);
      MSG message{};
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(preview.window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
      }
    } else {
      quickSearchSettings(); automaticUpdates(); updateConfirmationDialog(); scenarios(); languages(); desktopFenceMenu(); contextInventory(); contextCommands();
      std::cout << "PASS " << checks << " settings, language and context menu assertions\n";
    }
  }
  catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << "\n"; result = 1; }
  CoUninitialize(); return result;
}
