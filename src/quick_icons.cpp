#include "quick_icons.h"
#include <shlobj.h>
#include <algorithm>
#include <condition_variable>
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
  } else if (source.path.size() >= MAX_PATH) {
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
  if (source.kind == Source::Kind::Stock) return L"stock:" + std::to_wstring(source.stock);
  wchar_t id[40]{}; StringFromGUID2(source.known, id, ARRAYSIZE(id)); return std::wstring(L"known:") + id;
}
}
struct Cache::State {
  struct Entry { Image icon; bool ready = false; unsigned long long used = 0; };
  struct Job { std::wstring key; Source source; };
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::wstring, Entry, OrdinalLess> entries;
  std::deque<Job> queue;
  HWND notify;
  UINT message;
  int size;
  Loader loader;
  bool stopped = false;
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
    { std::lock_guard lock(mutex); stopped = true; notify = nullptr; queue.clear(); }
    changed.notify_all();
  }
  void work() {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex); changed.wait(lock, [&] { return stopped || !queue.empty(); });
        if (stopped) break;
        job = std::move(queue.front()); queue.pop_front();
      }
      Image icon;
      try { if (SUCCEEDED(initialized)) icon = loader(job.source, size); } catch (...) { }
      {
        std::lock_guard lock(mutex);
        if (stopped) break;
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
    : state(std::make_shared<State>(notify, message, size, std::move(loader))) {
  try {
    // At most two workers: a slow network icon never creates a thread per row.
    for (int i = 0; i < 2; ++i) {
      HMODULE module = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&State::run), &module)) throw std::bad_alloc();
      std::unique_ptr<State::Worker> worker;
      try { worker = std::make_unique<State::Worker>(State::Worker{state, module}); }
      catch (...) { FreeLibrary(module); throw; }
      if (!TrySubmitThreadpoolCallback(State::run, worker.get(), nullptr)) { FreeLibrary(module); throw std::bad_alloc(); }
      worker.release();
    }
  } catch (...) { state->stop(); throw; }
}
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
  state->queue.push_back({std::move(key), source}); state->changed.notify_one(); return {};
}
}
