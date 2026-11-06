#include "quick_folders.h"
#include "navigation.h"
#include "core.h"
#include <atomic>
#include <memory>
#include <new>
#include <shlobj.h>
#include <vector>
namespace quick {
namespace {
constexpr wchar_t helperClass[] = L"FolderNote.QuickFolders.Helper.v1";
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t runName[] = L"FolderNote.QuickFolders";
constexpr UINT showMessage = WM_APP + 0x511, foregroundMessage = WM_APP + 0x512;
std::atomic<HWND> eligible{nullptr};
HWND receiver = nullptr;
struct MenuRequest { HWND target; DWORD process; POINT point; };
struct MiddleGesture {
  HWND target = nullptr;
  DWORD process = 0;
  POINT down{};
  bool swallowed = false, dragged = false, suppressUp = false;
} middle;
bool classIs(HWND window, const wchar_t *name) {
  wchar_t cls[128]{}; GetClassNameW(window, cls, ARRAYSIZE(cls));
  return wcscmp(cls, name) == 0;
}
bool folderTabsSurface(HWND window, HWND frame) {
  // The in-process tab band owns its middle-button gestures (tab close).
  // Check only class names/parents here; never call Shell or UIA in the hook.
  for (HWND current = window; current && current != frame; current = GetParent(current))
    if (classIs(current, L"FolderNote.ExplorerTabs.Band.v1")) return true;
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
  if (!window || !pid || (expectedProcess && pid != expectedProcess) || !navigation::quickEnabled() ||
      !navigation::dialogQuickEnabled() || (!isExplorer(window) && !isFileDialog(window))) return false;
  const auto targetIntegrity = integrity(pid), ownIntegrity = integrity(GetCurrentProcessId());
  return targetIntegrity != 0xffffffff && ownIntegrity != 0xffffffff && targetIntegrity <= ownIntegrity;
}
void updateTarget() {
  HWND window = GetForegroundWindow();
  eligible.store(eligibleTarget(window) ? window : nullptr);
}
void CALLBACK foregroundEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD) {
  if (event == EVENT_OBJECT_SHOW && (!window || GetAncestor(window, GA_ROOT) != GetForegroundWindow() ||
      !classIs(GetForegroundWindow(), L"#32770"))) return;
  if (receiver) PostMessageW(receiver, foregroundMessage, 0, 0);
}
LRESULT CALLBACK mouseHook(int code, WPARAM message, LPARAM parameter) {
  if (code == HC_ACTION && (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK ||
                            message == WM_MBUTTONUP || (message == WM_MOUSEMOVE && middle.target))) {
    const auto *mouse = reinterpret_cast<MSLLHOOKSTRUCT *>(parameter);
    if (message == WM_MBUTTONUP && middle.suppressUp) { middle = {}; return 1; }
    const HWND under = WindowFromPoint(mouse->pt), root = GetAncestor(under, GA_ROOT);
    if (folderTabsSurface(under, root)) {
      // Old loaded bands close on bare UP. Do not deliver an orphan release
      // after a gesture started in the file list, including MOVE into the band.
      const bool fromFileList = middle.target != nullptr;
      middle = {};
      if (fromFileList && message == WM_MBUTTONUP) return 1;
      if (fromFileList && message == WM_MOUSEMOVE) middle.suppressUp = true;
      return CallNextHookEx(nullptr, code, message, parameter);
    }
    const bool modified = ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU) |
                           GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    HWND foreground = GetForegroundWindow();
    if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) {
      middle = {};
      // These cheap checks run on the hook thread. Process identity, dialog
      // contents and integrity are checked on the helper thread before launch.
      if (!modified && foreground && root == foreground &&
          (classIs(foreground, L"CabinetWClass") || classIs(foreground, L"ExploreWClass") || classIs(foreground, L"#32770"))) {
        middle.target = foreground; middle.down = mouse->pt;
        GetWindowThreadProcessId(foreground, &middle.process);
        middle.swallowed = eligible.load() == foreground;
      }
      if (middle.swallowed) return 1;
    } else if (middle.target) {
      middle.dragged = middle.dragged || modified ||
          abs(mouse->pt.x - middle.down.x) > GetSystemMetrics(SM_CXDRAG) ||
          abs(mouse->pt.y - middle.down.y) > GetSystemMetrics(SM_CYDRAG);
      if (message == WM_MBUTTONUP) {
        const auto completed = middle; middle = {};
        DWORD process = 0; GetWindowThreadProcessId(foreground, &process);
        if (!completed.dragged && foreground == completed.target && process == completed.process &&
            root == completed.target) {
          auto request = new (std::nothrow) MenuRequest{completed.target, completed.process, mouse->pt};
          if (request && !PostMessageW(receiver, showMessage, 0, reinterpret_cast<LPARAM>(request))) delete request;
        }
        // Unknown targets retain native input while awaiting full validation.
        if (completed.swallowed) return 1;
      }
    }
  }
  return CallNextHookEx(nullptr, code, message, parameter);
}
struct HookStart { HANDLE ready; HHOOK hook = nullptr; };
bool acquireHelperMutex(HANDLE mutex, DWORD timeout = 5000) {
  const auto acquired = WaitForSingleObject(mutex, timeout);
  return acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED;
}
DWORD WINAPI hookThread(void *argument) {
  auto *state = static_cast<HookStart *>(argument);
  MSG msg; PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  state->hook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
  SetEvent(state->ready);
  if (!state->hook) return 1;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  UnhookWindowsHookEx(state->hook);
  return 0;
}
LRESULT CALLBACK helperProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
  if (message == WM_DESTROY) {
    MSG pending{};
    while (PeekMessageW(&pending, window, showMessage, showMessage, PM_REMOVE))
      delete reinterpret_cast<MenuRequest *>(pending.lParam);
    PostQuitMessage(0); return 0;
  }
  try {
    if (message == foregroundMessage) { updateTarget(); return 0; }
    if (message == showMessage) {
      std::unique_ptr<MenuRequest> request(reinterpret_cast<MenuRequest *>(l));
      if (request && request->target == GetForegroundWindow() && eligibleTarget(request->target, request->process) &&
          request->target == GetForegroundWindow()) {
        eligible.store(request->target);
        launchMenu(request->target, nullptr, request->point);
      }
      return 0;
    }
  } catch (...) { }
  return DefWindowProcW(window, message, w, l);
}
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
  return found;
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
      if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) fn::fail(L"无法移除快速定位启动项", error);
    } else if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) fn::fail(L"无法更新快速定位启动项", error);
    return;
  }
  HKEY key = nullptr;
  auto command = L"\"" + exe + L"\" --quick-helper";
  auto error = RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (!error) { error = RegSetValueExW(key, runName, 0, REG_SZ,
      reinterpret_cast<const BYTE *>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))); RegCloseKey(key); }
  if (error) fn::fail(L"选项已保存，但无法启用快速定位启动项", error);
}
void stopHelper(bool removeStartup) {
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
  HWND window = FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr);
  if (window) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(window, WM_CLOSE, 0, 0);
    if (process) { WaitForSingleObject(process, 2000); CloseHandle(process); }
  }
}
void syncHelper(const std::wstring &exe) {
  stopHelper();
  if (!navigation::quickEnabled() || !navigation::dialogQuickEnabled()) return;
  auto command = L"\"" + exe + L"\" --quick-helper";
  configureStartup(exe, true);
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    fn::fail(L"选项已保存，但无法启动文件对话框组件");
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
int helper(HINSTANCE instance) {
  if (!navigation::quickEnabled() || !navigation::dialogQuickEnabled()) return 0;
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FolderNote.QuickFolders.Helper.v1");
  if (!mutex) return 1;
  // A settings/upgrade restart may outlive stopHelper's short UI wait. Wait in
  // this new background process, so it can take over once the old owner exits.
  if (!acquireHelperMutex(mutex)) { CloseHandle(mutex); return 0; }
  if (!navigation::quickEnabled() || !navigation::dialogQuickEnabled()) { ReleaseMutex(mutex); CloseHandle(mutex); return 0; }
  if (FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr)) { ReleaseMutex(mutex); CloseHandle(mutex); return 0; }
  WNDCLASSW cls{}; cls.lpfnWndProc = helperProc; cls.hInstance = instance; cls.lpszClassName = helperClass;
  RegisterClassW(&cls);
  receiver = CreateWindowExW(0, helperClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (!receiver) { ReleaseMutex(mutex); CloseHandle(mutex); return 1; }
  auto event = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
  auto shown = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, foregroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
  updateTarget();
  HookStart hook{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  DWORD threadId = 0;
  HANDLE thread = hook.ready ? CreateThread(nullptr, 0, hookThread, &hook, 0, &threadId) : nullptr;
  if (thread) WaitForSingleObject(hook.ready, INFINITE);
  if (thread && hook.hook) {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  eligible = nullptr;
  if (thread) { PostThreadMessageW(threadId, WM_QUIT, 0, 0); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
  if (hook.ready) CloseHandle(hook.ready);
  if (event) UnhookWinEvent(event);
  if (shown) UnhookWinEvent(shown);
  if (IsWindow(receiver)) DestroyWindow(receiver);
  receiver = nullptr;
  ReleaseMutex(mutex); CloseHandle(mutex);
  return hook.hook ? 0 : 1;
}
}
