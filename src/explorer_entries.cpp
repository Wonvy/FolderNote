#include "explorer_entries.h"
#include "explorer_refresh.h"
#include "core.h"
#include "language.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <shlobj.h>
#include <shlwapi.h>
#include <string_view>
#include <utility>

namespace explorer_entries {
namespace {
constexpr wchar_t explorer[] = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\)";
constexpr wchar_t classes[] = LR"(Software\Classes\CLSID\)";
constexpr wchar_t changes[] = LR"(Software\FolderNote\ExplorerEntryChanges\)";
constexpr wchar_t pinValue[] = L"System.IsPinnedToNameSpaceTree";
constexpr wchar_t nonEnum[] = LR"(Software\Microsoft\Windows\CurrentVersion\Policies\NonEnum)";
constexpr wchar_t computer[] = L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
constexpr wchar_t network[] = L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}";
constexpr wchar_t libraries[] = L"{031E4825-7B94-4DC3-B131-E946B44C8DD5}";
constexpr wchar_t home[] = L"{F874310E-B6B7-47DC-BC84-B9E6B38F5903}";
constexpr wchar_t gallery[] = L"{E88865EA-0E1C-4E20-9AA6-EDCD0212C87C}";
constexpr wchar_t oneDrive[] = L"{018D5C66-4533-4307-9B53-224DE2ED1FE6}";
constexpr std::array surfaces{Surface::ThisPc, Surface::Navigation, Surface::Desktop};
constexpr std::array mounts{L"MyComputer\\NameSpace", L"MyComputer\\NameSpace_36354489"};
constexpr wchar_t hiddenPrefix[] = L"FolderNote.Hidden.";

struct Key {
  HKEY handle = nullptr;
  Key() = default;
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;
  Key(Key &&other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
  ~Key() { if (handle) RegCloseKey(handle); }
};

bool missing(LSTATUS status) {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

Key open(HKEY root, const std::wstring &path, REGSAM access = KEY_READ) {
  Key result;
  if (root) RegOpenKeyExW(root, path.c_str(), 0, access | KEY_WOW64_64KEY, &result.handle);
  return result;
}

Key openForChange(const std::wstring &path, bool create) {
  Key result;
  constexpr REGSAM access = KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY;
  const auto status = create
      ? RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, access, nullptr,
                        &result.handle, nullptr)
      : RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, &result.handle);
  if (status && (create || !missing(status))) fn::fail(L"无法修改资源管理器入口", status);
  return result;
}

std::wstring guid(std::wstring_view text) {
  if (text.size() != 38 || text.front() != L'{' || text.back() != L'}') return {};
  std::wstring result(text);
  for (size_t index = 1; index < result.size() - 1; ++index) {
    auto &digit = result[index];
    if (index == 9 || index == 14 || index == 19 || index == 24) {
      if (digit != L'-') return {};
    } else {
      if (digit >= L'a' && digit <= L'f') digit -= L'a' - L'A';
      if (!(digit >= L'0' && digit <= L'9') && !(digit >= L'A' && digit <= L'F')) return {};
    }
  }
  return result;
}

std::vector<std::wstring> ids(HKEY key, bool values = false) {
  std::vector<std::wstring> result;
  if (!key) return result;
  for (DWORD index = 0;; ++index) {
    wchar_t name[256]{};
    DWORD length = ARRAYSIZE(name);
    const auto status = values ? RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr)
                               : RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status == ERROR_MORE_DATA) continue;
    if (status != ERROR_SUCCESS) fn::fail(L"无法读取资源管理器入口", status);
    if (auto id = guid(std::wstring_view(name, length)); !id.empty())
      result.push_back(std::move(id));
  }
  return result;
}

std::optional<DWORD> dword(HKEY key, const wchar_t *name) {
  if (!key) return {};
  DWORD data = 0, size = sizeof(data);
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return {};
  return data;
}

std::wstring stringValue(HKEY key, const wchar_t *name = nullptr) {
  if (!key) return {};
  wchar_t data[2048]{};
  DWORD size = sizeof(data);
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                   nullptr, data, &size) != ERROR_SUCCESS) return {};
  return data;
}

