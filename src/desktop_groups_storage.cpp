#include "desktop_groups_storage.h"
#include "desktop_groups_model.h"
#include "quick_folders.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

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

namespace {
class CopyResultSink final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperationProgressSink> {
public:
  std::wstring copiedPath;
  HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT hrCopy,
                                         IShellItem *newlyCreated) override {
    if (SUCCEEDED(hrCopy) && newlyCreated && copiedPath.empty()) {
      PWSTR raw = nullptr;
      if (SUCCEEDED(newlyCreated->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
        copiedPath = quick::normalize(raw);
        CoTaskMemFree(raw);
      }
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
}

std::wstring copyToBoxStorage(std::span<const std::wstring> roots, const std::wstring &source, HWND owner) {
  if (source.empty() || isDesktopShellItem(source)) return {};
  const auto folder = boxStorageFolder(roots);
  if (folder.empty()) return {};
  const bool created = CreateDirectoryW(folder.c_str(), nullptr) != FALSE;
  if (!safeDirectory(folder)) return {};

  Microsoft::WRL::ComPtr<IShellItem> srcItem;
  Microsoft::WRL::ComPtr<IShellItem> dstFolder;
  Microsoft::WRL::ComPtr<IFileOperation> op;
  auto sink = Microsoft::WRL::Make<CopyResultSink>();
  if (SUCCEEDED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&srcItem))) &&
      SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&dstFolder))) &&
      SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) && sink) {
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR);
    if (SUCCEEDED(op->CopyItem(srcItem.Get(), dstFolder.Get(), nullptr, sink.Get()))) {
      op->PerformOperations();
    }
  }
  if (created) {
    if (sink->copiedPath.empty()) RemoveDirectoryW(folder.c_str());
    else if (folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }
  return sink ? sink->copiedPath : std::wstring{};
}

std::wstring createShortcutInFolder(const std::wstring &targetPath, const std::wstring &destinationFolder) {
  if (targetPath.empty() || !safeDirectory(destinationFolder)) return {};
  PIDLIST_ABSOLUTE id = nullptr;
  if (FAILED(SHParseDisplayName(targetPath.c_str(), nullptr, &id, 0, nullptr)) || !id) return {};
  const std::unique_ptr<void, decltype(&CoTaskMemFree)> ownedId(static_cast<void *>(id), CoTaskMemFree);

  std::wstring baseName;
  Microsoft::WRL::ComPtr<IShellItem> shellItem;
  if (SUCCEEDED(SHCreateItemFromIDList(id, IID_PPV_ARGS(&shellItem))) && shellItem) {
    PWSTR disp = nullptr;
    if (SUCCEEDED(shellItem->GetDisplayName(SIGDN_NORMALDISPLAY, &disp)) && disp) {
      baseName = disp;
      CoTaskMemFree(disp);
    }
  }
  if (baseName.empty()) baseName = fs::path(targetPath).stem().wstring();
  if (baseName.empty()) baseName = L"Shortcut";
  for (auto &ch : baseName) {
    if (wcschr(L"<>:\"/\\|?*", ch) || ch < 32) ch = L'_';
  }

  Microsoft::WRL::ComPtr<IShellLinkW> link;
  Microsoft::WRL::ComPtr<IPersistFile> persist;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
      FAILED(link->SetIDList(id)) || FAILED(link.As(&persist))) return {};
  if (!isDesktopShellItem(targetPath)) {
    const auto parentDir = fs::path(targetPath).parent_path().wstring();
    if (!parentDir.empty()) link->SetWorkingDirectory(parentDir.c_str());
  }

  const fs::path dest(destinationFolder);
  for (unsigned attempt = 1; attempt <= 10000; ++attempt) {
    const auto candidateName = attempt == 1 ? (baseName + L".lnk") : collisionName(fs::path(baseName + L".lnk"), false, attempt);
    const auto full = (dest / candidateName).native();
    if (full.size() > 32760) return {};
    if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
    if (SUCCEEDED(persist->Save(full.c_str(), TRUE))) {
      if (full.size() < MAX_PATH)
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, full.c_str(), nullptr);
      return full;
    }
    return {};
  }
  return {};
}

std::wstring createBoxShortcut(std::span<const std::wstring> roots, const std::wstring &targetPath) {
  const auto folder = boxStorageFolder(roots);
  if (folder.empty()) return {};
  const bool created = CreateDirectoryW(folder.c_str(), nullptr) != FALSE;
  if (!safeDirectory(folder)) return {};
  auto shortcut = createShortcutInFolder(targetPath, folder);
  if (created) {
    if (shortcut.empty()) RemoveDirectoryW(folder.c_str());
    else if (folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }
  return shortcut;
}

bool rollbackBoxMove(const std::wstring &moved, const std::wstring &previous) {
  const auto attributes = GetFileAttributesW(moved.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || !MoveFileExW(moved.c_str(), previous.c_str(), MOVEFILE_WRITE_THROUGH))
    return false;
  notifyMove(moved, previous, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
  return true;
}
}
