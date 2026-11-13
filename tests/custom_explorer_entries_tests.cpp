// Exercise the production registration code with a disposable HKCU override.
#include "../src/custom_explorer_entries.h"
#include "../src/core.h"
#include <fstream>
#include <iostream>
#include <shlobj.h>
#include <stdexcept>
#include <wrl/client.h>

using namespace explorer_entries;
namespace {
fn::fs::path personalDesktop, sharedDesktop;
HRESULT testKnownFolder(REFKNOWNFOLDERID folder, DWORD, HANDLE, PWSTR *path) {
  const auto &value = folder == FOLDERID_Desktop ? personalDesktop : sharedDesktop;
  const auto bytes = (value.native().size() + 1) * sizeof(wchar_t);
  *path = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
  if (!*path) return E_OUTOFMEMORY;
  memcpy(*path, value.c_str(), bytes);
  return S_OK;
}
void testNotify(LONG, UINT, LPCVOID, LPCVOID) {}
}

#define SHGetKnownFolderPath testKnownFolder
#define SHChangeNotify testNotify
#include "../src/custom_explorer_entries.cpp"
#undef SHChangeNotify
#undef SHGetKnownFolderPath

namespace {
constexpr wchar_t thisPc[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace\\";
constexpr wchar_t desktop[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\";
constexpr wchar_t classes[] = L"Software\\Classes\\CLSID\\";
constexpr wchar_t hideIcons[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel";
constexpr wchar_t records[] = L"Software\\FolderNote\\ExplorerEntries\\Custom\\";

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fixture {
  std::wstring registry;
  fn::fs::path directory;
  HKEY key = nullptr;
  bool overridden = false;
  void init() {
    GUID guid{};
    wchar_t id[39]{};
    check(SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, id, _countof(id)), "create test id");
    registry = std::wstring(L"Software\\FolderNoteCustomEntryTests\\") + id;
    directory = fn::fs::temp_directory_path() / (std::wstring(L"FolderNoteEntryTests-") + id);
    personalDesktop = directory / L"Desktop";
    sharedDesktop = directory / L"PublicDesktop";
    check(fn::fs::create_directories(personalDesktop) && fn::fs::create_directory(sharedDesktop),
          "create isolated desktop folders");
    check(RegCreateKeyExW(HKEY_CURRENT_USER, registry.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                          nullptr, &key, nullptr) == ERROR_SUCCESS, "create disposable registry");
    check(RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS, "override HKCU");
    overridden = true;
  }
  ~Fixture() {
    if (overridden) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (key) {
      RegCloseKey(key);
      RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
    }
    std::error_code ignored;
    if (!directory.empty()) {
      fn::fs::remove(personalDesktop / L"Test.lnk", ignored);
      fn::fs::remove(personalDesktop / L"Test (2).lnk", ignored);
      fn::fs::remove(personalDesktop / L"Test (3).lnk", ignored);
      fn::fs::remove(sharedDesktop / L"Test.lnk", ignored);
      fn::fs::remove(personalDesktop, ignored);
      fn::fs::remove(sharedDesktop, ignored);
      fn::fs::remove(directory, ignored);
    }
  }
};
struct Key {
  HKEY value = nullptr;
  ~Key() { if (value) RegCloseKey(value); }
};
bool keyExists(const std::wstring &path) {
  Key key;
  const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key.value);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return false;
  check(status == ERROR_SUCCESS, "open fixture registry key");
  return true;
}
DWORD dword(const std::wstring &path, const std::wstring &name) {
  DWORD value = 0, size = sizeof(value);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS,
        "read fixture DWORD");
  return value;
}
std::wstring string(const std::wstring &path, const wchar_t *name = nullptr) {
  wchar_t value[4096]{};
  DWORD size = sizeof(value);
  check(RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS,
        "read fixture string");
  return value;
}
void writeString(HKEY key, const wchar_t *name, const std::wstring &value) {
  check(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
        "write fixture string");
}
void desktopTest(const fn::fs::path &target, const fn::fs::path &otherTarget) {
  const auto shared = sharedDesktop / L"Test.lnk";
  std::ofstream(shared) << "Existing public shortcut";
  addCustom(Surface::Desktop, L"Test", target.native());
  addCustom(Surface::Desktop, L"Test", otherTarget.native());
  auto entries = customEntries(Surface::Desktop);
  check(entries.size() == 2, "add two desktop links with the same requested name");
  for (const auto &entry : entries) {
    const auto path = fn::fs::path(entry.shortcut);
    check(path == personalDesktop / L"Test (2).lnk" || path == personalDesktop / L"Test (3).lnk",
          "use distinct suffixed names");
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
          SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Load(path.c_str(), STGM_READ)), "create a readable shortcut");
    wchar_t actual[MAX_PATH]{};
    check(SUCCEEDED(link->GetPath(actual, _countof(actual), nullptr, SLGP_RAWPATH)) &&
          std::wstring(actual) == entry.target, "shortcut points to the chosen folder");
  }
  check(entries[0].shortcut != entries[1].shortcut, "keep both shortcuts");
  removeCustom(entries.front());
  check(!fn::fs::exists(entries.front().shortcut), "remove the owned shortcut");
  std::ofstream(fn::fs::path(entries.back().shortcut), std::ios::trunc) << "Unrelated replacement";
  check(removeAllCustom() == 0 && customEntries(Surface::Desktop).empty(), "clean up desktop records");
  check(fn::fs::exists(entries.back().shortcut) && fn::fs::exists(shared), "preserve replacement and public shortcut");
}
void run(Fixture &fixture) {
  const auto target = fixture.directory.native();
  check(customEntries(Surface::ThisPc).empty(), "empty fixture");
  addCustom(Surface::ThisPc, L"FolderNote Test", target);
  auto entries = customEntries(Surface::ThisPc);
  check(entries.size() == 1 && entries[0].target == target, "load saved This PC entry");
  auto computerEntry = entries.front();
  const auto computerClass = classes + computerEntry.id;
  check(string(thisPc + computerEntry.id) == computerEntry.name, "publish This PC namespace");
  check(string(computerClass + L"\\Instance\\InitPropertyBag", L"TargetFolderPath") == target, "register folder target");
  check(dword(computerClass, L"System.IsPinnedToNameSpaceTree") == 0, "This PC entry is not a navigation root");

  bool duplicateRejected = false;
  try { addCustom(Surface::ThisPc, L"foldernote test", target + L"\\."); }
  catch (const fn::Error &) { duplicateRejected = true; }
  check(duplicateRejected && customEntries(Surface::ThisPc).size() == 1, "reject normalized duplicate");
  addCustom(Surface::Navigation, L"FolderNote Test", target);
  entries = customEntries(Surface::Navigation);
  check(entries.size() == 1 && entries[0].id != computerEntry.id, "allow same target in a different location");
  const auto navigationEntry = entries.front();
  check(string(desktop + navigationEntry.id) == navigationEntry.name, "publish navigation root in desktop namespace");
  check(dword(hideIcons, navigationEntry.id) == 1, "exclude navigation root from actual desktop");
  check(dword(classes + navigationEntry.id, L"System.IsPinnedToNameSpaceTree") == 1, "pin navigation root");

  // Substitute someone else's registration at the same location. Removing the
  // stored entry should preserve the unrelated key because it lacks our marker.
  check(RegDeleteTreeW(HKEY_CURRENT_USER, (thisPc + computerEntry.id).c_str()) == ERROR_SUCCESS, "remove fixture marker");
  {
    Key replacement;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, (thisPc + computerEntry.id).c_str(), 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &replacement.value, nullptr) == ERROR_SUCCESS,
          "create unrelated registration");
    writeString(replacement.value, nullptr, L"Unrelated");
  }
  removeCustom(computerEntry);
  removeCustom(computerEntry);
  check(customEntries(Surface::ThisPc).empty() && !keyExists(computerClass), "remove This PC registration and record");
  check(string(thisPc + computerEntry.id) == L"Unrelated", "preserve unrelated registration");
  check(removeAllCustom() == 0 && !keyExists(classes + navigationEntry.id) && !keyExists(desktop + navigationEntry.id),
        "uninstall removes only app-owned navigation roots");
  check(!keyExists(records + navigationEntry.id) && customEntries(Surface::Navigation).empty(), "remove saved navigation record");
  desktopTest(fixture.directory, personalDesktop);
  check(fn::fs::is_directory(fixture.directory), "preserve original folder");
}
}
int main() {
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 1;
  int result = 0;
  {
    Fixture fixture;
    try {
      fixture.init();
      run(fixture);
      std::cout << "PASS isolated custom Explorer entry registration and removal\n";
    } catch (const std::exception &error) {
      std::cerr << "FAIL " << error.what() << '\n';
      result = 1;
    } catch (const fn::Error &error) {
      std::wcerr << L"FAIL " << error.message << L'\n';
      result = 1;
    }
  }
  CoUninitialize();
  return result;
}