std::wstring caption(std::wstring value) {
  if (value.empty() || value.front() != L'@') return value;
  wchar_t data[1024]{};
  if (SUCCEEDED(SHLoadIndirectString(value.c_str(), data, ARRAYSIZE(data), nullptr))) return data;
  return {};
}

std::wstring path(const wchar_t *suffix) { return std::wstring(explorer) + suffix; }
std::wstring classPath(const std::wstring &id) { return std::wstring(classes) + id; }
std::wstring explorerClass(const std::wstring &id) { return path(L"CLSID\\") + id; }

const wchar_t *surfaceName(Surface surface) {
  switch (surface) {
    case Surface::ThisPc: return L"ThisPC";
    case Surface::Navigation: return L"Navigation";
    case Surface::Desktop: return L"Desktop";
  }
  fn::fail(L"资源管理器入口位置无效", ERROR_INVALID_PARAMETER);
}

std::wstring journalPath(Surface surface) { return std::wstring(changes) + surfaceName(surface); }

std::wstring overridePath(Surface surface, const std::wstring &id, bool suppressEnumeration = false) {
  if (surface == Surface::ThisPc && suppressEnumeration) return nonEnum;
  switch (surface) {
    case Surface::ThisPc: return path(L"HideMyComputerIcons");
    case Surface::Navigation: return classPath(id);
    case Surface::Desktop: return path(L"HideDesktopIcons\\NewStartPanel");
  }
  fn::fail(L"资源管理器入口位置无效", ERROR_INVALID_PARAMETER);
}

const wchar_t *overrideName(Surface surface, const std::wstring &id) {
  return surface == Surface::Navigation ? pinValue : id.c_str();
}

struct SavedValue {
  DWORD version = 1;
  DWORD hasOriginal = 0;
  DWORD original = 0;
  DWORD expected = 0;
  DWORD createdClass = 0;
  // Keep a recoverable previous app value while a subsequent write is pending.
  DWORD previousExpected = 0;
  DWORD pending = 0;
  DWORD suppressEnumeration = 0;
};

// Version 2 uses the same binary shape. The last field is a mask of
// renamed This PC junctions (two branches per registry hive).

std::optional<SavedValue> savedValue(HKEY key, const std::wstring &id) {
  if (!key) return {};
  SavedValue saved;
  DWORD size = sizeof(saved);
  auto status = RegGetValueW(key, nullptr, id.c_str(), RRF_RT_REG_BINARY, nullptr, &saved, &size);
  if (missing(status)) return {};
  if (status) fn::fail(L"无法读取入口原始设置", status);
  if (size != sizeof(saved) || (saved.version != 1 && saved.version != 2) || saved.hasOriginal > 1 ||
      saved.expected > 1 || saved.createdClass > 1 || saved.previousExpected > 1 || saved.pending > 1 ||
      saved.suppressEnumeration > (saved.version == 1 ? 1u : 15u))
    fn::fail(L"入口原始设置格式无效", ERROR_INVALID_DATA);
  return saved;
}

void save(HKEY key, const std::wstring &id, const SavedValue &saved) {
  auto status = RegSetValueExW(key, id.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE *>(&saved), sizeof(saved));
  if (status) fn::fail(L"无法保存入口原始设置", status);
}

void erase(HKEY key, const wchar_t *name) {
  auto status = RegDeleteValueW(key, name);
  if (status && !missing(status)) fn::fail(L"无法恢复资源管理器入口", status);
}

std::optional<DWORD> readOverride(HKEY key, const wchar_t *name) {
  if (!key) return {};
  DWORD value = 0, size = sizeof(value);
  const auto status = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (missing(status)) return {};
  if (status) fn::fail(L"无法读取当前入口设置", status);
  return value;
}

void writeOverride(HKEY key, const wchar_t *name, DWORD value) {
  auto status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
  if (status) fn::fail(L"无法修改资源管理器入口", status);
}

