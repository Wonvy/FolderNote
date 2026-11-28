#include "context_inventory.h"
#include "core.h"
#include "language.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <optional>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sstream>
#include <utility>
#include <winver.h>

namespace context {
namespace {
constexpr wchar_t classes[] = L"Software\\Classes\\";
constexpr wchar_t journal[] = L"Software\\ShibaKit\\ContextMenuChanges";
constexpr wchar_t sendToJournal[] = L"Software\\ShibaKit\\ContextSendToChanges";
constexpr wchar_t blockJournal[] = L"Software\\ShibaKit\\ContextShellBlockChanges";
constexpr wchar_t overlayJournal[] = L"Software\\ShibaKit\\ContextMachineOverlayChanges";
constexpr wchar_t handlerJournal[] = L"Software\\ShibaKit\\ContextHandlerRegistrations";
constexpr wchar_t disabledHandlerId[] = L"{00000000-0000-0000-0000-000000000000}";
constexpr wchar_t commandStore[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\";
constexpr wchar_t blockedExtensions[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked";
constexpr wchar_t overlayOwner[] = L"ShibaKit.ContextOverlayOwner";
constexpr DWORD maxValueBytes = 65536;
constexpr size_t maxEntries = 8192;
constexpr const wchar_t *fields[]{L"LegacyDisable", L"MUIVerb", L"Icon", L"ProgrammaticAccessOnly"};
constexpr auto legacyDisable = fields[0];
constexpr auto programmaticOnly = fields[3];
constexpr DWORD hiddenVelocityId = 0x639bc8;
constexpr DWORD disabledCommandFlag = 0x8;

struct Key {
  HKEY handle = nullptr;
  Key() = default;
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;
  Key(Key &&other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
  ~Key() { if (handle) RegCloseKey(handle); }
  explicit operator bool() const { return handle != nullptr; }
};
Key open(HKEY root, const std::wstring &path, REGSAM access = KEY_READ) {
  Key result;
  RegOpenKeyExW(root, path.c_str(), 0, access, &result.handle);
  return result;
}
struct Value {
  DWORD type = REG_NONE;
  std::vector<BYTE> data;
  bool operator==(const Value &) const = default;
};
using MaybeValue = std::optional<Value>;
MaybeValue read(HKEY key, const wchar_t *name = nullptr) {
  if (!key) return {};
  Value result;
  DWORD bytes = 0;
  auto error = RegQueryValueExW(key, name, nullptr, &result.type, nullptr, &bytes);
  if (error != ERROR_SUCCESS) {
    if (error == ERROR_FILE_NOT_FOUND) return {};
    fn::fail(L"无法读取右键菜单注册项", error);
  }
  if (bytes > maxValueBytes) throw fn::Error{lang::text(L"右键菜单注册项过大。")};
  result.data.resize(bytes);
  error = RegQueryValueExW(key, name, nullptr, &result.type, result.data.data(), &bytes);
  if (error != ERROR_SUCCESS) fn::fail(L"无法读取右键菜单注册项", error);
  result.data.resize(bytes);
  return result;
}
std::wstring stringValue(const MaybeValue &value) {
  if (!value || (value->type != REG_SZ && value->type != REG_EXPAND_SZ) ||
      value->data.empty() || value->data.size() % sizeof(wchar_t)) return {};
  std::wstring result(value->data.size() / sizeof(wchar_t), L'\0');
  memcpy(result.data(), value->data.data(), value->data.size());
  if (result.back() == L'\0') result.pop_back();
  if (result.find(L'\0') != result.npos) return {};
  return result;
}
MaybeValue stringData(std::wstring_view text) {
  Value result{REG_SZ, std::vector<BYTE>((text.size() + 1) * sizeof(wchar_t))};
  if (!text.empty()) memcpy(result.data.data(), text.data(), text.size() * sizeof(wchar_t));
  return result;
}
void write(HKEY key, const wchar_t *name, const MaybeValue &value) {
  auto result = value ? RegSetValueExW(key, name, 0, value->type, value->data.data(), static_cast<DWORD>(value->data.size()))
                      : RegDeleteValueW(key, name);
  if (result != ERROR_SUCCESS && (value || result != ERROR_FILE_NOT_FOUND))
    fn::fail(L"无法修改右键菜单注册项", result);
}
std::vector<std::wstring> children(HKEY key, bool values = false) {
  std::vector<std::wstring> result;
  wchar_t name[16384]{};
  for (DWORD index = 0; key && index < maxEntries; ++index) {
    DWORD length = ARRAYSIZE(name);
    auto status = values ? RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr)
                         : RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status == ERROR_SUCCESS && length) result.emplace_back(name, length);
  }
  return result;
}
bool equal(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                                      static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool starts(std::wstring_view value, std::wstring_view prefix) {
  return value.size() >= prefix.size() && equal(value.substr(0, prefix.size()), prefix);
}
std::wstring expand(std::wstring_view text) {
  std::wstring input(text);
  const auto size = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
  if (!size || size > 32768) return input;
  std::wstring output(size, L'\0');
  if (ExpandEnvironmentStringsW(input.c_str(), output.data(), size) != size) return input;
  output.pop_back();
  return output;
}
std::wstring caption(std::wstring title) {
  if (!title.empty() && title.front() == L'@') {
    wchar_t buffer[2048]{};
    const auto resource = expand(title);
    if (SUCCEEDED(SHLoadIndirectString(resource.c_str(), buffer, ARRAYSIZE(buffer), nullptr)) && *buffer) return buffer;
    return {};
  }
  return title;
}
std::wstring firstCaption(HKEY key, std::initializer_list<const wchar_t *> names) {
  for (auto field : names) {
    auto label = caption(stringValue(read(key, field)));
    if (!label.empty()) return label;
  }
  return {};
}
std::wstring displayMenuText(std::wstring_view text) {
  std::wstring label;
  label.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'&' && i + 1 < text.size()) ++i;
    label += text[i];
  }
  return label;
}
std::wstring defaultIcon(HKEY root, const std::wstring &path) {
  auto key = open(root, path + L"\\DefaultIcon");
  return stringValue(read(key.handle));
}
std::wstring sourceName(bool machine, const std::wstring &key) {
  return (machine ? L"HKLM\\" : L"HKCU\\") + key;
}
bool protectedVerb(std::wstring_view name) {
  constexpr std::wstring_view names[]{L"open", L"opennewwindow", L"opennewprocess", L"opennewtab", L"explore",
      L"find", L"edit", L"print", L"printto", L"properties", L"runas", L"runasuser", L"delete", L"rename",
      L"cut", L"copy", L"paste", L"share", L"pin", L"unpin", L"format", L"install", L"eject", L"mount",
      L"unmount", L"cmd", L"powershell", L"openwith", L"new"};
  return starts(name, L"ShibaKit.") || starts(name, L"Windows.") || starts(name, L"Microsoft.") ||
      std::any_of(std::begin(names), std::end(names), [&](auto value) { return equal(name, value); });
}
std::vector<std::wstring> cascadeIds(HKEY verb) {
  const auto list = stringValue(read(verb, L"SubCommands"));
  std::vector<std::wstring> ids;
  size_t begin = 0;
  while (begin < list.size() && ids.size() < 64) {
    auto end = list.find(L';', begin);
    if (end == list.npos) end = list.size();
    auto id = std::wstring_view(list).substr(begin, end - begin);
    const auto first = id.find_first_not_of(L" \t");
    if (first != id.npos) {
      id = id.substr(first, id.find_last_not_of(L" \t") - first + 1);
      if (id != L"|" && id != L"||") {
        if (id.size() > 255 || id.find_first_of(L"\\/\r\n") != id.npos) return {};
        ids.emplace_back(id);
      }
    }
    begin = end + 1;
  }
  return begin <= list.size() && ids.size() == 64 ? std::vector<std::wstring>{} : ids;
}
std::wstring cascadePath(HKEY verb) {
  auto relative = stringValue(read(verb, L"ExtendedSubCommandsKey"));
  if (relative.empty() || relative.size() > 512 || relative.front() == L'\\' || relative.back() == L'\\' ||
      relative.find_first_of(L":/\r\n") != relative.npos || relative.find(L"\\\\") != relative.npos) return {};
  return relative;
}
std::vector<std::wstring> cascadeKeys(HKEY root, HKEY verb) {
  auto target = cascadePath(verb);
  if (target.empty()) return {};
  if (root != HKEY_CLASSES_ROOT) target.insert(0, classes);
  target += L"\\shell";
  auto parent = open(root, target);
  auto names = children(parent.handle);
  if (names.size() > 64) return {};
  std::vector<std::wstring> paths;
  for (const auto &name : names) {
    auto path = target + L'\\' + name;
    auto command = open(root, path + L"\\command");
    if (!stringValue(read(command.handle)).empty()) paths.push_back(std::move(path));
  }
  return paths;
}
MaybeValue verbData(HKEY root, const std::wstring &path, HKEY verb) {
  const auto subCommands = read(verb, L"SubCommands");
  const auto extended = read(verb, L"ExtendedSubCommandsKey");
  const auto &cascade = subCommands ? subCommands : extended;
  if (cascade) {
    // A cascade has no default command. Preserve exact original type and data
    // under a distinct identity so a changed child list is never restored over.
    Value id{REG_BINARY, {0x46, 0x4E, 0x53, static_cast<BYTE>(subCommands ? 0x43 : 0x45)}};
    const auto type = reinterpret_cast<const BYTE *>(&cascade->type);
    id.data.insert(id.data.end(), type, type + sizeof(cascade->type));
    id.data.insert(id.data.end(), cascade->data.begin(), cascade->data.end());
    return id;
  }
  const auto command = open(root, path + L"\\command");
  return read(command.handle);
}
bool hasDword(HKEY key, const wchar_t *field, DWORD value, DWORD mask = MAXDWORD) {
  const auto data = read(key, field);
  if (!data || data->type != REG_DWORD || data->data.size() != sizeof(DWORD)) return false;
  DWORD current = 0;
  memcpy(&current, data->data.data(), sizeof(current));
  return (current & mask) == value;
}
bool hiddenVerb(HKEY key, bool subItem = false) {
  return hasDword(key, L"HideBasedOnVelocityId", hiddenVelocityId) ||
      (!subItem && (read(key, legacyDisable) || read(key, programmaticOnly) ||
                   hasDword(key, L"CommandFlags", disabledCommandFlag, disabledCommandFlag)));
}
bool staticVerb(HKEY verb, const std::wstring &path, std::wstring_view name, HKEY root = HKEY_CURRENT_USER) {
  if (!starts(path, classes) || protectedVerb(name)) return false;
  const auto shell = open(root, path.substr(0, path.find_last_of(L'\\')));
  if (equal(stringValue(read(shell.handle)), name)) return false;
  auto command = open(root, path + L"\\command");
  if (read(command.handle, L"DelegateExecute") ||
      read(verb, L"ExplorerCommandHandler") || read(verb, L"CommandStateHandler") ||
      open(root, path + L"\\ddeexec") || open(root, path + L"\\DropTarget")) return false;
  const auto subCommands = read(verb, L"SubCommands");
  const auto extended = read(verb, L"ExtendedSubCommandsKey");
  if (subCommands || extended) {
    if (command || (subCommands && extended) ||
        (subCommands ? cascadeIds(verb).empty() : cascadeKeys(root, verb).empty())) return false;
  } else if (stringValue(read(command.handle)).empty()) return false;
  auto title = stringValue(read(verb, L"MUIVerb"));
  if (title.empty()) title = stringValue(read(verb));
  if (!title.empty() && title.front() == L'@') {
    auto module = std::wstring_view(title).substr(1);
    module = module.substr(0, module.find(L','));
    const auto basename = std::filesystem::path(module).filename().native();
    for (auto system : {L"shell32.dll", L"imageres.dll", L"explorerframe.dll", L"windows.storage.dll"})
      if (equal(basename, system)) return false;
  }
  return true;
}
void copyable(ExistingItem &item) {
  auto command = expand(item.command);
  if (command.empty() || command.find_first_of(L"\r\n") != command.npos) return;
  size_t end;
  std::wstring program;
  if (command.front() == L'"') {
    end = command.find(L'"', 1);
    if (end == command.npos) return;
    program = command.substr(1, end++ - 1);
  } else {
    end = command.find_first_of(L" \t");
    program = command.substr(0, end);
  }
  // Launch only a directly addressable executable. Shell verb wrappers and
  // commands using other substitution conventions need their original handler.
  if (!std::filesystem::path(program).is_absolute() || program.find_first_of(L"%{}") != program.npos ||
      !equal(std::filesystem::path(program).extension().native(), L".exe")) return;
  const auto basename = std::filesystem::path(program).filename().native();
  if (equal(basename, L"cmd.exe") || equal(basename, L"powershell.exe") || equal(basename, L"pwsh.exe") ||
      equal(basename, L"rundll32.exe") || equal(basename, L"wscript.exe") || equal(basename, L"cscript.exe") ||
      equal(basename, L"mshta.exe")) return;
  auto first = command.find_first_not_of(L" \t", end);
  std::wstring arguments;
  if (first != command.npos) {
    if (command.find_first_of(L"{}", first) != command.npos) return;
    for (size_t i = first; i < command.size(); ++i) {
      auto ch = command[i];
      if (ch != L'%') { arguments += ch; continue; }
      if (++i == command.size() || std::wstring_view(L"1lLvV").find(command[i]) == std::wstring_view::npos) return;
      if (i + 1 < command.size() && iswalnum(command[i + 1])) return;
      arguments += L"{path}";
    }
  }
  item.program = std::move(program);
  item.arguments = std::move(arguments);
}
void changed(bool refreshComputer = false) {
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
  if (!refreshComputer) return;
  // Drives are displayed in the virtual This PC folder, not in a filesystem directory.
  PIDLIST_ABSOLUTE computer = nullptr;
  if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_DRIVES, &computer)) && computer)
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, computer, nullptr);
  CoTaskMemFree(computer);
}

