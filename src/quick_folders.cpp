#include "quick_folders.h"
#include "language.h"
#include "navigation.h"
#include "navigation_registration.h"
#include "core.h"
#include <wrl/client.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <memory>
#include <optional>
#include <sddl.h>
namespace quick {
namespace {
constexpr wchar_t dataKey[] = L"Software\\FolderNote\\QuickFolders";
constexpr size_t frequencyLimit = 256;
constexpr size_t registryValueLimit = 1024 * 1024;
struct Key {
  HKEY value = nullptr;
  ~Key() { if (value) RegCloseKey(value); }
};
struct Lock {
  HANDLE handle = nullptr;
  Lock() {
    // A user-specific mutex protects read/modify/write across Explorer processes.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) fn::fail(lang::text(L"无法读取用户信息"));
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    BOOL ok = GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid))
      fn::fail(lang::text(L"无法读取用户信息"));
    auto name = std::wstring(L"Local\\FolderNote.QuickFolders.") + sid;
    LocalFree(sid);
    handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!handle) fn::fail(lang::text(L"无法读取快速文件夹"));
    DWORD result = WaitForSingleObject(handle, 1000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
      CloseHandle(handle); handle = nullptr;
      throw fn::Error{lang::text(L"快速文件夹正在更新，请稍后重试。")};
    }
  }
  ~Lock() { if (handle) { ReleaseMutex(handle); CloseHandle(handle); } }
};
std::vector<std::wstring> readStrings(const wchar_t *name) {
  DWORD type = 0, size = 0;
  auto result = RegGetValueW(HKEY_CURRENT_USER, dataKey, name, RRF_RT_ANY, &type, nullptr, &size);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
  if (result) fn::fail(lang::text(L"无法读取快速文件夹"), result);
  if (type != REG_MULTI_SZ || size < 4 || size % 2 || size > registryValueLimit)
    throw fn::Error{lang::text(L"快速文件夹数据格式损坏，未覆盖原记录。")};
  std::vector<wchar_t> bytes(size / 2 + 2);
  result = RegGetValueW(HKEY_CURRENT_USER, dataKey, name, RRF_RT_REG_MULTI_SZ, nullptr, bytes.data(), &size);
  if (result) fn::fail(lang::text(L"无法读取快速文件夹"), result);
  const size_t count = size / 2;
  if (count < 2 || bytes[count - 1] || bytes[count - 2])
    throw fn::Error{lang::text(L"快速文件夹数据不完整，未覆盖原记录。")};
  std::vector<std::wstring> out;
  for (size_t i = 0; i < count && bytes[i];) {
    const size_t begin = i;
    while (i < count && bytes[i]) ++i;
    if (i == count) throw fn::Error{lang::text(L"快速文件夹数据不完整。")};
    out.emplace_back(bytes.data() + begin, i - begin);
    ++i;
  }
  return out;
}
std::vector<std::wstring> read(const wchar_t *name, size_t limit, bool *truncated = nullptr) {
  if (truncated) *truncated = false;
  std::vector<std::wstring> out;
  for (const auto &entry : readStrings(name)) {
    auto path = normalize(entry);
    if (!path.empty() && std::none_of(out.begin(), out.end(), [&](const auto &p) { return same(p, path); })) {
      if (out.size() < limit) out.push_back(std::move(path));
      else if (truncated) *truncated = true;
    }
  }
  return out;
}
void write(const wchar_t *name, const std::vector<std::wstring> &paths) {
  std::vector<wchar_t> bytes;
  for (const auto &path : paths) { bytes.insert(bytes.end(), path.begin(), path.end()); bytes.push_back(0); }
  bytes.push_back(0);
  if (paths.empty()) bytes.push_back(0);
  if (bytes.size() * sizeof(wchar_t) > registryValueLimit) throw fn::Error{lang::text(L"文件夹列表过大，请移除部分记录。")};
  Key key;
  auto error = RegCreateKeyExW(HKEY_CURRENT_USER, dataKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value, nullptr);
  if (!error) error = RegSetValueExW(key.value, name, 0, REG_MULTI_SZ,
                                    reinterpret_cast<const BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size() * 2));
  if (error) fn::fail(lang::text(L"无法保存快速文件夹"), error);
}
struct Frequency {
  std::wstring path;
  unsigned long count;
  std::wstring encoded() const { return std::to_wstring(count) + L'\t' + path; }
};
std::vector<Frequency> readFrequency() {
  std::vector<Frequency> entries;
  for (const auto &entry : readStrings(L"VisitFrequency")) {
    // A tab is invalid in a normalized path, so it can delimit the count.
    auto separator = entry.find(L'\t');
    if (separator == entry.npos || entry.find_first_not_of(L"0123456789") != separator)
      throw fn::Error{lang::text(L"访问次数数据格式损坏，未覆盖原记录。")};
    errno = 0;
    auto count = wcstoul(entry.c_str(), nullptr, 10);
    auto path = normalize(entry.substr(separator + 1));
    if (!count || errno == ERANGE || path.empty() || (!entries.empty() && entries.back().count < count))
      throw fn::Error{lang::text(L"访问次数数据格式损坏，未覆盖原记录。")};
    entries.push_back({std::move(path), count});
  }
  return entries;
}
void recordFrequency(std::vector<Frequency> entries, const std::wstring &path) {
  auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return same(entry.path, path); });
  unsigned long count = 1;
  if (found != entries.end()) {
    count = found->count == ULONG_MAX ? ULONG_MAX : found->count + 1;
    entries.erase(found);
  }
  // Higher counts come first. A newly visited folder leads others with the same count.
  auto position = std::lower_bound(entries.begin(), entries.end(), count,
      [](const auto &entry, auto visits) { return entry.count > visits; });
  const auto updated = entries.insert(position, {path, count});
  std::vector<std::wstring> values;
  auto updatedValue = updated->encoded();
  // Reserve space for the latest visit so a new folder can build a count even
  // when the list already contains the maximum number of folders.
  size_t remainingEntries = frequencyLimit - 1;
  size_t remainingChars = registryValueLimit / sizeof(wchar_t) - updatedValue.size() - 2;
  for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
    if (entry == updated) values.push_back(std::move(updatedValue));
    else if (remainingEntries && entry->path.size() < remainingChars) {
      auto value = entry->encoded();
      if (value.size() >= remainingChars) continue;
      remainingChars -= value.size() + 1;
      --remainingEntries;
      values.push_back(std::move(value));
    }
  }
  write(L"VisitFrequency", values);
}
std::atomic<unsigned> pending{0};
DWORD generation() {
  DWORD value = 0, bytes = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, dataKey, L"HistoryGeneration", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
  return value;
}
void storeVisit(const std::wstring &value, bool countVisit, std::optional<DWORD> expected = std::nullopt) {
  auto path = normalize(value);
  if (path.empty()) return;
  Lock lock;
  if (expected && (generation() != *expected || !navigation::quickEnabled())) return;
  bool truncated = false;
  auto items = read(L"History", historyLimit, &truncated);
  auto frequency = countVisit ? readFrequency() : std::vector<Frequency>{};
  // Also shorten an older installation's longer history when its current
  // directory was already first, rather than retaining the old 200 entries.
  if (truncated || items.empty() || !same(items.front(), path)) {
    std::erase_if(items, [&](const auto &p) { return same(p, path); });
    items.insert(items.begin(), path);
    if (items.size() > historyLimit) items.resize(historyLimit);
    write(L"History", items);
  }
  if (countVisit) recordFrequency(std::move(frequency), path);
}
struct Visit { std::wstring path; HMODULE module; DWORD generation; };
void CALLBACK record(PTP_CALLBACK_INSTANCE instance, void *argument) {
  std::unique_ptr<Visit> request(static_cast<Visit *>(argument));
  FreeLibraryWhenCallbackReturns(instance, request->module);
  try { storeVisit(request->path, false, request->generation); } catch (...) { }
  --pending;
}
}
bool same(const std::wstring &a, const std::wstring &b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::wstring normalize(const std::wstring &input) {
  if (input.empty() || input.size() > 32760) return {};
  auto p = input;
  std::replace(p.begin(), p.end(), L'/', L'\\');
  if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
  else if (p.starts_with(L"\\\\?\\")) p.erase(0, 4);
  for (wchar_t c : p) if (c < 32 || wcschr(L"\"<>|*?", c)) return {};
  const bool drive = p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\';
  const bool unc = p.starts_with(L"\\\\") && p.size() > 4 && p[2] != L'.';
  if (!drive && !unc) return {};
  if (p.find(L':', drive ? 2 : 0) != p.npos) return {};
  size_t anchor = 3;
  if (unc) {
    auto separator = p.find(L'\\', 2);
    if (separator == p.npos || separator == 2 || separator + 1 == p.size()) return {};
    auto end = p.find(L'\\', separator + 1);
    anchor = end == p.npos ? p.size() : end;
    auto share = p.substr(separator + 1, anchor - separator - 1);
    if (share.empty() || share == L"." || share == L"..") return {};
  }
  auto root = p.substr(0, anchor);
  std::vector<std::wstring> parts;
  for (size_t i = anchor; i < p.size();) {
    auto end = p.find(L'\\', i);
    if (end == p.npos) end = p.size();
    auto part = p.substr(i, end - i); i = end + 1;
    if (part.empty() || part == L".") continue;
    if (part == L"..") { if (!parts.empty()) parts.pop_back(); continue; }
    parts.push_back(part);
  }
  for (const auto &part : parts) { if (root.back() != L'\\') root += L'\\'; root += part; }
  return root;
}
State load() {
  Lock lock;
  State state{read(L"Pins", pinLimit), read(L"History", historyLimit), {}};
  for (auto &entry : readFrequency()) state.frequent.push_back(std::move(entry.path));
  return state;
}
void visit(const std::wstring &value) {
  storeVisit(value, true);
}
void touchHistory(const std::wstring &path) {
  storeVisit(path, false);
}
void visitAsync(const std::wstring &path) noexcept {
  visitAsync(path, generation());
}
DWORD historyGeneration() { return generation(); }
void visitAsync(const std::wstring &path, DWORD expectedGeneration) noexcept {
  if (path.empty() || pending.fetch_add(1) >= 16) { if (!path.empty()) --pending; return; }
  HMODULE module = nullptr;
  try {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&record), &module)) { --pending; return; }
    auto item = std::make_unique<Visit>(Visit{path, module, expectedGeneration});
    if (TrySubmitThreadpoolCallback(record, item.get(), nullptr)) { item.release(); return; }
  } catch (...) { }
  if (module) FreeLibrary(module);
  --pending;
}
void pin(const std::wstring &value, bool enabled) {
  auto path = normalize(value);
  if (path.empty()) throw fn::Error{lang::text(L"请选择有效的文件夹路径。")};
  Lock lock;
  auto items = read(L"Pins", pinLimit);
  auto found = std::find_if(items.begin(), items.end(), [&](const auto &p) { return same(p, path); });
  if (enabled) {
    if (found != items.end()) return;
    if (items.size() >= pinLimit) throw fn::Error{lang::text(L"最多可置顶 64 个文件夹。")};
    items.push_back(path);
  } else if (found != items.end()) items.erase(found);
  write(L"Pins", items);
}
void movePin(const std::wstring &path, int direction) {
  Lock lock;
  auto items = read(L"Pins", pinLimit);
  for (size_t i = 0; i < items.size(); ++i) if (same(items[i], path)) {
    if (direction == -1 && i > 0) std::swap(items[i], items[i - 1]);
    else if (direction == 1 && i + 1 < items.size()) std::swap(items[i], items[i + 1]);
    else return;
    write(L"Pins", items); return;
  }
}
void clearHistory() {
  Lock lock;
  DWORD next = generation() + 1;
  auto error = RegSetKeyValueW(HKEY_CURRENT_USER, dataKey, L"HistoryGeneration", REG_DWORD, &next, sizeof(next));
  if (error) fn::fail(lang::text(L"无法清空历史记录"), error);
  write(L"History", {});
  write(L"VisitFrequency", {});
}
std::wstring viewPath(IShellView *view) {
  using Microsoft::WRL::ComPtr;
  if (!view) return {};
  ComPtr<IFolderView> folder;
  ComPtr<IPersistFolder2> persistent;
  if (FAILED(view->QueryInterface(IID_PPV_ARGS(&folder))) || !folder ||
      FAILED(folder->GetFolder(IID_PPV_ARGS(&persistent))) || !persistent) return {};
  PIDLIST_ABSOLUTE id = nullptr;
  if (FAILED(persistent->GetCurFolder(&id)) || !id) { CoTaskMemFree(id); return {}; }
  std::wstring path(32768, 0);
  bool ok = SHGetPathFromIDListEx(id, path.data(), static_cast<DWORD>(path.size()), GPFIDL_DEFAULT) != FALSE;
  CoTaskMemFree(id);
  if (!ok) return {};
  path.resize(wcslen(path.c_str()));
  return normalize(path);
}
std::wstring executable() {
  std::wstring dll(32768, L'\0');
  DWORD bytes = static_cast<DWORD>(dll.size() * sizeof(wchar_t));
  auto key = std::wstring(navreg::userKey) + L"\\InprocServer32";
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, dll.data(), &bytes) == ERROR_SUCCESS) {
    dll.resize(wcslen(dll.c_str()));
    auto exe = std::filesystem::path(dll).parent_path() / L"FolderNote.exe";
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return exe.native();
  }
  auto process = fn::modulePath();
  if (_wcsicmp(process.filename().c_str(), L"FolderNote.exe") == 0) return process.native();
  throw fn::Error{lang::text(L"未找到已安装的 FolderNote，请重新安装后再试。")};
}
void launchMenu(HWND target, HWND view, POINT point) {
  auto exe = executable();
  auto command = L"\"" + exe + L"\" --quick-menu " + std::to_wstring(reinterpret_cast<UINT_PTR>(target)) +
                 L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(view)) + L" " + std::to_wstring(point.x) + L" " + std::to_wstring(point.y);
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    fn::fail(lang::text(L"无法打开快速文件夹菜单"));
  AllowSetForegroundWindow(process.dwProcessId);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
}
