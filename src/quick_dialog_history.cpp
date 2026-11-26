#include "quick_dialog_history.h"
#include "quick_folders.h"
#include "navigation.h"
#include "runtime_guard.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace quick::dialog {
namespace {
struct Observation {
  std::mutex mutex;
  HWND target = nullptr;
  DWORD process = 0, thread = 0;
  unsigned long long sequence = 0;
  bool busy = false, stopped = false;
  std::wstring lastPath;
};
std::shared_ptr<Observation> observer;
struct Request {
  std::shared_ptr<Observation> state;
  HWND target;
  DWORD process, thread, generation;
  unsigned long long sequence;
  HMODULE module = nullptr;
};
bool matches(const Request &request) {
  DWORD process = 0;
  return request.target == GetForegroundWindow() && IsWindowVisible(request.target) &&
      GetWindowThreadProcessId(request.target, &process) == request.thread && process == request.process;
}
// The accessibility query runs third-party providers inside the background
// helper that also owns the middle-click hooks. An access violation there must
// not take middle-click down: contain it, log it and stop reading dialog paths
// for the rest of this helper session (history from Explorer is unaffected).
std::atomic<bool> pathReaderFaulted{false};
__declspec(noinline) void readPathInto(std::wstring *path, HWND target) { *path = currentPath(target); }
bool guardedReadPath(std::wstring *path, HWND target) {
  __try { readPathInto(path, target); return true; }
  __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}
void CALLBACK readHistory(PTP_CALLBACK_INSTANCE instance, void *argument) {
  std::unique_ptr<Request> request(static_cast<Request *>(argument));
  FreeLibraryWhenCallbackReturns(instance, request->module);
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  try {
    std::wstring path;
    if (SUCCEEDED(initialized) && matches(*request) && navigation::quickEnabled() && navigation::dialogQuickEnabled()) {
      path = normalize(safeCurrentPath(request->target));
    }
    std::lock_guard lock(request->state->mutex);
    auto &state = *request->state;
    if (!state.stopped && state.sequence == request->sequence && !path.empty() && matches(*request) &&
        navigation::quickEnabled() && navigation::dialogQuickEnabled() && !same(state.lastPath, path)) {
      state.lastPath = path;
      // Preserve the epoch from before the read, so clearing history also
      // cancels a slow accessibility query that is already in progress.
      visitAsync(path, request->generation);
    }
  } catch (...) {}
  if (SUCCEEDED(initialized)) CoUninitialize();
  std::lock_guard lock(request->state->mutex);
  request->state->busy = false;
}
}
void observeHistory(HWND target) noexcept {
  try {
    if (!observer) observer = std::make_shared<Observation>();
    DWORD process = 0;
    const auto thread = target ? GetWindowThreadProcessId(target, &process) : 0;
    std::lock_guard lock(observer->mutex);
    if (observer->target != target || observer->process != process || observer->thread != thread) {
      observer->target = target; observer->process = process; observer->thread = thread;
      observer->lastPath.clear(); ++observer->sequence;
    }
    if (!target || !process || !thread || observer->busy || pathReaderFaulted) return;
    auto request = std::make_unique<Request>(Request{observer, target, process, thread, historyGeneration(), observer->sequence});
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&readHistory), &request->module)) return;
    observer->busy = true;
    if (TrySubmitThreadpoolCallback(readHistory, request.get(), nullptr)) { request.release(); return; }
    observer->busy = false;
    FreeLibrary(request->module);
  } catch (...) {}
}
void stopHistory() noexcept {
  if (!observer) return;
  { std::lock_guard lock(observer->mutex); observer->stopped = true; ++observer->sequence; }
  observer.reset();
}
bool pathReaderAvailable() noexcept { return !pathReaderFaulted.load(std::memory_order_relaxed); }
std::wstring safeCurrentPath(HWND target) noexcept {
  if (!target || !pathReaderAvailable()) return {};
  try {
    std::wstring raw;
    if (guardedReadPath(&raw, target)) return raw;
    if (!pathReaderFaulted.exchange(true))
      guard::log(L"读取文件对话框路径时发生访问冲突，本次后台会话内已停用对话框路径读取");
  } catch (...) {}
  return {};
}
}