struct Mutex {
  HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\ShibaKit.ContextMenuInventory.v1");
  Mutex() {
    if (!handle) fn::fail(L"无法锁定右键菜单修改");
    auto result = WaitForSingleObject(handle, 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
      CloseHandle(handle);
      handle = nullptr;
      throw fn::Error{lang::text(L"右键菜单正在修改，请稍后重试。")};
    }
  }
  ~Mutex() { ReleaseMutex(handle); CloseHandle(handle); }
};
std::wstring journalKey(std::wstring_view path, const wchar_t *root = journal) {
  unsigned long long hash = 14695981039346656037ULL;
  std::wstring folded(path);
  if (!folded.empty()) CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
  for (auto ch : folded) { hash ^= ch; hash *= 1099511628211ULL; }
  std::wostringstream id;
  id << root << L'\\' << std::hex << hash;
  return id.str();
}
struct Record {
  MaybeValue command, original, previous, expected;
};
void append(std::vector<BYTE> &data, const void *value, size_t size) {
  if (!size) return;
  const auto bytes = static_cast<const BYTE *>(value);
  data.insert(data.end(), bytes, bytes + size);
}
Value encode(const Record &record) {
  Value result{REG_BINARY, {}};
  for (const auto *value : {&record.command, &record.original, &record.previous, &record.expected}) {
    const DWORD type = *value ? (*value)->type : MAXDWORD;
    const DWORD size = *value ? static_cast<DWORD>((*value)->data.size()) : 0;
    append(result.data, &type, sizeof(type));
    append(result.data, &size, sizeof(size));
    if (*value) append(result.data, (*value)->data.data(), size);
  }
  if (result.data.size() > maxValueBytes) throw fn::Error{lang::text(L"右键菜单注册项过大。")};
  return result;
}
Record decode(const Value &value) {
  Record result;
  size_t offset = 0;
  auto valid = value.type == REG_BINARY;
  for (auto *part : {&result.command, &result.original, &result.previous, &result.expected}) {
    if (!valid || value.data.size() - offset < 2 * sizeof(DWORD)) { valid = false; break; }
    DWORD type = 0, size = 0;
    memcpy(&type, value.data.data() + offset, sizeof(type)); offset += sizeof(type);
    memcpy(&size, value.data.data() + offset, sizeof(size)); offset += sizeof(size);
    if (size > value.data.size() - offset || (type == MAXDWORD && size)) { valid = false; break; }
    if (type != MAXDWORD) *part = Value{type, std::vector<BYTE>(value.data.begin() + offset, value.data.begin() + offset + size)};
    offset += size;
  }
  if (!valid || offset != value.data.size() || !result.command)
    throw fn::Error{lang::text(L"右键菜单恢复记录无效。")};
  return result;
}
[[noreturn]] void conflict() {
  throw fn::Error{lang::text(L"右键菜单已被其他程序修改。已保留当前设置。")};
}
void verify(const Record &record, const MaybeValue &command, const MaybeValue &current) {
  if (command != record.command || (current != record.original && current != record.previous && current != record.expected)) conflict();
}
std::wstring verbName(const std::wstring &path) {
  const auto separator = path.find_last_of(L'\\');
  return separator == path.npos ? std::wstring() : path.substr(separator + 1);
}
bool isVerbPath(const std::wstring &path) {
  if (!starts(path, classes)) return false;
  auto parent = path.find_last_of(L'\\');
  if (parent == path.npos || parent <= std::size(classes) || protectedVerb(verbName(path))) return false;
  const std::wstring_view prefix(path.data(), parent);
  return prefix.size() >= 6 && equal(prefix.substr(prefix.size() - 6), L"\\shell");
}
Key writableVerb(const std::wstring &path) {
  if (!isVerbPath(path)) conflict();
  auto verb = open(HKEY_CURRENT_USER, path, KEY_READ | KEY_SET_VALUE);
  if (!verb || !staticVerb(verb.handle, path, verbName(path))) conflict();
  return verb;
}
Key backup(const std::wstring &path, bool create = false, const wchar_t *root = journal) {
  auto keyPath = journalKey(path, root);
  Key result = open(HKEY_CURRENT_USER, keyPath, KEY_READ | KEY_SET_VALUE);
  if (result) {
    auto stored = stringValue(read(result.handle, L"Path"));
    if (!stored.empty() && !equal(stored, path)) conflict();
    if (stored.empty() && create) write(result.handle, L"Path", stringData(path));
  } else if (create) {
    auto error = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0, KEY_READ | KEY_SET_VALUE,
                                 nullptr, &result.handle, nullptr);
    if (error != ERROR_SUCCESS) fn::fail(L"无法备份右键菜单注册项", error);
    write(result.handle, L"Path", stringData(path));
  }
  return result;
}
void clearBackupIfEmpty(const std::wstring &path, HKEY key) {
  if (std::none_of(std::begin(fields), std::end(fields), [&](auto field) { return read(key, field).has_value(); }))
    RegDeleteKeyW(HKEY_CURRENT_USER, journalKey(path).c_str());
}
void restoreRecord(HKEY verb, HKEY saved, const wchar_t *field, const Value &value, const MaybeValue &command) {
  auto record = decode(value);
  auto current = read(verb, field);
  verify(record, command, current);
  if (current != record.original) write(verb, field, record.original);
  write(saved, field, {});
}
void restoreLocked(const std::wstring &path) {
  auto saved = backup(path);
  if (!saved) return;
  auto verb = writableVerb(path);
  const auto command = verbData(HKEY_CURRENT_USER, path, verb.handle);
  bool conflicted = false;
  for (const auto field : fields) {
    try {
      if (auto record = read(saved.handle, field)) restoreRecord(verb.handle, saved.handle, field, *record, command);
    } catch (const fn::Error &) { conflicted = true; }
  }
  clearBackupIfEmpty(path, saved.handle);
  if (conflicted) conflict();
}
void change(const ExistingItem &item, const wchar_t *field, const MaybeValue &desired) {
  const auto &path = item.registryKey;
  if (item.machine || path.empty()) conflict();
  auto verb = writableVerb(path);
  const auto command = verbData(HKEY_CURRENT_USER, path, verb.handle);
  if (item.command != stringValue(command) || item.cascadeChildren != cascadeIds(verb.handle) ||
      item.cascadeTarget != cascadePath(verb.handle)) conflict();
  auto saved = backup(path, desired.has_value());
  if (!saved) return;
  auto stored = read(saved.handle, field);
  if (!desired) {
    if (stored) restoreRecord(verb.handle, saved.handle, field, *stored, command);
    clearBackupIfEmpty(path, saved.handle);
    return;
  }
  const auto current = read(verb.handle, field);
  Record record{command, current, current, desired};
  if (stored) {
    const auto previous = decode(*stored);
    verify(previous, command, current);
    record.original = previous.original;
  } else if ((field == legacyDisable || field == programmaticOnly) && current) {
    conflict();
  }
  if (current != desired) {
    write(saved.handle, field, encode(record));
    write(verb.handle, field, desired);
    record.previous = desired;
    write(saved.handle, field, encode(record));
  }
  clearBackupIfEmpty(path, saved.handle);
}
void changeVisibility(const ExistingItem &item, bool hidden) {
  if (hidden) {
    const auto verb = writableVerb(item.registryKey);
    if (hiddenVerb(verb.handle)) conflict();
    change(item, legacyDisable, stringData(L""));
    try {
      change(item, programmaticOnly, stringData(L""));
    } catch (...) {
      change(item, legacyDisable, {});
      throw;
    }
  } else {
    change(item, programmaticOnly, {});
    change(item, legacyDisable, {});
  }
}

