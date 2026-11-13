#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <functional>

// The child owns a real native picker. Tests never send input to a user's app,
// press OK, select an installation destination or change the foreground app.
struct Shared {
  unsigned long long window;
  volatile LONG changes, returned;
  wchar_t path[32768];
};
struct ChildState { Shared *shared; HANDLE changed; };
LRESULT CALLBACK hidePicker(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
  if (message == WM_APP + 91) { ShowWindow(window, SW_HIDE); return 0; }
  if (message == WM_SHOWWINDOW && w) PostMessageW(window, WM_APP + 91, 0, 0);
  return DefSubclassProc(window, message, w, l);
}
int CALLBACK browseCallback(HWND window, UINT message, LPARAM l, LPARAM data) {
  auto state = reinterpret_cast<ChildState *>(data);
  if (message == BFFM_INITIALIZED) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    SetWindowPos(window, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowSubclass(window, hidePicker, 1, 0); PostMessageW(window, WM_APP + 91, 0, 0);
    state->shared->window = reinterpret_cast<UINT_PTR>(window); SetEvent(state->changed);
  } else if (message == BFFM_SELCHANGED) {
    state->shared->path[0] = 0;
    SHGetPathFromIDListEx(reinterpret_cast<PCIDLIST_ABSOLUTE>(l), state->shared->path, ARRAYSIZE(state->shared->path), GPFIDL_DEFAULT);
    InterlockedIncrement(&state->shared->changes); SetEvent(state->changed);
  }
  return 0;
}
int child(const wchar_t *name, bool modern) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
  HANDLE changed = OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(name) + L".event").c_str());
  auto shared = mapping ? static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
  if (!shared || !changed) return 2;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ChildState state{shared, changed};
  BROWSEINFOW info{}; info.lpszTitle = L"FolderNote isolated folder dialog test";
  info.ulFlags = BIF_RETURNONLYFSDIRS | (modern ? BIF_NEWDIALOGSTYLE : 0);
  info.lpfn = browseCallback; info.lParam = reinterpret_cast<LPARAM>(&state);
  auto selected = SHBrowseForFolderW(&info);
  InterlockedExchange(&shared->returned, selected ? 2 : 1); CoTaskMemFree(selected);
  SetEvent(changed); UnmapViewOfFile(shared); CloseHandle(mapping); CloseHandle(changed); CoUninitialize(); return 0;
}

#ifndef FOLDERNOTE_LEGACY_FIXTURE_ONLY
#include "../src/quick_folders.h"
#include "../src/quick_menu_gate.h"
namespace {
HWND simulatedForeground = nullptr, helper = nullptr;
bool allowTarget = true;
int checks = 0;
HWND fakeForeground() { return simulatedForeground; }
BOOL fakeSetForeground(HWND window) { return window == simulatedForeground; }
HWND fakeFindWindow(HWND parent, HWND after, LPCWSTR name, LPCWSTR title) {
  if (parent == HWND_MESSAGE && name && !wcscmp(name, L"FolderNote.QuickFolders.Helper.v1")) return helper;
  return FindWindowExW(parent, after, name, title);
}
void check(bool ok, const char *message) { ++checks; if (!ok) throw std::runtime_error(message); }
}
#define GetForegroundWindow fakeForeground
#define SetForegroundWindow fakeSetForeground
#define FindWindowExW fakeFindWindow
#include "../src/quick_legacy.cpp"
#undef FindWindowExW
#undef SetForegroundWindow
#undef GetForegroundWindow