struct Lock {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FolderNote.ExplorerEntryChanges.v1");
  Lock() {
    if (!mutex) fn::fail(L"无法锁定入口设置");
    const auto result = WaitForSingleObject(mutex, 5000);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return;
    const auto error = result == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError();
    CloseHandle(mutex);
    fn::fail(L"无法锁定入口设置", error);
  }
  ~Lock() { ReleaseMutex(mutex); CloseHandle(mutex); }
};

struct Candidate {
  std::wstring label;
  bool saved = false;
};
using Candidates = std::map<std::wstring, Candidate>;

void discover(Candidates &candidates, const std::wstring &branch) {
  for (auto root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    auto folder = open(root, branch);
    for (const auto &id : ids(folder.handle)) {
      auto &candidate = candidates[id];
      auto key = open(folder.handle, id);
      if (auto name = stringValue(key.handle); !name.empty()) candidate.label = std::move(name);
    }
  }
}

std::optional<DWORD> eitherDword(HKEY user, HKEY machine, const wchar_t *name) {
  auto value = dword(user, name);
  return value ? value : dword(machine, name);
}

std::optional<DWORD> enumerationPolicy(HKEY root, const std::wstring &id) {
  return dword(open(root, nonEnum).handle, id.c_str());
}

bool policyHidden(HKEY root, const std::wstring &id) {
  return enumerationPolicy(root, id).value_or(0) != 0;
}

bool junctionExists(const wchar_t *space, const std::wstring &id, HKEY root) {
  return open(root, path(space) + L"\\NameSpace\\" + id).handle ||
         open(root, path(space) + L"\\NameSpace_36354489\\" + id).handle;
}

std::wstring targetKnownFolder(HKEY user, HKEY machine) {
  constexpr wchar_t bag[] = L"Instance\\InitPropertyBag";
  auto target = stringValue(open(user, bag).handle, L"TargetKnownFolder");
  if (target.empty()) target = stringValue(open(machine, bag).handle, L"TargetKnownFolder");
  return guid(target);
}

std::wstring targetPath(HKEY user, HKEY machine, const std::wstring &knownFolder) {
  constexpr wchar_t bag[] = L"Instance\\InitPropertyBag";
  auto target = stringValue(open(user, bag).handle, L"TargetFolderPath");
  if (target.empty()) target = stringValue(open(machine, bag).handle, L"TargetFolderPath");
  if (!target.empty() || knownFolder.empty()) return target;

  GUID id{};
  PWSTR raw = nullptr;
  if (FAILED(CLSIDFromString(knownFolder.c_str(), &id))) return {};
  const auto result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw);
  std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
  return SUCCEEDED(result) && path ? std::wstring(path.get()) : std::wstring{};
}

