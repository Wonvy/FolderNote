#include "desktop_folder_links.h"
#include "core.h"
#include <algorithm>
#include <optional>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

namespace desktop_folder_links {
namespace {
constexpr wchar_t journalPath[] = L"Software\\FolderNote\\DesktopFolderLinkChanges";
constexpr wchar_t readError[] = L"无法读取桌面快捷方式记录";
constexpr wchar_t writeError[] = L"无法更改桌面快捷方式";
constexpr DWORD invalidLinkAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE;
constexpr size_t maxValueName = 16383;
constexpr DWORD linkHeaderSize = 0x4c;

struct File {
  HANDLE handle;
  explicit File(const fn::fs::path &path, DWORD access = GENERIC_READ, DWORD share = FILE_SHARE_READ)
      : handle(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)) {}
  ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
};

struct Key {
  HKEY handle = nullptr;
  explicit Key(bool write) {
    auto result = write ? RegCreateKeyExW(HKEY_CURRENT_USER, journalPath, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &handle, nullptr)
                        : RegOpenKeyExW(HKEY_CURRENT_USER, journalPath, 0, KEY_QUERY_VALUE, &handle);
    if (result != ERROR_SUCCESS && (write || result != ERROR_FILE_NOT_FOUND))
      fn::fail(write ? writeError : readError, result);
  }
  ~Key() { if (handle) RegCloseKey(handle); }
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;
};

struct Lock {
  HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\FolderNote.DesktopFolderLinks");
  Lock() {
    if (!handle) fn::fail(writeError);
    const auto result = WaitForSingleObject(handle, 5000);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return;
    const auto error = result == WAIT_FAILED ? GetLastError() : ERROR_BUSY;
    CloseHandle(handle);
    fn::fail(writeError, error);
  }
  ~Lock() { ReleaseMutex(handle); CloseHandle(handle); }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

struct SavedVisibility {
  FileIdentity identity;
  std::uint64_t hidden = 0;
};
static_assert(sizeof(SavedVisibility) == 48);

bool equal(const std::wstring &a, const std::wstring &b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

fn::fs::path desktop(REFKNOWNFOLDERID folder) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &raw))) return {};
  fn::fs::path result(raw);
  CoTaskMemFree(raw);
  return result;
}

std::optional<SavedVisibility> savedVisibility(const Key &key, const std::wstring &path) {
  if (!key.handle || path.size() > maxValueName) return {};
  SavedVisibility saved;
  DWORD bytes = sizeof(saved);
  const auto result = RegGetValueW(key.handle, nullptr, path.c_str(), RRF_RT_REG_BINARY,
                                  nullptr, &saved, &bytes);
  if (result == ERROR_FILE_NOT_FOUND) return {};
  if (result != ERROR_SUCCESS) fn::fail(readError, result);
  if (bytes != sizeof(saved) || saved.hidden > 1) fn::fail(readError, ERROR_INVALID_DATA);
  return saved;
}

struct FileState {
  FileIdentity identity;
  DWORD attributes = 0;
  bool singleLink = false;
};

