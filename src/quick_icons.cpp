#include "quick_icons.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

namespace quick::icons {
namespace {
Image adopt(HICON icon) {
  try { return std::make_shared<Icon>(icon); }
  catch (...) { DestroyIcon(icon); throw; }
}
}
Image read(const Source &source, int size) {
  if (source.kind == Source::Kind::File && source.path.size() > 4 &&
      !_wcsicmp(source.path.c_str() + source.path.size() - 4, L".lnk")) {
    // SHGetFileInfo may resolve a shortcut target through a slow Shell handler.
    // Read its explicit icon without resolving/launching it, then use its target.
    static thread_local int depth = 0;
    if (depth >= 4) return {};
    struct Level { int &depth; Level(int &value) : depth(value) { ++depth; } ~Level() { --depth; } } level(depth);
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&persist)) || FAILED(persist->Load(source.path.c_str(), STGM_READ))) return {};
    std::wstring iconPath(32768, L'\0'); int index = 0;
    if (SUCCEEDED(link->GetIconLocation(iconPath.data(), static_cast<int>(iconPath.size()), &index)) && iconPath[0]) {
      std::wstring expanded(32768, L'\0');
      const auto length = ExpandEnvironmentStringsW(iconPath.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
      if (length && length <= expanded.size()) {
        HICON iconLarge = nullptr, iconSmall = nullptr;
        ExtractIconExW(expanded.c_str(), index, &iconLarge, &iconSmall, 1);
        const auto result = size > 16 ? (iconLarge ? iconLarge : iconSmall) : (iconSmall ? iconSmall : iconLarge);
        if (iconLarge && iconLarge != result) DestroyIcon(iconLarge);
        if (iconSmall && iconSmall != result) DestroyIcon(iconSmall);
        if (result) return adopt(result);
      }
    }
    std::wstring target(32768, L'\0');
    if (SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) && target[0])
      return read(Source::file(target.c_str()), size);
    PIDLIST_ABSOLUTE id = nullptr;
    SHFILEINFOW info{};
    if (SUCCEEDED(link->GetIDList(&id)) && id) {
      const auto loaded = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(id), 0, &info, sizeof(info),
          SHGFI_PIDL | SHGFI_ICON | (size > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON));
      CoTaskMemFree(id);
      if (loaded && info.hIcon) return adopt(info.hIcon);
      if (info.hIcon) DestroyIcon(info.hIcon);
    }
    return {};
  }
  if (source.kind == Source::Kind::Child) {
    if (source.path.empty() || source.displayName.empty()) return {};
    if (auto exact = read(Source::file((std::filesystem::path(source.path) / source.displayName).native()), size)) return exact;
    Microsoft::WRL::ComPtr<IShellFolder> desktop, parent;
    Microsoft::WRL::ComPtr<IEnumIDList> children;
    PIDLIST_ABSOLUTE parentId = nullptr;
    if (FAILED(SHParseDisplayName(source.path.c_str(), nullptr, &parentId, 0, nullptr)) || !parentId) return {};
    struct ReleaseId { PIDLIST_ABSOLUTE value; ~ReleaseId() { CoTaskMemFree(value); } } releaseId{parentId};
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop ||
        FAILED(desktop->BindToObject(parentId, nullptr, IID_PPV_ARGS(&parent))) || !parent ||
        FAILED(parent->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &children)) || !children) return {};
    const auto deadline = GetTickCount64() + 8000;
    for (int i = 0; i < 20000 && GetTickCount64() < deadline; ++i) {
      PITEMID_CHILD child = nullptr;
      if (children->Next(1, &child, nullptr) != S_OK || !child) break;
      STRRET display{}; PWSTR label = nullptr;
      bool match = SUCCEEDED(parent->GetDisplayNameOf(child, SHGDN_INFOLDER, &display)) &&
          SUCCEEDED(StrRetToStrW(&display, child, &label)) && label &&
          CompareStringOrdinal(label, -1, source.displayName.c_str(), -1, TRUE) == CSTR_EQUAL;
      CoTaskMemFree(label);
      PIDLIST_ABSOLUTE absolute = match ? ILCombine(parentId, child) : nullptr;
      CoTaskMemFree(child);
      if (absolute) {
        PWSTR filePath = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(absolute, SIGDN_FILESYSPATH, &filePath)) && filePath) {
          const auto actual = Source::file(filePath);
          CoTaskMemFree(filePath); CoTaskMemFree(absolute);
          return read(actual, size);
        }
        SHFILEINFOW info{};
        const auto loaded = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absolute), 0, &info, sizeof(info),
            SHGFI_PIDL | SHGFI_ICON | (size > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON));
        CoTaskMemFree(absolute);
        if (loaded && info.hIcon) return adopt(info.hIcon);
        if (info.hIcon) DestroyIcon(info.hIcon);
        return {};
      }
    }
    return {};
  }
  if (source.kind == Source::Kind::Stock) {
    SHSTOCKICONINFO info{sizeof(info)};
    if (SUCCEEDED(SHGetStockIconInfo(source.stock, SHGSI_ICON | (size > 16 ? SHGSI_LARGEICON : SHGSI_SMALLICON), &info)) && info.hIcon)
      return adopt(info.hIcon);
    return {};
  }
  SHFILEINFOW info{};
  UINT flags = SHGFI_ICON | (size > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON);
  PIDLIST_ABSOLUTE id = nullptr;
  DWORD_PTR result = 0;
  if (source.kind == Source::Kind::Known) {
    if (SUCCEEDED(SHGetKnownFolderIDList(source.known, KF_FLAG_DONT_VERIFY, nullptr, &id)))
      result = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(id), 0, &info, sizeof(info), flags | SHGFI_PIDL);
  } else if (source.path.size() >= MAX_PATH || source.path.starts_with(L"::{") ||
             !_wcsnicmp(source.path.c_str(), L"shell:", 6)) {
    if (SUCCEEDED(SHParseDisplayName(source.path.c_str(), nullptr, &id, 0, nullptr)))
      result = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(id), 0, &info, sizeof(info), flags | SHGFI_PIDL);
  } else {
    // Do not use SHGFI_USEFILEATTRIBUTES: it bypasses the folder's desktop.ini
    // and would return the generic icon instead of its Explorer/custom icon.
    result = SHGetFileInfoW(source.path.c_str(), 0, &info, sizeof(info), flags);
  }
  CoTaskMemFree(id);
  if (result && info.hIcon) return adopt(info.hIcon);
  if (info.hIcon) DestroyIcon(info.hIcon);
  return {};
}
namespace {
struct OrdinalLess {
  bool operator()(const std::wstring &a, const std::wstring &b) const {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  }
};
std::wstring keyFor(const Source &source) {
  if (source.kind == Source::Kind::File) return L"file:" + source.path;
  if (source.kind == Source::Kind::Child) return L"child:" + source.path + L"\\" + source.displayName;
  if (source.kind == Source::Kind::Stock) return L"stock:" + std::to_wstring(source.stock);
  wchar_t id[40]{}; StringFromGUID2(source.known, id, ARRAYSIZE(id)); return std::wstring(L"known:") + id;
}
}
struct Cache::State {
  struct Entry { Image icon; bool ready = false; unsigned long long used = 0; };
  struct Job { std::wstring key; Source source; };
  std::mutex mutex;
  std::map<std::wstring, Entry, OrdinalLess> entries;
  std::deque<Job> queue;
  HWND notify;
  UINT message;
  int size;
  Loader loader;
  bool stopped = false;
  size_t activeWorkers = 0;
  unsigned long long clock = 0;
  State(HWND window, UINT event, int pixels, Loader load) : notify(window), message(event), size(pixels), loader(std::move(load)) {}
  struct Worker { std::shared_ptr<State> state; HMODULE module; };
  static void CALLBACK run(PTP_CALLBACK_INSTANCE instance, void *context) {
    std::unique_ptr<Worker> worker(static_cast<Worker *>(context));
    // A menu lives in an EXE; Explorer tabs live in a DLL. Retain the module
    // until a slow icon provider returns, even if its last window has closed.
    FreeLibraryWhenCallbackReturns(instance, worker->module);
    CallbackMayRunLong(instance);
    worker->state->work();
  }
  void stop() {
    std::lock_guard lock(mutex);
    stopped = true;
    notify = nullptr;
    queue.clear();
  }
  void work() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
      Job job;
      {
        std::lock_guard lock(mutex);
        if (stopped || queue.empty()) {
          if (activeWorkers > 0) --activeWorkers;
          break;
        }
        job = std::move(queue.front()); queue.pop_front();
      }
      Image icon;
      try { if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) icon = loader(job.source, size); } catch (...) { }
      {
        std::lock_guard lock(mutex);
        if (stopped) {
          if (activeWorkers > 0) --activeWorkers;
          break;
        }
        auto found = entries.find(job.key);
        if (found != entries.end()) { found->second.icon = std::move(icon); found->second.ready = true; }
        // No pointers in the notification. Cancellation clears notify under
        // this same lock before any popup windows are destroyed.
        if (notify) PostMessageW(notify, message, 0, 0);
      }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
  }
};
Cache::Cache(HWND notify, UINT message, int size, Loader loader)
    : state(std::make_shared<State>(notify, message, size, std::move(loader))) {}
