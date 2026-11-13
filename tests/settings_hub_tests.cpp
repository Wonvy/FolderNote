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
  auto secondTab = childBounds(GetDlgItem(window, 41), window);
  check(bounds.top >= secondTab.bottom, "version does not overlap navigation tabs");
  auto page = childBounds(navigation::managerPage ? navigation::managerPage : navigation::body, window);
  check(bounds.right <= page.left, "version stays clear of the right-hand page");
  HDC dc = GetDC(version); auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(version, WM_GETFONT, 0, 0)));
  SIZE extent{}; auto text = windowText(version); GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
  SelectObject(dc, oldFont); ReleaseDC(version, dc);
  check(extent.cx <= bounds.right - bounds.left && extent.cy <= bounds.bottom - bounds.top,
        "version text is not clipped at current scale");
}
void inspectNewTabOption() {
  if (!navigation::tabsSupported()) return;
  navigation::showPage(navigation::SettingsPage::Features);
  navigation::scrollPosition = navigation::contentHeight();
  navigation::layoutBody();
  RECT client{}; GetClientRect(navigation::body, &client);
  auto bounds = childBounds(navigation::preferNewTabCheck, navigation::body);
  check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
        "new-tab option is reachable within the feature scroller at every scale");
  auto description = childBounds(navigation::placements.back().window, navigation::body);
  check(description.top >= bounds.bottom && description.bottom <= client.bottom,
        "new-tab description stays below its control and inside scrollable content");
  HDC dc = GetDC(navigation::preferNewTabCheck);
  auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(navigation::preferNewTabCheck, WM_GETFONT, 0, 0)));
  SIZE extent{}; auto text = windowText(navigation::preferNewTabCheck);
  GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
  SelectObject(dc, oldFont); ReleaseDC(navigation::preferNewTabCheck, dc);
  check(extent.cx + navigation::scale(28) <= bounds.right - bounds.left && extent.cy <= bounds.bottom - bounds.top,
        "new-tab checkbox text fits at current scale");
  navigation::showPage(navigation::SettingsPage::QuickFolders);
}
void scenarios() {
  Hub hub;
  navigation::showPage(navigation::SettingsPage::Preferences);
  check(visibleStyle(GetDlgItem(hub.window, navigation::languageChoice)) && visibleStyle(navigation::themeChoice),
        "language and theme controls are visible on Preferences");
  navigation::showPage(navigation::SettingsPage::ContextMenus);
  check(!visibleStyle(GetDlgItem(hub.window, navigation::languageChoice)) && !visibleStyle(navigation::themeChoice),
        "preferences are hidden on other pages");
  check(visibleStyle(navigation::contextPage) && GetParent(navigation::contextPage) == hub.window &&
        !visibleStyle(navigation::body) && !visibleStyle(navigation::managerPage),
        "context manager is an embedded settings page");
  SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(49, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hub.window, 49)));
  check(visibleStyle(navigation::entriesPage) && !visibleStyle(navigation::contextPage) &&
        !visibleStyle(GetDlgItem(hub.window, IDOK)), "folder entry manager is embedded and applies changes directly");
  auto location = explorer_entries_page::locationControl(navigation::entriesPage);
  check(SendMessageW(location, CB_GETCOUNT, 0, 0) == 3 &&
        windowText(GetDlgItem(hub.window, 30)) == L"文件夹入口管理", "folder manager exposes three locations");
  RECT pageBounds{};
  GetClientRect(navigation::entriesPage, &pageBounds);
  const auto locationBounds = childBounds(location, navigation::entriesPage);
  check(locationBounds.left >= 0 && locationBounds.right <= pageBounds.right &&
        locationBounds.top >= 0 && locationBounds.bottom <= pageBounds.bottom,
        "folder location picker fits the page");
  const auto entries = FindWindowExW(navigation::entriesPage, nullptr, WC_LISTVIEWW, nullptr);
  check(entries && (GetWindowLongPtrW(entries, GWL_STYLE) & (LVS_OWNERDRAWFIXED | LVS_NOCOLUMNHEADER)) ==
        (LVS_OWNERDRAWFIXED | LVS_NOCOLUMNHEADER) && Header_GetItemCount(ListView_GetHeader(entries)) == 1,
        "folder entries use a compact owner-drawn list without metadata columns");
  check(ListView_IsGroupViewEnabled(entries) && ListView_GetGroupCount(entries) == 3,
        "This PC uses the three Explorer groups");
  SendMessageW(explorer_entries_page::viewControl(navigation::entriesPage, true), BM_CLICK, 0, 0);
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
  SendMessageW(explorer_entries_page::viewControl(navigation::entriesPage, false), BM_CLICK, 0, 0);
  check(ListView_GetView(entries) == LV_VIEW_DETAILS && (GetWindowLongPtrW(entries, GWL_STYLE) & LVS_OWNERDRAWFIXED),
        "switching back restores the owner-drawn list rows");
  navigation::showPage(navigation::SettingsPage::Features);
  check(visibleStyle(navigation::body) && !visibleStyle(navigation::managerPage), "only feature page initially visible");
  check(!navigation::notesEnabled(), "isolated notes default remains off");
  check(!GetDlgItem(navigation::body, 19) && !GetDlgItem(navigation::body, 20),
        "copy and move no longer require separate settings controls");
  check(!navigation::preferNewTabEnabled(), "new-tab preference defaults off");
  if (navigation::tabsSupported()) {
    check(GetDlgItem(navigation::body, 21) == navigation::preferNewTabCheck &&
          !IsWindowEnabled(navigation::preferNewTabCheck), "new-tab option starts disabled when tabs are off");
    SendMessageW(navigation::tabsCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(18, BN_CLICKED), 0);
    check(IsWindowEnabled(navigation::preferNewTabCheck), "enabling tabs enables new-tab preference");
    SendMessageW(navigation::preferNewTabCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(navigation::tabsCheck, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(18, BN_CLICKED), 0);
    check(!IsWindowEnabled(navigation::preferNewTabCheck) &&
          SendMessageW(navigation::preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
          "disabling tabs retains the subordinate draft choice");
    SendMessageW(navigation::tabsCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(18, BN_CLICKED), 0);
    check(IsWindowEnabled(navigation::preferNewTabCheck) &&
          SendMessageW(navigation::preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
          "reenabling tabs restores the checked subordinate option");
  } else {
    check(!navigation::tabsCheck && !navigation::preferNewTabCheck, "unsupported systems do not show Win10-only options");
  }
  SendMessageW(navigation::quickCheck, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(14, BN_CLICKED), 0);
  check(IsWindowEnabled(navigation::dialogCheck), "enabling quick folders still enables dialog support");
  SendMessageW(navigation::notesCheck, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(navigation::themeChoice, CB_SETCURSEL, DWORD(ui::ThemeMode::Dark), 0);
  SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(17, CBN_SELCHANGE), reinterpret_cast<LPARAM>(navigation::themeChoice));
  check(navigation::appearance->mode == ui::ThemeMode::Dark && ui::configuredTheme() == ui::ThemeMode::System,
        "theme preview is not prematurely persisted");

  // Route through real sidebar notifications instead of constructing a second
  // manager. Switching pages must not save or discard feature drafts.
  SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(41, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hub.window, 41)));
  HWND manager = navigation::managerPage;
  check(manager && GetParent(manager) == hub.window && GetAncestor(manager, GA_ROOT) == hub.window,
        "folder manager is embedded in the same settings window");
  check((GetWindowLongPtrW(manager, GWL_STYLE) & WS_CHILD) != 0, "folder manager is a child page");
  check(!visibleStyle(navigation::body) && visibleStyle(manager), "manager and feature pages are mutually exclusive");
  check(!navigation::notesEnabled() && SendMessageW(navigation::notesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
        "switching pages retains unsaved feature selection");
  check(navigation::appearance->mode == ui::ThemeMode::Dark, "page switch retains theme preview");
  check(!visibleStyle(navigation::settingsScroll.window()), "feature scrollbar hides on manager page");
  inspectVersion(hub.window);

  for (UINT dpi : {96u, 144u, 192u}) {
    RECT target{0, 0, MulDiv(980, dpi, 96), MulDiv(660, dpi, 96)};
    SendMessageW(hub.window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&target));
    inspectVersion(hub.window);
    inspectNewTabOption();
    MoveWindow(hub.window, 0, 0, MulDiv(900, dpi, 96), MulDiv(500, dpi, 96), FALSE);
    inspectVersion(hub.window);
    inspectNewTabOption();
    check(visibleStyle(manager) && !visibleStyle(navigation::body), "resize preserves selected page");
    SendMessageW(hub.window, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    check(!visibleStyle(navigation::settingsScroll.window()), "wheel over hub does not resurrect the hidden feature scrollbar");
  }
  for (int pass = 0; pass < 4; ++pass) {
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(40, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hub.window, 40)));
    check(visibleStyle(navigation::body) && !visibleStyle(manager), "sidebar returns to feature page");
    check(SendMessageW(navigation::notesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED &&
          SendMessageW(navigation::themeChoice, CB_GETCURSEL, 0, 0) == DWORD(ui::ThemeMode::Dark),
          "repeated switches keep all draft controls");
    if (navigation::preferNewTabCheck)
      check(SendMessageW(navigation::preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
            "switching settings pages preserves new-tab draft");
    SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(41, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hub.window, 41)));
    check(navigation::managerPage == manager, "switches reuse the embedded manager instance");
  }
  HWND features = navigation::body, version = GetDlgItem(hub.window, 31);
  SendMessageW(hub.window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
  check(!IsWindow(hub.window) && !IsWindow(manager) && !IsWindow(features) && !IsWindow(version),
        "closing the hub destroys both pages and fixed controls");
  check(!navigation::notesEnabled() && ui::configuredTheme() == ui::ThemeMode::System && !navigation::preferNewTabEnabled(),
        "cancel does not commit feature, theme, or new-tab drafts");
  navigation::saveSettings(false, navigation::Action::Parent, navigation::Action::Parent,
                          false, true, ui::ThemeMode::System, false, true);
  Hub reopened;
  if (navigation::tabsSupported()) {
    check(!IsWindowEnabled(navigation::preferNewTabCheck) &&
          SendMessageW(navigation::preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
          "reopening settings retains saved new-tab choice while tabs are disabled");
  }
  SendMessageW(reopened.window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
  check(navigation::preferNewTabEnabled(), "cancel leaves previously saved new-tab choice untouched");
}
void languages() {
  lang::select(lang::Choice::English);
  {
    Hub hub;
    check(windowText(GetDlgItem(hub.window, navigation::languageExport)) == L"Export language template" &&
          windowText(GetDlgItem(hub.window, IDOK)) == L"Save", "English settings labels load");
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
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try { Registry registry; scenarios(); languages(); contextInventory(); contextCommands(); std::cout << "PASS " << checks << " settings, language and context menu assertions\n"; }
  catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << "\n"; result = 1; }
  CoUninitialize(); return result;
}
