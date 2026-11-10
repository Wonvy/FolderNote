#include "../src/navigation.h"
#include "../src/core.h"
#include <iostream>
#include <limits>
#include <stdexcept>
static int n = 0;
static void check(bool value) {
  ++n;
  if (!value)
    throw std::runtime_error("navigation/settings assertion " + std::to_string(n));
}

// Redirect HKCU only inside this test process. User settings are never touched.
class IsolatedSettings {
  HKEY realUser = nullptr, temporary = nullptr;
  std::wstring path;
  bool redirected = false;

public:
  IsolatedSettings() {
    if (RegOpenCurrentUser(KEY_ALL_ACCESS, &realUser) != ERROR_SUCCESS)
      throw std::runtime_error("open test registry");
    path = L"Software\\FolderNote-NavigationTests-" + std::to_wstring(GetCurrentProcessId()) +
           L"-" + std::to_wstring(GetTickCount64());
    DWORD disposition = 0;
    const auto result = RegCreateKeyExW(realUser, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                       nullptr, &temporary, &disposition);
    if (result != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
      if (temporary)
        RegCloseKey(temporary);
      RegCloseKey(realUser);
      throw std::runtime_error("create test registry");
    }
    if (RegOverridePredefKey(HKEY_CURRENT_USER, temporary) != ERROR_SUCCESS) {
      RegCloseKey(temporary);
      RegDeleteTreeW(realUser, path.c_str());
      RegCloseKey(realUser);
      throw std::runtime_error("redirect test registry");
    }
    redirected = true;
  }
  ~IsolatedSettings() {
    if (redirected)
      RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(temporary);
    RegDeleteTreeW(realUser, path.c_str());
    RegCloseKey(realUser);
  }
  void set(DWORD type, const void *data, DWORD bytes) {
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"BlankDoubleClick", type,
                          data, bytes) == ERROR_SUCCESS);
  }
};

static bool registryValueExists(const std::wstring &path, const wchar_t *name) {
  const auto result = RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_ANY,
                                  nullptr, nullptr, nullptr);
  check(result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND);
  return result == ERROR_SUCCESS;
}

static void setText(const std::wstring &path, const wchar_t *name, const wchar_t *text) {
  check(RegSetKeyValueW(HKEY_CURRENT_USER, path.c_str(), name, REG_SZ, text,
                        static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
}

static void checkText(const std::wstring &path, const wchar_t *name, const wchar_t *text) {
  wchar_t value[128]{};
  DWORD size = sizeof(value);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ, nullptr,
                     value, &size) == ERROR_SUCCESS);
  check(wcscmp(value, text) == 0);
}

