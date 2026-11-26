#include "quick_folders.h"
#include "language.h"
#include "quick_legacy.h"
#include "quick_dialog_history.h"
#include "quick_find_external.h"
#include "tabs_routing.h"
#include "navigation.h"
#include "core.h"
#include "desktop_groups_process.h"
#include "middle_click.h"
#include "remote_input.h"
#include "runtime_guard.h"
#include <atomic>
#include <memory>
#include <new>
#include <shlobj.h>
#include <vector>
namespace quick {
namespace {
constexpr wchar_t helperClass[] = L"FolderNote.QuickFolders.Helper.v1";
constexpr wchar_t readyProperty[] = L"FolderNote.QuickFolders.Ready.v2";
constexpr wchar_t propMouseHook[] = L"FolderNote.QuickFolders.MouseHook.v1";
constexpr wchar_t propKeyboardHook[] = L"FolderNote.QuickFolders.KeyboardHook.v1";
constexpr wchar_t propRehookCount[] = L"FolderNote.QuickFolders.RehookCount.v1";
constexpr wchar_t propRehookReason[] = L"FolderNote.QuickFolders.RehookReason.v1";
constexpr wchar_t propRehookTick[] = L"FolderNote.QuickFolders.RehookTick.v1";
constexpr wchar_t propMouseTick[] = L"FolderNote.QuickFolders.MouseTick.v1";
constexpr wchar_t propKeyboardTick[] = L"FolderNote.QuickFolders.KeyboardTick.v1";
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t runName[] = L"FolderNote.QuickFolders";
constexpr UINT showMessage = WM_APP + 0x511, foregroundMessage = WM_APP + 0x512, rehookMessage = WM_APP + 0x513,
               rehookLogMessage = WM_APP + 0x514;
// A crashed helper is restarted at most this often; beyond it the supervisor
// gives up so a deterministic startup crash cannot loop forever.
constexpr int supervisorRestartLimit = 3;
constexpr DWORD supervisorRestartWindow = 10 * 60 * 1000;
constexpr UINT_PTR historyTimer = 2;
std::atomic<HWND> eligible{nullptr};
std::atomic<DWORD> activeHookThreadId{0};
// Tick count of the most recent mouse callback. Windows silently removes a
// low-level hook after a timeout; consecutive missed pointer moves signal it.
std::atomic<DWORD> lastMouseCallback{0};
std::atomic<unsigned> hookRehookCount{0};
std::atomic<unsigned> lastRehookReasonCode{0};
std::atomic<DWORD> lastRehookTick{0};
HWND receiver = nullptr;
void publishHookInstallState(bool mouseInstalled, bool keyboardInstalled) noexcept {
  if (!receiver) return;
  if (mouseInstalled) SetPropW(receiver, propMouseHook, reinterpret_cast<HANDLE>(1));
  else RemovePropW(receiver, propMouseHook);
  if (keyboardInstalled) SetPropW(receiver, propKeyboardHook, reinterpret_cast<HANDLE>(1));
  else RemovePropW(receiver, propKeyboardHook);
}
void recordHookRehook(HookRehookReason reason, bool mouseInstalled, bool keyboardInstalled) noexcept {
  const unsigned count = ++hookRehookCount;
  const unsigned code = static_cast<unsigned>(reason);
  const DWORD now = GetTickCount();
  const DWORD tick = now ? now : 1;
  lastRehookReasonCode.store(code, std::memory_order_relaxed);
  lastRehookTick.store(tick, std::memory_order_relaxed);
  if (receiver) {
    publishHookInstallState(mouseInstalled, keyboardInstalled);
    SetPropW(receiver, propRehookCount, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(count)));
    SetPropW(receiver, propRehookReason, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(code)));
    SetPropW(receiver, propRehookTick, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tick)));
    // Never write files on the hook thread: a slow disk would delay hook
    // callbacks past LowLevelHooksTimeout. The UI thread logs it instead.
    PostMessageW(receiver, rehookLogMessage, code, count);
  }
}
// Pure decision, kept separate so it can be verified without real hooks.
// Never compare GetLastInputInfo against keyboard callbacks: upstream KVM
// hooks (Deskflow, Synergy, Barrier, Input Leap) legitimately swallow keys
// when redirecting input, which would otherwise make us rehook ahead of them.
bool mouseHookNeedsRefresh(bool remoteCaptured, bool cursorMoved,
                           DWORD mouseSeen, DWORD mouseNow, int &missedChecks) {
  if (remoteCaptured || !cursorMoved || mouseNow != mouseSeen) {
    missedChecks = 0;
    return false;
  }
  if (++missedChecks < 2) return false;
  missedChecks = 0;
  return true;
}
struct MenuRequest { HWND target; DWORD process; POINT point; bool desktopBox = false; };
struct MiddleGesture {
  HWND target = nullptr;
  DWORD process = 0;
  POINT down{};
  bool swallowed = false, dragged = false, suppressUp = false, desktopBox = false;
} middle;
bool helperEnabled() {
  return navigation::quickEnabled() || tabs::routing::enabled() || quick_find::external::enabled();
}
bool launchHelper(const std::wstring &exe, const desktop_groups::DesktopLaunchContext &context) {
  using desktop_groups::DesktopLaunchMode;
  if (context.mode == DesktopLaunchMode::Unavailable) return false;
  if (context.mode == DesktopLaunchMode::Explorer)
    return desktop_groups::executeFromDesktop(exe, L"--quick-helper --from-desktop-shell", context);
  auto command = L"\"" + exe + L"\" --quick-helper";
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) return false;
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return true;
}
bool classIs(HWND window, const wchar_t *name) {
  wchar_t cls[128]{}; GetClassNameW(window, cls, ARRAYSIZE(cls));
  return wcscmp(cls, name) == 0;
}
bool desktopClass(HWND window) {
  return classIs(window, L"Progman") || classIs(window, L"WorkerW");
}
bool trayClass(HWND window) {
  return classIs(window, L"Shell_TrayWnd") || classIs(window, L"SecondaryTrayWnd");
}
bool localInputSurface(HWND window, HWND frame) {
  // Tab bands own their middle-button gestures (tab close), even under a Shell root.
  // Check only class names/parents here; never call Shell or UIA in the hook.
  for (HWND current = window; current && current != frame; current = GetParent(current))
    if (classIs(current, L"FolderNote.ExplorerTabs.Band.v1")) return true;
  return false;
}
bool desktopPanelSurface(HWND window, HWND frame) {
  for (HWND current = window; current && current != frame; current = GetParent(current))
    if (classIs(current, L"FolderNote.DesktopGroups.Panel")) return true;
  return false;
}
DWORD integrity(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), token = nullptr;
  if (!process) return 0xffffffff;
  BOOL ok = OpenProcessToken(process, TOKEN_QUERY, &token); CloseHandle(process);
  if (!ok) return 0xffffffff;
  DWORD bytes = 0; GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
  std::vector<BYTE> buffer(bytes);
  ok = GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), bytes, &bytes); CloseHandle(token);
  if (!ok) return 0xffffffff;
  auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data())->Label.Sid;
  return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}
