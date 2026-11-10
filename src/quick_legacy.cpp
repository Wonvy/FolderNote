#include "quick_legacy.h"
#include "quick_folders.h"
#include "quick_menu_gate.h"
#include <shlobj.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace quick::legacy {
namespace {
constexpr wchar_t namespaceClass[] = L"SHBrowseForFolder ShellNameSpace Control";
constexpr size_t maximumPath = 32767, maximumPending = 8;
constexpr uint32_t protocolVersion = 1;
struct Header {
  uint32_t version;
  uint32_t process;
  uint64_t window;
  uint32_t characters; // Excludes the single terminating NUL.
  uint32_t reserved;
};
static_assert(sizeof(Header) == 24);
static_assert(sizeof(wchar_t) == 2);

bool classIs(HWND window, const wchar_t *name) {
  wchar_t actual[128]{};
  return GetClassNameW(window, actual, ARRAYSIZE(actual)) && !_wcsicmp(actual, name);
}
bool matches(HWND window, DWORD process) {
  DWORD actual = 0;
  return window && GetWindowThreadProcessId(window, &actual) && actual == process;
}
bool eligible(HWND target, DWORD process, bool (*authorized)(HWND, DWORD)) {
  return process && process != GetCurrentProcessId() && matches(target, process) &&
         target == GetForegroundWindow() && isDialog(target) && authorized && authorized(target, process) &&
         matches(target, process) && target == GetForegroundWindow();
}
struct Pending {
  HANDLE process = nullptr;
  DWORD id = 0;
  void *remote = nullptr;
  bool inFlight = false;
  ~Pending() {
    if (remote && process && !inFlight) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (process) CloseHandle(process);
  }
};
thread_local std::map<ULONG_PTR, std::unique_ptr<Pending>> pending;
thread_local ULONG_PTR nextToken = 0;

void CALLBACK completed(HWND, UINT, ULONG_PTR token, LRESULT) {
  // Timed cleanup may have removed a dead process. An opaque token, rather
  // than a context pointer, keeps a late callback harmless in that case.
  auto found = pending.find(token);
  if (found != pending.end()) { found->second->inFlight = false; pending.erase(found); }
}
ULONG_PTR token() {
  do { ++nextToken; } while (!nextToken || pending.contains(nextToken));
  return nextToken;
}
}

bool isDialog(HWND window) {
  if (!classIs(window, L"#32770") || GetAncestor(window, GA_ROOT) != window) return false;
  for (const int id : {IDOK, IDCANCEL}) {
    const auto button = GetDlgItem(window, id);
    if (!button || !IsChild(window, button) || !classIs(button, L"Button")) return false;
  }
  // The non-resizable classic template predates the namespace host control.
  // Require its complete native control signature rather than accepting an
  // arbitrary tree-based settings dialog. These children belong to the root.
  const auto classicTree = GetDlgItem(window, 14145);
  const auto classicPrompt = GetDlgItem(window, 14146);
  const auto classicStatus = GetDlgItem(window, 14147);
  if (GetParent(classicTree) == window && classIs(classicTree, L"SysTreeView32") &&
      GetParent(classicPrompt) == window && classIs(classicPrompt, L"Static") &&
      GetParent(classicStatus) == window && classIs(classicStatus, L"Static")) return true;
  bool found = false;
  EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
    auto &result = *reinterpret_cast<bool *>(value);
    if (!classIs(child, namespaceClass)) return TRUE;
    EnumChildWindows(child, [](HWND descendant, LPARAM output) -> BOOL {
      if (classIs(descendant, L"SysTreeView32")) { *reinterpret_cast<bool *>(output) = true; return FALSE; }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return !result;
  }, reinterpret_cast<LPARAM>(&found));
  return found;
}

