// Exercise the real registry code against isolated, disposable HKCU subtrees.
// No elevation, installed COM class, or actual HKLM key is touched.
#include "../src/navigation_registration.h"
#include "../src/explorer_tabs.h"
#include <objbase.h>
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void check(bool value) {
  ++checks;
  if (!value)
    throw std::runtime_error("registration assertion");
}

int main() {
  GUID guid{};
  CoCreateGuid(&guid);
  wchar_t token[40];
  StringFromGUID2(guid, token, 40);
  auto rootPath = std::wstring(L"Software\\FolderNoteRegistrationTests\\") + token;
  HKEY root = nullptr, user = nullptr, machine = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, rootPath.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                      nullptr, &root, nullptr) ||
      RegCreateKeyExW(root, L"User", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr) ||
      RegCreateKeyExW(root, L"Machine", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr))
    return 1;
  if (RegOverridePredefKey(HKEY_CURRENT_USER, user) || RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine))
    return 2;
  int result = 0;
  try {
    const std::wstring first = L"S-1-5-21-1-2-3-1001", second = L"S-1-5-21-1-2-3-1002";
    check(navreg::validSid(first));
    check(navreg::validSid(navreg::currentUserSid()));
    check(!navreg::validSid(first + L"\\OtherKey"));
    check(!navreg::validSid(first + L" --quiet"));
    check(!navreg::validSid(L"S-1-5-"));
    check(!navreg::machineRegistered(first));
    navreg::registerMachine(first);
    HKEY probe = nullptr;
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, 0,
                        KEY_READ | KEY_WOW64_64KEY, &probe) == ERROR_FILE_NOT_FOUND);
    navreg::registerMachine(first); // Reinstall is idempotent.
    navreg::registerMachine(second);
    // Model an upgrade with existing BHO owners and a legacy toolbar value.
    // A shared toolbar belonging to another product must survive migration.
    constexpr wchar_t otherToolbar[] = L"{E716E421-473A-44D0-9526-3340FF357C19}";
    constexpr wchar_t otherValue[] = L"Other toolbar registration";
    HKEY toolbar = nullptr;
    check(RegCreateKeyExW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, 0, nullptr, 0,
                         KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &toolbar, nullptr) == ERROR_SUCCESS);
    DWORD legacyFlag = 1;
    auto ownValueError = RegSetValueExW(toolbar, tabs::clsidText, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&legacyFlag), sizeof(legacyFlag));
    auto otherValueError = RegSetValueExW(toolbar, otherToolbar, 0, REG_SZ,
                                         reinterpret_cast<const BYTE *>(otherValue), sizeof(otherValue));
    RegCloseKey(toolbar);
    check(ownValueError == ERROR_SUCCESS && otherValueError == ERROR_SUCCESS);
    navreg::registerMachine(first);
    check(RegGetValueW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, tabs::clsidText,
                       RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND);
    // Clean up legacy entries regardless of their old value type.
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, &toolbar) == ERROR_SUCCESS);
    constexpr wchar_t legacyText[] = L"";
    ownValueError = RegSetValueExW(toolbar, tabs::clsidText, 0, REG_SZ,
                                   reinterpret_cast<const BYTE *>(legacyText), sizeof(legacyText));
    RegCloseKey(toolbar);
    check(ownValueError == ERROR_SUCCESS);
    navreg::registerMachine(first);
    navreg::registerMachine(first);
    check(RegGetValueW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, tabs::clsidText,
                       RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND);
    wchar_t otherData[64];
    DWORD otherSize = sizeof(otherData);
    check(RegGetValueW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, otherToolbar,
                       RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, otherData, &otherSize) == ERROR_SUCCESS &&
          std::wstring(otherData) == otherValue);
    check(navreg::machineRegistered(first));
    check(navreg::machineRegistered(second));
    DWORD flag = 0, size = sizeof(flag);
    check(RegGetValueW(HKEY_LOCAL_MACHINE, navreg::machineKey, L"NoInternetExplorer",
                       RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &flag, &size) == 0 && flag == 1);
    const fn::fs::path directory = L"C:\\FolderNote Test\\Versions\\fixture";
    navreg::registerUser(directory);
    check(RegGetValueW(HKEY_CURRENT_USER, tabs::userKey, nullptr, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
    wchar_t dll[1024];
    size = sizeof(dll);
    auto inproc = std::wstring(navreg::userKey) + L"\\InprocServer32";
    check(RegGetValueW(HKEY_CURRENT_USER, inproc.c_str(), nullptr, RRF_RT_REG_SZ,
                       nullptr, dll, &size) == 0 &&
          std::wstring(dll) == (directory / L"FolderNoteNavigation.dll").native());
    // Never put a user-writable DLL path in machine-wide COM registration.
    auto machineClass = inproc.substr(std::wstring(L"Software\\Classes\\").size());
    machineClass = L"Software\\Classes\\" + machineClass;
    size = sizeof(dll);
    check(RegGetValueW(HKEY_LOCAL_MACHINE, machineClass.c_str(), nullptr, RRF_RT_REG_SZ,
                       nullptr, dll, &size) == ERROR_FILE_NOT_FOUND);
    navreg::unregisterMachine(first);
    check(!navreg::machineRegistered(first));
    check(navreg::machineRegistered(second));
    navreg::unregisterMachine(first); // Repeat uninstall preserves other user.
    check(navreg::machineRegistered(second));
    navreg::unregisterMachine(second);
    check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, navreg::machineKey, 0, KEY_READ, &probe) == ERROR_FILE_NOT_FOUND);
    auto toolbarMissing = RegGetValueW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, tabs::clsidText, RRF_RT_ANY, nullptr, nullptr, nullptr);
    check(toolbarMissing == ERROR_FILE_NOT_FOUND || toolbarMissing == ERROR_PATH_NOT_FOUND);
    otherSize = sizeof(otherData);
    check(RegGetValueW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, otherToolbar,
                       RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, otherData, &otherSize) == ERROR_SUCCESS &&
          std::wstring(otherData) == otherValue);
    navreg::unregisterUser();
    navreg::unregisterUser();
    check(RegOpenKeyExW(HKEY_CURRENT_USER, navreg::userKey, 0, KEY_READ, &probe) == ERROR_FILE_NOT_FOUND);
    check(RegOpenKeyExW(HKEY_CURRENT_USER, tabs::userKey, 0, KEY_READ, &probe) == ERROR_FILE_NOT_FOUND);
    wchar_t executable[] = L"Install.exe", mode[] = L"--register-navigation", invalid[] = L"S-1-5\\bad";
    wchar_t *arguments[] = {executable, mode, invalid};
    int helperResult = 0;
    check(navreg::handleHelper(3, arguments, helperResult) && helperResult != 0);
    check(navreg::handleHelper(2, arguments, helperResult) && helperResult != 0);
    check(!navreg::handleHelper(1, arguments, helperResult));
    std::cout << "PASS " << checks << " isolated registration assertions: per-user COM, legacy toolbar migration, "
                 "owner isolation, repeat operations, strict helper arguments\n";
  } catch (...) {
    result = 1;
    std::cerr << "FAIL isolated navigation registration at assertion " << checks << "\n";
  }
  RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
  RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
  RegCloseKey(user);
  RegCloseKey(machine);
  RegCloseKey(root);
  RegDeleteTreeW(HKEY_CURRENT_USER, rootPath.c_str());
  return result;
}
