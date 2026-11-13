#include "../src/core.h"
#include "../src/explorer_entries.h"
#include "../src/removed_entries.h"
#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace explorer_entries;
constexpr wchar_t explorer[] = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\)";
constexpr wchar_t classes[] = LR"(Software\Classes\CLSID\)";
constexpr wchar_t pin[] = L"System.IsPinnedToNameSpaceTree";
constexpr wchar_t cloud[] = L"{55A00000-1000-2000-3000-100000000001}";
constexpr wchar_t machineOnly[] = L"{55A00000-1000-2000-3000-100000000002}";
constexpr wchar_t managed[] = L"{55A00000-1000-2000-3000-100000000003}";
constexpr wchar_t application[] = L"{55A00000-1000-2000-3000-100000000004}";
constexpr wchar_t nonEnum[] = LR"(Software\Microsoft\Windows\CurrentVersion\Policies\NonEnum)";

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct Key {
  HKEY handle = nullptr;
  ~Key() { if (handle) RegCloseKey(handle); }
};

void create(HKEY root, const std::wstring &path, Key &key) {
  require(!RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                          nullptr, &key.handle, nullptr), "create test registry key");
}

struct IsolatedRegistry {
  std::wstring path = L"Software\\FolderNoteExplorerEntriesTest-" + std::to_wstring(GetCurrentProcessId());
  Key root, user, machine, real;
  IsolatedRegistry() {
    require(!RegOpenKeyExW(HKEY_CURRENT_USER, L"", 0, KEY_ALL_ACCESS, &real.handle), "open real test parent");
    create(real.handle, path, root);
    create(root.handle, L"User", user);
    create(root.handle, L"Machine", machine);
    require(!RegOverridePredefKey(HKEY_CURRENT_USER, user.handle), "isolate user hive");
    if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine.handle)) {
      RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
      throw std::runtime_error("isolate machine hive");
    }
  }
  ~IsolatedRegistry() {
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegDeleteTreeW(real.handle, path.c_str());
  }
};

std::wstring branch(const wchar_t *name) { return std::wstring(explorer) + name; }
std::wstring classPath(const wchar_t *id) { return std::wstring(classes) + id; }

void writeText(HKEY hive, const std::wstring &path, const wchar_t *name, const std::wstring &value) {
  Key key;
  create(hive, path, key);
  require(!RegSetValueExW(key.handle, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))), "write test text");
}

void writeDword(HKEY hive, const std::wstring &path, const wchar_t *name, DWORD value) {
  Key key;
  create(hive, path, key);
  require(!RegSetValueExW(key.handle, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)),
          "write test DWORD");
}

std::wstring readText(HKEY hive, const std::wstring &path, const wchar_t *name = nullptr) {
  wchar_t value[128]{};
  DWORD size = sizeof(value);
  require(!RegGetValueW(hive, path.c_str(), name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                        nullptr, value, &size), "read test text");
  return value;
}

std::optional<DWORD> readDword(HKEY hive, const std::wstring &path, const wchar_t *name) {
  DWORD value = 0, size = sizeof(value);
  auto status = RegGetValueW(hive, path.c_str(), name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                            nullptr, &value, &size);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
  require(!status, "read test DWORD");
  return value;
}

bool exists(HKEY hive, const std::wstring &path) {
  Key key;
  const auto status = RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key.handle);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return false;
  require(!status, "open test mount");
  return true;
}

Entry entry(Surface surface, const std::wstring &id) {
  const auto entries = inventory(surface);
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &row) { return row.id == id; });
  require(found != entries.end(), "entry appears in inventory");
  return *found;
}
}