bool useEnumerationOverride(const std::wstring &id) {
  auto user = open(HKEY_CURRENT_USER, classPath(id));
  auto machine = open(HKEY_LOCAL_MACHINE, classPath(id));
  if (!targetKnownFolder(user.handle, machine.handle).empty() ||
      eitherDword(user.handle, machine.handle, pinValue).value_or(0) != 0)
    return false;
  for (auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
    if (junctionExists(L"Desktop", id, root) ||
        dword(open(root, path(L"HideDesktopIcons\\NewStartPanel")).handle, id.c_str()))
      return false;
  constexpr wchar_t command[] = L"Shell\\Open\\Command";
  // NonEnum applies to the CLSID globally. Restrict it to application-specific
  // junctions that are registered only under This PC.
  if (junctionExists(L"MyComputer", id, HKEY_CURRENT_USER)) return user.handle != nullptr;
  return junctionExists(L"MyComputer", id, HKEY_LOCAL_MACHINE) &&
         (!stringValue(open(user.handle, command).handle).empty() ||
          !stringValue(open(machine.handle, command).handle).empty());
}

HKEY mountRoot(unsigned slot) { return slot < mounts.size() ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
std::wstring mountPath(unsigned slot) { return path(mounts[slot % mounts.size()]); }
std::wstring parked(const std::wstring &id) { return hiddenPrefix + id; }

unsigned mountMask(const std::wstring &id, bool hidden) {
  unsigned mask = 0;
  for (unsigned slot = 0; slot < 2 * mounts.size(); ++slot) {
    if (open(mountRoot(slot), mountPath(slot) + L"\\" + (hidden ? parked(id) : id)).handle)
      mask |= 1u << slot;
  }
  return mask;
}

void renameMount(unsigned slot, const std::wstring &from, const std::wstring &to) {
  auto key = open(mountRoot(slot), mountPath(slot), KEY_READ | KEY_WRITE);
  if (!key.handle) fn::fail(L"无法修改资源管理器入口", ERROR_ACCESS_DENIED);
  const auto status = RegRenameKey(key.handle, from.c_str(), to.c_str());
  if (status) fn::fail(L"无法修改资源管理器入口", status);
}

void requireWritableMounts(unsigned mask) {
  for (unsigned slot = 0; slot < 2 * mounts.size(); ++slot)
    if ((mask & (1u << slot)) && !open(mountRoot(slot), mountPath(slot), KEY_READ | KEY_WRITE).handle)
      throw fn::Error{lang::text(L"此入口由所有用户共享。请以管理员身份打开 FolderNote 后重试。")};
}

void restoreMounts(const std::wstring &id, unsigned mask) {
  mask &= mountMask(id, true);
  if (mask & mountMask(id, false)) fn::fail(L"无法恢复资源管理器入口", ERROR_ALREADY_EXISTS);
  requireWritableMounts(mask);
  for (unsigned slot = 0; slot < 2 * mounts.size(); ++slot)
    if (mask & (1u << slot)) renameMount(slot, parked(id), id);
}

std::optional<Entry> describe(Surface surface, const std::wstring &id, const Candidate &candidate, HKEY journal) {
  auto user = open(HKEY_CURRENT_USER, classPath(id));
  auto machine = open(HKEY_LOCAL_MACHINE, classPath(id));
  const auto overrideKey = overridePath(surface, id);
  const auto name = overrideName(surface, id);
  auto userOverride = open(HKEY_CURRENT_USER, overrideKey);
  const auto machineOverride = open(HKEY_LOCAL_MACHINE, overrideKey);
  auto attributes = eitherDword(open(user.handle, L"ShellFolder").handle,
                                open(machine.handle, L"ShellFolder").handle, L"Attributes");
  const auto userShell = open(HKEY_CURRENT_USER, explorerClass(id) + L"\\ShellFolder");
  if (auto shellOverride = dword(userShell.handle, L"Attributes")) attributes = shellOverride;
  const bool enumerated = !(attributes.value_or(0) & SFGAO_NONENUMERATED);
  const bool registered = user.handle || machine.handle;
  const auto override = eitherDword(userOverride.handle, machineOverride.handle, name);
  const auto pin = eitherDword(user.handle, machine.handle, pinValue);
  if (!candidate.saved && !registered && candidate.label.empty()) return {};

  bool ownSuppression = false;
  if (surface == Surface::ThisPc && candidate.saved) {
    try {
      const auto saved = savedValue(journal, id);
      ownSuppression = saved && saved->version == 1 && saved->suppressEnumeration &&
                       enumerationPolicy(HKEY_CURRENT_USER, id) == saved->expected;
      if (saved && saved->version == 2)
        ownSuppression = !(mountMask(id, false)) && (mountMask(id, true) & saved->suppressEnumeration);
    } catch (const fn::Error &) {
      // A damaged journal should not prevent listing the other entries.
    }
  }
  const bool blocked = policyHidden(HKEY_LOCAL_MACHINE, id) ||
                       (policyHidden(HKEY_CURRENT_USER, id) && !ownSuppression);
  bool hidden = false;
  bool canChange = registered && enumerated && !blocked;
  if (surface == Surface::Navigation) {
    if (!pin && id != computer && id != network && !candidate.saved) return {};
    hidden = !enumerated || override.value_or(1) == 0;
    if (!pin || id == home || id == gallery) canChange = false;
  } else {
    if (!enumerated && !candidate.saved) return {};
    hidden = override.value_or(0) != 0;
  }

  auto shellName = open(HKEY_CURRENT_USER, explorerClass(id));
  auto icon = stringValue(open(shellName.handle, L"DefaultIcon").handle);
  if (icon.empty()) icon = stringValue(open(user.handle, L"DefaultIcon").handle);
  if (icon.empty()) icon = stringValue(open(machine.handle, L"DefaultIcon").handle);
  auto label = caption(stringValue(shellName.handle));
  for (const auto *field : {L"LocalizedString", static_cast<const wchar_t *>(nullptr)}) {
    if (label.empty()) label = caption(stringValue(user.handle, field));
    if (label.empty()) label = caption(stringValue(machine.handle, field));
  }
  const auto knownFolder = targetKnownFolder(user.handle, machine.handle);
  if (surface == Surface::ThisPc && !knownFolder.empty()) {
    const auto description = open(HKEY_LOCAL_MACHINE, path(L"FolderDescriptions\\") + knownFolder);
    if (label.empty()) label = caption(stringValue(description.handle, L"LocalizedName"));
    if (label.empty()) label = stringValue(description.handle, L"Name");
    const auto bag = open(description.handle, L"PropertyBag");
    const auto policy = stringValue(bag.handle, L"ThisPCPolicy");
    if (_wcsicmp(policy.c_str(), L"Hide") == 0) {
      hidden = true;
      canChange = false;
    }
  }
  if (label.empty() && candidate.label.find(L'!') == std::wstring::npos)
    label = caption(candidate.label);
  if (label.empty()) {
    if (!candidate.saved) return {};
    label = id;
  }
  if (blocked || ownSuppression) hidden = true;
  if (userOverride.handle) {
    DWORD type = 0, bytes = 0;
    const auto status = RegQueryValueExW(userOverride.handle, name, nullptr, &type, nullptr, &bytes);
    if (!missing(status) && (status || type != REG_DWORD || bytes != sizeof(DWORD))) canChange = false;
  }
  return Entry{id, std::move(label), surface, hidden, candidate.saved, canChange,
               targetPath(user.handle, machine.handle, knownFolder), std::move(icon)};
}

void removeEmptyClass(const std::wstring &location) {
  auto key = open(HKEY_CURRENT_USER, location);
  DWORD subkeys = 0, values = 0;
  if (!key.handle || RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr,
                                      nullptr, &values, nullptr, nullptr, nullptr, nullptr) ||
      subkeys || values)
    return;
  RegDeleteKeyExW(HKEY_CURRENT_USER, location.c_str(), KEY_WOW64_64KEY, 0);
}

void restoreOne(HKEY journal, Surface surface, const std::wstring &id) {
  auto saved = savedValue(journal, id);
  if (!saved) return;
  if (saved->version == 2) {
    if (surface != Surface::ThisPc) fn::fail(L"入口原始设置格式无效", ERROR_INVALID_DATA);
    restoreMounts(id, saved->suppressEnumeration);
    erase(journal, id.c_str());
    return;
  }
  const auto location = overridePath(surface, id, saved->suppressEnumeration != 0);
  const auto name = overrideName(surface, id);
  {
    auto key = openForChange(location, false);
    auto current = readOverride(key.handle, name);
    // A later external change supersedes our override.
    if (current && (*current == saved->expected || (saved->pending && *current == saved->previousExpected))) {
      if (saved->hasOriginal) writeOverride(key.handle, name, saved->original);
      else erase(key.handle, name);
    }
  }
  if (surface == Surface::Navigation && saved->createdClass) removeEmptyClass(location);
  erase(journal, id.c_str());
}
}

