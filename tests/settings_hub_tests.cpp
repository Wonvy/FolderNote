// Only hidden, test-owned windows and an isolated registry are used here.
// Compile the actual settings controller so page routing and layout are tested
// without starting Explorer, helpers, or a second interactive settings process.
#include "../src/navigation.cpp"
#include <iostream>
#include <stdexcept>

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
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try { Registry registry; scenarios(); std::cout << "PASS " << checks << " unified settings assertions: embedded routing, drafts, fixed version, scaling, cleanup\n"; }
  catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << "\n"; result = 1; }
  CoUninitialize(); return result;
}
