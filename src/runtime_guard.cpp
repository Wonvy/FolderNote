#include "runtime_guard.h"
#include "app_version.h"
#include <dbghelp.h>
#include <psapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <vector>

namespace guard {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t logMutexName[] = L"Local\\FolderNote.Log.v1";
constexpr uintmax_t logLimit = 1024 * 1024;
constexpr size_t keptDumps = 5;
wchar_t crashRole[32] = L"app";
wchar_t precomputedCrashDir[1024]{};
HANDLE crashTrigger = nullptr, crashDone = nullptr, dumpThread = nullptr;
volatile LONG dumpInitialized = 0;
struct PendingCrash {
  DWORD threadId = 0;
  EXCEPTION_POINTERS *pointers = nullptr;
  const void *address = nullptr;
  DWORD code = 0;
} pendingCrash;

fs::path root(const wchar_t *child) {
  PWSTR base = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &base)) || !base) return {};
  fs::path result = fs::path(base) / L"Programs" / L"FolderNote" / child;
  CoTaskMemFree(base);
  std::error_code error;
  fs::create_directories(result, error);
  return result;
}
std::wstring timestamp(bool fileSafe) {
  SYSTEMTIME now{}; GetLocalTime(&now);
  wchar_t text[40]{};
  swprintf_s(text, fileSafe ? L"%04u%02u%02u-%02u%02u%02u" : L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
  return text;
}
std::string utf8(const std::wstring &text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size > 0 ? size : 0, '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
  return result;
}
std::wstring wide(const std::string &text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(size > 0 ? size : 0, L'\0');
  if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
  return result;
}
HANDLE createSharedLogMutex() noexcept {
  PSECURITY_DESCRIPTOR sd = nullptr;
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size) {
      std::vector<BYTE> bytes(size);
      LPWSTR sid = nullptr;
      if (GetTokenInformation(token, TokenUser, bytes.data(), size, &size) &&
          ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(bytes.data())->User.Sid, &sid) && sid) {
        try {
          const auto sddl = std::wstring(L"D:(A;;GA;;;") + sid + L")S:(ML;;NW;;;ME)";
          ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
        } catch (...) {
          sd = nullptr;
        }
        LocalFree(sid);
      }
    }
    CloseHandle(token);
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  HANDLE mutex = CreateMutexW(sd ? &sa : nullptr, FALSE, logMutexName);
  if (!mutex && sd) mutex = CreateMutexW(nullptr, FALSE, logMutexName);
  if (sd) LocalFree(sd);
  return mutex;
}
struct LogLock {
  HANDLE mutex = createSharedLogMutex();
  DWORD wait = mutex ? WaitForSingleObject(mutex, 1000) : WAIT_FAILED;
  bool owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
  ~LogLock() { if (owned) ReleaseMutex(mutex); if (mutex) CloseHandle(mutex); }
};
// Module-relative address so a crash can be matched to a symbol build.
std::wstring describeAddress(const void *address) {
  HMODULE module = nullptr;
  wchar_t name[MAX_PATH]{};
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &module) && module && GetModuleFileNameW(module, name, MAX_PATH)) {
    wchar_t text[MAX_PATH + 32]{};
    _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"%s+0x%llx", fs::path(name).filename().c_str(),
                 static_cast<unsigned long long>(static_cast<const BYTE *>(address) - reinterpret_cast<const BYTE *>(module)));
    return text;
  }
  wchar_t text[32]{}; _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"0x%p", address);
  return text;
}
void pruneDumps(const fs::path &folder) {
  std::error_code error;
  std::vector<fs::directory_entry> dumps;
  for (const auto &entry : fs::directory_iterator(folder, error))
    if (entry.path().extension() == L".dmp") dumps.push_back(entry);
  if (dumps.size() <= keptDumps) return;
  std::sort(dumps.begin(), dumps.end(), [](const auto &a, const auto &b) {
    std::error_code ignored;
    return a.last_write_time(ignored) > b.last_write_time(ignored);
  });
  for (size_t i = keptDumps; i < dumps.size(); ++i) fs::remove(dumps[i].path(), error);
}
// Runs on a dedicated standby thread with a clean stack, avoiding heap/loader
// locks or stack exhaustion on the faulting thread inside crashFilter.
DWORD WINAPI dumpThreadProc(void *) {
  if (!crashTrigger || WaitForSingleObject(crashTrigger, INFINITE) != WAIT_OBJECT_0) return 0;
  // Write the minidump first with stack-only buffers so a crash inside the
  // heap lock still produces a dump before describeAddress/log allocate.
  wchar_t dumpPath[1280]{};
  const wchar_t *dumpFile = nullptr;
  if (precomputedCrashDir[0]) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    _snwprintf_s(dumpPath, ARRAYSIZE(dumpPath), _TRUNCATE, L"%s\\FolderNote-%s-%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
                 precomputedCrashDir, crashRole, app::version,
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION info{pendingCrash.threadId, pendingCrash.pointers, FALSE};
      const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
      if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                            pendingCrash.pointers ? &info : nullptr, nullptr, nullptr)) {
        const wchar_t *slash = wcsrchr(dumpPath, L'\\');
        dumpFile = slash ? slash + 1 : dumpPath;
      }
      CloseHandle(file);
      if (!dumpFile) DeleteFileW(dumpPath);
    }
  }
  try {
    wchar_t summary[256]{};
    _snwprintf_s(summary, ARRAYSIZE(summary), _TRUNCATE, L"崩溃：角色=%s 版本=%s 代码=0x%08lX 位置=%s 线程=%lu",
                 crashRole, app::version, pendingCrash.code,
                 pendingCrash.address ? describeAddress(pendingCrash.address).c_str() : L"?",
                 pendingCrash.threadId);
    if (precomputedCrashDir[0]) pruneDumps(fs::path(precomputedCrashDir));
    log(std::wstring(summary) + (dumpFile ? (std::wstring(L"；转储=") + dumpFile) : L"；未能写入转储"));
  } catch (...) {}
  if (crashDone) SetEvent(crashDone);
  return 0;
}
LONG WINAPI crashFilter(EXCEPTION_POINTERS *pointers) {
  static volatile LONG entered = 0;
  if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;
  if (crashTrigger && crashDone && dumpThread) {
    pendingCrash.threadId = GetCurrentThreadId();
    pendingCrash.pointers = pointers;
    pendingCrash.address = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionAddress : nullptr;
    pendingCrash.code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0ul;
    MemoryBarrier();
    SetEvent(crashTrigger);
    WaitForSingleObject(crashDone, 5000);
  }
  // Let Windows Error Reporting finish; the exit code stays the exception code,
  // which the helper supervisor uses to decide on a restart.
  return EXCEPTION_CONTINUE_SEARCH;
}
}