std::vector<Entry> inventory(Surface surface) {
  Candidates candidates;
  if (surface == Surface::ThisPc) {
    discover(candidates, path(L"MyComputer\\NameSpace"));
    discover(candidates, path(L"MyComputer\\NameSpace_36354489"));
  } else {
    discover(candidates, path(L"Desktop\\NameSpace"));
    discover(candidates, path(L"Desktop\\NameSpace_36354489"));
  }
  if (surface == Surface::Navigation) {
    for (const auto *id : {computer, network, libraries, home, gallery, oneDrive}) candidates.try_emplace(id);
    // Some vendors have a separate navigation registration outside Desktop\NameSpace.
    for (auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
      auto shellClasses = open(root, classes);
      for (const auto &id : ids(shellClasses.handle)) {
        auto key = open(shellClasses.handle, id);
        if (dword(key.handle, pinValue)) candidates.try_emplace(id);
      }
    }
  }
  if (surface == Surface::Desktop) {
    for (auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
      auto hide = open(root, path(L"HideDesktopIcons\\NewStartPanel"));
      for (const auto &id : ids(hide.handle, true)) candidates.try_emplace(id);
    }
    candidates.try_emplace(computer);
  }
  const auto journal = open(HKEY_CURRENT_USER, journalPath(surface));
  for (const auto &id : ids(journal.handle, true)) {
    auto &candidate = candidates[id];
    candidate.saved = true;
    if (surface == Surface::ThisPc && candidate.label.empty()) {
      for (unsigned slot = 0; slot < 2 * mounts.size(); ++slot) {
        auto key = open(mountRoot(slot), mountPath(slot) + L"\\" + parked(id));
        if (auto label = stringValue(key.handle); !label.empty()) {
          candidate.label = std::move(label);
          break;
        }
      }
    }
  }
  std::vector<Entry> result;
  for (const auto &[id, candidate] : candidates)
    if (auto entry = describe(surface, id, candidate, journal.handle)) result.push_back(std::move(*entry));
  std::sort(result.begin(), result.end(), [](const Entry &left, const Entry &right) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, SORT_DIGITSASNUMBERS, left.name.c_str(), -1,
                           right.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
  });
  return result;
}