bool folderShortcut(const File &file) {
  // The Shell Link header records target attributes without resolving the target.
  struct LinkHeader {
    DWORD size;
    CLSID clsid;
    DWORD flags;
    DWORD targetAttributes;
  };
  static_assert(sizeof(LinkHeader) == 28);
  constexpr CLSID shellLink = {0x00021401, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
  constexpr DWORD hasDarwinId = 0x1000;
  LinkHeader header{};
  DWORD read = 0;
  return ReadFile(file.handle, &header, sizeof(header), &read, nullptr) && read == sizeof(header) &&
      header.size == linkHeaderSize && IsEqualGUID(header.clsid, shellLink) && !(header.flags & hasDarwinId) &&
      (header.targetAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

void readMetadata(const File &file, Entry &entry) {
  constexpr LONGLONG maxLinkBytes = 1024 * 1024;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < linkHeaderSize ||
      size.QuadPart > maxLinkBytes || !SetFilePointerEx(file.handle, {}, nullptr, FILE_BEGIN))
    return;
  std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
      read != bytes.size()) return;

  // Read the same handle used for classification. Loading the saved data and
  // asking for its raw path does not require resolving or opening the target.
  Microsoft::WRL::ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(bytes.data(), read));
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  Microsoft::WRL::ComPtr<IPersistStream> persist;
  if (!stream || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
      FAILED(link.As(&persist)) || FAILED(persist->Load(stream.Get()))) return;
  std::wstring value(32768, L'\0');
  if (link->GetPath(value.data(), static_cast<int>(value.size()), nullptr, SLGP_RAWPATH) == S_OK)
    entry.target.assign(value.c_str());
  value.front() = L'\0';
  int index = 0;
  if (SUCCEEDED(link->GetIconLocation(value.data(), static_cast<int>(value.size()), &index)) && value.front())
    entry.iconLocation = std::wstring(value.c_str()) + L"," + std::to_wstring(index);
}

std::optional<FileState> inspect(const File &file) {
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO id{};
  if (file.handle == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &basic, sizeof(basic)) ||
      (basic.FileAttributes & invalidLinkAttributes) ||
      !GetFileInformationByHandleEx(file.handle, FileStandardInfo, &standard, sizeof(standard)) ||
      standard.EndOfFile.QuadPart < linkHeaderSize || !folderShortcut(file) ||
      !GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id))) return {};
  FileState state;
  state.identity.volume = id.VolumeSerialNumber;
  std::copy_n(id.FileId.Identifier, state.identity.fileId.size(), state.identity.fileId.begin());
  state.identity.created = static_cast<std::uint64_t>(basic.CreationTime.QuadPart);
  state.identity.written = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
  state.attributes = basic.FileAttributes;
  state.singleLink = standard.NumberOfLinks == 1;
  return state;
}

bool isHidden(const FileState &state) {
  return (state.attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}

void changeVisibility(const std::wstring &path, std::optional<bool> hidden, const FileIdentity *selected) {
  const auto personal = desktop(FOLDERID_Desktop);
  const auto shared = desktop(FOLDERID_PublicDesktop);
  const fn::fs::path shortcut(path);
  if (personal.empty() || equal(personal.native(), shared.native()) ||
      !equal(shortcut.parent_path().native(), personal.native()) ||
      !equal(shortcut.extension().native(), L".lnk") ||
      shortcut.filename().native().find(L':') != std::wstring::npos || path.size() > maxValueName)
    fn::fail(writeError, ERROR_INVALID_NAME);

  File file(shortcut, GENERIC_READ | FILE_WRITE_ATTRIBUTES);
  if (file.handle == INVALID_HANDLE_VALUE) fn::fail(writeError);
  const auto state = inspect(file);
  if (!state || !state->singleLink || (selected && *selected != state->identity))
    fn::fail(writeError, ERROR_FILE_INVALID);

  Key key(true);
  auto saved = savedVisibility(key, path);
  if (saved && saved->identity != state->identity) {
    if (!hidden || !selected) fn::fail(writeError, ERROR_FILE_INVALID);
    // An explicit edit of the replacement starts a new record. Restoration
    // must always match the original file identity stored in the journal.
    saved.reset();
  }
  const bool current = isHidden(*state);
  const bool original = saved ? saved->hidden != 0 : current;
  const bool desired = hidden.value_or(original);
  if (!saved && desired != original) {
    const SavedVisibility backup{state->identity, original ? 1u : 0u};
    const auto result = RegSetValueExW(key.handle, path.c_str(), 0, REG_BINARY,
        reinterpret_cast<const BYTE *>(&backup), sizeof(backup));
    if (result != ERROR_SUCCESS) fn::fail(writeError, result);
  }
  if (desired != current) {
    FILE_BASIC_INFO update{};
    update.FileAttributes = state->attributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_HIDDEN);
    if (desired) update.FileAttributes |= FILE_ATTRIBUTE_HIDDEN;
    if (!update.FileAttributes) update.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileInformationByHandle(file.handle, FileBasicInfo, &update, sizeof(update))) fn::fail(writeError);
  }
  if (saved && desired == original) {
    const auto result = RegDeleteValueW(key.handle, path.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) fn::fail(writeError, result);
  }
  if (desired != current)
    SHChangeNotify(SHCNE_ATTRIBUTES | SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}