int main() {
  using namespace navigation;
  try {
    Click a{{100, 100}, 1000, (HWND)1, (HWND)2, true};
    auto b = a;
    b.time = 1200;
    check(isDoubleClick(a, b, 500, 4, 4));
    b.time = 1501;
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    b.point.x += 3;
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    b.point.y += 3;
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    b.root = (HWND)3;
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    b.view = (HWND)4;
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    b.blank = false;
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    a.blank = false;
    check(!isDoubleClick(a, b, 500, 4, 4));
    a.blank = true;
    a.time = 0xfffffff0;
    b = a;
    b.time = 20;
    check(isDoubleClick(a, b, 500, 4, 4));
    a.root = nullptr;
    b = a;
    check(!isDoubleClick(a, b, 500, 4, 4));
    a = {{-100, -200}, 1000, (HWND)1, (HWND)2, true};
    b = a;
    b.point.x += 2;
    b.point.y -= 2;
    b.time += 500;
    check(isDoubleClick(a, b, 500, 4, 4));
    a.point.x = std::numeric_limits<LONG>::min();
    b.point.x = std::numeric_limits<LONG>::max();
    check(!isDoubleClick(a, b, 500, 4, 4));
    b = a;
    check(!isDoubleClick(a, b, 500, -1, 4));
    a.view = nullptr;
    b = a;
    check(!isDoubleClick(a, b, 500, 4, 4));
    {
      IsolatedSettings settings;
      check(configuredAction() == Action::Parent);
      check(!notesEnabled());
      check(preferredAction() == Action::Parent);
      check(!quickEnabled() && dialogQuickEnabled());
      check(!tabsEnabled());
      check(!preferNewTabEnabled());
      saveSettings(false, Action::Parent, Action::Parent, false, true, ui::ThemeMode::System, true, true);
      check(tabsEnabled() && preferNewTabEnabled());
      saveSettings(false, Action::Parent, Action::Parent); check(tabsEnabled() && preferNewTabEnabled());
      // Disabling tabs and legacy saves must retain the user's subordinate choice.
      saveSettings(false, Action::Parent, Action::Parent, false, true, ui::ThemeMode::System, false);
      check(!tabsEnabled() && preferNewTabEnabled());
      syncNoteMenus(); check(preferNewTabEnabled());
      check(ui::configuredTheme() == ui::ThemeMode::System);
      for (auto theme : {ui::ThemeMode::Light, ui::ThemeMode::Dark, ui::ThemeMode::System}) {
        saveSettings(false, Action::Parent, Action::Parent, false, true, theme);
        check(ui::configuredTheme() == theme && preferNewTabEnabled());
        saveSettings(true, Action::Back, Action::Parent); // Old callers and upgrades preserve appearance.
        check(ui::configuredTheme() == theme && preferNewTabEnabled());
      }
      bool badTheme = false;
      try { saveSettings(false, Action::Off, Action::Back, true, false, ui::ThemeMode(9), true, false); }
      catch (const fn::Error &) { badTheme = true; }
      check(badTheme && ui::configuredTheme() == ui::ThemeMode::System && configuredAction() == Action::Back &&
            !tabsEnabled() && preferNewTabEnabled());
      saveSettings(false, Action::Parent, Action::Parent, false, true, ui::ThemeMode::System, true, false);
      check(tabsEnabled() && !preferNewTabEnabled());
      saveSettings(false, Action::Parent, Action::Parent, false, true, ui::ThemeMode::System, false, false);
      DWORD invalidTheme = 99;
      check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"Theme", REG_DWORD, &invalidTheme, sizeof(invalidTheme)) == ERROR_SUCCESS);
      check(ui::configuredTheme() == ui::ThemeMode::System);
      saveSettings(false, Action::Off, Action::Back, true, false);
      check(quickEnabled() && !dialogQuickEnabled() && configuredAction() == Action::Off && !notesEnabled());
      saveSettings(false, Action::Parent, Action::Parent);
      check(quickEnabled() && !dialogQuickEnabled()); // Legacy save / upgrade preserves the new switch.
      saveSettings(false, Action::Parent, Action::Parent, false, true);
      check(!quickEnabled() && dialogQuickEnabled());
      check(RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FolderNote") == ERROR_SUCCESS);
      for (DWORD value : {0UL, 1UL, 2UL}) {
        settings.set(REG_DWORD, &value, sizeof(value));
        check(configuredAction() == Action(value));
        check(preferredAction() == (value == 2 ? Action::Back : Action::Parent));
      }
      for (DWORD value : {3UL, 0xffffffffUL}) {
        settings.set(REG_DWORD, &value, sizeof(value));
        check(configuredAction() == Action::Off);
      }
      const wchar_t invalid[] = L"1";
      settings.set(REG_SZ, invalid, sizeof(invalid));
      check(configuredAction() == Action::Off);

      // Portable/modern-only settings must not manufacture broken legacy commands.
      saveSettings(false, Action::Back, Action::Back);
      check(!notesEnabled());
      check(configuredAction() == Action::Back);
      check(preferredAction() == Action::Back);
      for (auto verb : fn::verbs)
        check(!registryValueExists(std::wstring(L"Software\\Classes\\Directory\\shell\\") + verb, nullptr));

      const std::wstring edit = L"Software\\Classes\\Directory\\shell\\FolderNote.Edit";
      const std::wstring clear = L"Software\\Classes\\Directory\\shell\\FolderNote.Clear";
      const std::wstring repair = L"Software\\Classes\\Directory\\shell\\FolderNote.Repair";
      const std::wstring settingsMenu = L"Software\\Classes\\Directory\\Background\\shell\\FolderNote.Settings";
      const std::wstring unrelated = L"Software\\Classes\\Directory\\shell\\AnotherTool";
      for (auto path : {edit, clear, settingsMenu, unrelated}) {
        setText(path, nullptr, L"existing title");
        setText(path + L"\\command", nullptr, L"existing command");
      }
      setText(edit, L"Icon", L"original icon");

      syncNoteMenus(); // Installer applies the persisted disabled flag.
      check(registryValueExists(edit, L"LegacyDisable"));
      check(registryValueExists(clear, L"LegacyDisable"));
      check(!registryValueExists(repair, L"LegacyDisable"));
      check(!registryValueExists(settingsMenu, L"LegacyDisable"));
      check(!registryValueExists(unrelated, L"LegacyDisable"));
      check(configuredAction() == Action::Back);

      // Every combination is independent, including both features disabled.
      for (bool notes : {false, true}) {
        for (auto action : {Action::Off, Action::Parent, Action::Back}) {
          saveSettings(notes, action, Action::Back);
          check(notesEnabled() == notes);
          check(configuredAction() == action);
          check(preferredAction() == Action::Back);
          check(registryValueExists(edit, L"LegacyDisable") == !notes);
          check(registryValueExists(clear, L"LegacyDisable") == !notes);
          checkText(edit + L"\\command", nullptr, L"existing command");
          checkText(edit, L"Icon", L"original icon");
          checkText(settingsMenu + L"\\command", nullptr, L"existing command");
        }
      }
      saveSettings(false, Action::Off, Action::Back);
      syncNoteMenus();
      check(!notesEnabled() && configuredAction() == Action::Off && preferredAction() == Action::Back);
      bool rejected = false;
      try { saveSettings(true, Action(123), Action::Parent); } catch (const fn::Error &) { rejected = true; }
      check(rejected);
      check(!notesEnabled() && configuredAction() == Action::Off && preferredAction() == Action::Back);
      rejected = false;
      try { saveSettings(true, Action::Parent, Action::Off); } catch (const fn::Error &) { rejected = true; }
      check(rejected);
      check(!notesEnabled() && configuredAction() == Action::Off && preferredAction() == Action::Back);

      // Corrupt values must not accidentally opt into note menus.
      check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"NotesEnabled",
                            REG_SZ, invalid, sizeof(invalid)) == ERROR_SUCCESS);
      check(!notesEnabled());
      check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerOpenInNewTab",
                            REG_SZ, invalid, sizeof(invalid)) == ERROR_SUCCESS);
      check(!preferNewTabEnabled());
      DWORD invalidNewTab = 2;
      check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerOpenInNewTab",
                            REG_DWORD, &invalidNewTab, sizeof(invalidNewTab)) == ERROR_SUCCESS);
      check(!preferNewTabEnabled());
      DWORD invalidPreference = 17;
      check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"NavigationPreference",
                            REG_DWORD, &invalidPreference, sizeof(invalidPreference)) == ERROR_SUCCESS);
      check(preferredAction() == Action::Parent);
    }
    std::cout << "PASS " << n
              << " gesture and configuration checks: interval, distance, root, view, nonblank, "
                 "tick rollover, negative coordinates, independent switches, menu preservation, preference migration\n";
  } catch (const fn::Error &error) {
    std::wcerr << error.message << L'\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (...) {
    return 1;
  }
  return 0;
}
