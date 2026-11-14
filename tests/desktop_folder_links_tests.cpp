#include "../src/core.h"
#include "shell_paths.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <shlobj.h>
#include <stdexcept>
#include <wrl/client.h>

namespace {
fn::fs::path personalDesktop, publicDesktop;

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

HRESULT testKnownFolder(REFKNOWNFOLDERID folder, DWORD, HANDLE, PWSTR *path) {
  const auto &value = folder == FOLDERID_Desktop ? personalDesktop : publicDesktop;
  const auto bytes = (value.native().size() + 1) * sizeof(wchar_t);
  *path = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
  if (!*path) return E_OUTOFMEMORY;
  memcpy(*path, value.c_str(), bytes);
  return S_OK;
}

void testNotify(LONG, UINT, LPCVOID, LPCVOID) {}
} // namespace

#define SHGetKnownFolderPath testKnownFolder
#define SHChangeNotify testNotify
#include "../src/desktop_folder_links.cpp"
#undef SHChangeNotify
#undef SHGetKnownFolderPath

namespace {
struct Fixture {
  HKEY user = nullptr, registry = nullptr;
  std::wstring key;
  fn::fs::path root;

  Fixture() {
    GUID id{};
    check(SUCCEEDED(CoCreateGuid(&id)), "allocate unique fixture id");
    wchar_t token[40]{};
    check(StringFromGUID2(id, token, ARRAYSIZE(token)) != 0, "format fixture id");
    root = fn::fs::temp_directory_path() / (L"FolderNoteDesktopLinks-" + std::wstring(token));
    personalDesktop = root / L"Desktop";
    publicDesktop = root / L"PublicDesktop";
    fn::fs::create_directories(personalDesktop);
    fn::fs::create_directories(publicDesktop);
    key = std::wstring(L"Software\\FolderNoteDesktopLinkTests\\") + token;
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &user) == ERROR_SUCCESS &&
          RegCreateKeyExW(user, key.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                          &registry, nullptr) == ERROR_SUCCESS &&
          RegOverridePredefKey(HKEY_CURRENT_USER, registry) == ERROR_SUCCESS, "isolate shortcut journal");
  }

  ~Fixture() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(registry);
    RegDeleteTreeW(user, key.c_str());
    RegCloseKey(user);
    std::error_code error;
    fn::fs::remove_all(root, error);
  }
};

void shortcut(const fn::fs::path &path, const fn::fs::path &target) {
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(target.c_str())), "create shell link target");
  check(SUCCEEDED(link->SetIconLocation(L"C:\\Fixture\\folder.ico", -17)), "set the link's custom icon");
  Microsoft::WRL::ComPtr<IPersistFile> persist;
  check(SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Save(path.c_str(), TRUE)), "save shell link");
}

desktop_folder_links::Entry entry(const fn::fs::path &path) {
  const auto list = desktop_folder_links::inventory();
  const auto found = std::find_if(list.begin(), list.end(), [&](const auto &item) { return item.path == path.native(); });
  check(found != list.end(), "find expected shortcut");
  return *found;
}

bool hidden(const fn::fs::path &path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  check(attributes != INVALID_FILE_ATTRIBUTES, "shortcut still exists");
  return (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}

void exercise(const Fixture &fixture) {
  using namespace desktop_folder_links;
  const auto target = fixture.root / L"target";
  fn::fs::create_directory(target);
  const auto visible = personalDesktop / L"Folder.lnk";
  const auto initiallyHidden = personalDesktop / L"Initially hidden.lnk";
  const auto shared = publicDesktop / L"Shared folder.lnk";
  const auto fileLink = personalDesktop / L"File.lnk";
  const auto text = fixture.root / L"target.txt";
  std::ofstream(text) << "text";
  for (const auto &path : {visible, initiallyHidden, shared}) shortcut(path, target);
  shortcut(fileLink, text);
  check(SetFileAttributesW(initiallyHidden.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE), "seed hidden shortcut");
  check(SetFileAttributesW(visible.c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED), "seed unrelated attributes");
  const auto original = GetFileAttributesW(visible.c_str());
  check(inventory().size() == 3, "include only folder shortcuts");
  check(entry(shared).shared && !entry(shared).canChange, "public desktop shortcut is read-only");

  const auto first = entry(visible);
  check(first.canChange && !first.hidden && !first.changed, "personal folder shortcut is actionable");
  check(sameShellTestPath(first.target, target.native()) &&
        first.iconLocation == L"C:\\Fixture\\folder.ico,-17",
        "read the stored target and icon location from the folder link");
  setHidden(first, true);
  check(entry(visible).hidden && entry(visible).changed &&
        GetFileAttributesW(visible.c_str()) == (original | FILE_ATTRIBUTE_HIDDEN), "hide preserves other attributes");
  restore(entry(visible));
  check(!entry(visible).hidden && !entry(visible).changed && GetFileAttributesW(visible.c_str()) == original,
        "restore original visibility and attributes");

  setHidden(entry(initiallyHidden), false);
  setHidden(entry(visible), true);
  check(!hidden(initiallyHidden) && hidden(visible), "show and hide independent links");
  check(restoreAll() == 0 && hidden(initiallyHidden) && !hidden(visible), "restore all remembers both original states");

  setHidden(entry(visible), true);
  const auto old = entry(visible);
  const auto relocated = fixture.root / L"old.lnk";
  fn::fs::rename(visible, relocated);
  shortcut(visible, target);
  bool refused = false;
  try { restore(old); }
  catch (const fn::Error &) { refused = true; }
  check(refused && !entry(visible).changed && restoreAll() == 1 && !hidden(visible) && hidden(relocated),
        "stale journal never changes a replacement file");
  setHidden(entry(visible), true);
  check(restoreAll() == 0 && !hidden(visible), "an explicit edit manages the replacement independently");
}
} // namespace

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  int result = 0;
  try {
    const Fixture fixture;
    exercise(fixture);
  } catch (const fn::Error &error) {
    std::wcerr << error.message << L'\n';
    result = 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  CoUninitialize();
  return result;
}