bool eligibleTarget(HWND window, DWORD expectedProcess = 0) {
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  if (!window || !pid || (expectedProcess && pid != expectedProcess) || !navigation::quickEnabled()) return false;
  if (!isDesktop(window) && (!navigation::dialogQuickEnabled() || (!isExplorer(window) && !isFileDialog(window)))) return false;
  const auto targetIntegrity = integrity(pid), ownIntegrity = integrity(GetCurrentProcessId());
  return targetIntegrity != 0xffffffff && ownIntegrity != 0xffffffff && targetIntegrity <= ownIntegrity;
}
HWND searchTarget(HWND window) {
  HWND search = nullptr;
  const auto findOptions = quick_find::settings();
  if ((findOptions.explorer && (isDesktop(window) || isExplorer(window))) || (findOptions.dialogs && isFileDialog(window))) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    const auto targetLevel = integrity(process), ownLevel = integrity(GetCurrentProcessId());
    if (targetLevel != 0xffffffff && ownLevel != 0xffffffff && targetLevel <= ownLevel) search = window;
  }
  return search;
}
// File dialogs navigate without a foreground change. Instead of polling every
// 250 ms, watch name changes (the address band's accessible name carries the
// path) only in the foreground dialog's process, debounced, plus a slow
// fallback in case an event is missed. No timer runs when no dialog is active.
constexpr UINT historyFallbackMs = 1000, historyDebounceMs = 300;
constexpr UINT_PTR historyDebounceTimer = 3;
HWND observedDialog = nullptr;
HWINEVENTHOOK dialogNameHook = nullptr;
DWORD dialogNameProcess = 0;
void CALLBACK dialogNameEvent(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
  if (receiver && window && observedDialog && GetAncestor(window, GA_ROOT) == observedDialog)
    SetTimer(receiver, historyDebounceTimer, historyDebounceMs, nullptr);
}
void watchDialog(HWND dialogWindow) {
  DWORD process = 0;
  if (dialogWindow) GetWindowThreadProcessId(dialogWindow, &process);
  observedDialog = process ? dialogWindow : nullptr;
  if (process != dialogNameProcess) {
    if (dialogNameHook) UnhookWinEvent(dialogNameHook);
    dialogNameHook = nullptr; dialogNameProcess = 0;
    if (process) {
      dialogNameHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, dialogNameEvent,
                                       process, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
      if (dialogNameHook) dialogNameProcess = process;
    }
  }
  if (!receiver) return;
  if (observedDialog) SetTimer(receiver, historyTimer, historyFallbackMs, nullptr);
  else { KillTimer(receiver, historyTimer); KillTimer(receiver, historyDebounceTimer); }
}
void updateTarget() {
  HWND window = GetForegroundWindow();
  eligible.store(eligibleTarget(window) ? window : nullptr);
  const bool dialogWindow = isFileDialog(window);
  dialog::observeHistory(eligible.load() == window && dialogWindow ? window : nullptr);
  quick_find::external::target(searchTarget(window));
  watchDialog(dialogWindow ? window : nullptr);
}
void CALLBACK foregroundEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD) {
  if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
    if (const DWORD tid = activeHookThreadId.load())
      PostThreadMessageW(tid, rehookMessage, static_cast<WPARAM>(HookRehookReason::DesktopSwitch), 0);
    if (receiver) PostMessageW(receiver, foregroundMessage, 0, 0);
    return;
  }
  if (event == EVENT_OBJECT_SHOW && (!window || GetAncestor(window, GA_ROOT) != GetForegroundWindow() ||
      !classIs(GetForegroundWindow(), L"#32770"))) return;
  if (receiver) PostMessageW(receiver, foregroundMessage, 0, 0);
}
LRESULT CALLBACK mouseHook(int code, WPARAM message, LPARAM parameter) {
  if (code == HC_ACTION) {
    const DWORD now = GetTickCount();
    lastMouseCallback.store(now, std::memory_order_relaxed);
    static DWORD publishedTick = 0;
    static HWND publishedReceiver = nullptr;
    if (receiver && (!publishedTick || publishedReceiver != receiver || now - publishedTick >= 250)) {
      publishedTick = now ? now : 1;
      publishedReceiver = receiver;
      SetPropW(receiver, propMouseTick, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(publishedTick)));
    }
    quick_find::external::mouse(message, *reinterpret_cast<const MSLLHOOKSTRUCT *>(parameter));
  }
  if (code == HC_ACTION && (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK ||
                            message == WM_MBUTTONUP || (message == WM_MOUSEMOVE && middle.target))) {
    const auto *mouse = reinterpret_cast<MSLLHOOKSTRUCT *>(parameter);
    if (message == WM_MBUTTONUP && middle.suppressUp) { middle = {}; return 1; }
    const HWND under = WindowFromPoint(mouse->pt), root = GetAncestor(under, GA_ROOT);
    if (localInputSurface(under, root)) {
      // Old loaded bands close on bare UP. Do not deliver an orphan release
      // after a gesture started in the file list, including MOVE into the band.
      const bool fromFileList = middle.target != nullptr;
      middle = {};
      if (fromFileList && message == WM_MBUTTONUP) return 1;
      if (fromFileList && message == WM_MOUSEMOVE) middle.suppressUp = true;
      return CallNextHookEx(nullptr, code, message, parameter);
    }
    const bool allowed = !((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU) |
                           GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000);
    HWND foreground = GetForegroundWindow();
    if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) {
      middle = {};
      if (remoteInputCaptured()) return CallNextHookEx(nullptr, code, message, parameter);
      // These cheap checks run on the hook thread. Process identity, dialog
      // contents and integrity are checked on the helper thread before launch.
      const bool inBox = root && desktopClass(root) && desktopPanelSurface(under, root);
      if (allowed && root && (desktopClass(root) || (root == foreground &&
          (classIs(root, L"CabinetWClass") || classIs(root, L"ExploreWClass") || classIs(root, L"#32770"))))) {
        middle.target = root; middle.down = mouse->pt; middle.desktopBox = inBox;
        GetWindowThreadProcessId(root, &middle.process);
        middle.swallowed = inBox || (root == foreground && eligible.load() == root);
      }
      if (middle.swallowed) return 1;
    } else if (middle.target) {
      middle.dragged = middle.dragged || !allowed || !middleClickWithin(middle.down, mouse->pt);
      if (message == WM_MBUTTONUP) {
        const auto completed = middle; middle = {};
        DWORD process = 0; GetWindowThreadProcessId(foreground, &process);
        // Explorer can keep Progman or Shell_TrayWnd foreground while its desktop
        // view lives under WorkerW. Use the foreground desktop when applicable.
        const bool desktop = desktopClass(completed.target) && (desktopClass(foreground) || trayClass(foreground));
        const bool boxDesktop = completed.desktopBox && desktopClass(completed.target) &&
                                desktopPanelSurface(under, root);
        if (!completed.dragged && root == completed.target &&
            (((foreground == completed.target || desktop) && process == completed.process) || boxDesktop)) {
          const HWND anchor = desktopClass(foreground) ? foreground : completed.target;
          auto request = new (std::nothrow) MenuRequest{anchor, completed.process, mouse->pt, boxDesktop};
          if (request && !PostMessageW(receiver, showMessage, 0, reinterpret_cast<LPARAM>(request))) delete request;
        }
        // Unknown targets retain native input while awaiting full validation.
        if (completed.swallowed) return 1;
      }
    }
  }
  return CallNextHookEx(nullptr, code, message, parameter);
}
LRESULT CALLBACK keyboardHook(int code, WPARAM message, LPARAM parameter) {
  if (code == HC_ACTION) {
    const DWORD now = GetTickCount();
    static DWORD publishedTick = 0;
    static HWND publishedReceiver = nullptr;
    if (receiver && (!publishedTick || publishedReceiver != receiver || now - publishedTick >= 250)) {
      publishedTick = now ? now : 1;
      publishedReceiver = receiver;
      SetPropW(receiver, propKeyboardTick, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(publishedTick)));
    }
    try { if (quick_find::external::keyboard(message, *reinterpret_cast<KBDLLHOOKSTRUCT *>(parameter))) return 1; } catch (...) {}
  }
  // Candidate navigation stays in the popup; ordinary application input passes through.
  return CallNextHookEx(nullptr, code, message, parameter);
}
struct HookStart { HANDLE ready; HHOOK hook = nullptr, keyboard = nullptr; bool search = false; };
bool acquireHelperMutex(HANDLE mutex, DWORD timeout = 5000) {
  const auto acquired = WaitForSingleObject(mutex, timeout);
  return acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED;
}
DWORD WINAPI hookThread(void *argument) {
  auto *state = static_cast<HookStart *>(argument);
  MSG msg; PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  const auto module = GetModuleHandleW(nullptr);
  state->hook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, module, 0);
  if (state->search) state->keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, module, 0);
  publishHookInstallState(state->hook != nullptr, state->keyboard != nullptr);
  activeHookThreadId.store(GetCurrentThreadId());
  SetEvent(state->ready);
  if (!state->hook) {
    activeHookThreadId.store(0);
    return 1;
  }
  // Check health every 2 seconds and reinstall the mouse hook only after
  // consecutive uncaptured pointer moves went unseen. Never periodically
  // reinstall WH_KEYBOARD_LL, or we would steal hook-chain priority ahead of
  // software KVMs (Deskflow, Synergy, Barrier, Input Leap).
  const UINT_PTR healthTimer = SetTimer(nullptr, 0, 2000, nullptr);
  POINT seenCursor{};
  GetCursorPos(&seenCursor);
  DWORD seenMouse = lastMouseCallback.load();
  int missedMouseChecks = 0;
  bool pendingFullRehook = false;
  HookRehookReason pendingReason = HookRehookReason::DesktopSwitch;
  const auto refreshMouseHook = [&](HookRehookReason reason, bool record) {
    if (middle.target == nullptr) {
      if (HHOOK freshMouse = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, module, 0)) {
        if (state->hook) UnhookWindowsHookEx(state->hook);
        state->hook = freshMouse;
      }
    }
    if (record) recordHookRehook(reason, state->hook != nullptr, state->keyboard != nullptr);
  };
  const auto refreshAllHooks = [&](HookRehookReason reason) {
    refreshMouseHook(reason, false);
    if (state->search) {
      if (HHOOK freshKey = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, module, 0)) {
        if (state->keyboard) UnhookWindowsHookEx(state->keyboard);
        state->keyboard = freshKey;
      }
    }
    recordHookRehook(reason, state->hook != nullptr, state->keyboard != nullptr);
  };
  const auto checkHealth = [&] {
    const bool captured = remoteInputCaptured();
    if (pendingFullRehook && !captured) {
      pendingFullRehook = false;
      missedMouseChecks = 0;
      refreshAllHooks(pendingReason);
    } else {
      POINT cursor{};
      const bool haveCursor = GetCursorPos(&cursor) != FALSE;
      const bool moved = haveCursor && (cursor.x != seenCursor.x || cursor.y != seenCursor.y);
      if (mouseHookNeedsRefresh(captured, moved, seenMouse, lastMouseCallback.load(), missedMouseChecks))
        refreshMouseHook(HookRehookReason::MissedMouseMove, true);
      if (haveCursor) seenCursor = cursor;
    }
    seenMouse = lastMouseCallback.load();
  };
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd == nullptr && msg.message == rehookMessage) {
      const auto reason = msg.wParam >= static_cast<WPARAM>(HookRehookReason::MissedMouseMove) &&
                          msg.wParam <= static_cast<WPARAM>(HookRehookReason::PowerResume)
                              ? static_cast<HookRehookReason>(msg.wParam)
                              : HookRehookReason::DesktopSwitch;
      if (remoteInputCaptured()) {
        pendingFullRehook = true;
        pendingReason = reason;
      } else {
        pendingFullRehook = false;
        missedMouseChecks = 0;
        refreshAllHooks(reason);
      }
      continue;
    }
    if (msg.hwnd == nullptr && msg.message == WM_TIMER && msg.wParam == healthTimer) {
      checkHealth();
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  activeHookThreadId.store(0);
  if (healthTimer) KillTimer(nullptr, healthTimer);
  if (state->hook) UnhookWindowsHookEx(state->hook);
  if (state->keyboard) UnhookWindowsHookEx(state->keyboard);
  return 0;
}
// Resident menu thread: showing the menu from the already-running helper skips
// a process start (and its 200 ms gate race) on every middle click. While it is
// busy (menu open, slow copy/move, dialog) or unavailable, the proven separate
// --quick-menu process is used instead, so a click is never dropped.
constexpr UINT menuRunMessage = WM_APP + 0x515;
constexpr wchar_t inProcessMenuValue[] = L"QuickMenuInProcess";
MenuRunner inProcessMenu = nullptr;
HANDLE menuThread = nullptr;
DWORD menuThreadId = 0;
std::atomic<bool> menuBusy{false};
std::atomic<bool> menuStopping{false};
struct MenuJob { HINSTANCE instance; HWND target; POINT point; };
DWORD WINAPI menuThreadProc(void *ready) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  MSG msg; PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  SetEvent(static_cast<HANDLE>(ready));
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd || msg.message != menuRunMessage) { TranslateMessage(&msg); DispatchMessageW(&msg); continue; }
    std::unique_ptr<MenuJob> job(reinterpret_cast<MenuJob *>(msg.lParam));
    if (menuStopping.load(std::memory_order_relaxed)) { menuBusy = false; continue; }
    const ULONGLONG started = GetTickCount64();
    try { if (job && inProcessMenu) inProcessMenu(job->instance, job->target, nullptr, job->point); }
    catch (...) { guard::log(L"常驻中键菜单出现未知异常"); }
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed > 60000) guard::log(L"常驻中键菜单占用了 " + std::to_wstring(elapsed / 1000) + L" 秒（期间改用独立进程）");
    menuBusy = false;
  }
  if (SUCCEEDED(initialized)) CoUninitialize();
  return 0;
}
bool inProcessMenuAllowed() {
  DWORD value = 1, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", inProcessMenuValue, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    value = 1;
  return value != 0;
}
void startMenuThread() {
  if (!inProcessMenu || menuThread || !inProcessMenuAllowed()) return;
  menuStopping = false;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ready) return;
  menuThread = CreateThread(nullptr, 0, menuThreadProc, ready, 0, &menuThreadId);
  // Wait indefinitely like hookThread: menuThreadProc only initializes COM and
  // creates its message queue before signaling, so closing ready early is unsafe.
  if (menuThread) WaitForSingleObject(ready, INFINITE);
  CloseHandle(ready);
  if (!menuThread) guard::log(L"无法启动常驻中键菜单线程，改用独立进程");
}
void signalMenuThreadQuit() {
  menuStopping = true;
  if (menuThread && menuThreadId) PostThreadMessageW(menuThreadId, WM_QUIT, 0, 0);
}
void joinMenuThread() {
  if (!menuThread) return;
  // Wait after hooks, receiver window and singleton mutex are already released,
  // so a new helper can start immediately while any in-progress Copy/Move or
  // Shell call on this thread finishes cleanly before CRT static teardown.
  WaitForSingleObject(menuThread, INFINITE);
  CloseHandle(menuThread);
  menuThread = nullptr; menuThreadId = 0; menuBusy = false;
}
void showMenu(HINSTANCE instance, HWND target, POINT point) {
  if (menuThread) {
    if (WaitForSingleObject(menuThread, 0) != WAIT_TIMEOUT) {
      CloseHandle(menuThread);
      menuThread = nullptr; menuThreadId = 0; menuBusy = false;
      guard::log(L"常驻中键菜单线程已提前退出，改用独立进程");
    } else if (dialog::pathReaderAvailable() && !menuBusy.exchange(true)) {
      auto job = new (std::nothrow) MenuJob{instance, target, point};
      if (job && PostThreadMessageW(menuThreadId, menuRunMessage, 0, reinterpret_cast<LPARAM>(job))) return;
      delete job;
      menuBusy = false;
    }
  }
  launchMenu(target, nullptr, point);
}
LRESULT CALLBACK helperProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  LRESULT routingResult = 0;
  if (tabs::routing::message(window, message, w, l, routingResult)) return routingResult;
  if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
  if (message == WM_DESTROY) {
    for (const auto *prop : {readyProperty, propMouseHook, propKeyboardHook, propRehookCount,
                             propRehookReason, propRehookTick, propMouseTick, propKeyboardTick}) {
      RemovePropW(window, prop);
    }
    tabs::routing::stop(); KillTimer(window, 1); KillTimer(window, historyTimer); KillTimer(window, historyDebounceTimer);
    watchDialog(nullptr); legacy::shutdown(); dialog::stopHistory();
    MSG pending{};
    while (PeekMessageW(&pending, window, showMessage, showMessage, PM_REMOVE))
      delete reinterpret_cast<MenuRequest *>(pending.lParam);
    PostQuitMessage(0); return 0;
  }
  try {
    if (message == WM_POWERBROADCAST && (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND)) {
      if (const DWORD tid = activeHookThreadId.load())
        PostThreadMessageW(tid, rehookMessage, static_cast<WPARAM>(HookRehookReason::PowerResume), 0);
      updateTarget();
      return TRUE;
    }
    if (message == WM_COPYDATA) {
      const bool accepted = legacy::receive(reinterpret_cast<const COPYDATASTRUCT *>(l), eligibleTarget);
      if (accepted) SetTimer(window, 1, 1000, nullptr);
      return accepted ? TRUE : FALSE;
    }
    if (message == WM_TIMER && w == 1) { if (!legacy::reap()) KillTimer(window, 1); return 0; }
    if (message == WM_TIMER && w == historyTimer) { updateTarget(); return 0; }
    if (message == WM_TIMER && w == historyDebounceTimer) { KillTimer(window, historyDebounceTimer); updateTarget(); return 0; }
    if (message == foregroundMessage) { updateTarget(); return 0; }
    if (message == rehookLogMessage) {
      guard::log(std::wstring(L"中键钩子重挂：原因=") + hookRehookReasonText(static_cast<HookRehookReason>(w)) +
                 L"，累计 " + std::to_wstring(l) + L" 次");
      return 0;
    }
    if (message == showMessage) {
      std::unique_ptr<MenuRequest> request(reinterpret_cast<MenuRequest *>(l));
      const auto desktopActive = [&](HWND current) {
        return desktopClass(request->target) && (isDesktop(current) || trayClass(current));
      };
      const HWND fgBefore = GetForegroundWindow();
      if (request && (request->desktopBox || request->target == fgBefore || desktopActive(fgBefore)) &&
          eligibleTarget(request->target, request->process) &&
          (!desktopClass(request->target) || isDesktop(GetAncestor(WindowFromPoint(request->point), GA_ROOT)))) {
        const HWND fgAfter = GetForegroundWindow();
        if (request->desktopBox || request->target == fgAfter || desktopActive(fgAfter)) {
          eligible.store(request->target);
          showMenu(GetModuleHandleW(nullptr), request->target, request->point);
        }
      }
      return 0;
    }
  } catch (const fn::Error &error) {
    guard::log(L"后台组件消息处理失败：" + error.message);
  } catch (const std::exception &error) {
    guard::log(L"后台组件消息处理异常：" + std::wstring(error.what(), error.what() + strlen(error.what())));
  } catch (...) {
    guard::log(L"后台组件消息处理出现未知异常");
  }
  return DefWindowProcW(window, message, w, l);
}
}
void setInProcessMenu(MenuRunner runner) noexcept { inProcessMenu = runner; }
const wchar_t *hookRehookReasonText(HookRehookReason reason) noexcept {
  switch (reason) {
  case HookRehookReason::None: return L"无（初始挂载）";
  case HookRehookReason::MissedMouseMove: return L"指针移动未收到回调";
  case HookRehookReason::DesktopSwitch: return L"系统桌面/锁屏切换";
  case HookRehookReason::PowerResume: return L"系统睡眠唤醒";
  }
  return L"未知";
}
HookDiagnostics queryHookDiagnostics(HWND helperWindow) noexcept {
  HookDiagnostics diag{};
  HWND target = helperWindow ? helperWindow : FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr);
  if (!target && receiver && IsWindow(receiver)) target = receiver;
  if (!target || !IsWindow(target)) return diag;
  diag.helperRunning = GetPropW(target, readyProperty) != nullptr || target == receiver;
  diag.mouseHookInstalled = GetPropW(target, propMouseHook) != nullptr;
  diag.keyboardHookInstalled = GetPropW(target, propKeyboardHook) != nullptr;
  diag.rehookCount = static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(GetPropW(target, propRehookCount)));
  const auto rawReason = static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(GetPropW(target, propRehookReason)));
  if (rawReason <= static_cast<unsigned>(HookRehookReason::PowerResume)) {
    diag.lastRehookReason = static_cast<HookRehookReason>(rawReason);
  }
  const DWORD now = GetTickCount();
  const DWORD nowTick = now ? now : 1;
  if (const auto mouseTick = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(target, propMouseTick)))) {
    diag.msSinceLastMouseCallback = nowTick - mouseTick;
  }
  if (const auto keyTick = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(target, propKeyboardTick)))) {
    diag.msSinceLastKeyboardCallback = nowTick - keyTick;
  }
  if (const auto rehookTick = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(target, propRehookTick)))) {
    diag.msSinceLastRehook = nowTick - rehookTick;
  }
  return diag;
}
bool isDesktop(HWND window) {
  if (!window || !desktopClass(window) || GetAncestor(window, GA_ROOT) != window) return false;
  HWND shell = GetShellWindow();
  DWORD shellPid = 0, pid = 0;
  GetWindowThreadProcessId(shell, &shellPid); GetWindowThreadProcessId(window, &pid);
  if (!shell || !shellPid || pid != shellPid) return false;
  if (window == shell) return true;
  // Exclude taskbar, wallpaper-only WorkerW windows and class-name lookalikes
  // in other processes. The icon view may be nested on newer Windows builds.
  bool view = false;
  EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
    if (classIs(child, L"SHELLDLL_DefView")) { *reinterpret_cast<bool *>(data) = true; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&view));
  return view;
}
bool isExplorer(HWND window) {
  if (!classIs(window, L"CabinetWClass") && !classIs(window, L"ExploreWClass")) return false;
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  wchar_t image[32768]{}, windows[MAX_PATH]{}; DWORD count = ARRAYSIZE(image);
  BOOL ok = QueryFullProcessImageNameW(process, 0, image, &count); CloseHandle(process);
  return ok && GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) &&
         !_wcsicmp(image, (std::wstring(windows) + L"\\explorer.exe").c_str());
}
bool isFileDialog(HWND window) {
  if (!classIs(window, L"#32770")) return false;
  bool found = false;
  EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
    if (classIs(child, L"SHELLDLL_DefView")) { *reinterpret_cast<bool *>(data) = true; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&found));
  return found || legacy::isDialog(window);
}
bool addressEdit(HWND edit, HWND dialog) {
  if (!edit || !IsChild(dialog, edit) || !classIs(edit, L"Edit")) return false;
  // Never accept the filename edit or send an Open/Save command. The address
  // band uses 41477 on supported Windows common dialogs; also recognize its host.
  bool address = GetDlgCtrlID(edit) == 41477;
  for (HWND h = GetParent(edit); h && h != dialog; h = GetParent(h)) {
    wchar_t cls[128]{}; GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (wcsstr(cls, L"Breadcrumb") || wcsstr(cls, L"Address Band")) address = true;
    if (classIs(h, L"FileNameControlHost")) return false;
  }
  return address;
}
void configureStartup(const std::wstring &exe, bool enabled) {
  if (!enabled) {
    HKEY key = nullptr;
    auto error = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
    if (!error) {
      error = RegDeleteValueW(key, runName); RegCloseKey(key);
      if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) fn::fail(lang::text(L"无法移除快速定位启动项"), error);
    } else if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) fn::fail(lang::text(L"无法更新快速定位启动项"), error);
    return;
  }
  HKEY key = nullptr;
  auto command = L"\"" + exe + L"\" --quick-helper";
  auto error = RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (!error) { error = RegSetValueExW(key, runName, 0, REG_SZ,
      reinterpret_cast<const BYTE *>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))); RegCloseKey(key); }
  if (error) fn::fail(lang::text(L"选项已保存，但无法启用快速定位启动项"), error);
}
constexpr wchar_t supervisorClass[] = L"FolderNote.QuickFolders.Supervisor.v1";
LRESULT CALLBACK supervisorProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_CLOSE) {
    if (const auto stopEvent = reinterpret_cast<HANDLE>(GetWindowLongPtrW(window, GWLP_USERDATA)))
      SetEvent(stopEvent);
    DestroyWindow(window);
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
DWORD waitWithPump(HANDLE first, HANDLE second, DWORD timeoutMs) {
  HANDLE handles[2]{first, second};
  const DWORD count = second ? 2 : 1;
  const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;
  for (;;) {
    DWORD remaining = INFINITE;
    if (timeoutMs != INFINITE) {
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) return WAIT_TIMEOUT;
      remaining = static_cast<DWORD>(deadline - now);
    }
    const DWORD wait = MsgWaitForMultipleObjects(count, handles, FALSE, remaining, QS_ALLINPUT);
    if (wait != WAIT_OBJECT_0 + count) return wait;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
}
bool stopHelper(bool removeStartup) {
  // Close transient menus as well, including a hidden 1.6.1 popup waiting in
  // GetMessage. Posting WM_CLOSE wakes old versions without killing Explorer
  // or a settings/note editor that might contain unsaved input.
  EnumWindows([](HWND window, LPARAM) -> BOOL {
    if (!classIs(window, L"FolderNote.QuickFolders.Popup.v2") && !classIs(window, L"FolderNote.QuickMenu")) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
      wchar_t image[32768]{}; DWORD size = ARRAYSIZE(image);
      BOOL valid = QueryFullProcessImageNameW(process, 0, image, &size); CloseHandle(process);
      if (valid && !_wcsicmp(std::filesystem::path(image).filename().c_str(), L"FolderNote.exe")) PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
  }, 0);
  if (removeStartup) configureStartup({}, false);
  // Stop any supervisor first so a crashing child cannot respawn mid-shutdown.
  HANDLE supProcess = nullptr;
  if (HWND supWindow = FindWindowExW(HWND_MESSAGE, nullptr, supervisorClass, nullptr)) {
    DWORD supPid = 0; GetWindowThreadProcessId(supWindow, &supPid);
    supProcess = supPid ? OpenProcess(SYNCHRONIZE, FALSE, supPid) : nullptr;
    PostMessageW(supWindow, WM_CLOSE, 0, 0);
  }
  bool posted = true;
  HWND window = FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr);
  if (window) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    posted = PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
    if (process) { if (posted) WaitForSingleObject(process, 2000); CloseHandle(process); }
  }
  if (supProcess) {
    WaitForSingleObject(supProcess, 2000);
    CloseHandle(supProcess);
  }
  return posted;
}
void syncHelper(const std::wstring &exe) {
  if (!stopHelper(false))
    throw fn::Error{lang::text(L"设置已保存，但旧的文件夹后台组件未能退出。请以管理员身份打开 FolderNote 设置并再次保存，或注销后重新登录。")};
  if (!helperEnabled()) { configureStartup({}, false); return; }
  const auto context = desktop_groups::desktopLaunchContext();
  configureStartup(exe, true);
  if (!launchHelper(exe, context))
    fn::fail(lang::text(L"选项已保存，但无法启动文件夹后台组件"));
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto window = FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr);
    if (window && GetPropW(window, readyProperty) &&
        desktop_groups::desktopReceiverStatus(window, context) == desktop_groups::DesktopReceiverStatus::Compatible) return;
    Sleep(50);
  }
  fn::fail(lang::text(L"选项已保存，但无法启动文件夹后台组件"), ERROR_TIMEOUT);
}
void ensureHelperRunning() noexcept {
  try {
    if (!helperEnabled() || FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr)) return;
    static std::atomic<ULONGLONG> lastAttempt{0};
    const auto now = GetTickCount64();
    auto previous = lastAttempt.load();
    if (now - previous < 5000 || !lastAttempt.compare_exchange_strong(previous, now)) return;
    const auto context = desktop_groups::desktopLaunchContext();
    if (context.mode == desktop_groups::DesktopLaunchMode::Unavailable) return;
    launchHelper(executable(), context);
  } catch (...) {}
}
namespace {
// The background helper hosts global hooks and third-party Shell/UIA code; a
// crash there used to silently disable middle-click until the user toggled the
// setting. This thin parent restarts it after abnormal exits. It owns no hooks,
// windows or COM state, so it has nothing that can crash with the child.
int superviseHelper(HINSTANCE instance) {
  std::wstring exe;
  try { exe = fn::modulePath().native(); } catch (...) { return 1; }
  struct SupervisorState {
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND window = nullptr;
    ~SupervisorState() {
      if (window && IsWindow(window)) DestroyWindow(window);
      if (stopEvent) CloseHandle(stopEvent);
    }
  } state;
  if (!state.stopEvent) return 1;
  WNDCLASSW cls{}; cls.lpfnWndProc = supervisorProc; cls.hInstance = instance; cls.lpszClassName = supervisorClass;
  RegisterClassW(&cls);
  state.window = CreateWindowExW(0, supervisorClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (state.window) SetWindowLongPtrW(state.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.stopEvent));

  guard::log(L"后台组件守护进程启动");
  DWORD windowStart = GetTickCount();
  int restarts = 0;
  for (;;) {
    if (!helperEnabled() || WaitForSingleObject(state.stopEvent, 0) == WAIT_OBJECT_0) return 0;
    auto command = L"\"" + exe + L"\" --quick-helper --supervised";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
      guard::log(L"无法启动后台组件，错误 " + std::to_wstring(GetLastError()));
      return 1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = waitWithPump(state.stopEvent, process.hProcess, INFINITE);
    if (wait == WAIT_OBJECT_0) {
      if (HWND childWindow = FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr))
        PostMessageW(childWindow, WM_CLOSE, 0, 0);
      WaitForSingleObject(process.hProcess, 2000);
      CloseHandle(process.hProcess);
      return 0;
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code == 0 || !helperEnabled() || WaitForSingleObject(state.stopEvent, 0) == WAIT_OBJECT_0) return 0;
    // Another helper instance (e.g. started by Settings) already took over.
    if (FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr)) return 0;
    const DWORD now = GetTickCount();
    if (now - windowStart > supervisorRestartWindow) { windowStart = now; restarts = 0; }
    wchar_t exitText[16]{};
    swprintf_s(exitText, L"0x%08lX", code);
    if (++restarts > supervisorRestartLimit) {
      guard::log(std::wstring(L"后台组件反复异常退出（") + exitText + L"），已停止自动重启");
      return static_cast<int>(code);
    }
    guard::log(std::wstring(L"后台组件异常退出（") + exitText + L"），第 " + std::to_wstring(restarts) + L" 次自动重启");
    if (waitWithPump(state.stopEvent, nullptr, guard::crashExitCode(code) ? 1000 : 3000) == WAIT_OBJECT_0) return 0;
    if (!helperEnabled() || WaitForSingleObject(state.stopEvent, 0) == WAIT_OBJECT_0 ||
        FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr)) return 0;
    try {
      if (_wcsicmp(executable().c_str(), exe.c_str()) != 0) {
        guard::log(L"检测到后台组件版本已变更，旧守护进程退出");
        return 0;
      }
    } catch (...) { return 0; }
  }
}
}
int helper(HINSTANCE instance, bool delegated, bool supervised) {
  if (!helperEnabled()) return 0;
  // Explorer and its router must have matching integrity levels. Inheriting
  // an administrator settings/installer token silently rejects every frame.
  const auto context = desktop_groups::desktopLaunchContext();
  if (context.mode != desktop_groups::DesktopLaunchMode::Direct) {
    if (delegated || supervised || context.mode != desktop_groups::DesktopLaunchMode::Explorer) return 1;
    const auto running = FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr);
    if (running && desktop_groups::desktopReceiverStatus(running, context) == desktop_groups::DesktopReceiverStatus::Elevated &&
        !stopHelper(false)) return 1;
    return launchHelper(fn::modulePath().native(), context) ? 0 : 1;
  }
  if (!supervised) return superviseHelper(instance);
  guard::installCrashHandler(L"helper");
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FolderNote.QuickFolders.Helper.v1");
  if (!mutex) return 1;
  // A settings/upgrade restart may outlive stopHelper's short UI wait. Wait in
  // this new background process, so it can take over once the old owner exits.
  if (!acquireHelperMutex(mutex)) { CloseHandle(mutex); return 0; }
  if (!helperEnabled()) { ReleaseMutex(mutex); CloseHandle(mutex); return 0; }
  if (FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr)) { ReleaseMutex(mutex); CloseHandle(mutex); return 0; }
  WNDCLASSW cls{}; cls.lpfnWndProc = helperProc; cls.hInstance = instance; cls.lpszClassName = helperClass;
  RegisterClassW(&cls);
  receiver = CreateWindowExW(0, helperClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (!receiver) { ReleaseMutex(mutex); CloseHandle(mutex); return 1; }
  const bool quickMode = navigation::quickEnabled(), routeMode = tabs::routing::enabled();
  const bool searchMode = quick_find::external::enabled();
  const bool searchReady = !searchMode || quick_find::external::start();
  const bool observe = quickMode || searchMode;
  const bool router = routeMode && tabs::routing::start(receiver);
  auto event = observe ? SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT) : nullptr;
  auto desktopSwitch = observe ? SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, foregroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT) : nullptr;
  auto shown = observe ? SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, foregroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT) : nullptr;
  if (observe) updateTarget();
  HookStart hook{observe ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr, nullptr, nullptr, searchMode};
  DWORD threadId = 0;
  HANDLE thread = hook.ready ? CreateThread(nullptr, 0, hookThread, &hook, 0, &threadId) : nullptr;
  if (thread) WaitForSingleObject(hook.ready, INFINITE);
  const bool ready = (!observe || (thread && hook.hook)) && searchReady && (!searchMode || hook.keyboard) && (!routeMode || router) &&
                     SetPropW(receiver, readyProperty, reinterpret_cast<HANDLE>(1));
  if (ready) {
    if (quickMode) startMenuThread();
    guard::log(std::wstring(L"后台组件就绪：中键=") + (quickMode ? L"开" : L"关") + L"，标签路由=" + (routeMode ? L"开" : L"关") +
               L"，快捷搜索=" + (searchMode ? L"开" : L"关") + L"，菜单=" + (menuThread ? L"常驻线程" : L"独立进程"));
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  } else {
    guard::log(std::wstring(L"后台组件启动失败：钩子=") + (hook.hook ? L"成功" : L"失败") + L"，键盘钩子=" +
               (hook.keyboard ? L"成功" : L"失败") + L"，搜索=" + (searchReady ? L"就绪" : L"失败") +
               L"，路由=" + (!routeMode || router ? L"就绪" : L"失败"));
  }
  eligible = nullptr;
  signalMenuThreadQuit();
  quick_find::external::stop();
  tabs::routing::stop();
  if (thread) { PostThreadMessageW(threadId, WM_QUIT, 0, 0); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
  if (hook.ready) CloseHandle(hook.ready);
  if (event) UnhookWinEvent(event);
  if (desktopSwitch) UnhookWinEvent(desktopSwitch);
  if (shown) UnhookWinEvent(shown);
  watchDialog(nullptr);
  if (IsWindow(receiver)) DestroyWindow(receiver);
  receiver = nullptr;
  ReleaseMutex(mutex); CloseHandle(mutex);
  joinMenuThread();
  return ready ? 0 : 1;
}
}
