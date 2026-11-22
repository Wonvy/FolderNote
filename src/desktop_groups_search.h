#pragma once
#include "desktop_groups_model.h"
#include <functional>
#include <span>

namespace desktop_groups::search {
inline constexpr ULONG_PTR copyDataCommand = 0x464e5331; // FNS1
inline constexpr size_t requestLimit = 65536;
struct Entry {
  size_t group = 0, tab = 0;
  std::wstring groupName, tabName, path, name, label;
};
using Canceled = std::function<bool()>;
std::vector<Entry> collect(const std::vector<Group> &groups, Canceled canceled = {}, size_t limit = 20000);
bool matches(const std::vector<Group> &groups, const Entry &entry);
std::wstring encode(const Entry &entry);
std::optional<Entry> decode(std::span<const wchar_t> text);
bool sameApplication(HWND window);
HWND receiver();
bool locate(const Entry &entry, HWND target = nullptr);
}
