#include "../src/quick_folders.h"
#include "../src/navigation.h"
#include <iostream>
#include <thread>
#include <stdexcept>
#include <utility>

namespace {
HWND simulatedForeground = nullptr, underPointer = nullptr;
int modifier = 0, checks = 0;
std::vector<std::pair<HWND, POINT>> launches;
HWND fakeForeground() { return simulatedForeground; }
HWND fakeWindowFromPoint(POINT) { return underPointer; }
SHORT fakeKeys(int key) { return key == modifier ? SHORT(-32768) : 0; }
LRESULT fakeNextHook(HHOOK, int, WPARAM, LPARAM) { return 73; }
void check(bool condition, const char *reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
}
namespace quick {
void captureMenu(HWND target, HWND, POINT point) { launches.push_back({target, point}); }
}
// Run production hook/receiver logic on disposable hidden windows. No input
// is injected, global hook installed, menu process started, or user setting read.
#define GetForegroundWindow fakeForeground
#define WindowFromPoint fakeWindowFromPoint
#define GetAsyncKeyState fakeKeys
#define CallNextHookEx fakeNextHook
#define launchMenu captureMenu
#include "../src/quick_runtime.cpp"
#undef launchMenu
#undef CallNextHookEx
#undef GetAsyncKeyState
#undef WindowFromPoint
#undef GetForegroundWindow

struct Isolated {
  HKEY real = nullptr, temporary = nullptr;
  std::wstring name;
  Isolated() {
    GUID id{}; CoCreateGuid(&id); wchar_t token[40]{}; StringFromGUID2(id, token, ARRAYSIZE(token));
    name = std::wstring(L"Software\\FolderNoteRuntimeTests\\") + token;
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &real) == ERROR_SUCCESS &&
          RegCreateKeyExW(real, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &temporary, nullptr) == ERROR_SUCCESS &&
          RegOverridePredefKey(HKEY_CURRENT_USER, temporary) == ERROR_SUCCESS, "isolate quick runtime settings");
  }
  ~Isolated() { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(temporary); RegDeleteTreeW(real, name.c_str()); RegCloseKey(real); }
};
static void enable(bool value) {
  DWORD enabled = value ? 1 : 0, dialogs = 1;
  check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled)) == ERROR_SUCCESS &&
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersInDialogs", REG_DWORD, &dialogs, sizeof(dialogs)) == ERROR_SUCCESS,
        "write isolated runtime switches");
}
static LRESULT mouse(UINT message, POINT point = {20, 20}) {
  MSLLHOOKSTRUCT data{}; data.pt = point;
  return quick::mouseHook(HC_ACTION, message, reinterpret_cast<LPARAM>(&data));
}
static void drain() {
  MSG message{};
  while (PeekMessageW(&message, quick::receiver, quick::showMessage, quick::showMessage, PM_REMOVE))
    quick::helperProc(message.hwnd, message.message, message.wParam, message.lParam);
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try {
    Isolated settings; enable(true);
    WNDCLASSW shell{}; shell.hInstance = GetModuleHandleW(nullptr); shell.lpfnWndProc = DefWindowProcW; shell.lpszClassName = L"SHELLDLL_DefView";
    check(RegisterClassW(&shell) != 0, "register disposable file-dialog view");
    auto dialog = [&] {
      HWND root = CreateWindowW(L"#32770", L"Isolated runtime dialog", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, shell.hInstance, nullptr);
      check(root && CreateWindowW(shell.lpszClassName, L"", WS_CHILD, 0, 0, 100, 50, root, nullptr, shell.hInstance, nullptr), "create hidden test file dialog");
      return root;
    };
    HWND first = dialog(), second = dialog();
    HWND ordinary = CreateWindowW(L"#32770", L"Ordinary dialog", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, shell.hInstance, nullptr);
    quick::receiver = CreateWindowW(L"STATIC", L"Isolated runtime receiver", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, shell.hInstance, nullptr);
    check(ordinary && quick::receiver, "create isolated request receiver");
    simulatedForeground = underPointer = first; quick::eligible = nullptr;
    check(mouse(WM_MBUTTONDOWN) == 73, "uncached target keeps its native middle-down input");
    quick::updateTarget();
    check(mouse(WM_MBUTTONUP) == 73, "uncached gesture keeps matching native middle-up input"); drain();
    check(launches.size() == 1 && launches.back().first == first, "foreground cache race still launches exactly once after full validation");
    quick::eligible = nullptr;
    mouse(WM_MBUTTONDOWN, {30, 30}); mouse(WM_MBUTTONUP, {30, 30});
    mouse(WM_MBUTTONDBLCLK, {50, 50}); mouse(WM_MBUTTONUP, {50, 50}); drain();
    check(launches.size() == 3 && launches[1].second.x == 30 && launches[2].second.x == 50,
          "native double-click repeats and queued requests retain their own coordinates");
    const auto beforeRejects = launches.size();
    mouse(WM_MBUTTONDOWN); simulatedForeground = underPointer = second; quick::eligible = second; mouse(WM_MBUTTONUP); drain();
    check(launches.size() == beforeRejects, "release in a different foreground target never launches");
    simulatedForeground = underPointer = first; quick::eligible = first;
    mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); simulatedForeground = underPointer = second; drain();
    check(launches.size() == beforeRejects, "target changing before receiver handles request cancels it");
    simulatedForeground = underPointer = first;
    mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); enable(false); drain();
    check(launches.size() == beforeRejects, "disable before queued request is processed prevents launch"); enable(true);
    simulatedForeground = underPointer = ordinary; quick::eligible = nullptr;
    check(mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73, "ordinary dialog native input is not swallowed"); drain();
    check(launches.size() == beforeRejects, "ordinary non-file dialog fails full target validation");
    simulatedForeground = underPointer = first; quick::eligible = first;
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
      modifier = key; mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); modifier = 0;
      mouse(WM_MBUTTONDOWN); modifier = key; mouse(WM_MBUTTONUP); modifier = 0;
    }
    drain(); check(launches.size() == beforeRejects, "modifiers at press or release cancel the menu gesture");
    const int jitter = std::min(3, GetSystemMetrics(SM_CXDRAG));
    mouse(WM_MBUTTONDOWN); mouse(WM_MOUSEMOVE, {20 + jitter, 20}); mouse(WM_MBUTTONUP, {20 + jitter, 20}); drain();
    check(launches.size() == beforeRejects + 1, "ordinary three-pixel wheel press jitter is tolerated");
    const int drag = GetSystemMetrics(SM_CXDRAG) + 10;
    mouse(WM_MBUTTONDOWN); mouse(WM_MOUSEMOVE, {20 + drag, 20}); mouse(WM_MOUSEMOVE); mouse(WM_MBUTTONUP); drain();
    check(launches.size() == beforeRejects + 1, "large drag returning to origin still cancels the gesture");
    auto stale = new quick::MenuRequest{first, GetCurrentProcessId() + 1, {20, 20}};
    quick::helperProc(quick::receiver, quick::showMessage, 0, reinterpret_cast<LPARAM>(stale));
    check(launches.size() == beforeRejects + 1, "reused target handle with mismatched process is rejected");
    WNDCLASSW bandClass = shell; bandClass.lpszClassName = L"FolderNote.ExplorerTabs.Band.v1";
    check(RegisterClassW(&bandClass) != 0, "register disposable FolderNote tab band");
    HWND band = CreateWindowW(bandClass.lpszClassName, L"Tab band background", WS_CHILD, 0, 0, 100, 30,
                              first, nullptr, shell.hInstance, nullptr);
    HWND tabs = CreateWindowW(L"STATIC", L"Tab control", WS_CHILD, 0, 0, 70, 30, band, nullptr, shell.hInstance, nullptr);
    HWND nested = CreateWindowW(L"STATIC", L"Tab control child", WS_CHILD, 0, 0, 20, 20, tabs, nullptr, shell.hInstance, nullptr);
    HWND overflow = CreateWindowW(L"BUTTON", L"Overflow", WS_CHILD, 70, 0, 30, 30, band, nullptr, shell.hInstance, nullptr);
    HWND list = FindWindowExW(first, nullptr, shell.lpszClassName, nullptr);
    check(band && tabs && nested && overflow && list, "create hidden tab descendants and ordinary file list");
    for (bool cached : {false, true}) {
      simulatedForeground = first;
      const auto beforeTabs = launches.size();
      for (HWND target : {band, tabs, nested, overflow}) {
        quick::eligible = cached ? first : nullptr; underPointer = target;
        check(mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73 &&
              mouse(WM_MBUTTONDBLCLK) == 73 && mouse(WM_MBUTTONUP) == 73,
              "cached and uncached tab band, blank area and children retain all middle input");
        MSG queued{};
        check(!PeekMessageW(&queued, quick::receiver, quick::showMessage, quick::showMessage, PM_NOREMOVE),
              "middle input over tab band does not even queue a popup request");
      }
      drain(); check(launches.size() == beforeTabs, "tab band gestures never open quick folder popup");
      quick::eligible = cached ? first : nullptr; underPointer = list;
      check(mouse(WM_MBUTTONDOWN) == (cached ? 1 : 73), "ordinary file-list middle-down retains original interception policy");
      underPointer = nested;
      check(mouse(WM_MBUTTONUP) == 1, "file-list press released over tab is consumed to protect older bands from orphan release");
      drain(); check(launches.size() == beforeTabs, "cross-area release on tab cancels menu gesture");
      quick::eligible = cached ? first : nullptr; underPointer = list;
      mouse(WM_MBUTTONDOWN); underPointer = band;
      check(mouse(WM_MOUSEMOVE) == 73 && mouse(WM_MBUTTONUP) == 1,
              "entering band while pressed retains release suppression for older loaded bands");
      underPointer = list; mouse(WM_MBUTTONDOWN); underPointer = nested; mouse(WM_MOUSEMOVE);
      underPointer = list;
      check(mouse(WM_MBUTTONUP) == 1, "returning from band cannot turn cancelled gesture into an orphan release");
      drain(); check(launches.size() == beforeTabs, "cross-region moves never queue a folder menu");
      quick::eligible = cached ? first : nullptr; underPointer = band;
      check(mouse(WM_MBUTTONDOWN) == 73, "tab press is always passed through");
      underPointer = list; check(mouse(WM_MBUTTONUP) == 73, "tab press cannot turn into a file-list menu on release");
      drain(); check(launches.size() == beforeTabs, "cross-area release on file list does not open popup");
      quick::eligible = cached ? first : nullptr; underPointer = list;
      check(mouse(WM_MBUTTONDOWN) == (cached ? 1 : 73) && mouse(WM_MBUTTONUP) == (cached ? 1 : 73),
              "ordinary file-list gesture preserves cached and uncached input handling");
      drain(); check(launches.size() == beforeTabs + 1 && launches.back().first == first,
                     "ordinary file-list middle click still opens one popup after band exclusion");
    }
    DestroyWindow(band); UnregisterClassW(bandClass.lpszClassName, bandClass.hInstance);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, nullptr), started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool acquired = false;
    std::thread replacement([&] { SetEvent(started); acquired = quick::acquireHelperMutex(mutex, 1000); if (acquired) ReleaseMutex(mutex); });
    WaitForSingleObject(started, INFINITE); ReleaseMutex(mutex); replacement.join();
    check(acquired, "replacement helper waits for previous mutex owner and then takes over");
    CloseHandle(started); CloseHandle(mutex);
    DestroyWindow(quick::receiver); quick::receiver = nullptr;
    DestroyWindow(first); DestroyWindow(second); DestroyWindow(ordinary); UnregisterClassW(shell.lpszClassName, shell.hInstance);
    std::cout << "PASS " << checks << " quick runtime assertions: foreground race, per-click targets, queued coordinates, gesture filtering, helper handoff\n";
  } catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; result = 1; }
  CoUninitialize(); return result;
}