void setHidden(const Entry &entry, bool hidden) {
  const auto id = guid(entry.id);
  if (id.empty() || !entry.canChange) throw fn::Error{lang::text(L"无法修改此入口的显示状态。")};
  Lock lock;
  auto journal = openForChange(journalPath(entry.surface), true);
  auto previous = savedValue(journal.handle, id);
  if (entry.surface == Surface::ThisPc && previous && previous->version == 2) {
    if (hidden && !mountMask(id, false)) return;
    restoreOne(journal.handle, entry.surface, id);
    previous.reset();
    if (!hidden) { refresh(entry.surface); return; }
  }
  if (entry.surface == Surface::ThisPc && previous && previous->suppressEnumeration) {
    if (hidden && useEnumerationOverride(id)) requireWritableMounts(mountMask(id, false));
    restoreOne(journal.handle, entry.surface, id);
    previous.reset();
    if (!hidden) {
      refresh(entry.surface);
      return;
    }
  }
  if (policyHidden(HKEY_CURRENT_USER, id) || policyHidden(HKEY_LOCAL_MACHINE, id))
    throw fn::Error{lang::text(L"无法修改此入口的显示状态。")};
  if (entry.surface == Surface::ThisPc && hidden && !previous && useEnumerationOverride(id)) {
    const auto mask = mountMask(id, false);
    if (!mask || mountMask(id, true)) fn::fail(L"无法修改资源管理器入口", ERROR_ALREADY_EXISTS);
    // Check all registrations before changing either hive.
    requireWritableMounts(mask);
    SavedValue saved;
    saved.version = 2;
    saved.suppressEnumeration = mask;
    save(journal.handle, id, saved);
    try {
      for (unsigned slot = 0; slot < 2 * mounts.size(); ++slot)
        if (mask & (1u << slot)) renameMount(slot, id, parked(id));
    } catch (...) {
      restoreOne(journal.handle, entry.surface, id);
      throw;
    }
    return;
  }
  const auto location = overridePath(entry.surface, id);
  const auto name = overrideName(entry.surface, id);
  const DWORD desired = (entry.surface == Surface::Navigation ? !hidden : hidden) ? 1 : 0;
  const auto existingKey = open(HKEY_CURRENT_USER, location);
  const auto current = readOverride(existingKey.handle, name);
  if (current && *current == desired) return;
  SavedValue saved;
  if (previous && current && (*current == previous->expected ||
                              (previous->pending && *current == previous->previousExpected))) {
    saved = *previous;
    saved.pending = 1;
    saved.previousExpected = *current;
  } else {
    saved.hasOriginal = current.has_value();
    saved.original = current.value_or(0);
    saved.createdClass = entry.surface == Surface::Navigation && !existingKey.handle;
  }
  saved.expected = desired;
  save(journal.handle, id, saved);
  try {
    auto key = openForChange(location, true);
    writeOverride(key.handle, name, desired);
  } catch (...) {
    if (saved.createdClass && !previous) removeEmptyClass(location);
    if (previous) save(journal.handle, id, *previous);
    else erase(journal.handle, id.c_str());
    throw;
  }
  // The pending form can also be recovered if a process exits after the write.
  if (saved.pending) {
    saved.pending = 0;
    try { save(journal.handle, id, saved); }
    catch (const fn::Error &) {} // The pending record still restores the original value.
  }
  refresh(entry.surface);
}