void log(const std::wstring &message) noexcept {
  try {
    const auto folder = root(L"Logs");
    if (folder.empty()) return;
    const auto path = folder / L"FolderNote.log";
    wchar_t prefix[96]{};
    swprintf_s(prefix, L" [%lu %s] ", GetCurrentProcessId(), crashRole);
    const auto line = utf8(timestamp(false) + prefix + message) + "\r\n";
    LogLock lock;
    if (!lock.owned) return;
    std::error_code error;
    if (fs::exists(path, error) && fs::file_size(path, error) > logLimit) {
      fs::remove(folder / L"FolderNote.2.log", error);
      fs::rename(folder / L"FolderNote.1.log", folder / L"FolderNote.2.log", error);
      fs::rename(path, folder / L"FolderNote.1.log", error);
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
  } catch (...) {}
}

void installCrashHandler(const wchar_t *role) noexcept {
  if (role) wcsncpy_s(crashRole, role, _TRUNCATE);
  if (!InterlockedExchange(&dumpInitialized, 1)) {
    try {
      const auto crashes = root(L"Crashes");
      if (!crashes.empty()) wcsncpy_s(precomputedCrashDir, crashes.c_str(), _TRUNCATE);
      root(L"Logs");
    } catch (...) {}
    crashTrigger = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    crashDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (crashTrigger && crashDone) dumpThread = CreateThread(nullptr, 0, dumpThreadProc, nullptr, 0, nullptr);
  }
  SetUnhandledExceptionFilter(crashFilter);
}

std::wstring recentLog(size_t lines) noexcept {
  try {
    const auto folder = root(L"Logs");
    if (folder.empty()) return {};
    std::deque<std::string> tail;
    for (const auto *name : {L"FolderNote.1.log", L"FolderNote.log"}) {
      HANDLE file = CreateFileW((folder / name).c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) continue;
      std::string content;
      char buffer[8192];
      DWORD readBytes = 0;
      while (ReadFile(file, buffer, sizeof(buffer), &readBytes, nullptr) && readBytes > 0)
        content.append(buffer, readBytes);
      CloseHandle(file);
      size_t start = 0;
      while (start < content.size()) {
        const size_t end = content.find('\n', start);
        const size_t stop = end == std::string::npos ? content.size() : end;
        std::string line = content.substr(start, stop - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        tail.push_back(std::move(line));
        if (tail.size() > lines) tail.pop_front();
        start = end == std::string::npos ? content.size() : end + 1;
      }
    }
    std::wstring result;
    for (const auto &line : tail) result += wide(line) + L"\r\n";
    return result;
  } catch (...) { return {}; }
}

std::wstring lastCrash() noexcept {
  try {
    const auto folder = root(L"Crashes");
    std::error_code error;
    fs::directory_entry newest;
    size_t count = 0;
    for (const auto &entry : fs::directory_iterator(folder, error)) {
      if (entry.path().extension() != L".dmp") continue;
      ++count;
      std::error_code ignored;
      if (newest.path().empty() || entry.last_write_time(ignored) > newest.last_write_time(ignored)) newest = entry;
    }
    if (!count) return L"无";
    return newest.path().filename().native() + L"（共保留 " + std::to_wstring(count) + L" 份，目录：" + folder.native() + L"）";
  } catch (...) { return L"无法读取"; }
}
}