std::wstring canonicalClsid(std::wstring_view value) {
  if (value.size() != 38 || value.front() != L'{') return {};
  std::wstring id(value);
  GUID guid{};
  wchar_t canonical[40]{};
  if (FAILED(CLSIDFromString(id.c_str(), &guid)) || !StringFromGUID2(guid, canonical, ARRAYSIZE(canonical))) return {};
  return canonical;
}
std::wstring finalLocalPath(const std::wstring &path, bool directory = false) {
  if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return {};
  const wchar_t drive[]{path[0], L':', L'\\', L'\0'};
  const auto type = GetDriveTypeW(drive);
  if (type != DRIVE_FIXED) return {};
  HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  wchar_t resolved[32768]{};
  const auto length = GetFinalPathNameByHandleW(file, resolved, ARRAYSIZE(resolved), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  BY_HANDLE_FILE_INFORMATION info{};
  const bool valid = GetFileInformationByHandle(file, &info) &&
      ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
  CloseHandle(file);
  if (!valid || length < 7 || length >= ARRAYSIZE(resolved) ||
      !std::wstring_view(resolved, length).starts_with(L"\\\\?\\") || resolved[5] != L':') return {};
  return std::wstring(resolved + 4, length - 4);
}
bool microsoftModule(const std::wstring &path) {
  DWORD unused = 0;
  const auto bytes = GetFileVersionInfoSizeW(path.c_str(), &unused);
  if (!bytes || bytes > 1024 * 1024) return false;
  std::vector<BYTE> data(bytes);
  if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data())) return false;
  struct Translation { WORD language, codepage; };
  void *translations = nullptr;
  UINT translationSize = 0;
  if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &translations, &translationSize)) return false;
  const auto entries = static_cast<const Translation *>(translations);
  for (size_t i = 0; i < translationSize / sizeof(Translation); ++i) {
    wchar_t key[64]{};
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\CompanyName",
               static_cast<unsigned>(entries[i].language), static_cast<unsigned>(entries[i].codepage));
    void *company = nullptr;
    UINT size = 0;
    if (VerQueryValueW(data.data(), key, &company, &size) && company && size &&
        starts(std::wstring_view(static_cast<const wchar_t *>(company), size - 1), L"Microsoft")) return true;
  }
  return false;
}
bool isDll(const std::wstring &path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD bytes = 0;
  IMAGE_DOS_HEADER dos{};
  struct Header { DWORD signature; IMAGE_FILE_HEADER file; } pe{};
  LARGE_INTEGER offset{};
  bool valid = false;
  if (ReadFile(file, &dos, sizeof(dos), &bytes, nullptr) && bytes == sizeof(dos) &&
      dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= static_cast<LONG>(sizeof(dos))) {
    offset.QuadPart = dos.e_lfanew;
    valid = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
        ReadFile(file, &pe, sizeof(pe), &bytes, nullptr) && bytes == sizeof(pe) &&
        pe.signature == IMAGE_NT_SIGNATURE && (pe.file.Characteristics & IMAGE_FILE_DLL);
  }
  CloseHandle(file);
  return valid;
}
std::wstring thirdPartyModule(const std::wstring &id) {
  if (id.empty() || equal(id, fn::settingsId) ||
      std::any_of(std::begin(fn::ids), std::end(fn::ids), [&](auto own) { return equal(id, own); })) return {};
  auto server = open(HKEY_CLASSES_ROOT, L"CLSID\\" + id + L"\\InprocServer32");
  auto path = expand(stringValue(read(server.handle)));
  if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
  if (!equal(std::filesystem::path(path).extension().native(), L".dll")) return {};
  auto final = finalLocalPath(path);
  if (final.empty() || starts(std::filesystem::path(final).filename().native(), L"ShibaKit")) return {};
  wchar_t windows[MAX_PATH]{};
  const auto length = GetSystemWindowsDirectoryW(windows, ARRAYSIZE(windows));
  if (!length || length >= ARRAYSIZE(windows)) return {};
  const auto system = finalLocalPath(windows, true);
  if (system.empty() || starts(final, system + L'\\') || !isDll(final) || microsoftModule(final)) return {};
  return final;
}
Key blockKey(HKEY root, const wchar_t *path, bool writable = false, bool create = false) {
  Key key;
  const REGSAM access = KEY_QUERY_VALUE | (writable ? KEY_SET_VALUE : 0);
  auto result = create ? RegCreateKeyExW(root, path, 0, nullptr, 0, access, nullptr, &key.handle, nullptr)
                       : RegOpenKeyExW(root, path, 0, access, &key.handle);
  if (result != ERROR_SUCCESS && (create || result != ERROR_FILE_NOT_FOUND))
    fn::fail(L"无法修改右键菜单注册项", result);
  return key;
}
bool machineBlocked(const std::wstring &id) {
  const auto machine = blockKey(HKEY_LOCAL_MACHINE, blockedExtensions);
  return read(machine.handle, id.c_str()).has_value();
}
bool validBlockRecord(const MaybeValue &saved) {
  auto token = stringValue(saved);
  constexpr std::wstring_view prefix = L"ShibaKit ";
  GUID guid{};
  return saved && saved->type == REG_SZ && token.size() == prefix.size() + 38 &&
      token.starts_with(prefix) && SUCCEEDED(CLSIDFromString(token.c_str() + prefix.size(), &guid));
}
Value ownerToken() {
  GUID token{};
  wchar_t value[40]{};
  if (FAILED(CoCreateGuid(&token)) || !StringFromGUID2(token, value, ARRAYSIZE(value)))
    throw fn::Error{lang::text(L"右键菜单恢复记录无效。")};
  return *stringData(std::wstring(L"ShibaKit ") + value);
}
void readBlockState(ExistingItem &item) {
  const auto blocked = blockKey(HKEY_CURRENT_USER, blockedExtensions);
  const auto backup = blockKey(HKEY_CURRENT_USER, blockJournal);
  const auto current = read(blocked.handle, item.shellBlockId.c_str());
  const auto saved = read(backup.handle, item.shellBlockId.c_str());
  const auto externallyBlocked = machineBlocked(item.shellBlockId);
  item.shellBlock = true;
  item.hidden = current.has_value() || externallyBlocked;
  item.managed = validBlockRecord(saved) && (!current || current == saved);
  item.canHide = !externallyBlocked && (!current || item.managed) && (!saved || validBlockRecord(saved));
}
void changeBlock(const std::wstring &id, bool hidden) {
  if (canonicalClsid(id).empty() || (hidden && machineBlocked(id))) conflict();
  auto backup = blockKey(HKEY_CURRENT_USER, blockJournal, true, hidden);
  auto saved = read(backup.handle, id.c_str());
  auto blocked = blockKey(HKEY_CURRENT_USER, blockedExtensions, true, hidden);
  const auto current = read(blocked.handle, id.c_str());
  if ((saved && !validBlockRecord(saved)) || (current && current != saved)) conflict();
  if (hidden) {
    if (current) return;
    if (!saved) {
      saved = ownerToken();
      // Save ownership before changing Explorer's setting so interrupted
      // updates can still be restored. Other programs' values are left alone.
      write(backup.handle, id.c_str(), saved);
    }
    write(blocked.handle, id.c_str(), saved);
  } else if (saved) {
    if (current) write(blocked.handle, id.c_str(), {});
    write(backup.handle, id.c_str(), {});
  }
}
std::wstring handlerClsid(HKEY root, const std::wstring &path) {
  const auto key = open(root, path);
  if (!key) return {};
  auto value = stringValue(read(key.handle));
  return canonicalClsid(value.empty() ? verbName(path) : value);
}
bool isHandlerPath(const std::wstring &path) {
  constexpr std::wstring_view segment = L"\\shellex\\ContextMenuHandlers\\";
  const auto child = path.rfind(L'\\');
  return starts(path, classes) && child != path.npos && child + 1 < path.size() &&
      child + 1 >= std::size(classes) + segment.size() &&
      equal(std::wstring_view(path).substr(child + 1 - segment.size(), segment.size()), segment);
}
bool verifiedBlockItem(const ExistingItem &item) {
  if (!isHandlerPath(item.registryKey)) return false;
  const auto root = item.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  if (!equal(handlerClsid(root, item.registryKey), item.shellBlockId)) return false;
  auto module = thirdPartyModule(item.shellBlockId);
  return !module.empty() && equal(module, item.shellBlockModule);
}