void addDesktop(const fn::fs::path &directory, bool shared, const Key &key, std::vector<Entry> &result) {
  if (directory.empty()) return;
  std::error_code error;
  const fn::fs::directory_iterator end;
  for (auto it = fn::fs::directory_iterator(directory, error); !error && it != end; it.increment(error)) {
    const auto &path = it->path();
    if (!equal(path.extension().native(), L".lnk")) continue;
    File file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    const auto state = inspect(file);
    if (!state) continue;
    Entry entry;
    entry.path = path.native();
    entry.name = path.stem().native();
    entry.identity = state->identity;
    entry.hidden = isHidden(*state);
    entry.shared = shared;
    readMetadata(file, entry);
    if (!shared) {
      try {
        const auto saved = savedVisibility(key, entry.path);
        entry.changed = saved && saved->identity == entry.identity;
        if (state->singleLink && entry.path.size() <= maxValueName) {
          File writable(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
          entry.canChange = writable.handle != INVALID_HANDLE_VALUE;
        }
      } catch (const fn::Error &) {}
    }
    result.push_back(std::move(entry));
  }
}
} // namespace

std::vector<Entry> inventory() {
  std::vector<Entry> result;
  const auto personal = desktop(FOLDERID_Desktop);
  const auto shared = desktop(FOLDERID_PublicDesktop);
  Key key(false);
  if (!equal(personal.native(), shared.native())) addDesktop(personal, false, key, result);
  addDesktop(shared, true, key, result);
  std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) {
    return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  return result;
}

void setHidden(const Entry &entry, bool hidden) {
  if (!entry.canChange) fn::fail(writeError, ERROR_ACCESS_DENIED);
  Lock lock;
  changeVisibility(entry.path, hidden, &entry.identity);
}

void restore(const Entry &entry) {
  if (!entry.canChange) fn::fail(writeError, ERROR_ACCESS_DENIED);
  Lock lock;
  changeVisibility(entry.path, std::nullopt, &entry.identity);
}

size_t restoreAll() try {
  Lock lock;
  Key key(false);
  if (!key.handle) return 0;
  std::vector<std::wstring> paths;
  std::wstring name(maxValueName + 1, L'\0');
  for (DWORD index = 0; ; ++index) {
    auto length = static_cast<DWORD>(name.size());
    const auto result = RegEnumValueW(key.handle, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (result == ERROR_NO_MORE_ITEMS) break;
    if (result != ERROR_SUCCESS) fn::fail(readError, result);
    paths.emplace_back(name.data(), length);
  }
  const auto personal = desktop(FOLDERID_Desktop);
  size_t failed = 0;
  for (const auto &path : paths) {
    const fn::fs::path shortcut(path);
    const bool wrongFolder = !personal.empty() && !equal(shortcut.parent_path().native(), personal.native());
    const DWORD attrs = GetFileAttributesW(path.c_str());
    const DWORD err = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool gone = attrs == INVALID_FILE_ATTRIBUTES &&
                      (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
    if (wrongFolder || gone) {
      try {
        Key writable(true);
        RegDeleteValueW(writable.handle, path.c_str());
      } catch (const fn::Error &) { ++failed; }
      continue;
    }
    try { changeVisibility(path, std::nullopt, nullptr); }
    catch (const fn::Error &) { ++failed; }
  }
  return failed;
} catch (const fn::Error &) {
  return 1;
}
} // namespace desktop_folder_links
