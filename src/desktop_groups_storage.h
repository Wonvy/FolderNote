#pragma once
#include <windows.h>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace desktop_groups {
// The personal Desktop must be the first root.
std::wstring boxStorageFolder(std::span<const std::wstring> roots);
void ensureHiddenBoxStorage(std::span<const std::wstring> roots);
std::wstring boxDesktopItem(std::span<const std::wstring> roots, const std::wstring &path);
std::wstring boxStoredItem(std::span<const std::wstring> roots, const std::wstring &path);
// Return the new path, or an empty string when the original could not be moved.
std::wstring storeBoxItem(std::span<const std::wstring> roots, const std::wstring &path);
std::wstring restoreBoxItem(std::span<const std::wstring> roots, const std::wstring &path);
std::vector<std::wstring> listBoxStorageItems(std::span<const std::wstring> roots);
void unhideBoxStorage(std::span<const std::wstring> roots);
size_t restoreAllBoxStorage(std::span<const std::wstring> roots,
                            std::vector<std::pair<std::wstring, std::wstring>> *moved = nullptr);
std::wstring transferItemToFolder(const std::wstring &source, const std::wstring &destinationFolder,
                                  bool move, HWND owner, bool renameOnCollision = true,
                                  const std::function<void(double)> &onProgress = {});
std::wstring copyToBoxStorage(std::span<const std::wstring> roots, const std::wstring &source, HWND owner,
                              const std::function<void(double)> &onProgress = {});
std::wstring moveToBoxStorage(std::span<const std::wstring> roots, const std::wstring &source, HWND owner,
                              const std::function<void(double)> &onProgress = {});
std::wstring createShortcutInFolder(const std::wstring &targetPath, const std::wstring &destinationFolder);
std::wstring createBoxShortcut(std::span<const std::wstring> roots, const std::wstring &targetPath);
std::wstring createNewFolderIn(const std::wstring &parentFolder, const std::wstring &baseName);
std::wstring createNewFolderInBox(std::span<const std::wstring> roots, const std::wstring &baseName);
std::wstring createNewTextFileIn(const std::wstring &parentFolder, const std::wstring &baseName);
std::wstring createNewTextFileInBox(std::span<const std::wstring> roots, const std::wstring &baseName);
std::wstring saveHBitmapToPngInFolder(HBITMAP bitmap, const std::wstring &parentFolder, const std::wstring &prefix);
std::wstring saveClipboardImageToFolder(const std::wstring &parentFolder, const std::wstring &prefix);
std::wstring saveClipboardImageToBox(std::span<const std::wstring> roots, const std::wstring &prefix);
bool clipboardHasBitmap();
bool clipboardHasPasteableContent();
bool rollbackBoxMove(const std::wstring &moved, const std::wstring &previous);
}