struct ClassView {
  std::vector<std::wstring> children;
  std::vector<std::pair<std::wstring, MaybeValue>> values;
};
std::vector<std::wstring> overlayParents(const std::wstring &path) {
  std::vector<std::wstring> result;
  for (auto separator = path.find(L'\\', std::size(classes) - 1); separator != path.npos;
       separator = path.find(L'\\', separator + 1)) result.push_back(path.substr(0, separator));
  return result;
}
std::vector<ClassView> snapshotClasses(const std::wstring &path) {
  std::vector<ClassView> result;
  for (const auto &parent : overlayParents(path)) {
    const auto merged = open(HKEY_CLASSES_ROOT, parent.substr(std::size(classes) - 1));
    if (!merged) conflict();
    ClassView view{children(merged.handle), {}};
    if (parent == path.substr(0, path.rfind(L'\\')))
      std::erase_if(view.children, [&](const auto &name) { return equal(name, verbName(path)); });
    view.values.emplace_back(L"", read(merged.handle));
    for (const auto &name : children(merged.handle, true))
      view.values.emplace_back(name, read(merged.handle, name.c_str()));
    result.push_back(std::move(view));
  }
  return result;
}
bool sameClassViews(const std::vector<ClassView> &a, const std::vector<ClassView> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].children.size() != b[i].children.size() || a[i].values.size() != b[i].values.size()) return false;
    for (const auto &name : a[i].children)
      if (std::none_of(b[i].children.begin(), b[i].children.end(), [&](const auto &other) { return equal(name, other); })) return false;
    for (const auto &[name, value] : a[i].values)
      if (std::none_of(b[i].values.begin(), b[i].values.end(), [&](const auto &other) {
            return equal(name, other.first) && value == other.second;
          })) return false;
  }
  return true;
}
bool ownedOverlay(HKEY key, const MaybeValue &token, bool pending = false) {
  DWORD subkeys = 0, values = 0;
  if (!key || !validBlockRecord(token) ||
      RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                       nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS || subkeys) return false;
  const auto marker = read(key, legacyDisable);
  const auto access = read(key, programmaticOnly);
  const auto owner = read(key, overlayOwner);
  if (owner == token) {
    const auto empty = stringData(L"");
    // Earlier overlays used only LegacyDisable. A pending transaction may
    // also have been interrupted after writing just its ownership token.
    if (marker == empty && (access == empty ? values == 3 : !access && values == 2)) return true;
    return pending && values == 1 && !marker && !access;
  }
  // Existing installations may have stored the token in LegacyDisable itself.
  return (values == 1 && marker == token) || (pending && !values);
}
void eraseEmptyOverlayParents(const std::wstring &path, const std::wstring &firstMissing) {
  if (firstMissing.empty()) return;
  auto parents = overlayParents(path);
  for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent) {
    if (!equal(*parent, firstMissing) && !starts(*parent, firstMissing + L'\\')) break;
    const auto user = open(HKEY_CURRENT_USER, *parent);
    if (!user) continue;
    DWORD subkeys = 0, values = 0;
    if (RegQueryInfoKeyW(user.handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                        &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS || subkeys || values) break;
    if (RegDeleteKeyW(HKEY_CURRENT_USER, parent->c_str()) != ERROR_SUCCESS) break;
  }
}
bool ownedHandlerOverlay(HKEY key, const MaybeValue &token, bool pending) {
  DWORD subkeys = 0, values = 0;
  if (!key || !validBlockRecord(token) ||
      RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                       nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS || subkeys) return false;
  if (pending && !values) return true;
  if (read(key, overlayOwner) != token) return false;
  return (values == 2 && read(key) == stringData(disabledHandlerId)) || (pending && values == 1 && !read(key));
}
void restoreHandlerRegistration(const std::wstring &path) {
  if (!isHandlerPath(path)) conflict();
  auto saved = backup(path, false, handlerJournal);
  if (!saved) return;
  if (const auto user = open(HKEY_CURRENT_USER, path)) {
    if (!ownedHandlerOverlay(user.handle, read(saved.handle, L"Token"), !read(saved.handle, L"Applied"))) conflict();
    const auto result = RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    if (result != ERROR_SUCCESS) fn::fail(L"无法修改右键菜单注册项", result);
  }
  const auto missing = stringValue(read(saved.handle, L"CleanupFrom"));
  if (!missing.empty() && !starts(missing, classes)) conflict();
  eraseEmptyOverlayParents(path, missing);
  const auto result = RegDeleteKeyW(HKEY_CURRENT_USER, journalKey(path, handlerJournal).c_str());
  if (result != ERROR_SUCCESS) fn::fail(L"无法修改右键菜单注册项", result);
}
void hideHandlerRegistration(const ExistingItem &item) {
  const auto &path = item.registryKey;
  if (!item.machine || !verifiedBlockItem(item) || open(HKEY_CURRENT_USER, path)) conflict();
  const auto relative = path.substr(std::size(classes) - 1);
  if (!equal(handlerClsid(HKEY_CLASSES_ROOT, relative), item.shellBlockId)) conflict();
  const auto before = snapshotClasses(path);
  std::wstring firstMissing;
  for (const auto &parent : overlayParents(path)) {
    if (equal(parent, L"Software\\Classes")) continue;
    if (!open(HKEY_CURRENT_USER, parent)) { firstMissing = parent; break; }
  }
  const auto saved = backup(path, true, handlerJournal);
  auto token = read(saved.handle, L"Token");
  if (token && !validBlockRecord(token)) conflict();
  if (!token) {
    token = ownerToken();
    write(saved.handle, L"CleanupFrom", stringData(firstMissing));
    write(saved.handle, L"Token", token);
  }
  Key user;
  DWORD disposition = 0;
  const auto result = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                                      nullptr, &user.handle, &disposition);
  if (result != ERROR_SUCCESS) fn::fail(L"无法修改右键菜单注册项", result);
  if (disposition != REG_CREATED_NEW_KEY) conflict();
  try {
    // Shadow only this menu registration in the merged Classes view.
    write(user.handle, overlayOwner, token);
    write(user.handle, nullptr, stringData(disabledHandlerId));
    if (handlerClsid(HKEY_CLASSES_ROOT, relative) != disabledHandlerId ||
        !ownedHandlerOverlay(user.handle, token, false) || !sameClassViews(before, snapshotClasses(path))) conflict();
    write(saved.handle, L"Applied", stringData(L"1"));
  } catch (...) {
    restoreHandlerRegistration(path);
    throw;
  }
}
void restoreOverlay(const std::wstring &path) {
  if (!isVerbPath(path)) conflict();
  auto saved = backup(path, false, overlayJournal);
  if (!saved) return;
  auto token = read(saved.handle, L"LegacyDisable");
  if (const auto user = open(HKEY_CURRENT_USER, path)) {
    if (!ownedOverlay(user.handle, token, !read(saved.handle, L"Applied"))) conflict();
    auto result = RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) fn::fail(L"无法修改右键菜单注册项", result);
  }
  auto missing = stringValue(read(saved.handle, L"CleanupFrom"));
  if (!missing.empty() && !starts(missing, classes)) conflict();
  eraseEmptyOverlayParents(path, missing);
  auto result = RegDeleteKeyW(HKEY_CURRENT_USER, journalKey(path, overlayJournal).c_str());
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) fn::fail(L"无法修改右键菜单注册项", result);
}
void hideMachineVerb(const ExistingItem &item) {
  const auto &path = item.registryKey;
  if (!item.machine || !isVerbPath(path) || open(HKEY_CURRENT_USER, path)) conflict();
  const auto verb = open(HKEY_LOCAL_MACHINE, path);
  const auto relative = path.substr(std::size(classes) - 1);
  const auto merged = open(HKEY_CLASSES_ROOT, relative);
  const auto command = verbData(HKEY_LOCAL_MACHINE, path, verb.handle);
  if (!verb || !merged || !staticVerb(verb.handle, path, verbName(path), HKEY_LOCAL_MACHINE) ||
      item.command != stringValue(command) || item.cascadeChildren != cascadeIds(verb.handle) ||
      item.cascadeTarget != cascadePath(verb.handle) || hiddenVerb(verb.handle)) conflict();
  if (verbData(HKEY_CLASSES_ROOT, relative, merged.handle) != command) conflict();
  const auto before = snapshotClasses(path);
  std::wstring firstMissing;
  for (const auto &parent : overlayParents(path)) {
    if (equal(parent, L"Software\\Classes")) continue;
    if (!open(HKEY_CURRENT_USER, parent)) { firstMissing = parent; break; }
  }
  auto saved = backup(path, true, overlayJournal);
  auto token = read(saved.handle, L"LegacyDisable");
  if (token && !validBlockRecord(token)) conflict();
  if (!token) {
    token = ownerToken();
    write(saved.handle, L"CleanupFrom", stringData(firstMissing));
    write(saved.handle, L"LegacyDisable", token);
  }
  Key user;
  DWORD disposition = 0;
  auto result = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                               nullptr, &user.handle, &disposition);
  if (result != ERROR_SUCCESS) fn::fail(L"无法修改右键菜单注册项", result);
  if (disposition != REG_CREATED_NEW_KEY) conflict();
  try {
    // Explorer may ignore a long LegacyDisable value. Keep ownership separate.
    write(user.handle, overlayOwner, token);
    write(user.handle, legacyDisable, stringData(L""));
    write(user.handle, programmaticOnly, stringData(L""));
    const auto after = open(HKEY_CLASSES_ROOT, path.substr(std::size(classes) - 1));
    if (read(after.handle, legacyDisable) != stringData(L"") ||
        read(after.handle, programmaticOnly) != stringData(L"") || !ownedOverlay(user.handle, token) ||
        !sameClassViews(before, snapshotClasses(path))) conflict();
    write(saved.handle, L"Applied", stringData(L"1"));
  } catch (...) {
    restoreOverlay(path);
    throw;
  }
}
void addCascadeChildren(ExistingItem &parent, HKEY root, HKEY verb) {
  auto paths = cascadeKeys(root, verb);
  if (!parent.cascadeChildren.empty()) {
    root = HKEY_LOCAL_MACHINE;
    paths.clear();
    for (const auto &id : parent.cascadeChildren) paths.emplace_back(commandStore + id);
  }
  for (const auto &path : paths) {
    const auto entry = open(root, path);
    if (!entry) continue;
    const auto command = open(root, path + L"\\command");
    ExistingItem item;
    const auto id = verbName(path);
    auto title = firstCaption(entry.handle, {L"MUIVerb", nullptr});
    item.label = title.empty() ? id : displayMenuText(title);
    item.source = sourceName(root == HKEY_LOCAL_MACHINE, path);
    item.registryKey = path;
    item.command = stringValue(read(command.handle));
    item.icon = stringValue(read(entry.handle, L"Icon"));
    item.machine = root == HKEY_LOCAL_MACHINE;
    item.hidden = parent.hidden || hiddenVerb(entry.handle, true);
    if (!protectedVerb(id) && !read(command.handle, L"DelegateExecute") &&
        !read(entry.handle, L"ExplorerCommandHandler") && !read(entry.handle, L"CommandStateHandler"))
      copyable(item);
    parent.children.push_back(std::move(item));
  }
}

