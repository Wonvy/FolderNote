#include "desktop_groups_search.h"
#include "core.h"
#include <algorithm>
#include <array>
#include <shlobj.h>

namespace desktop_groups::search {
namespace {
const std::wstring &tabName(const Group &group, size_t tab) { return tab ? group.tabs[tab - 1].name : group.name; }
const std::vector<std::wstring> &paths(const Group &group, size_t tab) { return tab ? group.tabs[tab - 1].paths : group.paths; }
}
std::vector<Entry> collect(const std::vector<Group> &groups, Canceled canceled, size_t limit) {
  std::vector<Entry> result;
  const auto deadline = GetTickCount64() + 5000;
  const auto done = [&] { return result.size() >= limit || GetTickCount64() >= deadline || (canceled && canceled()); };
  for (size_t g = 0; g < groups.size() && !done(); ++g) {
    const auto &group = groups[g];
    for (size_t t = 0; t <= group.tabs.size() && !done(); ++t) {
      const auto append = [&](const std::wstring &path, DWORD attributes) {
        if (done() || attributes == INVALID_FILE_ATTRIBUTES) return;
        auto name = std::filesystem::path(path).filename().wstring();
        if (name.empty()) return;
        Entry entry{g, t, group.name, tabName(group, t), path, name, name + L"  ·  " + group.name};
        if (t) entry.label += L" / " + entry.tabName;
        result.push_back(std::move(entry));
      };
      const auto &folder = folderForTab(group, t);
      if (folder.empty()) {
        for (const auto &path : paths(group, t)) {
          if (done()) break;
          append(path, GetFileAttributesW(path.c_str()));
        }
      } else {
        // Match the box's direct-child view; never recursively index a disk.
        std::error_code error;
        for (std::filesystem::directory_iterator item(folder, error), end; !error && item != end && !done(); item.increment(error)) {
          const auto path = item->path().wstring();
          const auto attributes = GetFileAttributesW(path.c_str());
          if (!(attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) append(path, attributes);
        }
      }
    }
  }
  return result;
}
bool matches(const std::vector<Group> &groups, const Entry &entry) {
  if (entry.group >= groups.size() || entry.path.empty() || entry.path.size() > 32760 ||
      entry.path.find(L'\0') != std::wstring::npos) return false;
  const auto &group = groups[entry.group];
  if (entry.tab > group.tabs.size() || group.name != entry.groupName || tabName(group, entry.tab) != entry.tabName) return false;
  const auto &folder = folderForTab(group, entry.tab);
  if (!folder.empty()) {
    const std::filesystem::path path(entry.path);
    return path.is_absolute() && path == path.lexically_normal() && samePath(path.parent_path().wstring(), folder);
  }
  const auto &entries = paths(group, entry.tab);
  return std::any_of(entries.begin(), entries.end(), [&](const auto &path) { return samePath(path, entry.path); });
}
std::wstring encode(const Entry &entry) {
  std::wstring result;
  for (const auto &part : {std::to_wstring(entry.group), std::to_wstring(entry.tab), entry.groupName, entry.tabName, entry.path}) {
    if (part.empty() || part.find(L'\0') != std::wstring::npos) return {};
    result += part; result.push_back(L'\0');
  }
  return result.size() <= requestLimit ? result : std::wstring{};
}
std::optional<Entry> decode(std::span<const wchar_t> text) {
  if (text.empty() || text.size() > requestLimit || text.back() != L'\0') return std::nullopt;
  std::array<std::wstring, 5> fields;
  for (auto &field : fields) {
    const auto end = std::find(text.begin(), text.end(), L'\0');
    if (end == text.end() || end == text.begin()) return std::nullopt;
    field.assign(text.begin(), end); text = text.subspan(field.size() + 1);
  }
  if (!text.empty() || fields[2].size() > groupNameLimit || fields[3].size() > groupNameLimit || fields[4].size() > 32760) return std::nullopt;
  const auto number = [](const std::wstring &value) -> std::optional<size_t> {
    size_t result = 0;
    for (auto digit : value) {
      if (digit < L'0' || digit > L'9' || result > 20000) return std::nullopt;
      result = result * 10 + digit - L'0';
    }
    return result <= 20000 ? std::optional(result) : std::nullopt;
  };
  const auto group = number(fields[0]), tab = number(fields[1]);
  if (!group || !tab) return std::nullopt;
  return Entry{*group, *tab, fields[2], fields[3], fields[4], {}, {}};
}
bool sameApplication(HWND window) {
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  if (!pid) return false;
  if (pid == GetCurrentProcessId()) return true;
  const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
  const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
  CloseHandle(process); path.resize(size);
  return queried && samePath(path, fn::modulePath().wstring());
}
HWND receiver() {
  const auto window = FindWindowW(L"ShibaKit.DesktopGroups.Receiver", nullptr);
  return window && GetPropW(window, L"ShibaKit.DesktopGroups.Ready") && sameApplication(window) ? window : nullptr;
}
bool locate(const Entry &entry, HWND target) {
  if (!target) target = receiver();
  const auto payload = encode(entry);
  if (!target || !sameApplication(target) || payload.empty()) return false;
  // A short-lived sender lets the receiver verify our executable identity.
  const auto sender = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!sender) return false;
  DWORD pid = 0; GetWindowThreadProcessId(target, &pid); AllowSetForegroundWindow(pid);
  COPYDATASTRUCT data{copyDataCommand, static_cast<DWORD>(payload.size() * sizeof(wchar_t)), const_cast<wchar_t *>(payload.data())};
  DWORD_PTR accepted = 0;
  const auto sent = SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&data),
      SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 500, &accepted);
  DestroyWindow(sender);
  return sent && accepted;
}
}
