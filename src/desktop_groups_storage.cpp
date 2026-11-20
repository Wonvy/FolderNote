#include "desktop_groups_storage.h"
#include "desktop_groups_model.h"
#include "quick_folders.h"
#include <algorithm>
#include <filesystem>
#include <shlobj.h>

namespace desktop_groups {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t storageName[] = L"FolderNote Boxes";

std::wstring longSpelling(const std::wstring &path) {
  const auto normalized = quick::normalize(path);
  if (normalized.empty()) return {};
  const auto needed = GetLongPathNameW(normalized.c_str(), nullptr, 0);
  if (!needed) return normalized;
  std::wstring expanded(needed, L'\0');
  const auto written = GetLongPathNameW(normalized.c_str(), expanded.data(), needed);
  if (!written || written >= needed) return normalized;
  expanded.resize(written);
  return expanded;
}

std::wstring findItem(std::vector<std::wstring> roots, const std::wstring &path) {
  auto item = desktopItem(roots, path);
  if (!item.empty()) return item;
  // Shell drags can use an 8.3 alias for a parent directory.
  for (auto &root : roots) root = longSpelling(root);
  return desktopItem(roots, longSpelling(path));
}

bool safeDirectory(const std::wstring &folder) {
  if (folder.empty()) return false;
  WIN32_FIND_DATAW data{};
  const auto search = FindFirstFileW(folder.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) return false;
  FindClose(search);
  return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
         (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !IsReparseTagNameSurrogate(data.dwReserved0));
}

void notifyMove(const std::wstring &from, const std::wstring &to, bool directory) {
  if (from.size() < MAX_PATH && to.size() < MAX_PATH)
    SHChangeNotify(directory ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   from.c_str(), to.c_str());
}

std::wstring collisionName(const fs::path &item, bool directory, unsigned number) {
  std::wstring suffix = L" (" + std::to_wstring(number) + L")";
  auto extension = directory ? std::wstring{} : item.extension().native();
  if (extension.size() + suffix.size() >= 255) extension.clear();
  auto stem = extension.empty() ? item.filename().native() : item.stem().native();
  const auto limit = 255 - suffix.size() - extension.size();
  if (stem.size() > limit) {
    // Avoid splitting a UTF-16 surrogate pair while shortening a long filename.
    stem.resize(limit - (IS_HIGH_SURROGATE(stem[limit - 1]) ? 1 : 0));
  }
  return stem + suffix + extension;
}

std::wstring moveToFolder(const std::wstring &source, const std::wstring &folder) {
  const auto attributes = GetFileAttributesW(source.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return {};
  const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const fs::path item(source), destination(folder);
  for (unsigned attempt = 1; attempt <= 10000; ++attempt) {
    const auto name = attempt == 1 ? item.filename().native() : collisionName(item, directory, attempt);
    auto target = (destination / name).native();
    if (target.size() > 32760) return {};
    // Keep this a single filesystem rename. Neither overwrite nor copy-and-delete on failure.
    if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
      notifyMove(source, target, directory);
      return target;
    }
    const auto error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS &&
        GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  }
  return {};
}
}

std::wstring boxStorageFolder(std::span<const std::wstring> roots) {
  const auto personal = roots.empty() ? std::wstring{} : quick::normalize(roots.front());
  return personal.empty() ? std::wstring{} : (fs::path(personal) / storageName).native();
}

std::wstring boxDesktopItem(std::span<const std::wstring> roots, const std::wstring &path) {
  auto source = findItem(std::vector<std::wstring>(roots.begin(), roots.end()), path);
  if (source.empty()) return {};
  const auto fullSource = longSpelling(source);
  if (samePath(longSpelling(boxStorageFolder(roots)), fullSource) ||
      std::any_of(roots.begin(), roots.end(), [&](const auto &root) { return samePath(longSpelling(root), fullSource); }))
    return {};
  return source;
}

std::wstring boxStoredItem(std::span<const std::wstring> roots, const std::wstring &path) {
  const auto folder = boxStorageFolder(roots);
  return safeDirectory(folder) ? findItem({folder}, path) : std::wstring{};
}

std::wstring storeBoxItem(std::span<const std::wstring> roots, const std::wstring &path) {
  const auto folder = boxStorageFolder(roots);
  if (folder.empty()) return {};
  const auto source = boxDesktopItem(roots, path);
  if (source.empty()) return {};

  const bool created = CreateDirectoryW(folder.c_str(), nullptr) != FALSE;
  if (!safeDirectory(folder)) return {};
  auto moved = moveToFolder(source, folder);
  if (created) {
    if (moved.empty()) RemoveDirectoryW(folder.c_str());
    else if (folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }
  return moved;
}

std::wstring restoreBoxItem(std::span<const std::wstring> roots, const std::wstring &path) {
  const auto source = boxStoredItem(roots, path);
  if (source.empty()) return {};
  return moveToFolder(source, quick::normalize(roots.front()));
}

bool rollbackBoxMove(const std::wstring &moved, const std::wstring &previous) {
  const auto attributes = GetFileAttributesW(moved.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || !MoveFileExW(moved.c_str(), previous.c_str(), MOVEFILE_WRITE_THROUGH))
    return false;
  notifyMove(moved, previous, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
  return true;
}
}