void addVerbs(std::vector<ExistingItem> &out, bool machine, const std::wstring &className) {
  const auto root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  const auto shell = classes + className + L"\\shell";
  const auto verbs = open(root, shell);
  for (const auto &name : children(verbs.handle)) {
    if (starts(name, L"ShibaKit.")) continue;
    try {
      auto path = shell + L'\\' + name;
      auto overlay = backup(path, false, overlayJournal);
      auto token = read(overlay.handle, L"LegacyDisable");
      const auto user = open(HKEY_CURRENT_USER, path);
      const bool managedOverlay = validBlockRecord(token) &&
          (!user || ownedOverlay(user.handle, token, !read(overlay.handle, L"Applied")));
      if (!machine && managedOverlay) continue;
      auto verb = open(root, path);
      if (!verb) continue;
      auto command = open(root, path + L"\\command");
      const auto raw = stringValue(read(command.handle));
      auto label = firstCaption(verb.handle, {L"MUIVerb", nullptr});
      ExistingItem item;
      item.label = label.empty() ? name : displayMenuText(label);
      item.source = sourceName(machine, path);
      item.icon = stringValue(read(verb.handle, L"Icon"));
      item.command = raw;
      item.cascadeChildren = cascadeIds(verb.handle);
      item.cascadeTarget = cascadePath(verb.handle);
      item.registryKey = path;
      item.machine = machine;
      item.hidden = hiddenVerb(verb.handle);
      if (machine && (!user || managedOverlay) && staticVerb(verb.handle, path, name, root)) {
        item.managed = managedOverlay;
        item.hidden = item.hidden || (user && managedOverlay && hiddenVerb(user.handle));
        item.canHide = managedOverlay || (!token && !item.hidden);
        item.machineOverlay = true;
      } else if (!machine) {
        auto saved = backup(path);
        item.managed = saved && std::any_of(std::begin(fields), std::end(fields), [&](auto field) { return read(saved.handle, field).has_value(); });
        const auto writable = open(root, path, KEY_READ | KEY_SET_VALUE);
        item.canEdit = writable && staticVerb(verb.handle, path, name);
        item.canHide = item.canEdit && (!item.hidden || (saved && (read(saved.handle, legacyDisable) ||
                                                                  read(saved.handle, programmaticOnly))));
      }
      if (!protectedVerb(name) && !raw.empty() && !read(command.handle, L"DelegateExecute") &&
          !read(verb.handle, L"ExplorerCommandHandler") && !read(verb.handle, L"CommandStateHandler") &&
          !open(root, path + L"\\ddeexec")) copyable(item);
      addCascadeChildren(item, root, verb.handle);
      out.push_back(std::move(item));
    } catch (const fn::Error &) {
      ExistingItem item;
      item.label = name;
      item.source = sourceName(machine, shell + L'\\' + name);
      out.push_back(std::move(item));
    }
  }
}
void addHandlers(std::vector<ExistingItem> &out, bool machine, const std::wstring &className, bool disabled) {
  const auto root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  const auto handlerPath = classes + className +
      (disabled ? L"\\shellex\\-ContextMenuHandlers" : L"\\shellex\\ContextMenuHandlers");
  const auto handlers = open(root, handlerPath);
  for (const auto &name : children(handlers.handle)) {
    ExistingItem item;
    item.label = name;
    const auto path = handlerPath + L'\\' + name;
    item.source = sourceName(machine, path);
    item.registryKey = path;
    item.machine = machine;
    item.hidden = disabled;
    // The CLSID registration can supply a friendly caption without creating
    // the extension. Its actual menu items can depend on the selection.
    try {
      auto id = handlerClsid(root, path);
      if (!machine && !disabled) {
        const auto saved = backup(path, false, handlerJournal);
        if (saved) {
          const auto user = open(root, path);
          if (ownedHandlerOverlay(user.handle, read(saved.handle, L"Token"), !read(saved.handle, L"Applied"))) continue;
        }
      }
      if (!id.empty()) {
        auto clsidPath = std::wstring(L"CLSID\\") + id;
        auto metadata = open(HKEY_CLASSES_ROOT, clsidPath);
        auto label = firstCaption(metadata.handle, {L"LocalizedString", nullptr});
        if (!label.empty()) item.label = std::move(label);
        item.icon = defaultIcon(HKEY_CLASSES_ROOT, clsidPath);
        if (!disabled) {
          item.shellBlockModule = thirdPartyModule(id);
          const auto relative = path.substr(std::size(classes) - 1);
          const auto effective = handlerClsid(HKEY_CLASSES_ROOT, relative);
          const auto user = machine ? open(HKEY_CURRENT_USER, path) : Key{};
          const auto saved = machine ? backup(path, false, handlerJournal) : Key{};
          const auto token = read(saved.handle, L"Token");
          const bool owned = machine && ownedHandlerOverlay(user.handle, token, !read(saved.handle, L"Applied"));
          if (machine && user && !owned && !equal(effective, id)) continue;
          if (!item.shellBlockModule.empty()) {
            item.shellBlockId = std::move(id);
            readBlockState(item);
            if (owned || (machine && !user && !item.hidden && equal(effective, item.shellBlockId))) {
              item.handlerRegistration = true;
              item.hidden = owned;
              item.managed = owned;
              item.canHide = true;
            }
            if (!item.handlerRegistration && std::any_of(out.begin(), out.end(), [&](const auto &entry) {
                  return !entry.handlerRegistration && entry.shellBlockId == item.shellBlockId;
                })) continue;
          }
        }
      }
    } catch (const fn::Error &) {}
    item.registeredLabel = item.label;
    out.push_back(std::move(item));
  }
}
std::wstring extensionClass(std::wstring_view extension) {
  if (extension.empty() || extension.size() > 128 || extension.find_first_of(L"\\/:*?\r\n\t ") != extension.npos) return {};
  return extension.front() == L'.' ? std::wstring(extension) : L'.' + std::wstring(extension);
}
void addDistinct(std::vector<std::wstring> &out, std::wstring value) {
  if (!value.empty() && value.find_first_of(L"\\/\r\n") == value.npos &&
      std::none_of(out.begin(), out.end(), [&](const auto &name) { return equal(name, value); })) out.push_back(std::move(value));
}
std::vector<std::wstring> classNames(Scene scene, std::wstring_view extension) {
  switch (scene) {
  case Scene::File: return {L"*", L"AllFilesystemObjects"};
  case Scene::Folder: return {L"Directory", L"Folder", L"AllFilesystemObjects"};
  case Scene::FolderBackground: return {L"Directory\\Background"};
  case Scene::Drive: return {L"Drive", L"Folder"};
  case Scene::Desktop: return {L"DesktopBackground", L"Directory\\Background"};
  case Scene::Extension: {
    auto ext = extensionClass(extension);
    if (ext.size() < 2) return {};
    std::vector<std::wstring> result{ext, L"SystemFileAssociations\\" + ext};
    auto assoc = open(HKEY_CLASSES_ROOT, ext);
    auto userChoice = open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext + L"\\UserChoice");
    addDistinct(result, stringValue(read(userChoice.handle, L"ProgId")));
    addDistinct(result, stringValue(read(assoc.handle)));
    auto perceived = stringValue(read(assoc.handle, L"PerceivedType"));
    if (!perceived.empty() && perceived.find_first_of(L"\\/\r\n") == perceived.npos) result.push_back(L"SystemFileAssociations\\" + perceived);
    result.push_back(L"*");
    result.push_back(L"AllFilesystemObjects");
    return result;
  }
  default: return {};
  }
}
void addReadOnly(std::vector<ExistingItem> &out, std::wstring label, std::wstring source,
                 bool machine = false, std::wstring icon = {}) {
  ExistingItem item;
  item.label = std::move(label);
  item.source = std::move(source);
  item.machine = machine;
  item.icon = std::move(icon);
  out.push_back(std::move(item));
}
bool hasValue(HKEY key, const wchar_t *name) {
  return key && RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
void addNew(std::vector<ExistingItem> &out, HKEY root, bool machine) {
  auto add = [&](const std::wstring &path, const std::wstring &fallback, const std::wstring &progId, bool hidden) {
    auto key = open(root, path);
    if (!key) return;
    constexpr const wchar_t *methods[]{L"Directory", L"NullFile", L"FileName", L"Data", L"Command", L"Handler"};
    if (std::none_of(std::begin(methods), std::end(methods), [&](auto value) { return hasValue(key.handle, value); })) return;
    auto label = firstCaption(key.handle, {L"MenuText", L"ItemName"});
    auto icon = stringValue(read(key.handle, L"IconPath"));
    if (!progId.empty()) {
      if (label.empty()) {
        auto type = open(HKEY_CLASSES_ROOT, progId);
        label = firstCaption(type.handle, {L"FriendlyTypeName", nullptr});
      }
      if (icon.empty()) icon = defaultIcon(HKEY_CLASSES_ROOT, progId);
    }
    addReadOnly(out, label.empty() ? fallback : displayMenuText(label), sourceName(machine, path), machine, std::move(icon));
    out.back().hidden = hidden;
  };
  auto variants = [&](const std::wstring &base, const std::wstring &fallback, const std::wstring &progId) {
    add(base + L"\\ShellNew", fallback, progId, false);
    add(base + L"\\-ShellNew", fallback, progId, true);
  };
  variants(classes + std::wstring(L"Folder"), lang::text(L"文件夹"), L"Folder");
  auto list = open(root, L"Software\\Classes");
  for (const auto &ext : children(list.handle)) {
    if (!ext.starts_with(L'.')) continue;
    try {
      auto key = classes + ext;
      auto extension = open(root, key);
      auto merged = open(HKEY_CLASSES_ROOT, ext);
      auto progId = stringValue(read(merged.handle));
      if (progId.empty()) progId = stringValue(read(extension.handle));
      if (progId.find_first_of(L"\\/\r\n") != progId.npos) progId.clear();
      variants(key, ext, progId);
      // Office and other applications register under an extension's ProgID.
      // The default association can change without moving those registrations.
      for (const auto &candidate : children(extension.handle))
        if (!equal(candidate, L"ShellNew") && !equal(candidate, L"-ShellNew"))
          variants(key + L'\\' + candidate, ext, candidate);
    } catch (const fn::Error &) {}
  }
}
std::filesystem::path sendToDirectory() {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &raw))) return {};
  std::filesystem::path path(raw);
  CoTaskMemFree(raw);
  return path;
}
struct File {
  HANDLE handle;
  explicit File(const std::filesystem::path &path, DWORD access = FILE_READ_ATTRIBUTES)
      : handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {}
  ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
};
std::uint64_t combined(DWORD high, DWORD low) { return (std::uint64_t{high} << 32) | low; }
struct FileIdentity {
  std::uint64_t volume, index, created, written;
  bool operator==(const FileIdentity &) const = default;
};
FileIdentity fileIdentity(const BY_HANDLE_FILE_INFORMATION &info) {
  return {info.dwVolumeSerialNumber, combined(info.nFileIndexHigh, info.nFileIndexLow),
      combined(info.ftCreationTime.dwHighDateTime, info.ftCreationTime.dwLowDateTime),
      combined(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime)};
}
FileIdentity readFileIdentity(const Value &value) {
  if (value.type != REG_BINARY || value.data.size() != sizeof(FileIdentity)) conflict();
  FileIdentity result{};
  memcpy(&result, value.data.data(), sizeof(result));
  return result;
}
bool fileInfo(const File &file, BY_HANDLE_FILE_INFORMATION &info) {
  return file.handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file.handle, &info) &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}
