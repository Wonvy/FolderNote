#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace desktop_folder_links {
struct FileIdentity {
  std::uint64_t volume = 0;
  std::array<unsigned char, 16> fileId{};
  std::uint64_t created = 0;
  std::uint64_t written = 0;
  bool operator==(const FileIdentity &) const = default;
};

struct Entry {
  std::wstring path, name;
  bool hidden = false, changed = false, canChange = false, shared = false;
  FileIdentity identity;
  std::wstring target, iconLocation;
};

std::vector<Entry> inventory();
void setHidden(const Entry &entry, bool hidden);
void restore(const Entry &entry);
size_t restoreAll();
} // namespace desktop_folder_links
