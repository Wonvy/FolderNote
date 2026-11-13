#include "custom_explorer_entries.h"
#include "core.h"
#include "language.h"
#include "quick_folders.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <string_view>

namespace explorer_entries {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t entriesKey[] = L"Software\\FolderNote\\ExplorerEntries\\Custom";
constexpr wchar_t classesKey[] = L"Software\\Classes\\CLSID";
constexpr wchar_t thisPcKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\NameSpace";
constexpr wchar_t desktopKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace";
constexpr wchar_t hideDesktopKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel";
constexpr wchar_t ownerValue[] = L"FolderNote.CustomEntry";
constexpr wchar_t registryError[] = L"无法更新文件夹入口";
constexpr wchar_t createLinkError[] = L"无法创建桌面快捷方式";
constexpr wchar_t deleteLinkError[] = L"无法移除桌面快捷方式";

struct Handle {
  HANDLE value;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Lock {
  Handle mutex{CreateMutexW(nullptr, FALSE, L"Local\\FolderNote.CustomExplorerEntries")};
  Lock() {
    if (!mutex.value) fn::fail(registryError);
    auto result = WaitForSingleObject(mutex.value, 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
      throw fn::Error{lang::text(L"文件夹入口正在更新，请稍后重试。")};
  }
  ~Lock() { ReleaseMutex(mutex.value); }
};
struct Key {
  HKEY value = nullptr;
  Key() = default;
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;
  ~Key() { if (value) RegCloseKey(value); }
};

bool missing(LONG status) {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}
void check(LONG status) {
  if (status != ERROR_SUCCESS) fn::fail(registryError, status);
}
void checkShell(HRESULT status, const wchar_t *message) {
  if (FAILED(status)) fn::fail(message, static_cast<DWORD>(status));
}
void open(Key &key, const std::wstring &path, REGSAM access = KEY_READ) {
  auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, &key.value);
  if (!missing(status)) check(status);
}
bool create(Key &key, const std::wstring &path, REGSAM view = 0, REGSAM access = KEY_WRITE) {
  DWORD disposition = 0;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, access | view,
                       nullptr, &key.value, &disposition));
  return disposition == REG_CREATED_NEW_KEY;
}
void set(HKEY key, const wchar_t *name, DWORD value) {
  check(RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value)));
}
void set(HKEY key, const wchar_t *name, const std::wstring &value, DWORD type = REG_SZ) {
  check(RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
}
DWORD dword(HKEY key, const wchar_t *name, DWORD fallback) {
  DWORD value = 0, size = sizeof(value);
  auto status = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (missing(status)) return fallback;
  check(status);
  return value;
}
std::wstring string(HKEY key, const wchar_t *name) {
  DWORD size = 0;
  auto status = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size);
  if (missing(status)) return {};
  check(status);
  if (size > 65536) fn::fail(registryError, ERROR_INVALID_DATA);
  std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
  size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  check(RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &size));
  value.resize(wcslen(value.c_str()));
  return value;
}
std::wstring subkey(std::wstring_view path, const std::wstring &leaf) {
  return std::wstring(path) + L'\\' + leaf;
}
void deleteTree(const wchar_t *path, const std::wstring &leaf, REGSAM view = 0) {
  Key parent;
  open(parent, path, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | view);
  if (!parent.value) return;
  auto status = RegDeleteTreeW(parent.value, leaf.c_str());
  if (!missing(status)) check(status);
}
void deleteOwnedTree(const wchar_t *path, const std::wstring &id, REGSAM view = 0) {
  Key key;
  open(key, subkey(path, id), KEY_QUERY_VALUE | view);
  if (key.value && dword(key.value, ownerValue, 0) == 1) deleteTree(path, id, view);
}
bool validId(const std::wstring &id) {
  GUID parsed{};
  return id.size() == 38 && id.front() == L'{' && id.back() == L'}' &&
         SUCCEEDED(CLSIDFromString(id.c_str(), &parsed));
}
std::wstring newId() {
  GUID guid{};
  checkShell(CoCreateGuid(&guid), registryError);
  wchar_t id[39]{};
  StringFromGUID2(guid, id, _countof(id));
  return id;
}
bool validSurface(Surface surface) {
  return surface == Surface::ThisPc || surface == Surface::Navigation || surface == Surface::Desktop;
}
const wchar_t *location(Surface surface) {
  return surface == Surface::ThisPc ? thisPcKey : desktopKey;
}
std::vector<REGSAM> classViews() {
  SYSTEM_INFO info{};
  GetNativeSystemInfo(&info);
  if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
      info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) return {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
  return {0};
}
std::vector<CustomEntry> readEntries() {
  Key root;
  open(root, entriesKey);
  std::vector<CustomEntry> entries;
  if (!root.value) return entries;
  for (DWORD index = 0;; ++index) {
    wchar_t leaf[256]{};
    DWORD length = _countof(leaf);
    auto status = RegEnumKeyExW(root.value, index, leaf, &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    check(status);
    std::wstring id(leaf, length);
    if (!validId(id)) continue;
    Key record;
    open(record, subkey(entriesKey, id));
    if (!record.value) continue;
    const auto surface = static_cast<Surface>(dword(record.value, L"Surface", MAXDWORD));
    if (!validSurface(surface)) continue;
    auto name = string(record.value, L"Name");
    auto target = string(record.value, L"Target");
    if (name.empty() || target.empty()) continue;
    entries.push_back({std::move(id), std::move(name), std::move(target),
                       string(record.value, L"Shortcut"), surface});
  }
  return entries;
}
bool validTarget(const std::wstring &path) {
  if (path.empty()) return false;
  DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  // GetFileAttributes cannot query a UNC share itself.
  if (!path.starts_with(L"\\\\") || GetLastError() != ERROR_BAD_NETPATH) return false;
  Handle directory{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  BY_HANDLE_FILE_INFORMATION info{};
  return directory.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(directory.value, &info) &&
         (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
bool validName(const std::wstring &name, Surface surface) {
  if (name.empty() || name.size() > 240 || name.find_first_not_of(L" ") == name.npos ||
      std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32; })) return false;
  return surface != Surface::Desktop ||
         (name.find_first_of(L"\\/:*?\"<>|") == name.npos && name.back() != L'.' && name.back() != L' ');
}

// Use the Shell's folder instance for both namespace locations. The navigation
// entry belongs to the desktop namespace and is hidden on the actual desktop.
void registerClass(const CustomEntry &entry, REGSAM view) {
  const auto path = subkey(classesKey, entry.id);
  Key root, icon, server, instance, bag, folder;
  // Registry redirection can map both views to the same key, for example
  // when the caller uses RegOverridePredefKey for an isolated test.
  if (!create(root, path, view, KEY_WRITE | KEY_QUERY_VALUE) && dword(root.value, ownerValue, 0) != 1)
    fn::fail(registryError, ERROR_ALREADY_EXISTS);
  set(root.value, ownerValue, 1);
  set(root.value, nullptr, entry.name);
  set(root.value, L"System.IsPinnedToNameSpaceTree", entry.surface == Surface::Navigation ? 1u : 0u);
  set(root.value, L"SortOrderIndex", 0x42);
  create(icon, subkey(path, L"DefaultIcon"), view);
  set(icon.value, nullptr, L"%SystemRoot%\\System32\\imageres.dll,-3", REG_EXPAND_SZ);
  create(server, subkey(path, L"InProcServer32"), view);
  set(server.value, nullptr, L"%SystemRoot%\\System32\\shell32.dll", REG_EXPAND_SZ);
  set(server.value, L"ThreadingModel", L"Apartment");
  create(instance, subkey(path, L"Instance"), view);
  set(instance.value, L"CLSID", L"{0E5AAE11-A475-4C5B-AB00-C66DE400274E}");
  create(bag, subkey(path, L"Instance\\InitPropertyBag"), view);
  set(bag.value, L"Attributes", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY);
  set(bag.value, L"TargetFolderPath", entry.target);
  create(folder, subkey(path, L"ShellFolder"), view);
  set(folder.value, L"FolderValueFlags", 0x28);
  set(folder.value, L"Attributes", 0xF080004Du);
}
void registerFolder(const CustomEntry &entry) {
  for (auto view : classViews()) registerClass(entry, view);
  if (entry.surface == Surface::Navigation) {
    Key hidden;
    create(hidden, hideDesktopKey);
    set(hidden.value, entry.id.c_str(), 1);
  }
  Key root;
  if (!create(root, subkey(location(entry.surface), entry.id))) fn::fail(registryError, ERROR_ALREADY_EXISTS);
  set(root.value, ownerValue, 1);
  set(root.value, nullptr, entry.name);
}
void unregisterFolder(const CustomEntry &entry) {
  deleteOwnedTree(location(entry.surface), entry.id);
  for (auto view : classViews()) deleteOwnedTree(classesKey, entry.id, view);
  if (entry.surface == Surface::Navigation) {
    Key hidden;
    open(hidden, hideDesktopKey, KEY_SET_VALUE);
    if (!hidden.value) return;
    auto status = RegDeleteValueW(hidden.value, entry.id.c_str());
    if (!missing(status)) check(status);
  }
}

fn::fs::path knownPath(REFKNOWNFOLDERID folder) {
  PWSTR path = nullptr;
  auto result = SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &path);
  fn::fs::path value = SUCCEEDED(result) && path ? path : L"";
  CoTaskMemFree(path);
  return value;
}
std::wstring linkDescription(const CustomEntry &entry) {
  return L"FolderNote " + entry.id;
}
ComPtr<IShellLinkW> shellLink(const wchar_t *message) {
  ComPtr<IShellLinkW> link;
  checkShell(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), message);
  return link;
}
ComPtr<IStream> memoryStream(const wchar_t *message) {
  ComPtr<IStream> stream;
  checkShell(CreateStreamOnHGlobal(nullptr, TRUE, &stream), message);
  return stream;
}
std::vector<BYTE> serializeLink(const CustomEntry &entry) {
  auto link = shellLink(createLinkError);
  checkShell(link->SetPath(entry.target.c_str()), createLinkError);
  checkShell(link->SetDescription(linkDescription(entry).c_str()), createLinkError);
  ComPtr<IPersistStream> persist;
  checkShell(link.As(&persist), createLinkError);
  auto stream = memoryStream(createLinkError);
  checkShell(persist->Save(stream.Get(), TRUE), createLinkError);
  STATSTG stat{};
  checkShell(stream->Stat(&stat, STATFLAG_NONAME), createLinkError);
  if (!stat.cbSize.LowPart || stat.cbSize.HighPart) fn::fail(createLinkError, ERROR_INVALID_DATA);
  std::vector<BYTE> bytes(stat.cbSize.LowPart);
  ULONG read = 0;
  checkShell(stream->Seek({}, STREAM_SEEK_SET, nullptr), createLinkError);
  checkShell(stream->Read(bytes.data(), stat.cbSize.LowPart, &read), createLinkError);
  if (read != stat.cbSize.LowPart) fn::fail(createLinkError, ERROR_HANDLE_EOF);
  return bytes;
}
bool fileExists(const fn::fs::path &path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool deleteByHandle(HANDLE file) {
  FILE_DISPOSITION_INFO remove{TRUE};
  return SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove)) != FALSE;
}
void createDesktopLink(CustomEntry &entry, HKEY record) {
  const auto desktop = knownPath(FOLDERID_Desktop);
  if (desktop.empty()) fn::fail(createLinkError, ERROR_PATH_NOT_FOUND);
  const auto publicDesktop = knownPath(FOLDERID_PublicDesktop);
  const auto bytes = serializeLink(entry);
  for (unsigned attempt = 1; attempt <= 1000; ++attempt) {
    const auto name = attempt == 1 ? entry.name : entry.name + L" (" + std::to_wstring(attempt) + L")";
    const auto leaf = name + L".lnk";
    if (fileExists(desktop / leaf) || fileExists(desktop / name) || (!publicDesktop.empty() &&
        (fileExists(publicDesktop / leaf) || fileExists(publicDesktop / name)))) continue;
    entry.shortcut = (desktop / leaf).native();
    set(record, L"Shortcut", entry.shortcut);
    Handle file{CreateFileW(entry.shortcut.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
      auto error = GetLastError();
      if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
      fn::fail(createLinkError, error);
    }
    DWORD written = 0;
    const auto complete = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    if (!complete || written != bytes.size()) {
      const auto error = complete ? ERROR_WRITE_FAULT : GetLastError();
      deleteByHandle(file.value);
      fn::fail(createLinkError, error);
    }
    return;
  }
  fn::fail(createLinkError, ERROR_FILE_EXISTS);
}
bool linkMatches(HANDLE file, const CustomEntry &entry) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) fn::fail(deleteLinkError);
  if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY) ||
      info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow > 1024 * 1024) return false;
  std::vector<BYTE> bytes(info.nFileSizeLow);
  DWORD read = 0;
  if (!ReadFile(file, bytes.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow)
    fn::fail(deleteLinkError, ERROR_READ_FAULT);
  auto stream = memoryStream(deleteLinkError);
  checkShell(stream->Write(bytes.data(), read, nullptr), deleteLinkError);
  checkShell(stream->Seek({}, STREAM_SEEK_SET, nullptr), deleteLinkError);
  auto link = shellLink(deleteLinkError);
  ComPtr<IPersistStream> persist;
  checkShell(link.As(&persist), deleteLinkError);
  if (FAILED(persist->Load(stream.Get()))) return false;
  wchar_t description[128]{};
  return SUCCEEDED(link->GetDescription(description, _countof(description))) && description == linkDescription(entry);
}
void removeDesktopLink(const CustomEntry &entry) {
  if (entry.shortcut.empty() || !quick::same(fn::fs::path(entry.shortcut).extension().native(), L".lnk")) return;
  // Check ownership and delete through one handle, so an unrelated replacement
  // cannot be deleted between checking the description and removing the link.
  Handle file{CreateFileW(entry.shortcut.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE) {
    auto error = GetLastError();
    if (!missing(static_cast<LONG>(error))) fn::fail(deleteLinkError, error);
    return;
  }
  if (linkMatches(file.value, entry) && !deleteByHandle(file.value)) fn::fail(deleteLinkError);
}
void notify(const CustomEntry &entry, bool added) {
  if (entry.surface == Surface::Desktop) {
    if (!entry.shortcut.empty())
      SHChangeNotify(added ? SHCNE_CREATE : SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                     entry.shortcut.c_str(), nullptr);
    return;
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
  refresh(entry.surface);
}
void remove(const CustomEntry &entry) {
  if (entry.surface == Surface::Desktop) removeDesktopLink(entry);
  else unregisterFolder(entry);
  deleteTree(entriesKey, entry.id);
}
}

std::vector<CustomEntry> customEntries(Surface surface) {
  Lock lock;
  auto entries = readEntries();
  std::erase_if(entries, [&](const auto &entry) { return entry.surface != surface; });
  return entries;
}
void addCustom(Surface surface, const std::wstring &name, const std::wstring &target) {
  const auto path = quick::normalize(target);
  if (!validSurface(surface) || !validTarget(path))
    throw fn::Error{lang::text(L"请选择可访问的文件夹。")};
  if (!validName(name, surface)) throw fn::Error{lang::text(L"请输入有效的入口名称。")};
  CustomEntry entry{newId(), name, path, {}, surface};
  {
    Lock lock;
    auto entries = readEntries();
    if (std::any_of(entries.begin(), entries.end(), [&](const auto &existing) {
          return existing.surface == surface && quick::same(existing.target, path) && quick::same(existing.name, name);
        })) throw fn::Error{lang::text(L"此位置已添加相同的文件夹入口。")};
    Key record;
    if (!create(record, subkey(entriesKey, entry.id))) fn::fail(registryError, ERROR_ALREADY_EXISTS);
    try {
      set(record.value, L"Name", entry.name);
      set(record.value, L"Target", entry.target);
      set(record.value, L"Surface", static_cast<DWORD>(surface));
      if (surface == Surface::Desktop) createDesktopLink(entry, record.value);
      else registerFolder(entry);
    } catch (...) {
      // The record exists before any shell changes, so a failed cleanup can be
      // retried from the manager or during uninstall.
      try { remove(entry); } catch (...) {}
      throw;
    }
  }
  notify(entry, true);
}
void removeCustom(const CustomEntry &entry) {
  CustomEntry removed;
  {
    Lock lock;
    const auto entries = readEntries();
    auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &stored) {
      return stored.surface == entry.surface && quick::same(stored.id, entry.id);
    });
    if (found == entries.end()) return;
    removed = *found;
    remove(removed);
  }
  notify(removed, false);
}
size_t removeAllCustom() {
  Lock lock;
  size_t remaining = 0;
  for (const auto &entry : readEntries()) {
    try {
      remove(entry);
      notify(entry, false);
    } catch (const fn::Error &) {
      ++remaining;
    }
  }
  return remaining;
}
}