void restore(const Entry &entry) {
  const auto id = guid(entry.id);
  if (id.empty()) return;
  Lock lock;
  auto journal = openForChange(journalPath(entry.surface), false);
  restoreOne(journal.handle, entry.surface, id);
  refresh(entry.surface);
}

size_t restoreAll() {
  Lock lock;
  size_t failed = 0;
  for (const auto surface : surfaces) {
    try {
      auto journal = openForChange(journalPath(surface), false);
      for (const auto &id : ids(journal.handle, true)) {
        try { restoreOne(journal.handle, surface, id); }
        catch (const fn::Error &) { ++failed; }
      }
      if (journal.handle) refresh(surface);
    } catch (const fn::Error &) { ++failed; }
  }
  return failed;
}

void refresh(Surface surface) noexcept {
  PIDLIST_ABSOLUTE folder = nullptr;
  if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, surface == Surface::ThisPc ? CSIDL_DRIVES : CSIDL_DESKTOP,
                                           &folder)) && folder)
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, folder, nullptr);
  CoTaskMemFree(folder);
}

void refreshOpen(Surface surface, bool policyChanged) noexcept {
  if (policyChanged && surface == Surface::ThisPc) {
    // This PC-only extension entries can use the NonEnum policy. Tell Explorer
    // to invalidate its policy cache before asking the open view to enumerate.
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, nullptr);
  }
  if (surface == Surface::Navigation) {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    fn::refreshShellViews(nullptr);
    return;
  }
  PIDLIST_ABSOLUTE raw = nullptr;
  const auto &knownFolder = surface == Surface::ThisPc ? FOLDERID_ComputerFolder : FOLDERID_Desktop;
  const auto result = SHGetKnownFolderIDList(knownFolder, KF_FLAG_DONT_VERIFY, nullptr, &raw);
  fn::refresh_detail::Pidl folder(raw);
  if (SUCCEEDED(result) && folder)
    fn::refreshShellViews(folder.get(), surface == Surface::Desktop);
}

void refreshOpen(const Entry &entry) noexcept {
  try {
    const auto id = guid(entry.id);
    const auto journal = open(HKEY_CURRENT_USER, journalPath(entry.surface));
    const auto saved = savedValue(journal.handle, id);
    if (entry.surface != Surface::ThisPc || id.empty() ||
        (!(saved && saved->version == 2) && !useEnumerationOverride(id))) {
      refreshOpen(entry.surface, entry.surface == Surface::ThisPc);
      return;
    }

    // The default This PC view can keep app namespace objects across F5.
    // Notify it about the individual virtual item, whose PIDL is distinct
    // from the real filesystem folder the provider may open.
    auto location = std::wstring(L"::") + computer + L"\\::" + id;
    PIDLIST_ABSOLUTE raw = nullptr;
    const auto parsed = SHParseDisplayName(location.c_str(), nullptr, &raw, 0, nullptr);
    fn::refresh_detail::Pidl item(raw);
    if (FAILED(parsed) || !item) {
      refreshOpen(entry.surface, true);
      return;
    }
    const auto committed = describe(entry.surface, id, Candidate{entry.name, journal.handle != nullptr}, journal.handle);
    if (committed) {
      SHChangeNotify(committed->hidden ? SHCNE_DELETE : SHCNE_CREATE, SHCNF_IDLIST | SHCNF_FLUSH,
                     item.get(), nullptr);
    }
  } catch (...) {
    // Notifications are best effort. The visibility change is already saved.
  }
}
}
