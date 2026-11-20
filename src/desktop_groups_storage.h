#pragma once
#include <span>
#include <string>

namespace desktop_groups {
// The personal Desktop must be the first root.
std::wstring boxStorageFolder(std::span<const std::wstring> roots);
std::wstring boxDesktopItem(std::span<const std::wstring> roots, const std::wstring &path);
std::wstring boxStoredItem(std::span<const std::wstring> roots, const std::wstring &path);
// Return the new path, or an empty string when the original could not be moved.
std::wstring storeBoxItem(std::span<const std::wstring> roots, const std::wstring &path);
std::wstring restoreBoxItem(std::span<const std::wstring> roots, const std::wstring &path);
bool rollbackBoxMove(const std::wstring &moved, const std::wstring &previous);
}
