#include "desktop_groups_storage.h"
#include "desktop_groups_model.h"
#include "quick_folders.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace desktop_groups {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t storageName[] = L".desktop";

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

void hideStorageDirectory(const std::wstring &folder) {
  // Hidden + system keeps the folder out of Explorer even when "show hidden files" is on.
  constexpr DWORD protectedFlags = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
  const auto attrs = GetFileAttributesW(folder.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
      (attrs & protectedFlags) != protectedFlags) {
    if (SetFileAttributesW(folder.c_str(), attrs | protectedFlags) && folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }
}

bool ensureStorageDirectory(const std::wstring &folder, bool &created) {
  created = CreateDirectoryW(folder.c_str(), nullptr) != FALSE;
  if (!safeDirectory(folder)) return false;
  hideStorageDirectory(folder);
  return true;
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

void ensureHiddenBoxStorage(std::span<const std::wstring> roots) {
  const auto folder = boxStorageFolder(roots);
  if (safeDirectory(folder)) hideStorageDirectory(folder);
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

  bool created = false;
  if (!ensureStorageDirectory(folder, created)) return {};
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

std::vector<std::wstring> listBoxStorageItems(std::span<const std::wstring> roots) {
  std::vector<std::wstring> items;
  const auto folder = boxStorageFolder(roots);
  if (!safeDirectory(folder)) return items;
  std::error_code ec;
  const fs::directory_iterator end;
  for (fs::directory_iterator it(folder, ec); !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().native();
    if (name.empty() || !_wcsicmp(name.c_str(), L"desktop.ini")) continue;
    const auto normalized = quick::normalize(it->path().native());
    if (!normalized.empty()) items.push_back(normalized);
  }
  return items;
}

void unhideBoxStorage(std::span<const std::wstring> roots) {
  const auto folder = boxStorageFolder(roots);
  if (!safeDirectory(folder)) return;
  const DWORD attrs = GetFileAttributesW(folder.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return;
  DWORD updated = attrs & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
  if (!updated) updated = FILE_ATTRIBUTE_NORMAL;
  if (updated != attrs && SetFileAttributesW(folder.c_str(), updated) && folder.size() < MAX_PATH)
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
}

size_t restoreAllBoxStorage(std::span<const std::wstring> roots,
                            std::vector<std::pair<std::wstring, std::wstring>> *moved) {
  if (roots.empty()) return 0;
  const auto desktop = quick::normalize(roots.front());
  const auto folder = boxStorageFolder(roots);
  if (desktop.empty() || !safeDirectory(folder)) return 0;
  size_t remaining = 0;
  for (const auto &item : listBoxStorageItems(roots)) {
    auto restored = moveToFolder(item, desktop);
    if (restored.empty()) {
      ++remaining;
    } else if (moved) {
      moved->emplace_back(item, std::move(restored));
    }
  }
  if (!remaining) {
    if (RemoveDirectoryW(folder.c_str()) && folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_RMDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  } else {
    unhideBoxStorage(roots);
  }
  return remaining;
}

namespace {
class CopyResultSink final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperationProgressSink> {
public:
  std::wstring copiedPath;
  std::function<void(double)> onProgress;
  HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT hrMove,
                                         IShellItem *newlyMoved) override {
    if (SUCCEEDED(hrMove) && newlyMoved && copiedPath.empty()) {
      PWSTR raw = nullptr;
      if (SUCCEEDED(newlyMoved->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
        copiedPath = quick::normalize(raw);
        CoTaskMemFree(raw);
      }
    }
    return S_OK;
  }
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
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT workTotal, UINT workSoFar) override {
    if (onProgress && workTotal > 0) {
      const double ratio = static_cast<double>(workSoFar) / static_cast<double>(workTotal);
      onProgress(ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio));
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
}

std::wstring transferItemToFolder(const std::wstring &source, const std::wstring &destinationFolder,
                                  bool move, HWND owner, bool renameOnCollision,
                                  const std::function<void(double)> &onProgress) {
  if (source.empty() || destinationFolder.empty() || isDesktopShellItem(source)) return {};
  if (move && samePath(fs::path(source).parent_path().native(), destinationFolder)) return quick::normalize(source);
  if (!safeDirectory(destinationFolder)) return {};

  Microsoft::WRL::ComPtr<IShellItem> srcItem;
  Microsoft::WRL::ComPtr<IShellItem> dstFolder;
  Microsoft::WRL::ComPtr<IFileOperation> op;
  auto sink = Microsoft::WRL::Make<CopyResultSink>();
  if (sink) sink->onProgress = onProgress;
  if (SUCCEEDED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&srcItem))) &&
      SUCCEEDED(SHCreateItemFromParsingName(destinationFolder.c_str(), nullptr, IID_PPV_ARGS(&dstFolder))) &&
      SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) && sink) {
    op->SetOwnerWindow(owner);
    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (renameOnCollision) flags |= FOF_RENAMEONCOLLISION;
    op->SetOperationFlags(flags);
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(op->Advise(sink.Get(), &cookie));
    const HRESULT queued = move
        ? op->MoveItem(srcItem.Get(), dstFolder.Get(), nullptr, nullptr)
        : op->CopyItem(srcItem.Get(), dstFolder.Get(), nullptr, nullptr);
    if (SUCCEEDED(queued)) op->PerformOperations();
    if (advised) op->Unadvise(cookie);
  }
  return sink ? sink->copiedPath : std::wstring{};
}

std::wstring transferToBoxStorage(std::span<const std::wstring> roots, const std::wstring &source,
                                  bool move, HWND owner, const std::function<void(double)> &onProgress) {
  if (source.empty() || isDesktopShellItem(source)) return {};
  const auto folder = boxStorageFolder(roots);
  if (folder.empty()) return {};
  if (move && samePath(fs::path(source).parent_path().native(), folder)) return quick::normalize(source);
  bool created = false;
  if (!ensureStorageDirectory(folder, created)) return {};

  auto movedPath = transferItemToFolder(source, folder, move, owner, true, onProgress);
  if (created) {
    if (movedPath.empty()) RemoveDirectoryW(folder.c_str());
    else if (folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }
  return movedPath;
}

std::wstring copyToBoxStorage(std::span<const std::wstring> roots, const std::wstring &source, HWND owner,
                              const std::function<void(double)> &onProgress) {
  return transferToBoxStorage(roots, source, false, owner, onProgress);
}

std::wstring moveToBoxStorage(std::span<const std::wstring> roots, const std::wstring &source, HWND owner,
                              const std::function<void(double)> &onProgress) {
  return transferToBoxStorage(roots, source, true, owner, onProgress);
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
  bool created = false;
  if (!ensureStorageDirectory(folder, created)) return {};
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

namespace {
template<typename Fn>
std::wstring withBoxStorageFolder(std::span<const std::wstring> roots, Fn &&createInside) {
  const auto folder = boxStorageFolder(roots);
  if (folder.empty()) return {};
  bool created = false;
  if (!ensureStorageDirectory(folder, created)) return {};
  auto result = createInside(folder);
  if (created) {
    if (result.empty()) RemoveDirectoryW(folder.c_str());
    else if (folder.size() < MAX_PATH)
      SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }
  return result;
}

std::wstring screenshotBaseName(const std::wstring &prefix) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t stamp[64]{};
  swprintf_s(stamp, L"%04u%02u%02u_%02u%02u%02u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  return (prefix.empty() ? std::wstring(L"截图") : prefix) + L"_" + stamp + L".png";
}

UINT pngClipboardFormat() {
  static const UINT format = RegisterClipboardFormatW(L"PNG");
  return format;
}
}

std::wstring createNewFolderIn(const std::wstring &parentFolder, const std::wstring &baseName) {
  if (!safeDirectory(parentFolder)) return {};
  const std::wstring label = baseName.empty() ? L"新建文件夹" : baseName;
  const fs::path dest(parentFolder);
  for (unsigned attempt = 1; attempt <= 10000; ++attempt) {
    const auto candidate = attempt == 1 ? label : collisionName(fs::path(label), true, attempt);
    const auto full = (dest / candidate).native();
    if (full.size() > 32760) return {};
    if (CreateDirectoryW(full.c_str(), nullptr)) {
      if (full.size() < MAX_PATH)
        SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, full.c_str(), nullptr);
      return quick::normalize(full);
    }
    const auto error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS &&
        GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  }
  return {};
}

std::wstring createNewFolderInBox(std::span<const std::wstring> roots, const std::wstring &baseName) {
  return withBoxStorageFolder(roots, [&](const std::wstring &folder) {
    return createNewFolderIn(folder, baseName);
  });
}

std::wstring createNewTextFileIn(const std::wstring &parentFolder, const std::wstring &baseName) {
  if (!safeDirectory(parentFolder)) return {};
  std::wstring fileName = baseName.empty() ? L"新建文本文档.txt" : baseName;
  if (fs::path(fileName).extension().empty()) fileName += L".txt";
  const fs::path dest(parentFolder);
  for (unsigned attempt = 1; attempt <= 10000; ++attempt) {
    const auto candidate = attempt == 1 ? fileName : collisionName(fs::path(fileName), false, attempt);
    const auto full = (dest / candidate).native();
    if (full.size() > 32760) return {};
    const HANDLE handle = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      if (full.size() < MAX_PATH)
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, full.c_str(), nullptr);
      return quick::normalize(full);
    }
    const auto error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS &&
        GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  }
  return {};
}

std::wstring createNewTextFileInBox(std::span<const std::wstring> roots, const std::wstring &baseName) {
  return withBoxStorageFolder(roots, [&](const std::wstring &folder) {
    return createNewTextFileIn(folder, baseName);
  });
}

std::wstring saveHBitmapToPngInFolder(HBITMAP bitmap, const std::wstring &parentFolder, const std::wstring &prefix) {
  if (!bitmap || !safeDirectory(parentFolder)) return {};
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory ||
      FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &wicBitmap)) || !wicBitmap)
    return {};

  const auto baseName = screenshotBaseName(prefix);
  const fs::path dest(parentFolder);
  for (unsigned attempt = 1; attempt <= 10000; ++attempt) {
    const auto candidate = attempt == 1 ? baseName : collisionName(fs::path(baseName), false, attempt);
    const auto full = (dest / candidate).native();
    if (full.size() > 32760) return {};
    if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
    const HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const auto err = GetLastError();
      if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) continue;
      return {};
    }
    CloseHandle(file);
    Microsoft::WRL::ComPtr<IWICStream> fileStream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(factory->CreateStream(&fileStream)) && fileStream &&
        SUCCEEDED(fileStream->InitializeFromFilename(full.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && encoder &&
        SUCCEEDED(encoder->Initialize(fileStream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && frame &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->WriteSource(wicBitmap.Get(), nullptr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit())) {
      fileStream.Reset();
      if (full.size() < MAX_PATH)
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, full.c_str(), nullptr);
      return quick::normalize(full);
    }
    fileStream.Reset();
    DeleteFileW(full.c_str());
    return {};
  }
  return {};
}

