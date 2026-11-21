#include "quick_dialog_history.h"
#include "quick_folders.h"
#include "navigation.h"
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
void CALLBACK readHistory(PTP_CALLBACK_INSTANCE instance, void *argument) {
  std::unique_ptr<Request> request(static_cast<Request *>(argument));
  FreeLibraryWhenCallbackReturns(instance, request->module);
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  try {
    std::wstring path;
    if (SUCCEEDED(initialized) && matches(*request) && navigation::quickEnabled() && navigation::dialogQuickEnabled())
      path = normalize(currentPath(request->target));
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
    if (!target || !process || !thread || observer->busy) return;
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
}
