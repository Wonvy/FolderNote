#include "../src/quick_folders.h"
#include "../src/navigation.h"
#include "../src/desktop_groups_process.h"
#include <iostream>
#include <map>
#include <thread>
#include <stdexcept>
#include <utility>

namespace {
HWND simulatedForeground = nullptr, underPointer = nullptr;
HWND simulatedShell = nullptr, foreignProcessWindow = nullptr;
DWORD simulatedOwnIntegrity = 0;
std::map<HWND, DWORD> simulatedWindowPids;
std::map<DWORD, DWORD> simulatedIntegrityByPid;
constexpr ULONG_PTR fakeIntegrityBase = 0x71000000;
int modifier = 0, extraModifier = 0, checks = 0;
desktop_groups::DesktopLaunchContext helperContext{desktop_groups::DesktopLaunchMode::Direct, 0, 8192};
bool launchSucceeds = true;
int directLaunches = 0, shellLaunches = 0;
std::wstring launchedExe, launchedArguments;
std::vector<std::pair<HWND, POINT>> launches;
HWND fakeForeground() { return simulatedForeground; }
HWND fakeWindowFromPoint(POINT) { return underPointer; }
HWND fakeShellWindow() { return simulatedShell; }
DWORD fakeWindowProcess(HWND window, DWORD *pid) {
  const auto thread = GetWindowThreadProcessId(window, pid);
  if (pid && window) {
    if (const auto it = simulatedWindowPids.find(window); it != simulatedWindowPids.end()) *pid = it->second;
    else if (window == foreignProcessWindow) *pid = GetCurrentProcessId() + 1;
  }
  return thread;
}
HANDLE fakeOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
  if (simulatedIntegrityByPid.contains(pid) || (simulatedOwnIntegrity && pid == GetCurrentProcessId()))
    return reinterpret_cast<HANDLE>(fakeIntegrityBase | (pid & 0xffff));
  return OpenProcess(access, inherit, pid);
}
BOOL fakeOpenProcessToken(HANDLE process, DWORD access, PHANDLE token) {
  const auto raw = reinterpret_cast<ULONG_PTR>(process);
  if (process == GetCurrentProcess() && simulatedOwnIntegrity) {
    if (token) *token = reinterpret_cast<HANDLE>(fakeIntegrityBase | (GetCurrentProcessId() & 0xffff));
    return TRUE;
  }
  if ((raw & ~ULONG_PTR(0xffff)) == fakeIntegrityBase) {
    if (token) *token = process;
    return TRUE;
  }
  return OpenProcessToken(process, access, token);
}
BOOL fakeGetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD len, PDWORD retLen) {
  const auto raw = reinterpret_cast<ULONG_PTR>(token);
  if (cls == TokenIntegrityLevel && (raw & ~ULONG_PTR(0xffff)) == fakeIntegrityBase) {
    const DWORD pid = static_cast<DWORD>(raw & 0xffff);
    DWORD level = SECURITY_MANDATORY_MEDIUM_RID;
    if (pid == (GetCurrentProcessId() & 0xffff) && simulatedOwnIntegrity) level = simulatedOwnIntegrity;
    else if (const auto it = simulatedIntegrityByPid.find(pid); it != simulatedIntegrityByPid.end()) level = it->second;
    static SID sid{SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, {SECURITY_MANDATORY_MEDIUM_RID}};
    sid.SubAuthority[0] = level;
    const DWORD needed = sizeof(TOKEN_MANDATORY_LABEL);
    if (retLen) *retLen = needed;
    if (!info || len < needed) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    auto *label = static_cast<TOKEN_MANDATORY_LABEL *>(info);
    label->Label.Sid = &sid;
    label->Label.Attributes = SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED;
    return TRUE;
  }
  return GetTokenInformation(token, cls, info, len, retLen);
}
BOOL fakeCloseHandle(HANDLE handle) {
  const auto raw = reinterpret_cast<ULONG_PTR>(handle);
  if ((raw & ~ULONG_PTR(0xffff)) == fakeIntegrityBase) return TRUE;
  return CloseHandle(handle);
}
SHORT fakeKeys(int key) { return key == modifier || key == extraModifier ? SHORT(-32768) : 0; }
LRESULT fakeNextHook(HHOOK, int, WPARAM, LPARAM) { return 73; }
void check(bool condition, const char *reason) { ++checks; if (!condition) throw std::runtime_error(reason); }
BOOL fakeCreateProcess(LPCWSTR exe, LPWSTR arguments, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                       BOOL, DWORD flags, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION process) {
  ++directLaunches; launchedExe = exe; launchedArguments = arguments;
  check(flags == CREATE_NO_WINDOW, "ordinary helper startup stays in the background");
  if (!launchSucceeds) return FALSE;
  process->hProcess = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  process->hThread = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  return process->hProcess && process->hThread;
}
HWND fakeFindWindowEx(HWND parent, HWND after, LPCWSTR name, LPCWSTR title) {
  if (parent == HWND_MESSAGE && name && !wcscmp(name, L"FolderNote.QuickFolders.Helper.v1")) return nullptr;
  return FindWindowExW(parent, after, name, title);
}
}
namespace desktop_groups {
DesktopLaunchContext fakeLaunchContext() { return helperContext; }
bool fakeExecuteFromDesktop(const std::wstring &exe, const wchar_t *arguments, const DesktopLaunchContext &) {
  ++shellLaunches; launchedExe = exe; launchedArguments = arguments; return launchSucceeds;
}
}
namespace quick {
void captureMenu(HWND target, HWND, POINT point) { launches.push_back({target, point}); }
}
// Run production hook/receiver logic on disposable hidden windows. No input
// is injected, global hook installed, menu process started, or user setting read.
#define GetForegroundWindow fakeForeground
#define WindowFromPoint fakeWindowFromPoint
#define GetShellWindow fakeShellWindow
#define GetWindowThreadProcessId fakeWindowProcess
#define OpenProcess fakeOpenProcess
#define OpenProcessToken fakeOpenProcessToken
#define GetTokenInformation fakeGetTokenInformation
#define CloseHandle fakeCloseHandle
#define GetAsyncKeyState fakeKeys
#define CallNextHookEx fakeNextHook
#define launchMenu captureMenu
#define desktopLaunchContext fakeLaunchContext
#define executeFromDesktop fakeExecuteFromDesktop
#define CreateProcessW fakeCreateProcess
#define FindWindowExW fakeFindWindowEx
#include "../src/quick_runtime.cpp"
#undef FindWindowExW
#undef CreateProcessW
#undef executeFromDesktop
#undef desktopLaunchContext
#undef launchMenu
#undef CallNextHookEx
#undef GetAsyncKeyState
#undef WindowFromPoint
#undef GetShellWindow
#undef CloseHandle
#undef GetTokenInformation
#undef OpenProcessToken
#undef OpenProcess
#undef GetWindowThreadProcessId
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
static void enable(bool value, bool withDialogs = true) {
  DWORD enabled = value ? 1 : 0, dialogs = withDialogs ? 1 : 0;
  check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled)) == ERROR_SUCCESS &&
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersInDialogs", REG_DWORD, &dialogs, sizeof(dialogs)) == ERROR_SUCCESS,
        "write isolated runtime switches");
}
static void helperLifetimeTests() {
  auto routing = [](DWORD tabs, DWORD prefer) {
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerTabsEnabled", REG_DWORD, &tabs, sizeof(tabs)) == ERROR_SUCCESS &&
          RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerOpenInNewTab", REG_DWORD, &prefer, sizeof(prefer)) == ERROR_SUCCESS,
          "write isolated routing switches");
  };
  enable(false); routing(0, 0);
  check(!quick::helperEnabled(), "disabled features do not require the helper");
  quick_find::external::saveEnabled(true);
  check(quick::helperEnabled(), "pinyin locator independently keeps the helper active");
  quick_find::external::saveEnabled(false);
  check(!quick::helperEnabled(), "turning off the final feature releases the helper");
  enable(true);
  check(quick::helperEnabled(), "middle menu alone keeps the helper active");
  enable(false); routing(1, 1);
  check(quick::helperEnabled() == navigation::tabsSupported(), "supported routing keeps helper active without middle menu");
  check(!quick::eligibleTarget(GetDesktopWindow()), "routing alone never enables the middle menu");
  routing(0, 1);
  check(!quick::helperEnabled(), "routing preference requires enabled custom tabs");
  routing(1, 0);
  check(!quick::helperEnabled(), "custom tabs without routing do not require helper");
  routing(0, 0); enable(true);
}
static void helperLaunchTests() {
  using desktop_groups::DesktopLaunchMode;
  const std::wstring exe = L"C:\\Program Files\\FolderNote\\FolderNote.exe";
  directLaunches = shellLaunches = 0;
  check(quick::launchHelper(exe, {DesktopLaunchMode::Direct, 1, 8192}) &&
        directLaunches == 1 && shellLaunches == 0 && launchedExe == exe &&
        launchedArguments == L"\"" + exe + L"\" --quick-helper", "ordinary settings launch one normal helper");
  check(quick::launchHelper(exe, {DesktopLaunchMode::Explorer, 1, 8192}) &&
        directLaunches == 1 && shellLaunches == 1 && launchedExe == exe &&
        launchedArguments == L"--quick-helper --from-desktop-shell",
        "administrator settings delegate helper launch to Explorer instead of inheriting elevation");
  check(!quick::launchHelper(exe, {}) && directLaunches == 1 && shellLaunches == 1,
        "unavailable desktop context never launches an elevated fallback");
  launchSucceeds = false;
  check(!quick::launchHelper(exe, {DesktopLaunchMode::Explorer, 1, 8192}) && directLaunches == 1,
        "failed Explorer delegation cannot fall back to an elevated router");
  launchSucceeds = true;
  helperContext.mode = DesktopLaunchMode::Explorer;
  const auto before = shellLaunches;
  check(quick::helper(GetModuleHandleW(nullptr)) == 0 && shellLaunches == before + 1 && directLaunches == 1,
        "directly starting the helper as administrator relaunches it through Explorer before taking the singleton");
  check(quick::helper(GetModuleHandleW(nullptr), true) == 1 && shellLaunches == before + 1,
        "a delegated process that is still elevated cannot recurse or own the router");
  helperContext.mode = DesktopLaunchMode::Unavailable;
  check(quick::helper(GetModuleHandleW(nullptr)) == 1 && shellLaunches == before + 1,
        "unverifiable desktop identity cannot start the helper");
  helperContext.mode = DesktopLaunchMode::Direct;
}
static LRESULT mouse(UINT message, POINT point = {20, 20}) {
  MSLLHOOKSTRUCT data{}; data.pt = point;
  return quick::mouseHook(HC_ACTION, message, reinterpret_cast<LPARAM>(&data));
}
static void hookHealthTests() {
  using quick::mouseHookNeedsRefresh;
  int missed = 0;
  check(!mouseHookNeedsRefresh(false, false, 100, 100, missed) && missed == 0,
        "an idle pointer never forces a mouse rehook");
  check(!mouseHookNeedsRefresh(false, true, 100, 100, missed) && missed == 1,
        "a single missed pointer move waits one more interval before rehooking");
  check(!mouseHookNeedsRefresh(false, true, 100, 250, missed) && missed == 0,
        "a moving pointer seen by the hook stays healthy and resets missed count");
  check(!mouseHookNeedsRefresh(false, true, 250, 250, missed) && missed == 1 &&
        mouseHookNeedsRefresh(false, true, 250, 250, missed) && missed == 0,
        "two consecutive uncaptured pointer moves missed by the hook trigger a rehook");
  missed = 1;
  check(!mouseHookNeedsRefresh(true, true, 250, 250, missed) && missed == 0,
        "swallowed pointer warps during Deskflow remote capture never steal hook priority");
  const DWORD before = quick::lastMouseCallback.load();
  Sleep(20);
  mouse(WM_MOUSEMOVE);
  check(quick::lastMouseCallback.load() != before, "every mouse callback refreshes the health timestamp");
  const int slop = quick::middleClickSlopX();
  check(slop >= quick::middleClickMinimumSlop && quick::middleClickWithin({0, 0}, {slop, 0}) &&
        !quick::middleClickWithin({0, 0}, {slop + 1, 0}) && quick::middleClickWithin({0, 0}, {0, -quick::middleClickSlopY()}),
        "all middle-click paths share one inclusive wheel-press tolerance");
}
static void drain();
static void remoteKvmCaptureTests(HWND target) {
  check(quick::detail::kvmClassName(L"DeskflowDesk") && quick::detail::kvmClassName(L"SynergyDesk") &&
        quick::detail::kvmClassName(L"BarrierDesk") && quick::detail::kvmClassName(L"InputLeapDesk") &&
        !quick::detail::kvmClassName(L"CabinetWClass"),
        "recognize software-KVM capture window class prefixes");
  check(quick::detail::kvmExecutableName(L"deskflow-core.exe") &&
        quick::detail::kvmExecutableName(L"deskflow-client.exe") &&
        quick::detail::kvmExecutableName(L"input-leapc.exe") &&
        !quick::detail::kvmExecutableName(L"explorer.exe"),
        "recognize software-KVM executable names");

  const auto instance = GetModuleHandleW(nullptr);
  for (auto name : {L"DeskflowDesk", L"DeskflowGui", L"UnidentifiedServiceTrap"}) {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = name;
    check(RegisterClassW(&cls) != 0, "register disposable KVM test window class");
  }

  HWND gui = CreateWindowExW(0, L"DeskflowGui", L"Deskflow Settings", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             80, 80, 360, 240, nullptr, nullptr, instance, nullptr);
  HWND desk = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, L"DeskflowDesk", L"",
                              WS_POPUP | WS_DISABLED, 200, 200, 1, 1, nullptr, nullptr, instance, nullptr);
  HWND serviceTrap = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, L"UnidentifiedServiceTrap", L"",
                                     WS_POPUP | WS_DISABLED, 240, 240, 1, 1, nullptr, nullptr, instance, nullptr);
  HWND cornerOverlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, L"UnidentifiedServiceTrap", L"",
                                       WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
  check(gui && desk && serviceTrap && cornerOverlay, "create disposable KVM capture and GUI windows");

  check(!quick::detail::isKvmCaptureWindow(gui, {100, 100}, true) && !quick::remoteInputCaptured(),
        "visible Deskflow settings GUI window never blocks local Quick Search or middle-click");
  check(!quick::detail::isKvmCaptureWindow(desk, {200, 200}, true),
        "hidden DeskflowDesk window while controlling local PC does not report remote capture");

  ShowWindow(serviceTrap, SW_SHOWNA);
  ShowWindow(cornerOverlay, SW_SHOWNA);
  check(quick::detail::isKvmCaptureWindow(serviceTrap, {240, 240}, true) &&
        !quick::detail::isKvmCaptureWindow(serviceTrap, {500, 500}, true) &&
        !quick::detail::isKvmCaptureWindow(cornerOverlay, {0, 0}, true),
        "service-mode 1x1 cursor trap matches only away from (0,0) when cursor is parked on it");
  ShowWindow(serviceTrap, SW_HIDE);
  ShowWindow(cornerOverlay, SW_HIDE);

  ShowWindow(desk, SW_SHOWNA);
  check(quick::detail::isKvmCaptureWindow(desk, {200, 200}, true) && quick::remoteInputCaptured(),
        "visible DeskflowDesk screen-leave trap activates remote capture guard");

  simulatedForeground = underPointer = target;
  quick::eligible = target;
  const auto before = launches.size();
  check(mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
        "middle-click passes straight through while Deskflow controls remote PC");
  drain();
  check(launches.size() == before, "remote middle-click never queues a local FolderNote popup");

  ShowWindow(desk, SW_HIDE);
  check(!quick::remoteInputCaptured(), "returning cursor to local PC clears remote capture guard");
  check(mouse(WM_MBUTTONDOWN) == 1 && mouse(WM_MBUTTONUP) == 1,
        "local middle-click interception resumes immediately after returning from remote PC");
  drain();
  check(launches.size() == before + 1 && launches.back().first == target,
        "local middle-click opens FolderNote popup normally once back on local PC");

  simulatedForeground = underPointer = nullptr;
  quick::eligible = nullptr;
  for (HWND w : {gui, desk, serviceTrap, cornerOverlay}) DestroyWindow(w);
  for (auto name : {L"DeskflowDesk", L"DeskflowGui", L"UnidentifiedServiceTrap"}) UnregisterClassW(name, instance);
}
static void drain() {
  MSG message{};
  while (PeekMessageW(&message, quick::receiver, quick::showMessage, quick::showMessage, PM_REMOVE))
    quick::helperProc(message.hwnd, message.message, message.wParam, message.lParam);
}
static void retiredTransferGestures(HWND shell, HWND surface) {
  enable(true, false); simulatedForeground = shell; underPointer = surface;
  DWORD oldEnabled = 1;
  for (auto name : {L"QuickFoldersCopy", L"QuickFoldersMove"})
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", name, REG_DWORD, &oldEnabled, sizeof(oldEnabled)) == ERROR_SUCCESS,
          "seed obsolete transfer switches from an upgrade");
  quick::updateTarget();
  const auto before = launches.size();
  for (int key : {VK_CONTROL, VK_SHIFT}) {
    modifier = key;
    check(mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73, "retired transfer shortcuts preserve native input");
  }
  drain(); check(launches.size() == before, "old transfer settings cannot launch modifier menus");
  modifier = 0;
  check(mouse(WM_MBUTTONDOWN) == 1 && mouse(WM_MBUTTONUP) == 1, "plain middle click remains the single menu entry");
  drain(); check(launches.size() == before + 1, "plain middle opens one menu with obsolete settings present");
}
static void desktopTests(HWND dialog) {
  const auto instance = GetModuleHandleW(nullptr);
  for (auto name : {L"Progman", L"WorkerW", L"FolderNote.DesktopGroups.Panel"}) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = DefWindowProcW; cls.lpszClassName = name;
    check(RegisterClassW(&cls) != 0, "register isolated desktop host class");
  }
  auto host = [&](const wchar_t *name) {
    HWND window = CreateWindowW(name, L"Hidden desktop fixture", WS_OVERLAPPED, 0, 0, 300, 200, nullptr, nullptr, instance, nullptr);
    check(window != nullptr, "create hidden desktop host"); return window;
  };
  auto view = [&](HWND parent) {
    HWND window = CreateWindowW(L"SHELLDLL_DefView", L"", WS_CHILD, 0, 0, 200, 100, parent, nullptr, instance, nullptr);
    check(window != nullptr, "create hidden desktop view"); return window;
  };
  HWND shell = host(L"Progman"), worker = host(L"WorkerW"), wallpaper = host(L"WorkerW");
  HWND shellView = view(shell), workerView = view(worker), ordinary = host(L"STATIC");
  simulatedShell = shell;
  retiredTransferGestures(shell, shellView);
  const auto box = CreateWindowW(L"FolderNote.DesktopGroups.Panel", L"", WS_CHILD,
      0, 0, 100, 100, shellView, nullptr, instance, nullptr);
  const auto content = CreateWindowW(L"STATIC", L"", WS_CHILD,
      0, 0, 100, 100, box, nullptr, instance, nullptr);
  check(box && content, "create desktop-hosted box input fixture");
  simulatedForeground = shell; underPointer = content; quick::eligible = shell;
  const auto beforeBoxClick = launches.size();
  check(mouse(WM_MBUTTONDOWN) == 1 && mouse(WM_MBUTTONUP) == 1, "desktop box swallows middle click for quick menu");
  drain(); check(launches.size() == beforeBoxClick + 1 && launches.back().first == shell,
                 "desktop box opens desktop middle menu when desktop is foreground");
  simulatedForeground = ordinary; underPointer = content;
  check(mouse(WM_MBUTTONDOWN) == 1 && mouse(WM_MBUTTONUP) == 1,
        "desktop box swallows middle click even while another window is foreground");
  drain(); check(launches.size() == beforeBoxClick + 2 && launches.back().first == shell,
                 "desktop box opens desktop middle menu without requiring focus activation");
  simulatedForeground = shell;
  DestroyWindow(box);
  check(quick::isDesktop(shell) && quick::isDesktop(worker), "recognize both shell and WorkerW desktop hosts");
  check(!quick::isDesktop(wallpaper) && !quick::isDesktop(ordinary) && !quick::isDesktop(shellView),
        "wallpaper-only hosts, applications and non-root children are excluded");
  enable(false, false);
  auto search = quick_find::Settings{}; search.explorer = true;
  check(quick_find::saveSettings(search) == ERROR_SUCCESS, "enable desktop/Explorer search without middle menus");
  check(quick::searchTarget(shell) == shell && quick::searchTarget(worker) == worker,
        "quick search admits both native desktop hosts independently of middle menu and dialog options");
  for (auto rejected : {wallpaper, ordinary, shellView, dialog, HWND{}})
    check(!quick::searchTarget(rejected), "search excludes wallpaper, unrelated apps, child views and disabled dialogs");
  foreignProcessWindow = worker;
  check(!quick::isDesktop(worker), "desktop class and view cannot impersonate the Shell process");
  check(!quick::searchTarget(worker), "search rejects desktop class impersonation");
  foreignProcessWindow = nullptr;
  search.explorer = false; search.dialogs = true;
  check(quick_find::saveSettings(search) == ERROR_SUCCESS && !quick::searchTarget(shell) && !quick::searchTarget(worker),
        "dialog-only search does not intercept desktop typing");
  quick_find::external::saveEnabled(false);
  simulatedShell = nullptr; check(!quick::isDesktop(shell), "no Shell window means no desktop target"); simulatedShell = shell;
  for (bool dialogs : {false, true}) {
    enable(true, dialogs);
    for (auto [target, surface] : {std::pair{shell, shellView}, std::pair{worker, workerView}}) {
      for (bool cached : {false, true}) {
        simulatedForeground = target; underPointer = surface; quick::eligible = cached ? target : nullptr;
        const auto before = launches.size();
        check(mouse(WM_MBUTTONDOWN) == (cached ? 1 : 73) && mouse(WM_MBUTTONUP) == (cached ? 1 : 73),
              "desktop press and release preserve matching input handling");
        drain(); check(launches.size() == before + 1 && launches.back().first == target,
                       "desktop opens exactly one menu with dialog support on or off");
      }
    }
    simulatedForeground = shell; underPointer = workerView; quick::eligible = shell;
    const auto before = launches.size();
    mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); simulatedForeground = worker; drain();
    check(launches.size() == before + 1 && launches.back().first == shell,
          "split WorkerW view uses foreground Progman and tolerates Progman/WorkerW focus handoff");
    simulatedForeground = ordinary; underPointer = workerView; quick::eligible = nullptr;
    mouse(WM_MBUTTONDOWN); simulatedForeground = shell; mouse(WM_MBUTTONUP); drain();
    check(launches.size() == before + 2, "first desktop click can activate the desktop from another app");
    for (HWND rejected : {wallpaper, ordinary}) {
      simulatedForeground = rejected; underPointer = rejected; quick::eligible = nullptr;
      mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); drain();
      check(launches.size() == before + 2, "non-desktop surfaces never launch a popup");
    }
    simulatedForeground = shell; underPointer = wallpaper;
    mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); drain();
    check(launches.size() == before + 2, "foreground Shell does not admit a wallpaper-only click surface");
  }
  enable(true, false);
  simulatedForeground = dialog; underPointer = dialog; quick::updateTarget();
  const auto before = launches.size();
  check(quick::eligible.load() == nullptr && mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
        "disabled dialog support retains native middle input while desktop helper is active");
  drain(); check(launches.size() == before, "disabled dialog support cannot queue a usable popup");
  simulatedForeground = shell; underPointer = shellView; quick::updateTarget();
  check(quick::eligible.load() == shell, "desktop remains eligible with dialog support disabled");
  for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
    modifier = key; mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); modifier = 0;
    mouse(WM_MBUTTONDOWN); modifier = key; mouse(WM_MBUTTONUP); modifier = 0;
  }
  mouse(WM_MBUTTONDOWN); mouse(WM_MOUSEMOVE, {20 + GetSystemMetrics(SM_CXDRAG) + 10, 20}); mouse(WM_MBUTTONUP);
  drain(); check(launches.size() == before, "desktop modifiers and drag gestures do not open menus");
  mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); simulatedForeground = ordinary; drain();
  check(launches.size() == before, "switching away from desktop before dispatch cancels request");
  simulatedForeground = shell;
  mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); enable(false, false); drain();
  check(launches.size() == before, "disabling quick folders cancels queued desktop requests");
  enable(true, false);
  mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); DestroyWindow(shell); drain();
  check(launches.size() == before, "destroyed desktop cannot launch a queued request");
  simulatedForeground = underPointer = simulatedShell = nullptr; quick::eligible = nullptr; quick::middle = {};
  for (HWND window : {worker, wallpaper, ordinary}) DestroyWindow(window);
  for (auto name : {L"Progman", L"WorkerW", L"FolderNote.DesktopGroups.Panel"}) UnregisterClassW(name, instance);
  enable(true);
}
static void legacyDialogTests() {
  // Model only the native Browse-for-Folder structure. The caption is
  // deliberately unrelated: recognition must not depend on UI language.
  WNDCLASSW container{}; container.hInstance = GetModuleHandleW(nullptr);
  container.lpfnWndProc = DefWindowProcW;
  container.lpszClassName = L"SHBrowseForFolder ShellNameSpace Control";
  check(RegisterClassW(&container) != 0, "register disposable legacy namespace host");
  WNDCLASSW treeClass = container; treeClass.lpszClassName = L"SysTreeView32";
  const bool ownsTreeClass = RegisterClassW(&treeClass) != 0;
  check(ownsTreeClass || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "provide disposable tree control class");
  struct Fixture { HWND root, host, tree; };
  std::vector<Fixture> fixtures;
  auto create = [&](bool namespaceHost, bool tree, bool ok, bool cancel, bool properRoot = true, bool nestedTree = true) {
    Fixture item{};
    item.root = CreateWindowW(properRoot ? L"#32770" : L"STATIC", L"Arbitrary localized title", WS_OVERLAPPED,
                              0, 0, 200, 160, nullptr, nullptr, container.hInstance, nullptr);
    check(item.root != nullptr, "create hidden legacy signature fixture");
    if (namespaceHost) item.host = CreateWindowW(container.lpszClassName, L"", WS_CHILD,
                              0, 0, 150, 100, item.root, nullptr, container.hInstance, nullptr);
    if (tree) item.tree = CreateWindowW(treeClass.lpszClassName, L"", WS_CHILD,
                              0, 0, 150, 100, namespaceHost && nestedTree ? item.host : item.root,
                              nullptr, container.hInstance, nullptr);
    auto button = [&](int id) { return CreateWindowW(L"BUTTON", L"Localized button", WS_CHILD,
                              0, 110, 60, 25, item.root, reinterpret_cast<HMENU>(INT_PTR(id)), container.hInstance, nullptr); };
    check((!namespaceHost || item.host) && (!tree || item.tree) && (!ok || button(IDOK)) && (!cancel || button(IDCANCEL)),
          "create hidden native browse signature descendants");
    fixtures.push_back(item); return item;
  };
  const auto legacy = create(true, true, true, true);
  check(quick::isFileDialog(legacy.root), "legacy Browse-for-Folder tree dialog is recognized independently of its title");
  auto classic = [&](bool prompt, bool status, int treeId = 14145) {
    auto item = create(false, true, true, true);
    SetWindowLongPtrW(item.tree, GWLP_ID, treeId);
    auto label = [&](int id) { return CreateWindowW(L"STATIC", L"Localized label", WS_CHILD,
                              0, 0, 150, 20, item.root, reinterpret_cast<HMENU>(INT_PTR(id)), container.hInstance, nullptr); };
    check((!prompt || label(14146)) && (!status || label(14147)), "create classic browse prompt and status controls");
    return item;
  };
  const auto classicValid = classic(true, true);
  check(quick::isFileDialog(classicValid.root), "classic Browse-for-Folder with native control identifiers is recognized");
  for (auto valid : {legacy, classicValid}) {
    for (bool cached : {false, true}) {
      for (int repeat = 0; repeat < 3; ++repeat) {
        simulatedForeground = valid.root; underPointer = valid.tree;
        quick::eligible = cached ? valid.root : nullptr;
        const auto before = launches.size();
        check(mouse(WM_MBUTTONDOWN) == (cached ? 1 : 73) && mouse(WM_MBUTTONUP) == (cached ? 1 : 73),
              "legacy tree preserves matching cached and uncached middle-button handling");
        drain();
        check(launches.size() == before + 1 && launches.back().first == valid.root,
              "each repeated legacy tree middle click opens exactly one menu for its own dialog");
      }
    }
  }
  const auto ordinaryTree = create(false, true, true, true);
  const auto missingTree = create(true, false, true, true);
  const auto missingOk = create(true, true, false, true);
  const auto missingCancel = create(true, true, true, false);
  const auto wrongRoot = create(true, true, true, true, false);
  const auto unrelatedTree = create(true, true, true, true, true, false);
  const auto classicMissingPrompt = classic(false, true), classicMissingStatus = classic(true, false);
  const auto classicWrongTree = classic(true, true, 14144);
  const auto classicNestedTree = classic(true, true);
  HWND unrelatedParent = CreateWindowW(L"STATIC", L"Unrelated control", WS_CHILD, 0, 0, 150, 100,
                                      classicNestedTree.root, nullptr, container.hInstance, nullptr);
  check(unrelatedParent && SetParent(classicNestedTree.tree, unrelatedParent), "nest a classic tree in an unrelated host");
  for (auto rejected : {ordinaryTree, missingTree, missingOk, missingCancel, wrongRoot, unrelatedTree,
                        classicMissingPrompt, classicMissingStatus, classicWrongTree, classicNestedTree}) {
    check(!quick::isFileDialog(rejected.root), "ordinary trees and incomplete or mismatched legacy signatures stay excluded");
    simulatedForeground = rejected.root; underPointer = rejected.tree ? rejected.tree : rejected.root;
    quick::eligible = nullptr;
    const auto before = launches.size();
    check(mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
          "unsupported tree dialogs retain native middle-button input");
    drain(); check(launches.size() == before, "unsupported tree dialog cannot launch a menu after queued validation");
  }
  simulatedForeground = legacy.root; underPointer = legacy.tree; quick::eligible = legacy.root;
  const auto beforeDisable = launches.size();
  mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP); enable(false); drain();
  check(launches.size() == beforeDisable, "disabling quick folders cancels a queued legacy dialog menu"); enable(true);
  mouse(WM_MBUTTONDOWN); mouse(WM_MBUTTONUP);
  DestroyWindow(legacy.root); drain();
  check(launches.size() == beforeDisable, "closing the legacy dialog before dispatch cancels its queued menu");
  simulatedForeground = underPointer = nullptr; quick::eligible = nullptr;
  for (auto item : fixtures) if (IsWindow(item.root)) DestroyWindow(item.root);
  if (ownsTreeClass) UnregisterClassW(treeClass.lpszClassName, treeClass.hInstance);
  UnregisterClassW(container.lpszClassName, container.hInstance);
}
static void elevatedDialogTests(HWND mediumDialog, HWND highDialog, HWND highOrdinary) {
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW desktopCls{}; desktopCls.hInstance = instance; desktopCls.lpfnWndProc = DefWindowProcW; desktopCls.lpszClassName = L"Progman";
  RegisterClassW(&desktopCls);
  WNDCLASSW explorerCls = desktopCls; explorerCls.lpszClassName = L"CabinetWClass";
  RegisterClassW(&explorerCls);
  HWND highDesktop = CreateWindowW(L"Progman", L"High desktop", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, instance, nullptr);
  HWND highExplorer = CreateWindowW(L"CabinetWClass", L"High explorer", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, instance, nullptr);
  check(highDesktop && highExplorer, "create elevated non-dialog fixtures");
  constexpr DWORD mediumPid = 10001, highPid = 10002;
  simulatedOwnIntegrity = SECURITY_MANDATORY_HIGH_RID;
  simulatedIntegrityByPid[mediumPid] = SECURITY_MANDATORY_MEDIUM_RID;
  simulatedIntegrityByPid[highPid] = SECURITY_MANDATORY_HIGH_RID;
  simulatedWindowPids[mediumDialog] = mediumPid;
  simulatedWindowPids[highDialog] = highPid;
  simulatedWindowPids[highOrdinary] = highPid;
  simulatedWindowPids[highDesktop] = highPid;
  simulatedWindowPids[highExplorer] = highPid;
  simulatedShell = highDesktop;
  quick::elevatedCompanion = true;
  enable(true, true);
  navigation::setElevatedDialogQuickEnabled(true);

  const auto before = launches.size();
  simulatedForeground = underPointer = mediumDialog;
  quick::updateTarget();
  check(quick::eligible.load() == nullptr && mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
        "elevated companion never swallows or targets medium-integrity file dialogs");
  drain();
  check(launches.size() == before, "elevated companion never launches a menu for medium-integrity dialogs");

  for (HWND nonDialog : {highDesktop, highExplorer, highOrdinary}) {
    simulatedForeground = underPointer = nonDialog;
    quick::updateTarget();
    check(quick::eligible.load() == nullptr && mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
          "elevated companion ignores high-integrity desktop, explorer, and non-file dialogs");
  }
  drain();
  check(launches.size() == before, "elevated companion only serves file dialogs");

  simulatedForeground = underPointer = highDialog;
  quick::updateTarget();
  check(quick::eligible.load() == highDialog && mouse(WM_MBUTTONDOWN) == 1 && mouse(WM_MBUTTONUP) == 1,
        "elevated companion intercepts middle click on high-integrity file dialog");
  drain();
  check(launches.size() == before + 1 && launches.back().first == highDialog,
        "elevated companion opens quick folder menu for high-integrity file dialog");

  enable(true, false);
  quick::updateTarget();
  check(quick::eligible.load() == nullptr && mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
        "disabling file-dialog support immediately pauses elevated companion");
  enable(true, true);
  navigation::setElevatedDialogQuickEnabled(false);
  quick::updateTarget();
  check(quick::eligible.load() == nullptr && mouse(WM_MBUTTONDOWN) == 73 && mouse(WM_MBUTTONUP) == 73,
        "disabling elevated-dialog option immediately pauses elevated companion");

  drain();
  check(launches.size() == before + 1, "disabled elevated companion never queues menus");
  quick::elevatedCompanion = false;
  quick::eligible = nullptr;
  quick::middle = {};
  simulatedForeground = underPointer = simulatedShell = nullptr;
  simulatedWindowPids.clear();
  simulatedIntegrityByPid.clear();
  simulatedOwnIntegrity = 0;
  DestroyWindow(highDesktop);
  DestroyWindow(highExplorer);
  UnregisterClassW(L"Progman", instance);
  UnregisterClassW(L"CabinetWClass", instance);
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try {
    Isolated settings; helperLifetimeTests(); helperLaunchTests(); hookHealthTests();
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
    const int jitter = 7;
    mouse(WM_MBUTTONDOWN); mouse(WM_MOUSEMOVE, {20 + jitter, 20}); mouse(WM_MBUTTONUP, {20 + jitter, 20}); drain();
    check(launches.size() == beforeRejects + 1, "wheel press finger jitter up to 9px is tolerated");
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
    desktopTests(first);
    remoteKvmCaptureTests(first);
    legacyDialogTests();
    elevatedDialogTests(first, second, ordinary);
    quick::publishHookInstallState(true, true);
    mouse(WM_MOUSEMOVE, {20, 20});
    quick::recordHookRehook(quick::HookRehookReason::MissedMouseMove, true, true);
    const auto hookDiag = quick::queryHookDiagnostics(quick::receiver);
    check(hookDiag.helperRunning && hookDiag.mouseHookInstalled && hookDiag.keyboardHookInstalled &&
          hookDiag.rehookCount == 1 &&
          hookDiag.lastRehookReason == quick::HookRehookReason::MissedMouseMove &&
          hookDiag.msSinceLastMouseCallback < 5000 &&
          hookDiag.msSinceLastRehook < 5000 &&
          std::wstring_view(quick::hookRehookReasonText(hookDiag.lastRehookReason)) == L"指针移动未收到回调",
          "hook diagnostics publish install state, callback ticks, rehook count, and last rehook reason");
    quick::watchDialog(first);
    check(quick::observedDialog == first && quick::dialogNameHook != nullptr && quick::dialogNameProcess != 0,
          "watchDialog installs a name-change hook while a file dialog is observed");
    quick::watchDialog(nullptr);
    check(quick::observedDialog == nullptr && quick::dialogNameHook == nullptr && quick::dialogNameProcess == 0,
          "watchDialog tears down the name-change hook when no dialog is observed");
    const size_t beforeFallback = launches.size();
    quick::showMenu(nullptr, first, {11, 22});
    check(launches.size() == beforeFallback + 1 && launches.back().first == first &&
          launches.back().second.x == 11 && launches.back().second.y == 22,
          "showMenu falls back to separate process when no resident menu thread exists");
    HANDLE dummy = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(dummy != nullptr, "create dummy handle for menu-thread fallback tests");
    quick::menuThread = dummy;
    quick::menuBusy = true;
    quick::showMenu(nullptr, first, {33, 44});
    check(launches.size() == beforeFallback + 2 && quick::menuThread == dummy,
          "showMenu falls back to separate process without closing a busy resident thread");
    SetEvent(dummy);
    quick::menuBusy = false;
    quick::showMenu(nullptr, first, {55, 66});
    check(quick::menuThread == nullptr && launches.size() == beforeFallback + 3,
          "showMenu detects and clears an exited resident menu thread and falls back");
    if (quick::menuThread) { CloseHandle(quick::menuThread); quick::menuThread = nullptr; }
    DestroyWindow(quick::receiver); quick::receiver = nullptr;
    DestroyWindow(first); DestroyWindow(second); DestroyWindow(ordinary); UnregisterClassW(shell.lpszClassName, shell.hInstance);
    std::cout << "PASS " << checks << " quick runtime assertions: foreground race, per-click targets, queued coordinates, gesture filtering, helper handoff\n";
  } catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; result = 1; }
  CoUninitialize(); return result;
}
