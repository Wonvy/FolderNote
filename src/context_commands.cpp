#include "context_commands.h"
#include "core.h"
#include "language.h"
#include "quick_folders.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

namespace context {
namespace {
using namespace std::literals;
constexpr auto configKey = L"Software\\ShibaKit";
constexpr auto configValue = L"ContextCommandsV1";
constexpr size_t maxCommands = 128, maxUnits = 256 * 1024;
constexpr auto menuName = L"ShibaKit.ContextCommands";
struct MenuLocation {
  const wchar_t *key;
  const wchar_t *argument;
};
constexpr std::array menuLocations{
    MenuLocation{L"*", L"%1"}, MenuLocation{L"Directory", L"%1"},
    MenuLocation{L"Directory\\Background", L"%V"}, MenuLocation{L"Drive", L"%1"},
    MenuLocation{L"DesktopBackground", L""}};

struct Configuration {
  std::wstring revision;
  std::vector<Command> commands;
  bool enabled = false;
};
struct Key {
  HKEY handle = nullptr;
  ~Key() { if (handle) RegCloseKey(handle); }
};
struct Handle {
  HANDLE handle;
  ~Handle() { if (handle) CloseHandle(handle); }
};
struct RegistryLock {
  Handle mutex{CreateMutexW(nullptr, FALSE, L"Local\\ShibaKit.ContextCommands.v1")};
  RegistryLock() {
    if (!mutex.handle) fn::fail(L"无法保存右键菜单配置");
    const auto result = WaitForSingleObject(mutex.handle, 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
      throw fn::Error{lang::text(L"正在更新右键菜单，请稍后重试。")};
  }
  ~RegistryLock() { ReleaseMutex(mutex.handle); }
};
struct FreeLocal {
  void operator()(void *data) const { LocalFree(data); }
};

[[noreturn]] void invalidConfiguration() {
  throw fn::Error{lang::text(L"右键菜单命令配置已损坏。")};
}
std::wstring menuKey(const MenuLocation &location) {
  return std::wstring(L"Software\\Classes\\") + location.key + L"\\shell\\" + menuName;
}
bool equal(std::wstring_view left, std::wstring_view right) {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
bool prefix(std::wstring_view value, std::wstring_view start) {
  return value.size() >= start.size() && equal(value.substr(0, start.size()), start);
}
bool validText(const std::wstring &value, size_t limit) {
  return value.size() <= limit && std::none_of(value.begin(), value.end(), [](wchar_t c) {
    return c < 32 && c != L'\t';
  });
}
bool validScene(Scene scene) {
  switch (scene) {
  case Scene::File: case Scene::Extension: case Scene::Folder: case Scene::FolderBackground:
  case Scene::Drive: case Scene::Desktop: return true;
  default: return false;
  }
}
bool hasPlaceholders(std::wstring_view value) {
  return value.find(L"{path}") != value.npos || value.find(L"{parent}") != value.npos;
}
void validateShellArguments(const Command &command) {
  wchar_t quoteCharacter = 0;
  const auto script = std::wstring_view(command.arguments);
  for (size_t i = 0; i < script.size(); ++i) {
    const auto ch = script[i];
    const auto tail = script.substr(i);
    if (tail.starts_with(L"{path}") || tail.starts_with(L"{parent}")) {
      if (quoteCharacter) throw fn::Error{lang::text(L"请在引号之外使用路径占位符。")};
    } else if ((command.mode == Mode::PowerShell && ch == L'`' && quoteCharacter != L'\'') ||
               (command.mode == Mode::Cmd && ch == L'^' && !quoteCharacter)) {
      if (i + 1 < script.size()) ++i;
    } else if (ch == L'"' || (command.mode == Mode::PowerShell && ch == L'\'')) {
      if (quoteCharacter == ch) quoteCharacter = 0;
      else if (!quoteCharacter) quoteCharacter = ch;
    }
  }
}
bool systemProgram(const std::wstring &value) {
  return value.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") == value.npos &&
      (equal(fn::fs::path(value).extension().native(), L".exe") || equal(fn::fs::path(value).extension().native(), L".com"));
}
void normalizeCommand(Command &command) {
  if (!validScene(command.scene) || static_cast<unsigned>(command.mode) > static_cast<unsigned>(Mode::Url) ||
      command.name.find_first_not_of(L" \t") == command.name.npos || !validText(command.name, 120) ||
      !validText(command.program, 4096) || !validText(command.arguments, 8192) || !validText(command.directory, 4096) ||
      !validText(command.pathFilter, 4096) || !validText(command.icon, 4096))
    throw fn::Error{lang::text(L"请输入有效的菜单名称、目标类型和命令。")};
  if (command.scene == Scene::Extension) {
    if (!command.extension.starts_with(L'.')) command.extension.insert(command.extension.begin(), L'.');
    if (command.extension.size() < 2 || command.extension.size() > 32 ||
        command.extension.find_first_not_of(L".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != command.extension.npos ||
        command.extension.find(L'.', 1) != command.extension.npos)
      throw fn::Error{lang::text(L"请输入有效的文件扩展名。")};
    std::transform(command.extension.begin(), command.extension.end(), command.extension.begin(),
                   [](wchar_t ch) { return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch; });
  } else command.extension.clear();
  if (!command.pathFilter.empty()) {
    auto path = quick::normalize(command.pathFilter);
    if (path.empty()) throw fn::Error{lang::text(L"请输入绝对文件夹路径。")};
    command.pathFilter = std::move(path);
  }
  if (!command.directory.empty() && !hasPlaceholders(command.directory) && quick::normalize(command.directory).empty())
    throw fn::Error{lang::text(L"请输入绝对文件夹路径。")};
  if (command.mode == Mode::Url) {
    if ((!prefix(command.program, L"https://") && !prefix(command.program, L"http://")) ||
        command.program.find_first_of(L" \t\"") != command.program.npos || hasPlaceholders(command.program))
      throw fn::Error{lang::text(L"请输入 HTTP 或 HTTPS 网址。")};
  } else if (command.mode == Mode::Program) {
    if (quick::normalize(command.program).empty() && !systemProgram(command.program))
      throw fn::Error{lang::text(L"请输入程序的绝对路径或 Windows 系统程序名。")};
  } else if (command.arguments.find_first_not_of(L" \t") == command.arguments.npos) {
    throw fn::Error{lang::text(L"请输入有效的菜单名称、目标类型和命令。")};
  } else {
    validateShellArguments(command);
  }
}

std::vector<wchar_t> readConfiguration() {
  DWORD bytes = 0;
  auto error = RegGetValueW(HKEY_CURRENT_USER, configKey, configValue, RRF_RT_REG_BINARY, nullptr, nullptr, &bytes);
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
  if (error || !bytes || bytes % sizeof(wchar_t) || bytes > maxUnits * sizeof(wchar_t)) invalidConfiguration();
  std::vector<wchar_t> data(bytes / sizeof(wchar_t));
  if (RegGetValueW(HKEY_CURRENT_USER, configKey, configValue, RRF_RT_REG_BINARY, nullptr, data.data(), &bytes) ||
      bytes != data.size() * sizeof(wchar_t)) invalidConfiguration();
  return data;
}
Configuration decode(const std::vector<wchar_t> &data) {
  if (data.empty()) return {};
  size_t offset = 0;
  auto number = [&] {
    if (offset >= data.size()) invalidConfiguration();
    return static_cast<unsigned>(data[offset++]);
  };
  auto string = [&] {
    size_t length = number();
    if (length > data.size() - offset) invalidConfiguration();
    std::wstring value(data.data() + offset, length);
    offset += length;
    return value;
  };
  Configuration result;
  auto enabled = number();
  if (enabled > 1) invalidConfiguration();
  result.enabled = enabled != 0;
  result.revision = string();
  if (result.revision.size() != 38) invalidConfiguration();
  auto count = number();
  if (count > maxCommands) invalidConfiguration();
  for (unsigned i = 0; i < count; ++i) {
    Command command;
    command.scene = static_cast<Scene>(number());
    command.mode = static_cast<Mode>(number());
    auto flags = number();
    if (flags > 3) invalidConfiguration();
    command.enabled = (flags & 1) != 0;
    command.includeChildren = (flags & 2) != 0;
    for (auto field : {&command.name, &command.extension, &command.program, &command.arguments,
                       &command.directory, &command.pathFilter, &command.icon})
      *field = string();
    normalizeCommand(command);
    result.commands.push_back(std::move(command));
  }
  if (offset != data.size()) invalidConfiguration();
  return result;
}
std::vector<wchar_t> encode(const Configuration &configuration) {
  std::vector<wchar_t> data;
  auto number = [&](unsigned value) { data.push_back(static_cast<wchar_t>(value)); };
  auto string = [&](const std::wstring &value) {
    number(static_cast<unsigned>(value.size()));
    data.insert(data.end(), value.begin(), value.end());
  };
  number(configuration.enabled);
  string(configuration.revision);
  number(static_cast<unsigned>(configuration.commands.size()));
  for (const auto &command : configuration.commands) {
    number(static_cast<unsigned>(command.scene));
    number(static_cast<unsigned>(command.mode));
    number((command.enabled ? 1u : 0u) | (command.includeChildren ? 2u : 0u));
    for (const auto field : {&command.name, &command.extension, &command.program, &command.arguments,
                            &command.directory, &command.pathFilter, &command.icon}) string(*field);
  }
  if (data.size() > maxUnits) throw fn::Error{lang::text(L"右键菜单命令数量或内容超出限制。")};
  return data;
}
void store(const std::vector<wchar_t> &data) {
  LSTATUS error;
  if (data.empty()) {
    error = RegDeleteKeyValueW(HKEY_CURRENT_USER, configKey, configValue);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
  } else {
    error = RegSetKeyValueW(HKEY_CURRENT_USER, configKey, configValue, REG_BINARY, data.data(),
                           static_cast<DWORD>(data.size() * sizeof(wchar_t)));
  }
  if (error) fn::fail(L"无法保存右键菜单配置", error);
}
std::wstring quote(std::wstring_view argument) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (auto ch : argument) {
    if (ch == L'\\') { ++slashes; continue; }
    result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    result += ch;
  }
  result.append(slashes * 2, L'\\');
  result += L'"';
  return result;
}
void registryString(const std::wstring &key, const wchar_t *name, const std::wstring &value) {
  auto error = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  if (error) fn::fail(L"无法注册 ShibaKit 右键菜单", error);
}
void removeRegistration() {
  for (const auto &location : menuLocations) {
    auto error = RegDeleteTreeW(HKEY_CURRENT_USER, menuKey(location).c_str());
    if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
      fn::fail(L"无法移除 ShibaKit 右键菜单", error);
  }
}
size_t locationIndex(Scene scene) {
  switch (scene) {
  case Scene::Folder: return 1;
  case Scene::FolderBackground: return 2;
  case Scene::Drive: return 3;
  case Scene::Desktop: return 4;
  default: return 0;
  }
}
std::wstring folderPrefix(std::wstring value) {
  if (!value.empty() && value.back() != L'\\') value += L'\\';
  return value;
}
std::wstring appliesTo(const Command &command) {
  std::wstring condition;
  if (command.scene == Scene::Extension)
    condition = L"System.FileExtension:=\"" + command.extension + L"\"";
  if (!command.pathFilter.empty() && command.scene != Scene::Desktop) {
    std::wstring property = (command.scene == Scene::File || command.scene == Scene::Extension)
        ? L"System.ItemFolderPathDisplay" : L"System.ItemPathDisplay";
    if (!condition.empty()) condition += L" AND ";
    condition += L"(" + property + L":=\"" + command.pathFilter + L"\"";
    if (command.includeChildren) condition += L" OR " + property + L":~<\"" + folderPrefix(command.pathFilter) + L"\"";
    condition += L")";
  }
  return condition;
}
std::wstring token(const Configuration &configuration, size_t index) {
  return configuration.revision + L":" + std::to_wstring(index);
}
void updateRegistration(const Configuration &configuration, const fn::fs::path &exe) {
  removeRegistration();
  if (configuration.enabled) {
    std::array<unsigned, menuLocations.size()> counts{};
    std::vector<size_t> order;
    for (size_t index = 0; index < configuration.commands.size(); ++index)
      if (configuration.commands[index].enabled) order.push_back(index);
    std::stable_partition(order.begin(), order.end(), [&](size_t index) {
      return configuration.commands[index].scene == Scene::Extension;
    });
    for (const auto index : order) {
      const auto &command = configuration.commands[index];
      const auto location = locationIndex(command.scene);
      const auto root = menuKey(menuLocations[location]);
      if (!counts[location]) {
        registryString(root, nullptr, L"");
        registryString(root, L"MUIVerb", L"ShibaKit");
        registryString(root, L"MultiSelectModel", L"Single");
        registryString(root, L"Icon", quote(exe.native()) + L",0");
      }
      // Static cascade verbs are enumerated by key name.
      const auto item = root + L"\\ExtendedSubCommandsKey\\shell\\" +
          std::to_wstring(1000 + counts[location]++);
      registryString(item, L"MUIVerb", command.name);
      registryString(item, L"MultiSelectModel", L"Single");
      if (!command.icon.empty()) registryString(item, L"Icon", command.icon);
      const auto filter = appliesTo(command);
      if (!filter.empty()) registryString(item, L"AppliesTo", filter);
      const auto invocation = quote(exe.native()) + L" --context-command " + quote(token(configuration, index)) +
          L" \"" + menuLocations[location].argument + L"|\"";
      registryString(item + L"\\command", nullptr, invocation);
    }
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
std::wstring makeRevision() {
  GUID id{};
  wchar_t revision[40]{};
  if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, revision, ARRAYSIZE(revision)))
    throw fn::Error{lang::text(L"无法创建右键菜单配置标识。")};
  return revision;
}

std::wstring replacePaths(std::wstring_view value, std::wstring_view path, std::wstring_view parent) {
  std::wstring result;
  for (size_t pos = 0; pos < value.size();) {
    auto tail = value.substr(pos);
    if (tail.starts_with(L"{path}")) { result += path; pos += 6; }
    else if (tail.starts_with(L"{parent}")) { result += parent; pos += 8; }
    else result += value[pos++];
  }
  return result;
}
std::wstring systemDirectory() {
  wchar_t path[MAX_PATH]{};
  const auto length = GetSystemDirectoryW(path, ARRAYSIZE(path));
  if (!length || length >= ARRAYSIZE(path)) fn::fail(L"无法找到 Windows 系统目录");
  return path;
}
std::wstring programArguments(const Command &command, const std::wstring &path, const std::wstring &parent) {
  const auto commandLine = L"ShibaKit " + command.arguments;
  int count = 0;
  std::unique_ptr<LPWSTR, FreeLocal> arguments(CommandLineToArgvW(commandLine.c_str(), &count));
  if (!arguments) fn::fail(L"无法启动右键菜单命令");
  std::wstring result;
  for (int i = 1; i < count; ++i) {
    result += L' ';
    result += quote(replacePaths(arguments.get()[i], path, parent));
  }
  return result;
}
std::wstring shellArguments(const Command &command) {
  if (command.mode == Mode::Cmd) {
    // Delayed expansion inserts the path after cmd parses metacharacters and
    // percent expansions. The selected path stays outside the command text.
    return L"/D /V:ON /S /C \"" + replacePaths(command.arguments, L"\"!SHIBAKIT_CONTEXT_PATH!\"", L"\"!SHIBAKIT_CONTEXT_PARENT!\"") + L"\"";
  }
  return replacePaths(command.arguments, L"($env:SHIBAKIT_CONTEXT_PATH)", L"($env:SHIBAKIT_CONTEXT_PARENT)");
}
std::wstring base64(std::wstring_view value) {
  constexpr wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto data = reinterpret_cast<const unsigned char *>(value.data());
  const auto size = value.size() * sizeof(wchar_t);
  std::wstring result;
  for (size_t offset = 0; offset < size; offset += 3) {
    const bool second = offset + 1 < size, third = offset + 2 < size;
    unsigned bits = unsigned(data[offset]) << 16;
    if (second) bits |= unsigned(data[offset + 1]) << 8;
    if (third) bits |= data[offset + 2];
    result += alphabet[(bits >> 18) & 63];
    result += alphabet[(bits >> 12) & 63];
    result += second ? alphabet[(bits >> 6) & 63] : L'=';
    result += third ? alphabet[bits & 63] : L'=';
  }
  return result;
}
std::wstring executable(const Command &command) {
  if (command.mode == Mode::Cmd) return systemDirectory() + L"\\cmd.exe";
  if (command.mode == Mode::PowerShell) return systemDirectory() + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  if (systemProgram(command.program)) return systemDirectory() + L"\\" + command.program;
  return command.program;
}
std::vector<wchar_t> commandEnvironment(const std::wstring &path, const std::wstring &parent) {
  auto environment = GetEnvironmentStringsW();
  if (!environment) fn::fail(L"无法读取命令运行环境");
  struct Environment { LPWCH value; ~Environment() { FreeEnvironmentStringsW(value); } } saved{environment};
  std::vector<std::wstring> variables;
  for (auto entry = environment; *entry; entry += wcslen(entry) + 1) {
    if (!prefix(entry, L"SHIBAKIT_CONTEXT_PATH=") && !prefix(entry, L"SHIBAKIT_CONTEXT_PARENT="))
      variables.emplace_back(entry);
  }
  variables.push_back(L"SHIBAKIT_CONTEXT_PATH=" + path);
  variables.push_back(L"SHIBAKIT_CONTEXT_PARENT=" + parent);
  std::sort(variables.begin(), variables.end(), [](const auto &a, const auto &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  std::vector<wchar_t> block;
  for (const auto &value : variables) block.insert(block.end(), value.c_str(), value.c_str() + value.size() + 1);
  block.push_back(0);
  return block;
}
std::wstring selectedPath(const Command &command, std::wstring argument) {
  if (!argument.ends_with(L'|')) return {};
  argument.pop_back();
  if (command.scene == Scene::Desktop) {
    PWSTR path = nullptr;
    auto result = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &path);
    std::wstring desktop = path ? path : L"";
    CoTaskMemFree(path);
    if (FAILED(result)) fn::fail(L"无法读取桌面文件夹", static_cast<DWORD>(result));
    return quick::normalize(desktop);
  }
  return quick::normalize(argument);
}
bool matches(const Command &command, const std::wstring &path) {
  if (path.empty()) return false;
  auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  const bool files = command.scene == Scene::File || command.scene == Scene::Extension;
  if (files == ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) return false;
  if (command.scene == Scene::Extension && !equal(fn::fs::path(path).extension().native(), command.extension)) return false;
  if (command.scene == Scene::Drive && !equal(path, fn::fs::path(path).root_path().native())) return false;
  if (command.pathFilter.empty()) return true;
  const auto directory = files ? fn::fs::path(path).parent_path().native() : path;
  return equal(directory, command.pathFilter) || (command.includeChildren && prefix(directory, folderPrefix(command.pathFilter)));
}
}

bool commandsEnabled() {
  RegistryLock lock;
  return decode(readConfiguration()).enabled;
}
std::vector<Command> loadCommands() {
  RegistryLock lock;
  return decode(readConfiguration()).commands;
}
std::wstring previewCommand(const Command &value) {
  auto command = value;
  normalizeCommand(command);
  if (command.mode == Mode::Url) return command.program;
  const auto file = executable(command);
  if (command.mode == Mode::Program)
    return quote(file) + programArguments(command, L"C:\\Example Folder\\Document.txt", L"C:\\Example Folder");
  if (command.mode == Mode::Cmd) return quote(file) + L" " + shellArguments(command);
  return quote(file) + L" -NoProfile -Command " + shellArguments(command);
}
void saveCommands(bool enabled, const std::vector<Command> &commands, const fn::fs::path &exe) {
  if (commands.size() > maxCommands) throw fn::Error{lang::text(L"右键菜单命令数量或内容超出限制。")};
  Configuration candidate{makeRevision(), commands, enabled};
  for (auto &command : candidate.commands) normalizeCommand(command);
  const auto serialized = encode(candidate);
  RegistryLock lock;
  const auto previous = readConfiguration();
  store(serialized);
  try {
    updateRegistration(candidate, exe);
  } catch (...) {
    store(previous);
    updateRegistration(decode(previous), exe);
    throw;
  }
}
void registerCommands(const fn::fs::path &exe) {
  RegistryLock lock;
  const auto current = decode(readConfiguration());
  if (!current.revision.empty()) updateRegistration(current, exe);
}
void unregisterCommands() {
  RegistryLock lock;
  removeRegistration();
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
int runCommand(const std::wstring &identifier, const std::wstring &selection) {
  const auto configuration = [] {
    RegistryLock lock;
    return decode(readConfiguration());
  }();
  const auto &items = configuration.commands;
  size_t index = 0;
  while (index < items.size() && token(configuration, index) != identifier) ++index;
  if (!configuration.enabled || index == items.size() || !items[index].enabled)
    throw fn::Error{lang::text(L"此菜单命令已更新或停用，请重新打开右键菜单。")};
  const auto &command = items[index];
  const auto path = selectedPath(command, selection);
  if (!matches(command, path)) throw fn::Error{lang::text(L"选中的项目不符合此命令的适用范围。")};
  auto parent = fn::fs::path(path).parent_path().native();
  if (parent.empty()) parent = path;
  auto directory = command.directory.empty() ? std::wstring{} :
      quick::normalize(replacePaths(command.directory, path, parent));
  if (!command.directory.empty() && directory.empty()) throw fn::Error{lang::text(L"请输入绝对文件夹路径。")};
  auto cwd = directory.empty() ? nullptr : directory.c_str();
  const auto program = executable(command);
  const auto extension = fn::fs::path(program).extension().native();
  if (command.mode == Mode::Url || (command.mode == Mode::Program && !equal(extension, L".exe") && !equal(extension, L".com"))) {
    if (command.mode == Mode::Program && !command.arguments.empty()) throw fn::Error{lang::text(L"打开文件时，请使用空参数。")};
    SHELLEXECUTEINFOW request{sizeof(request)};
    request.lpVerb = L"open";
    request.lpFile = program.c_str();
    request.lpDirectory = cwd;
    request.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&request)) fn::fail(L"无法启动右键菜单命令");
    return 0;
  }
  std::wstring commandLine = quote(program);
  std::vector<wchar_t> environment;
  if (command.mode == Mode::Program) commandLine += programArguments(command, path, parent);
  else {
    environment = commandEnvironment(path, parent);
    auto arguments = shellArguments(command);
    if (command.mode == Mode::PowerShell) arguments = L"-NoProfile -EncodedCommand " + base64(arguments);
    commandLine += L" " + arguments;
  }
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                      environment.empty() ? nullptr : environment.data(), cwd, &startup, &process))
    fn::fail(L"无法启动右键菜单命令");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return 0;
}
}