bool navigate(HWND target, const std::wstring &input, const detail::MenuRequest &request) {
  try {
    if (!request.current() || !isDialog(target) || input.empty() || input.size() > maximumPath ||
        input.find(L'\0') != input.npos) return false;
    auto path = normalize(input);
    if (path.empty() || path.size() > maximumPath) return false;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(target, &process) || !process) return false;
    HWND helper = FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr);
    if (!helper) return false;
    SetForegroundWindow(target);
    if (!request.current() || GetForegroundWindow() != target || !matches(target, process) || !isDialog(target)) return false;
    Header header{protocolVersion, process, static_cast<uint64_t>(reinterpret_cast<UINT_PTR>(target)),
                  static_cast<uint32_t>(path.size()), 0};
    std::vector<BYTE> bytes(sizeof(header) + (path.size() + 1) * sizeof(wchar_t));
    memcpy(bytes.data(), &header, sizeof(header));
    memcpy(bytes.data() + sizeof(header), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT data{copyDataTag, static_cast<DWORD>(bytes.size()), bytes.data()};
    DWORD_PTR accepted = 0;
    // WM_COPYDATA is a system message: Windows owns its cross-process buffer
    // lifetime, including a timeout after the receiving procedure has begun.
    return request.current() && SendMessageTimeoutW(helper, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 1000, &accepted) && accepted == 1;
  } catch (...) { return false; }
}

bool reap() noexcept {
  for (auto item = pending.begin(); item != pending.end();) {
    if (WaitForSingleObject(item->second->process, 0) == WAIT_OBJECT_0) {
      item->second->remote = nullptr; // Target address space has already gone.
      item = pending.erase(item);
    } else ++item;
  }
  return !pending.empty();
}

bool receive(const COPYDATASTRUCT *data, bool (*authorized)(HWND, DWORD)) {
  try {
    if (!data || data->dwData != copyDataTag || !data->lpData ||
        data->cbData < sizeof(Header) + sizeof(wchar_t) ||
        data->cbData > sizeof(Header) + (maximumPath + 1) * sizeof(wchar_t)) return false;
    Header header{}; memcpy(&header, data->lpData, sizeof(header));
    if (header.version != protocolVersion || header.reserved || !header.characters || header.characters > maximumPath ||
        header.window > static_cast<uint64_t>((std::numeric_limits<UINT_PTR>::max)()) ||
        data->cbData != sizeof(Header) + (size_t(header.characters) + 1) * sizeof(wchar_t)) return false;
    // Do not retain any sender-owned pointer beyond this WM_COPYDATA call.
    std::wstring path(size_t(header.characters) + 1, L'\0');
    memcpy(path.data(), static_cast<const BYTE *>(data->lpData) + sizeof(header), path.size() * sizeof(wchar_t));
    if (path.back() != L'\0' || path.find(L'\0') != header.characters) return false;
    path.pop_back();
    if (normalize(path) != path) return false;
    const auto target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(header.window));
    if (!eligible(target, header.process, authorized)) return false;
    reap();
    if (pending.size() >= maximumPending) return false;
    for (const auto &item : pending) if (item.second->id == header.process) return false;

    // Refuse an already hung receiver before queuing an uncancellable async
    // selection. This probe is not used as a completion fence for any request.
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(target, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 150, &ignored) ||
        !eligible(target, header.process, authorized)) return false;
    auto operation = std::make_unique<Pending>(); operation->id = header.process;
    operation->process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                     FALSE, header.process);
    if (!operation->process || WaitForSingleObject(operation->process, 0) != WAIT_TIMEOUT) return false;
    BOOL target32 = FALSE;
    if (!IsWow64Process(operation->process, &target32)) return false;
    const auto bytes = (path.size() + 1) * sizeof(wchar_t);
    operation->remote = VirtualAllocEx(operation->process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!operation->remote || (target32 && reinterpret_cast<UINT_PTR>(operation->remote) > UINT32_MAX)) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(operation->process, operation->remote, path.c_str(), bytes, &written) || written != bytes ||
        !eligible(target, header.process, authorized)) return false;
    const auto id = token(); const auto address = reinterpret_cast<LPARAM>(operation->remote);
    pending.emplace(id, std::move(operation));
    pending.at(id)->inFlight = true;
    // This WM_USER message is not system-marshaled. It points only at our
    // allocated UTF-16 data, never code. Keep it alive until the actual callback.
    if (!SendMessageCallbackW(target, BFFM_SETSELECTIONW, TRUE, address, completed, id)) {
      auto found = pending.find(id);
      if (found != pending.end()) { found->second->inFlight = false; pending.erase(found); }
      return false;
    }
    return true;
  } catch (...) { return false; }
}

void shutdown() noexcept {
  // A live receiver may still be reading an async message. Never free its
  // buffer during helper shutdown. At most eight <=64 KiB data blocks remain
  // until those target processes exit; normal callbacks free them immediately.
  for (auto &item : pending) item.second->remote = nullptr;
  pending.clear();
  // Do not reset nextToken: a late callback must never match a newer request.
}
}
