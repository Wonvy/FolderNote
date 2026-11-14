#include "desktop_groups_visibility.h"
#include "core.h"
#include "quick_folders.h"
#include <string>

namespace desktop_groups {
namespace {
constexpr wchar_t hiddenBy[] = L"FolderNote.DesktopGroups.HiddenBy";
constexpr wchar_t visibilityMutex[] = L"Local\\FolderNote.DesktopGroups.IconVisibility";

struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE handle = nullptr) : value(handle) {}
  ~Handle() { if (value) CloseHandle(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};

struct Lock {
  Handle mutex{CreateMutexW(nullptr, FALSE, visibilityMutex)};
  bool held = false;
  Lock() {
    if (mutex.value) {
      const auto status = WaitForSingleObject(mutex.value, 5000);
      held = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
    }
  }
  ~Lock() { if (held) ReleaseMutex(mutex.value); }
};

struct IconView {
  HWND handle = nullptr;
  DWORD shellPid = 0;
};

bool classIs(HWND window, const wchar_t *name) {
  wchar_t actual[64]{};
  return GetClassNameW(window, actual, ARRAYSIZE(actual)) && wcscmp(actual, name) == 0;
}

bool sameProcess(HWND window, DWORD pid) {
  DWORD owner = 0;
  GetWindowThreadProcessId(window, &owner);
  return owner != 0 && owner == pid;
}

bool desktopList(HWND window, DWORD pid) {
  const auto parent = GetParent(window);
  return classIs(window, L"SysListView32") && classIs(parent, L"SHELLDLL_DefView") &&
      sameProcess(window, pid) && sameProcess(parent, pid);
}

IconView desktopView() {
  IconView result;
  if (auto shell = GetShellWindow()) GetWindowThreadProcessId(shell, &result.shellPid);
  if (!result.shellPid) return result;
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    auto &view = *reinterpret_cast<IconView *>(data);
    if (!sameProcess(window, view.shellPid) || !quick::isDesktop(window)) return TRUE;
    EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
      auto &view = *reinterpret_cast<IconView *>(data);
      if (!desktopList(child, view.shellPid)) return TRUE;
      view.handle = child;
      return FALSE;
    }, data);
    return !view.handle;
  }, reinterpret_cast<LPARAM>(&result));
  return result;
}

bool stillDesktop(const IconView &view) {
  const auto shell = GetShellWindow();
  return view.handle && sameProcess(shell, view.shellPid) && desktopList(view.handle, view.shellPid) &&
      quick::isDesktop(GetAncestor(view.handle, GA_ROOT));
}

bool visible(HWND list) { return (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0; }

DWORD owner(HWND list) {
  return list ? static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(list, hiddenBy))) : 0;
}

bool setVisible(const IconView &view, bool show) {
  if (!stillDesktop(view)) return false;
  // Always queue the restoring show. A helper could die just after queuing
  // its asynchronous hide and before Explorer actually handles that request.
  if (!show && !visible(view.handle)) return true;
  if (!ShowWindowAsync(view.handle, show ? SW_SHOWNA : SW_HIDE)) return false;
  for (int i = 0; i < 25; ++i) {
    if (!stillDesktop(view)) return false;
    if (visible(view.handle) == show) return true;
    Sleep(20);
  }
  return false;
}

bool restore(DWORD onlyOwner) {
  const Lock lock;
  if (!lock.held) return false;
  const auto view = desktopView();
  const auto pid = owner(view.handle);
  if (!pid || (onlyOwner && pid != onlyOwner)) return true;
  if (!setVisible(view, true)) return !stillDesktop(view);
  RemovePropW(view.handle, hiddenBy);
  return true;
}

std::wstring readinessName(DWORD pid, HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
  return L"Local\\FolderNote.DesktopGroups.IconWatch." + std::to_wstring(pid) + L"." +
      std::to_wstring((static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

bool ensureWatch() {
  static Handle watcher;
  if (watcher.value) {
    if (WaitForSingleObject(watcher.value, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(watcher.value);
    watcher.value = nullptr;
  }
  const DWORD pid = GetCurrentProcessId();
  const auto readyName = readinessName(pid, GetCurrentProcess());
  if (readyName.empty()) return false;
  const Handle ready(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
  if (!ready.value) return false;
  ResetEvent(ready.value);

  const auto exe = fn::modulePath();
  auto command = L"\"" + exe.native() + L"\" --desktop-groups-watch " + std::to_wstring(pid);
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                      &startup, &process)) return false;
  const Handle thread(process.hThread);
  const HANDLE events[]{ready.value, process.hProcess};
  if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 2500) != WAIT_OBJECT_0 ||
      WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) {
    CloseHandle(process.hProcess);
    return false;
  }
  watcher.value = process.hProcess;
  return true;
}

bool ownExecutable(HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return false;
  path.resize(size);
  return CompareStringOrdinal(path.c_str(), -1, fn::modulePath().c_str(), -1, TRUE) == CSTR_EQUAL;
}
}

bool setDesktopIconsHidden(bool hide) try {
  if (!hide) return restoreDesktopIcons();
  const Lock lock;
  if (!lock.held) return false;
  const auto view = desktopView();
  if (!stillDesktop(view)) return false;
  const DWORD pid = GetCurrentProcessId();
  const DWORD previous = owner(view.handle);
  // The existing hidden state belongs to Explorer if we did not put it there.
  if (!previous && !visible(view.handle)) return true;
  if (!ensureWatch() || !stillDesktop(view)) return false;
  if (previous != pid && !SetPropW(view.handle, hiddenBy, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid))))
    return false;
  return setVisible(view, false);
} catch (...) {
  return false;
}

bool restoreDesktopIcons() { return restore(0); }

int watchDesktopIcons(DWORD helperPid) try {
  DWORD helperSession = 0, currentSession = 0;
  if (!helperPid || helperPid == GetCurrentProcessId() || !ProcessIdToSessionId(helperPid, &helperSession) ||
      !ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) || helperSession != currentSession) return 1;
  const Handle helper(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, helperPid));
  if (!helper.value || !ownExecutable(helper.value)) return 1;
  const auto name = readinessName(helperPid, helper.value);
  if (name.empty()) return 1;
  const Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
  if (!ready.value || !SetEvent(ready.value)) return 1;
  if (WaitForSingleObject(helper.value, INFINITE) != WAIT_OBJECT_0) return 1;
  for (int retry = 0; retry < 10; ++retry) {
    if (restore(helperPid)) return 0;
    Sleep(200);
  }
  return 1;
} catch (...) {
  return 1;
}
}