std::wstring saveClipboardImageToFolder(const std::wstring &parentFolder, const std::wstring &prefix) {
  if (!safeDirectory(parentFolder) || !OpenClipboard(nullptr)) return {};
  std::vector<BYTE> pngBytes;
  HBITMAP copiedBitmap = nullptr;
  if (const UINT cfPng = pngClipboardFormat(); cfPng && IsClipboardFormatAvailable(cfPng)) {
    if (const HANDLE data = GetClipboardData(cfPng)) {
      const SIZE_T bytes = GlobalSize(data);
      if (bytes >= 8 && bytes <= 128 * 1024 * 1024) {
        if (const auto *raw = static_cast<const BYTE *>(GlobalLock(data))) {
          constexpr BYTE magic[8]{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
          if (std::memcmp(raw, magic, sizeof(magic)) == 0) pngBytes.assign(raw, raw + bytes);
          GlobalUnlock(data);
        }
      }
    }
  }
  if (pngBytes.empty()) {
    if (const auto rawBmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
      copiedBitmap = static_cast<HBITMAP>(CopyImage(rawBmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
      if (!copiedBitmap) copiedBitmap = static_cast<HBITMAP>(CopyImage(rawBmp, IMAGE_BITMAP, 0, 0, 0));
    }
  }
  CloseClipboard();

  if (!pngBytes.empty()) {
    const auto baseName = screenshotBaseName(prefix);
    const fs::path dest(parentFolder);
    for (unsigned attempt = 1; attempt <= 10000; ++attempt) {
      const auto candidate = attempt == 1 ? baseName : collisionName(fs::path(baseName), false, attempt);
      const auto full = (dest / candidate).native();
      if (full.size() > 32760) return {};
      const HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        const auto err = GetLastError();
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) continue;
        return {};
      }
      DWORD written = 0;
      const bool saved = WriteFile(file, pngBytes.data(), static_cast<DWORD>(pngBytes.size()), &written, nullptr) &&
                         written == pngBytes.size();
      CloseHandle(file);
      if (saved) {
        if (full.size() < MAX_PATH)
          SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, full.c_str(), nullptr);
        return quick::normalize(full);
      }
      DeleteFileW(full.c_str());
      return {};
    }
    return {};
  }

  if (!copiedBitmap) return {};
  const std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> owned(copiedBitmap, DeleteObject);
  return saveHBitmapToPngInFolder(owned.get(), parentFolder, prefix);
}

std::wstring saveClipboardImageToBox(std::span<const std::wstring> roots, const std::wstring &prefix) {
  return withBoxStorageFolder(roots, [&](const std::wstring &folder) {
    return saveClipboardImageToFolder(folder, prefix);
  });
}

bool clipboardHasBitmap() {
  const UINT cfPng = pngClipboardFormat();
  return (cfPng && IsClipboardFormatAvailable(cfPng)) ||
         IsClipboardFormatAvailable(CF_BITMAP) ||
         IsClipboardFormatAvailable(CF_DIB) ||
         IsClipboardFormatAvailable(CF_DIBV5);
}

bool clipboardHasPasteableContent() {
  if (IsClipboardFormatAvailable(CF_HDROP) || clipboardHasBitmap()) return true;
  static const UINT shellIdList = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
  return shellIdList && IsClipboardFormatAvailable(shellIdList);
}
}