int main() {
  try {
    IsolatedRegistry registry;
    require(!isRemoved(Surface::ThisPc, cloud), "new entries have no removal marker");
    markRemoved(Surface::ThisPc, cloud, true);
    require(isRemoved(Surface::ThisPc, L"{55a00000-1000-2000-3000-100000000001}") &&
            !isRemoved(Surface::Navigation, cloud) && !isRemoved(Surface::Desktop, cloud),
            "removal markers follow registry identity and stay scoped to their surface");
    markRemoved(Surface::ThisPc, cloud, false);
    markRemoved(Surface::ThisPc, cloud, false);
    require(!isRemoved(Surface::ThisPc, cloud), "restoring an entry clears its removal marker");
    constexpr wchar_t settings[] = LR"(Software\FolderNote)";
    writeDword(HKEY_CURRENT_USER, settings, L"UnrelatedPreference", 42);
    markRemoved(Surface::Navigation, cloud, true);
    markRemoved(Surface::Desktop, L"C:\\Users\\Test\\Desktop\\Test.lnk", true);
    clearRemoved();
    require(!isRemoved(Surface::Navigation, cloud) && !isRemoved(Surface::Desktop, L"C:\\Users\\Test\\Desktop\\Test.lnk") &&
            readDword(HKEY_CURRENT_USER, settings, L"UnrelatedPreference") == 42u,
            "cleanup removes only FolderNote removal markers");
    const auto thisPc = branch(L"MyComputer\\NameSpace\\") + cloud;
    const auto desktop = branch(L"Desktop\\NameSpace\\") + cloud;
    const auto hideComputer = branch(L"HideMyComputerIcons");
    const auto hideDesktop = branch(L"HideDesktopIcons\\NewStartPanel");
    writeText(HKEY_LOCAL_MACHINE, thisPc, nullptr, L"Machine junction");
    writeText(HKEY_CURRENT_USER, thisPc, nullptr, L"User junction");
    writeText(HKEY_CURRENT_USER, desktop, nullptr, L"Account!Identifier");
    writeText(HKEY_CURRENT_USER, classPath(cloud), nullptr, L"Friendly name");
    writeText(HKEY_LOCAL_MACHINE, classPath(cloud) + L"\\DefaultIcon", nullptr, L"C:\\Fixture\\system.dll,-1");
    writeText(HKEY_CURRENT_USER, classPath(cloud) + L"\\DefaultIcon", nullptr, L"C:\\Fixture\\user.ico");
    writeText(HKEY_CURRENT_USER, classPath(cloud) + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", L"X:\\Fixture");
    writeDword(HKEY_CURRENT_USER, classPath(cloud), pin, 1);
    writeDword(HKEY_CURRENT_USER, hideDesktop, cloud, 1);

    const auto pc = inventory(Surface::ThisPc);
    require(pc.size() == 1 && pc.front().name == L"Friendly name" && !pc.front().hidden,
            "merge duplicate registrations and prefer the class label");
    const auto nav = entry(Surface::Navigation, cloud);
    const auto desk = entry(Surface::Desktop, cloud);
    require(nav.target == L"X:\\Fixture" && nav.iconLocation == L"C:\\Fixture\\user.ico",
            "read the configured target and prefer the per-user icon without opening the target");
    require(!nav.hidden && desk.hidden, "desktop and navigation visibility are independent");
    setHidden(pc.front(), true);
    require(entry(Surface::ThisPc, cloud).hidden && entry(Surface::ThisPc, cloud).changed,
            "refresh detects the saved override");
    require(readDword(HKEY_CURRENT_USER, hideComputer, cloud) == 1u &&
            !readDword(HKEY_LOCAL_MACHINE, hideComputer, cloud), "write This PC override only for the user");
    require(!entry(Surface::Navigation, cloud).hidden, "hiding in This PC does not unpin the navigation node");

    setHidden(desk, false);
    require(readDword(HKEY_CURRENT_USER, hideDesktop, cloud) == 0u, "show previously hidden desktop icon");
    setHidden(desk, true);
    restore(desk);
    require(readDword(HKEY_CURRENT_USER, hideDesktop, cloud) == 1u && !entry(Surface::Desktop, cloud).changed,
            "repeated toggles preserve the original vendor setting");

    writeText(HKEY_LOCAL_MACHINE, classPath(machineOnly), nullptr, L"Machine provider");
    writeDword(HKEY_LOCAL_MACHINE, classPath(machineOnly), pin, 1);
    const auto machineNav = entry(Surface::Navigation, machineOnly);
    setHidden(machineNav, true);
    require(readDword(HKEY_CURRENT_USER, classPath(machineOnly), pin) == 0u &&
            readDword(HKEY_LOCAL_MACHINE, classPath(machineOnly), pin) == 1u,
            "user override for a machine-only pin");
    restore(machineNav);
    Key probe;
    require(RegOpenKeyExW(HKEY_CURRENT_USER, classPath(machineOnly).c_str(), 0,
                          KEY_READ | KEY_WOW64_64KEY, &probe.handle) == ERROR_FILE_NOT_FOUND,
            "restore removes the empty app-created class key");

    writeText(HKEY_LOCAL_MACHINE, branch(L"MyComputer\\NameSpace_36354489\\") + managed, nullptr, L"Managed");
    writeText(HKEY_LOCAL_MACHINE, classPath(managed), nullptr, L"Managed provider");
    writeDword(HKEY_LOCAL_MACHINE, nonEnum, managed, 1);
    const auto restricted = entry(Surface::ThisPc, managed);
    require(restricted.hidden && !restricted.canChange, "respect an existing administrative restriction");

    const auto appMount = branch(L"MyComputer\\NameSpace\\") + application;
    const auto hiddenAppMount = branch(L"MyComputer\\NameSpace\\FolderNote.Hidden.") + application;
    writeText(HKEY_CURRENT_USER, appMount, nullptr, L"User app junction");
    writeText(HKEY_LOCAL_MACHINE, appMount, nullptr, L"Machine app junction");
    writeDword(HKEY_CURRENT_USER, appMount + L"\\Metadata", L"Flags", 42);
    writeDword(HKEY_LOCAL_MACHINE, appMount + L"\\Metadata", L"Flags", 84);
    writeText(HKEY_CURRENT_USER, classPath(application), nullptr, L"App provider");
    auto app = entry(Surface::ThisPc, application);
    setHidden(app, true);
    app = entry(Surface::ThisPc, application);
    require(app.hidden && app.changed && app.canChange &&
            !exists(HKEY_CURRENT_USER, appMount) && !exists(HKEY_LOCAL_MACHINE, appMount),
            "hide removes both application mounts from This PC enumeration");
    require(readText(HKEY_CURRENT_USER, hiddenAppMount) == L"User app junction" &&
            readText(HKEY_LOCAL_MACHINE, hiddenAppMount) == L"Machine app junction" &&
            readDword(HKEY_CURRENT_USER, hiddenAppMount + L"\\Metadata", L"Flags") == 42u &&
            readDword(HKEY_LOCAL_MACHINE, hiddenAppMount + L"\\Metadata", L"Flags") == 84u &&
            readText(HKEY_CURRENT_USER, classPath(application)) == L"App provider",
            "hide preserves the mount trees and the underlying class");
    setHidden(app, false);
    require(!exists(HKEY_CURRENT_USER, hiddenAppMount) && !exists(HKEY_LOCAL_MACHINE, hiddenAppMount) &&
            readText(HKEY_CURRENT_USER, appMount) == L"User app junction" &&
            readText(HKEY_LOCAL_MACHINE, appMount) == L"Machine app junction" &&
            readDword(HKEY_CURRENT_USER, appMount + L"\\Metadata", L"Flags") == 42u &&
            readDword(HKEY_LOCAL_MACHINE, appMount + L"\\Metadata", L"Flags") == 84u &&
            !entry(Surface::ThisPc, application).hidden, "show restores both mounts with their values and subkeys");
    writeText(HKEY_LOCAL_MACHINE, branch(L"Desktop\\NameSpace_36354489\\") + application, nullptr, L"Desktop copy");
    setHidden(app, true);
    require(!readDword(HKEY_CURRENT_USER, nonEnum, application) &&
            readDword(HKEY_CURRENT_USER, hideComputer, application) == 1u,
            "a second namespace registration precludes global enumeration suppression");

    setHidden(nav, true);
    writeDword(HKEY_CURRENT_USER, classPath(cloud), pin, 9);
    require(restoreAll() == 0, "restore user preferences for all surfaces");
    require(readDword(HKEY_CURRENT_USER, classPath(cloud), pin) == 9u &&
            !readDword(HKEY_CURRENT_USER, hideComputer, cloud) &&
            !readDword(HKEY_CURRENT_USER, hideComputer, application) &&
            readDword(HKEY_CURRENT_USER, hideDesktop, cloud) == 1u, "preserve a later external change");
    std::cout << "Explorer entry inventory and restoration passed\n";
    return 0;
  } catch (const fn::Error &error) {
    std::wcerr << error.message << L'\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