void updateFileVisibility(const File &file, DWORD attributes, bool hidden) {
  FILE_BASIC_INFO info{};
  info.FileAttributes = attributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_HIDDEN);
  if (hidden) info.FileAttributes |= FILE_ATTRIBUTE_HIDDEN;
  if (!info.FileAttributes) info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
  if (!SetFileInformationByHandle(file.handle, FileBasicInfo, &info, sizeof(info)))
    fn::fail(L"无法修改右键菜单注册项");
}
void changeSendTo(const std::wstring &path, bool hidden, const FileIdentity *selected = nullptr) {
  const auto directory = sendToDirectory();
  const std::filesystem::path target(path);
  if (directory.empty() || !equal(directory.native(), target.parent_path().native()) ||
      target.filename().empty() || target.filename().native().find(L':') != std::wstring::npos ||
      equal(target.filename().native(), L"desktop.ini")) conflict();
  File file(target, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
  BY_HANDLE_FILE_INFORMATION info{};
  if (!fileInfo(file, info)) conflict();
  const auto identity = fileIdentity(info);
  const bool currentlyHidden = (info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
  if (selected && *selected != identity) conflict();
  auto saved = backup(path, hidden, sendToJournal);
  const auto stored = read(saved.handle, L"Identity");
  if (stored && readFileIdentity(*stored) != identity) conflict();
  if (hidden) {
    if (currentlyHidden && !stored) conflict();
    if (!stored) {
      Value record{REG_BINARY, {}};
      append(record.data, &identity, sizeof(identity));
      write(saved.handle, L"Identity", record);
    }
    if (!currentlyHidden) updateFileVisibility(file, info.dwFileAttributes, true);
  } else if (stored) {
    if (currentlyHidden) updateFileVisibility(file, info.dwFileAttributes, false);
    const auto result = RegDeleteKeyW(HKEY_CURRENT_USER, journalKey(path, sendToJournal).c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) fn::fail(L"无法修改右键菜单注册项", result);
  }
  SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path.c_str(), nullptr);
}
void addSendTo(std::vector<ExistingItem> &out) {
  auto path = sendToDirectory();
  if (path.empty()) return;
  std::error_code error;
  const std::filesystem::directory_iterator end;
  for (auto item = std::filesystem::directory_iterator(path, error); !error && item != end; item.increment(error)) {
    auto full = item->path();
    if (equal(full.filename().native(), L"desktop.ini")) continue;
    try {
      ExistingItem entry;
      entry.label = full.stem().native();
      entry.source = full.native();
      wchar_t resource[MAX_PATH]{};
      int resourceId = 0;
      if (SUCCEEDED(SHGetLocalizedName(full.c_str(), resource, ARRAYSIZE(resource), &resourceId))) {
        auto localized = caption(L"@" + std::wstring(resource) + L"," + std::to_wstring(resourceId));
        if (!localized.empty()) entry.label = std::move(localized);
      }
      entry.icon = full.native();
      entry.sendTo = true;
      File file(full, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
      BY_HANDLE_FILE_INFORMATION info{};
      if (fileInfo(file, info)) {
        auto identity = fileIdentity(info);
        entry.fileVolume = info.dwVolumeSerialNumber;
        entry.fileIndex = identity.index;
        entry.fileCreated = identity.created;
        entry.fileWritten = identity.written;
        entry.hidden = (info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        auto saved = backup(entry.source, false, sendToJournal);
        auto stored = read(saved.handle, L"Identity");
        entry.managed = stored.has_value();
        entry.canHide = stored ? readFileIdentity(*stored) == identity : !entry.hidden;
      }
      out.push_back(std::move(entry));
    } catch (const fn::Error &) { addReadOnly(out, full.stem().native(), full.native()); }
  }
}
void addOpenWith(std::vector<ExistingItem> &out, std::wstring_view extension) {
  auto add = [&](const std::wstring &name, const std::wstring &source, bool machine, bool application) {
    if (name.empty() || name.find_first_of(L"\\/") != name.npos) return;
    const auto registration = application ? L"Applications\\" + name : name;
    auto key = open(HKEY_CLASSES_ROOT, registration);
    auto label = application ? firstCaption(key.handle, {L"FriendlyAppName", nullptr})
                             : firstCaption(key.handle, {L"FriendlyTypeName", nullptr});
    auto icon = defaultIcon(HKEY_CLASSES_ROOT, registration);
    addReadOnly(out, label.empty() ? name : std::move(label), source, machine, std::move(icon));
  };
  for (bool machine : {false, true}) {
    auto root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    std::vector<std::wstring> locations{std::wstring(classes) + L"Applications"};
    auto ext = extensionClass(extension);
    if (ext.size() > 1) {
      locations.push_back(classes + ext + L"\\OpenWithList");
      locations.push_back(classes + ext + L"\\OpenWithProgids");
      if (!machine) locations.push_back(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext + L"\\OpenWithList");
    }
    for (const auto &location : locations) {
      auto key = open(root, location);
      const bool applications = location.ends_with(L"\\Applications");
      const bool programs = !location.ends_with(L"\\OpenWithProgids");
      for (const auto &name : children(key.handle)) add(name, sourceName(machine, location + L'\\' + name), machine, programs);
      if (applications) continue;
      for (const auto &name : children(key.handle, true)) {
        if (equal(name, L"MRUList")) continue;
        try {
          auto label = stringValue(read(key.handle, name.c_str()));
          add(label.empty() ? name : label, sourceName(machine, location) + L" [" + name + L"]", machine, programs);
        } catch (const fn::Error &) {}
      }
    }
  }
}
}

std::vector<ExistingItem> inventory(Scene scene, std::wstring_view extension) {
  std::vector<ExistingItem> result;
  if (scene == Scene::SendTo) addSendTo(result);
  else if (scene == Scene::OpenWith) addOpenWith(result, extension);
  else if (scene == Scene::New) {
    addNew(result, HKEY_CURRENT_USER, false);
    addNew(result, HKEY_LOCAL_MACHINE, true);
  } else {
    const auto names = classNames(scene, extension);
    for (const auto &name : names)
      for (bool machine : {false, true}) {
        addVerbs(result, machine, name);
        addHandlers(result, machine, name, false);
      }
    for (const auto &name : names)
      for (bool machine : {false, true}) addHandlers(result, machine, name, true);
  }
  std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return CompareStringOrdinal(a.label.c_str(), -1, b.label.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  return result;
}
void setHidden(const ExistingItem &item, bool hidden) {
  if (!item.canHide) conflict();
  {
    Mutex lock;
    if (item.handlerRegistration) {
      if (hidden) hideHandlerRegistration(item);
      else restoreHandlerRegistration(item.registryKey);
    } else if (item.shellBlock) {
      if (hidden && !verifiedBlockItem(item)) conflict();
      changeBlock(item.shellBlockId, hidden);
    } else if (item.machineOverlay) {
      if (hidden) hideMachineVerb(item);
      else restoreOverlay(item.registryKey);
    } else if (item.sendTo) {
      const FileIdentity selected{item.fileVolume, item.fileIndex, item.fileCreated, item.fileWritten};
      changeSendTo(item.source, hidden, &selected);
    } else {
      changeVisibility(item, hidden);
    }
  }
  changed(!item.sendTo);
}
void setAppearance(const ExistingItem &item, std::wstring_view label, std::wstring_view icon) {
  if (!item.canEdit || label.size() > 256 || icon.size() > 2048 ||
      label.find(L'\0') != label.npos || icon.find(L'\0') != icon.npos) conflict();
  Mutex lock;
  if (label.empty() || label != item.label) change(item, fields[1], label.empty() ? MaybeValue{} : stringData(label));
  if (icon.empty() || icon != item.icon) change(item, fields[2], icon.empty() ? MaybeValue{} : stringData(icon));
  changed();
}
void restore(const ExistingItem &item) {
  if (!item.shellBlock && !item.machineOverlay && (item.machine || (!item.sendTo && item.registryKey.empty()))) conflict();
  {
    Mutex lock;
    if (item.handlerRegistration) restoreHandlerRegistration(item.registryKey);
    else if (item.shellBlock) changeBlock(item.shellBlockId, false);
    else if (item.machineOverlay) restoreOverlay(item.registryKey);
    else if (item.sendTo) changeSendTo(item.source, false);
    else restoreLocked(item.registryKey);
  }
  changed(!item.sendTo);
}
size_t restoreAll() {
  Mutex lock;
  size_t failed = 0;
  try {
    const auto backup = blockKey(HKEY_CURRENT_USER, blockJournal);
    for (const auto &id : children(backup.handle, true)) {
      try { changeBlock(id, false); }
      catch (const fn::Error &) { ++failed; }
    }
  } catch (const fn::Error &) { ++failed; }
  for (const auto scope : {journal, sendToJournal, overlayJournal, handlerJournal}) {
    auto root = open(HKEY_CURRENT_USER, scope);
    for (const auto &name : children(root.handle)) {
      try {
        auto full = std::wstring(scope) + L'\\' + name;
        const auto key = open(HKEY_CURRENT_USER, full);
        auto path = stringValue(read(key.handle, L"Path"));
        if (path.empty() || journalKey(path, scope) != full) conflict();
        if (scope == sendToJournal) changeSendTo(path, false);
        else if (scope == overlayJournal) restoreOverlay(path);
        else if (scope == handlerJournal) restoreHandlerRegistration(path);
        else restoreLocked(path);
      } catch (const fn::Error &) { ++failed; }
    }
  }
  changed(true);
  return failed;
}
}
