#include "../src/quick_dialog_history.h"
#include "../src/quick_folders.h"
#include "../src/navigation.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
HWND foreground = nullptr;
std::wstring candidate;
std::function<void()> duringRead;
bool submit = true;
struct Pending { PTP_SIMPLE_CALLBACK callback; void *argument; };
std::vector<Pending> pendingReads;
std::vector<std::pair<std::wstring, DWORD>> recorded;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
HWND activeWindow() { return foreground; }
BOOL enqueue(PTP_SIMPLE_CALLBACK callback, void *argument, PTP_CALLBACK_ENVIRON) {
  if (!submit) return FALSE;
  pendingReads.push_back({callback, argument}); return TRUE;
}
void releaseModule(PTP_CALLBACK_INSTANCE, HMODULE module) { FreeLibrary(module); }
void readNext() {
  check(pendingReads.size() == 1, "at most one background read per observer");
  const auto request = pendingReads.front(); pendingReads.clear(); request.callback(nullptr, request.argument);
}
struct Isolated {
  HKEY real = nullptr, key = nullptr;
  std::wstring name;
  Isolated() {
    GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, 40);
    name = std::wstring(L"Software\\ShibaKitDialogHistoryTests\\") + text;
    check(!RegOpenCurrentUser(KEY_ALL_ACCESS, &real) &&
        !RegCreateKeyExW(real, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) &&
        !RegOverridePredefKey(HKEY_CURRENT_USER, key), "isolate registry writes");
  }
  ~Isolated() { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(key); RegDeleteTreeW(real, name.c_str()); RegCloseKey(real); }
};
void enable(bool value) {
  DWORD on = 1, dialogs = value ? 1 : 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &on, sizeof(on));
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersInDialogs", REG_DWORD, &dialogs, sizeof(dialogs));
}
}
namespace quick::dialog {
std::wstring readCandidate(HWND) { if (duringRead) duringRead(); return candidate; }
void captureVisit(const std::wstring &path, DWORD epoch) { recorded.emplace_back(path, epoch); }
}
#define GetForegroundWindow activeWindow
#define TrySubmitThreadpoolCallback enqueue
#define FreeLibraryWhenCallbackReturns releaseModule
#define currentPath readCandidate
#define visitAsync captureVisit
#include "../src/quick_dialog_history.cpp"
#undef visitAsync
#undef currentPath
#undef FreeLibraryWhenCallbackReturns
#undef TrySubmitThreadpoolCallback
#undef GetForegroundWindow

int main() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HWND window = nullptr;
  int result = 0;
  try {
    Isolated isolation; enable(true);
    window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Unchanged filename.png",
        WS_POPUP | WS_VISIBLE, -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create a real isolated window"); foreground = window;
    const auto clipboard = GetClipboardSequenceNumber();
    candidate = L"D:\\设计\\导出 PNG";
    quick::dialog::observeHistory(window); quick::dialog::observeHistory(window);
    readNext(); check(recorded.size() == 1 && recorded.back().first == candidate, "record manually visited export folders without opening a menu");
    quick::dialog::observeHistory(window); readNext();
    check(recorded.size() == 1, "polling an unchanged directory does not reorder history repeatedly");
    candidate = L"D:/设计/第二个目录/";
    quick::dialog::observeHistory(window); readNext();
    check(recorded.size() == 2 && recorded.back().first == L"D:\\设计\\第二个目录", "follow navigation using normalized Unicode paths");
    candidate = L"D:\\设计\\清空前";
    quick::dialog::observeHistory(window);
    const auto epoch = quick::historyGeneration(); quick::clearHistory(); readNext();
    check(recorded.back().second == epoch && epoch != quick::historyGeneration(), "an in-flight read retains its old history epoch");
    const auto count = recorded.size();
    candidate = L"D:\\设计\\过期窗口";
    quick::dialog::observeHistory(window); quick::dialog::observeHistory(nullptr); readNext();
    check(recorded.size() == count, "ignore results after the foreground dialog changes");
    quick::dialog::observeHistory(window); duringRead = [] { foreground = nullptr; }; readNext(); duringRead = {};
    check(recorded.size() == count, "discard a query if focus changes during the read"); foreground = window;
    enable(false); quick::dialog::observeHistory(window); readNext();
    check(recorded.size() == count, "honor disabled dialog support"); enable(true);
    candidate = L"相对路径"; quick::dialog::observeHistory(window); readNext();
    check(recorded.size() == count, "never infer a directory from ambiguous text");
    candidate = L"D:\\设计\\退出前";
    quick::dialog::observeHistory(window); quick::dialog::stopHistory(); readNext();
    check(recorded.size() == count, "shutdown cancels pending reads without waiting on the provider");
    submit = false; quick::dialog::observeHistory(window); check(pendingReads.empty(), "handle worker scheduling failure");
    submit = true; quick::dialog::observeHistory(window); readNext();
    check(recorded.size() == count + 1, "retry after scheduling failure");
    wchar_t title[64]{}; GetWindowTextW(window, title, 64);
    check(!wcscmp(title, L"Unchanged filename.png") && clipboard == GetClipboardSequenceNumber(), "observation leaves filename and clipboard untouched");
    std::cout << "PASS passive dialog history: navigation, deduplication, generation, focus, settings and shutdown\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  quick::dialog::stopHistory(); while (!pendingReads.empty()) readNext();
  if (window) DestroyWindow(window);
  CoUninitialize(); return result;
}