Cache::~Cache() { state->stop(); } // Closing the menu never waits for a slow shell extension.
Image Cache::get(const Source &source) {
  auto key = keyFor(source);
  std::lock_guard lock(state->mutex);
  if (state->stopped) return {};
  auto found = state->entries.find(key);
  if (found != state->entries.end()) { found->second.used = ++state->clock; return found->second.icon; }
  if (state->queue.size() >= 64) return {};
  if (state->entries.size() >= 512) {
    auto oldest = state->entries.end();
    for (auto item = state->entries.begin(); item != state->entries.end(); ++item)
      if (item->second.ready && (oldest == state->entries.end() || item->second.used < oldest->second.used)) oldest = item;
    if (oldest == state->entries.end()) return {};
    state->entries.erase(oldest);
  }
  state->entries.emplace(key, State::Entry{{}, false, ++state->clock});
  state->queue.push_back({std::move(key), source});
  if (state->activeWorkers < 2) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&State::run), &module)) {
      std::unique_ptr<State::Worker> worker;
      try { worker = std::make_unique<State::Worker>(State::Worker{state, module}); }
      catch (...) { FreeLibrary(module); return {}; }
      ++state->activeWorkers;
      if (TrySubmitThreadpoolCallback(State::run, worker.get(), nullptr)) {
        worker.release();
      } else {
        --state->activeWorkers;
        FreeLibrary(module);
      }
    }
  }
  return {};
}
}
