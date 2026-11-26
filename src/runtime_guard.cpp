#include "runtime_guard.h"
#include "app_version.h"
#include <dbghelp.h>
#include <psapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

namespace guard {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t logMutexName[] = L"Local\\FolderNote.Log.v1";
constexpr uintmax_t logLimit = 1024 * 1024;
constexpr size_t keptDumps = 5;
wchar_t crashRole[32] = L"app";

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
struct LogLock {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, logMutexName);
  bool owned = mutex && WaitForSingleObject(mutex, 1000) != WAIT_TIMEOUT;
  ~LogLock() { if (owned) ReleaseMutex(mutex); if (mutex) CloseHandle(mutex); }
};
// Module-relative address so a crash can be matched to a symbol build.
std::wstring describeAddress(const void *address) {
  HMODULE module = nullptr;
  wchar_t name[MAX_PATH]{};
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &module) && module && GetModuleFileNameW(module, name, MAX_PATH)) {
    wchar_t text[MAX_PATH + 32]{};
    swprintf_s(text, L"%s+0x%llx", fs::path(name).filename().c_str(),
               static_cast<unsigned long long>(static_cast<const BYTE *>(address) - reinterpret_cast<const BYTE *>(module)));
    return text;
  }
  wchar_t text[32]{}; swprintf_s(text, L"0x%p", address);
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
LONG WINAPI crashFilter(EXCEPTION_POINTERS *pointers) {
  static volatile LONG entered = 0;
  if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;
  try {
    const auto *record = pointers ? pointers->ExceptionRecord : nullptr;
    wchar_t summary[256]{};
    swprintf_s(summary, L"崩溃：角色=%s 版本=%s 代码=0x%08lX 位置=%s 线程=%lu", crashRole, app::version,
               record ? record->ExceptionCode : 0ul, record ? describeAddress(record->ExceptionAddress).c_str() : L"?",
               GetCurrentThreadId());
    std::wstring dumpName;
    const auto folder = root(L"Crashes");
    if (!folder.empty()) {
      const auto path = folder / (std::wstring(L"FolderNote-") + crashRole + L"-" + app::version + L"-" +
                                  timestamp(true) + L"-" + std::to_wstring(GetCurrentProcessId()) + L".dmp");
      HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, pointers ? &info : nullptr, nullptr, nullptr))
          dumpName = path.filename().native();
        CloseHandle(file);
        if (dumpName.empty()) { std::error_code error; fs::remove(path, error); }
      }
      pruneDumps(folder);
    }
    log(std::wstring(summary) + (dumpName.empty() ? L"；未能写入转储" : L"；转储=" + dumpName));
  } catch (...) {}
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
  SetUnhandledExceptionFilter(crashFilter);
}

std::wstring recentLog(size_t lines) noexcept {
  try {
    const auto folder = root(L"Logs");
    if (folder.empty()) return {};
    std::deque<std::string> tail;
    for (const auto *name : {L"FolderNote.1.log", L"FolderNote.log"}) {
      std::ifstream input(folder / name, std::ios::binary);
      for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        tail.push_back(std::move(line));
        if (tail.size() > lines) tail.pop_front();
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