bool allowed(HWND window, DWORD pid) {
  DWORD actual = 0; GetWindowThreadProcessId(window, &actual);
  return allowTarget && window == simulatedForeground && actual == pid;
}
LRESULT CALLBACK helperProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_COPYDATA) return quick::legacy::receive(reinterpret_cast<COPYDATASTRUCT *>(l), allowed);
  return DefWindowProcW(window, message, w, l);
}
void pump() { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } }
bool waitFor(const std::function<bool()> &condition, DWORD timeout = 5000) {
  const auto end = GetTickCount64() + timeout;
  do { pump(); quick::legacy::reap(); if (condition()) return true; MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT); } while (GetTickCount64() < end);
  return false;
}
std::wstring token() { GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, ARRAYSIZE(text)); return text; }
struct Fixture {
  std::wstring name = L"Local\\FolderNote.LegacyTest." + token();
  HANDLE mapping = nullptr, changed = nullptr, process = nullptr;
  Shared *shared = nullptr;
  Fixture(const std::wstring &executable, bool modern) {
    try {
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
    changed = CreateEventW(nullptr, FALSE, FALSE, (name + L".event").c_str());
    shared = mapping ? static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    check(shared && changed, "create private fixture mapping and event"); ZeroMemory(shared, sizeof(*shared));
    auto command = L"\"" + executable + L"\" --child \"" + name + L"\" " + (modern ? L"new" : L"old");
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION info{};
    check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE,
          "start independent picker process");
    process = info.hProcess; CloseHandle(info.hThread);
    check(waitFor([&] { return shared->window != 0; }), "native picker initializes");
    simulatedForeground = reinterpret_cast<HWND>(static_cast<UINT_PTR>(shared->window));
    } catch (...) { close(); throw; }
  }
  ~Fixture() { close(); }
  void close() {
    if (shared && shared->window) PostMessageW(reinterpret_cast<HWND>(static_cast<UINT_PTR>(shared->window)), WM_CLOSE, 0, 0);
    if (process) {
      if (WaitForSingleObject(process, 3000) == WAIT_TIMEOUT) TerminateProcess(process, 2); // Only our disposable fixture.
      CloseHandle(process);
    }
    pump(); quick::legacy::reap();
    if (shared) UnmapViewOfFile(shared); if (mapping) CloseHandle(mapping); if (changed) CloseHandle(changed);
    shared = nullptr; process = mapping = changed = nullptr;
    simulatedForeground = nullptr;
  }
};
void dialogs(const std::wstring &executable, bool verifyNativeSelection) {
  wchar_t temp[MAX_PATH]{}; GetTempPathW(ARRAYSIZE(temp), temp);
  // Hosted Windows runners can expose a DOS alias in TEMP while Shell returns
  // a long path in BFFM_SELCHANGED. Use the same path spelling for both sides.
  wchar_t expanded[MAX_PATH]{};
  const auto length = GetLongPathNameW(temp, expanded, ARRAYSIZE(expanded));
  std::wstring tempDirectory = length && length < ARRAYSIZE(expanded) ? expanded : temp;
  if (!tempDirectory.empty() && tempDirectory.back() != L'\\') tempDirectory += L'\\';
  const std::wstring root = tempDirectory + L"FolderNote-Legacy-" + token();
  const auto destination = root + L"\\中文 空格 😀", other = root + L"\\Other folder";
  check(CreateDirectoryW(root.c_str(), nullptr) && CreateDirectoryW(destination.c_str(), nullptr) && CreateDirectoryW(other.c_str(), nullptr), "create private Unicode test directories");
  struct Cleanup { std::wstring root, first, second; ~Cleanup() { RemoveDirectoryW(first.c_str()); RemoveDirectoryW(second.c_str()); RemoveDirectoryW(root.c_str()); } } cleanup{root, destination, other};
  for (bool modern : {false, true}) {
    Fixture fixture(executable, modern);
    if (!quick::legacy::isDialog(simulatedForeground)) {
      EnumChildWindows(simulatedForeground, [](HWND window, LPARAM) -> BOOL { wchar_t cls[200]{}; GetClassNameW(window, cls, ARRAYSIZE(cls)); std::wcerr << L"child " << cls << L" id=" << GetDlgCtrlID(window) << L"\n"; return TRUE; }, 0);
    }
    check(quick::legacy::isDialog(simulatedForeground), "recognize real native classic and resizable folder pickers");
    const auto requestName = L"Local\\FolderNote.LegacyRequestTest." + token();
    quick::detail::MenuRequest request(requestName.c_str()); request.begin();
    allowTarget = false;
    check(!quick::legacy::navigate(simulatedForeground, destination, request), "broker permission rejection prevents dispatch");
    allowTarget = true;
    check(!quick::legacy::navigate(simulatedForeground, L"relative", request), "relative destination rejected");
    check(!quick::legacy::navigate(simulatedForeground, std::wstring(L"C:\\safe\0hidden", 14), request), "embedded NUL cannot change the effective destination");
    check(!quick::legacy::navigate(simulatedForeground, std::wstring(32768, L'a'), request), "oversized destination rejected before transport");
    check(quick::legacy::navigate(simulatedForeground, destination, request), "submit Unicode navigation across real process boundary");
    check(!quick::legacy::navigate(simulatedForeground, other, request), "pending selection blocks a second uncancellable request for the same process");
    if (!verifyNativeSelection) {
      check(waitFor([] { return quick::legacy::pending.empty(); }), "native picker acknowledges transport");
      check(IsWindow(simulatedForeground) && fixture.shared->returned == 0, "transport does not accept or close picker");
      quick::detail::MenuRequest newer(requestName.c_str()); newer.begin();
      check(!quick::legacy::navigate(simulatedForeground, destination, request), "obsolete menu cannot navigate picker");
      continue;
    }
    const auto selected = waitFor([&] { return !_wcsicmp(fixture.shared->path, destination.c_str()); });
    if (!selected) std::wcerr << L"Expected picker path: " << destination << L"\nActual picker path: "
                             << fixture.shared->path << L"\nSelection callbacks: " << fixture.shared->changes << L'\n';
    check(selected, "native picker selects exact requested Unicode path");
    check(waitFor([] { return quick::legacy::pending.empty(); }), "completion callback releases pending transport buffer");
    check(IsWindow(simulatedForeground) && fixture.shared->returned == 0, "navigation never accepts or closes picker");
    check(quick::legacy::navigate(simulatedForeground, other, request), "repeated request accepted after completion");
    check(waitFor([&] { return !_wcsicmp(fixture.shared->path, other.c_str()); }), "repeated request changes native selection");
    check(waitFor([] { return !quick::legacy::reap(); }), "completed requests leave no cleanup polling work");
    quick::detail::MenuRequest newer(requestName.c_str()); newer.begin();
    check(!quick::legacy::navigate(simulatedForeground, destination, request), "obsolete menu cannot navigate picker");
    check(fixture.shared->returned == 0, "confirmation still belongs to user after repeated navigation");
  }
}
#endif
int wmain(int argc, wchar_t **argv) {
  if (argc >= 4 && !wcscmp(argv[1], L"--child")) return child(argv[2], !wcscmp(argv[3], L"new"));
#ifdef FOLDERNOTE_LEGACY_FIXTURE_ONLY
  return 2;
#else
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = helperProc; cls.lpszClassName = L"FolderNote.LegacyTest.Helper";
    check(RegisterClassW(&cls) != 0, "register private broker");
    helper = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    check(helper != nullptr, "create private broker window");
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const bool transportOnly = argc == 2 && !wcscmp(argv[1], L"--transport-only");
    dialogs(argc == 2 && !transportOnly ? argv[1] : self, !transportOnly);
    if (transportOnly) std::cout << "Native selection assertions require an interactive Windows desktop.\n";
    quick::legacy::shutdown(); DestroyWindow(helper); helper = nullptr; UnregisterClassW(cls.lpszClassName, cls.hInstance);
    std::cout << "PASS " << checks << " legacy picker checks: native dialogs, cross-process Unicode, repeated navigation, no confirmation, permission and stale-request guards\n";
  } catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; result = 1; }
  quick::legacy::shutdown(); CoUninitialize(); return result;
#endif
}
