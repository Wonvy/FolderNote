// Compile the same uninstall flow against isolated HKCU/HKLM hives and files.
// Package removal and navigation shutdown are mocked: no installed app is touched.
#include "../src/core.h"
#include "../src/navigation.h"
#include "../src/navigation_registration.h"
#include <shellapi.h>
#include <shlobj.h>
#include <fstream>
#include <iostream>
static fn::fs::path fixture;
static int stops = 0;
namespace fn {
fs::path testRoot() { return fixture; }
} // namespace fn
namespace navigation {
void testStop() { ++stops; }
} // namespace navigation
#define localRoot testRoot
#define stop testStop
#define wWinMain uninstallEntry
#define FOLDERNOTE_UNINSTALL_ONLY
#define FOLDERNOTE_UNINSTALL_TEST
#include "../src/install.cpp"
#undef localRoot
#undef stop
#undef wWinMain
int wmain() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  GUID guid;
  CoCreateGuid(&guid);
  wchar_t token[40];
  StringFromGUID2(guid, token, 40);
  auto root = fn::fs::temp_directory_path() / (std::wstring(L"FolderNote-UninstallTest-") + token);
  fixture = root / L"Installed";
  fn::fs::create_directories(fixture);
  auto key = std::wstring(L"Software\\FolderNoteUninstallTests\\") + token;
  HKEY isolated = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                      &isolated, nullptr))
    return 1;
  HKEY isolatedMachine = nullptr;
  if (RegCreateKeyExW(isolated, L"Machine", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                      &isolatedMachine, nullptr))
    return 2;
  if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, isolatedMachine))
    return 2;
  if (RegOverridePredefKey(HKEY_CURRENT_USER, isolated))
    return 2;
  int result = 0;
  try {
    for (auto name : {L"FolderNote.exe", L"FolderNoteShell.dll", L"Install.exe", L"Uninstall.exe"})
      std::ofstream(fixture / name) << "test";
    std::ofstream(root / L"desktop.ini") << "user notes preserved";
    std::ofstream(fixture / L"keep.me") << "unrelated file";
    auto source=root/L"Source";fn::fs::create_directory(source);
    for(auto name:payloadNames)std::ofstream(source/name)<<"new payload";
    HANDLE legacyLock=CreateFileW((fixture/L"FolderNoteShell.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    auto first=stageVersion(source,fixture);
    HANDLE versionLock=CreateFileW((first/L"FolderNoteShell.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE navigationLock=CreateFileW((first/L"FolderNoteNavigation.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    auto second=stageVersion(source,fixture);
    if(first==second||!fn::fs::exists(second/L"Uninstall.exe")||!fn::fs::exists(second/L"FolderNoteNavigation.dll")||fn::fs::file_size(fixture/L"FolderNoteShell.dll")!=4)throw 7;
    CloseHandle(legacyLock);CloseHandle(versionLock);CloseHandle(navigationLock);
    auto exists = [](const wchar_t *path, const wchar_t *name) {
      DWORD bytes = 0;
      return RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_ANY, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
    };
    // First install persists Parent + notes off and removes old settings menus.
    value(L"Software\\Classes\\Directory\\shell\\FolderNote.Settings\\command", nullptr, L"old command");
    value(L"Software\\Classes\\Directory\\Background\\shell\\FolderNote.Settings\\command", nullptr, L"old command");
    registerMenus(second, false);
    if (navigation::notesEnabled() || navigation::configuredAction() != navigation::Action::Parent ||
        !exists(L"Software\\FolderNote", L"BlankDoubleClick") ||
        !exists(L"Software\\FolderNote", L"NotesEnabled")) throw 17;
    // Explicitly disabled navigation and enabled notes survive an upgrade.
    navigation::saveSettings(true, navigation::Action::Off, navigation::Action::Back);
    registerMenus(second, false);
    if (!navigation::notesEnabled() || navigation::configuredAction() != navigation::Action::Off ||
        navigation::preferredAction() != navigation::Action::Back ||
        exists(L"Software\\Classes\\Directory\\shell\\FolderNote.Edit", L"LegacyDisable")) throw 18;
    navigation::saveSettings(false, navigation::Action::Back, navigation::Action::Back);
    registerMenus(second, false);
    if (!exists(L"Software\\Classes\\Directory\\shell\\FolderNote.Edit", L"LegacyDisable") ||
        exists(L"Software\\Classes\\Directory\\shell\\FolderNote.Settings\\command", nullptr) ||
        exists(L"Software\\Classes\\Directory\\Background\\shell\\FolderNote.Settings\\command", nullptr) ||
        navigation::configuredAction() != navigation::Action::Back)
      throw 11;
    createSettingsShortcut(second);
    if (!fn::fs::exists(settingsShortcut())) throw 12;
    registerMenus(second, true);
    if (exists(L"Software\\Classes\\Directory\\shell\\FolderNote.Edit", nullptr) ||
        exists(L"Software\\Classes\\Directory\\Background\\shell\\FolderNote.Settings\\command", nullptr))
      throw 13;
    registerMenus(second, false);
    if (!exists(L"Software\\Classes\\Directory\\shell\\FolderNote.Edit", L"LegacyDisable")) throw 14;
    navreg::registerUser(second);
    navreg::registerMachine(navreg::currentUserSid());
    const std::wstring otherUser = L"S-1-5-21-1-2-3-1001";
    navreg::registerMachine(otherUser);
    if (uninstallEntry(nullptr, nullptr, nullptr, 0) != 0 || stops != 1)
      throw 1;
    if (navreg::machineRegistered(navreg::currentUserSid()) || !navreg::machineRegistered(otherUser))
      throw 9;
    HKEY navigationClass = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, navreg::userKey, 0, KEY_READ, &navigationClass) == ERROR_SUCCESS) {
      RegCloseKey(navigationClass);
      throw 10;
    }
    if (fn::fs::exists(fixture / L"FolderNote.exe") || fn::fs::exists(fixture / L"Uninstall.exe"))
      throw 2;
    if (fn::fs::exists(settingsShortcut()) ||
        exists(L"Software\\Classes\\Directory\\shell\\FolderNote.Settings\\command", nullptr) ||
        exists((std::wstring(L"Software\\Classes\\CLSID\\") + fn::settingsId + L"\\InprocServer32").c_str(), nullptr))
      throw 15;
    if (navigation::notesEnabled() || navigation::configuredAction() != navigation::Action::Back) throw 16;
    if(fn::fs::exists(first)||fn::fs::exists(second))throw 8;
    if (!fn::fs::exists(root / L"desktop.ini") || !fn::fs::exists(fixture / L"keep.me"))
      throw 3;
    HKEY menu;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\FolderNote.Edit", 0,
                      KEY_READ, &menu) == ERROR_SUCCESS) {
      RegCloseKey(menu);
      throw 4;
    }
    if (uninstallEntry(nullptr, nullptr, nullptr, 0) != 0)
      throw 5;
    std::ofstream(fixture / L"FolderNote.exe") << "locked";
    HANDLE locked = CreateFileW((fixture / L"FolderNote.exe").c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    int partial = uninstallEntry(nullptr, nullptr, nullptr, 0);
    CloseHandle(locked);
    if (partial != 2 || !fn::fs::exists(fixture / L"FolderNote.exe"))
      throw 6;
    std::cout << "PASS isolated uninstall: files/menu removal, note preservation, unrelated file "
                 "preservation, repeat uninstall, locked file report, upgrading with locked legacy and version DLLs, "
                 "navigation DLL and per-user registration removal, other user registration preserved, "
                 "independent feature switches preserved on upgrade, settings shortcut, modern/legacy migration\n";
  } catch (...) {
    result = 1;
    std::cerr << "FAIL isolated uninstall\n";
  }
  RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
  RegCloseKey(isolatedMachine);
  RegCloseKey(isolated);
  RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  CoUninitialize();
  return result;
}
