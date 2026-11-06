#include "../src/quick_folders.h"
#include "../src/navigation.h"
#include "../src/core.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
int checks = 0;
void check(bool value) { if (!value) throw std::runtime_error("quick folders assertion " + std::to_string(checks)); ++checks; }
struct Isolated {
  HKEY real = nullptr, key = nullptr;
  std::wstring name;
  Isolated() {
    GUID id{}; CoCreateGuid(&id); wchar_t text[40]{}; StringFromGUID2(id, text, 40);
    name = std::wstring(L"Software\\FolderNoteQuickTests\\") + text;
    if (RegOpenCurrentUser(KEY_ALL_ACCESS, &real) ||
        RegCreateKeyExW(real, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) ||
        RegOverridePredefKey(HKEY_CURRENT_USER, key)) throw std::runtime_error("isolated registry");
  }
  ~Isolated() { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(key); RegDeleteTreeW(real, name.c_str()); RegCloseKey(real); }
};
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    Isolated isolated;
    check(!navigation::quickEnabled() && navigation::dialogQuickEnabled());
    check(quick::load().pins.empty() && quick::load().history.empty());
    check(quick::normalize(L"c:/资料/项目/../项目/") == L"c:\\资料\\项目");
    check(quick::normalize(L"C:\\..\\..\\") == L"C:\\");
    check(quick::normalize(L"\\\\server\\share\\..\\资料") == L"\\\\server\\share\\资料");
    check(quick::normalize(L"\\\\?\\UNC\\server\\share\\资料\\") == L"\\\\server\\share\\资料");
    for (auto invalid : {L"relative", L"C:relative", L"shell:Downloads", L"\\\\.\\device", L"\\\\server", L"C:\\x\ny", L"C:\\x:stream", L"C:\\*"})
      check(quick::normalize(invalid).empty());
    quick::pin(L"D:\\置顶😀", true); quick::pin(L"d:\\置顶😀\\", true);
    quick::pin(L"D:\\另一项", true);
    check(quick::load().pins.size() == 2);
    quick::movePin(L"D:\\另一项", -1);
    check(quick::load().pins.front() == L"D:\\另一项");
    quick::visit(L"C:\\A"); quick::visit(L"C:\\B"); quick::visit(L"c:\\a\\");
    auto state = quick::load();
    check(state.history.size() == 2 && quick::same(state.history.front(), L"C:\\A"));
    for (unsigned i = 0; i < 230; ++i) quick::visit(L"C:\\History\\" + std::to_wstring(i));
    state = quick::load();
    check(state.history.size() == quick::historyLimit && state.history.front() == L"C:\\History\\229");
    quick::clearHistory();
    check(quick::load().history.empty() && quick::load().pins.size() == 2);
    std::atomic<bool> success{true}; std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) writers.emplace_back([&, t] {
      try { for (int i = 0; i < 30; ++i) quick::visit(L"D:\\Thread" + std::to_wstring(t) + L"\\" + std::to_wstring(i)); }
      catch (...) { success = false; }
    });
    for (auto &thread : writers) thread.join();
    check(success && quick::load().history.size() == quick::historyLimit && quick::load().pins.size() == 2);
    quick::visit(L"D:\\最后点击"); quick::visit(L"D:\\另一目录"); quick::visit(L"d:\\最后点击");
    check(quick::load().history.size() == 20 && quick::same(quick::load().history.front(), L"D:\\最后点击"));
    // Upgrade: an older 200-entry list must shrink even if the visited folder
    // already occupies its first slot. Pins and first-visit ordering survive.
    std::vector<wchar_t> legacy;
    for (int i = 0; i < 30; ++i) {
      auto path = L"D:\\旧历史\\" + std::to_wstring(i);
      legacy.insert(legacy.end(), path.begin(), path.end()); legacy.push_back(0);
    }
    legacy.push_back(0);
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote\\QuickFolders", L"History", REG_MULTI_SZ,
        legacy.data(), static_cast<DWORD>(legacy.size() * sizeof(wchar_t))) == ERROR_SUCCESS);
    quick::visit(L"D:\\旧历史\\0");
    state = quick::load();
    check(state.history.size() == 20 && state.history.front() == L"D:\\旧历史\\0" && state.history.back() == L"D:\\旧历史\\19");
    DWORD savedBytes = 0;
    check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote\\QuickFolders", L"History", RRF_RT_REG_MULTI_SZ,
        nullptr, nullptr, &savedBytes) == ERROR_SUCCESS && savedBytes < legacy.size() * sizeof(wchar_t));
    quick::pin(L"D:\\另一项", false);
    for (unsigned i = 1; i < quick::pinLimit; ++i) quick::pin(L"D:\\Pin" + std::to_wstring(i), true);
    bool rejected = false;
    try { quick::pin(L"D:\\too-many", true); } catch (const fn::Error &) { rejected = true; }
    check(rejected && quick::load().pins.size() == quick::pinLimit);
    const wchar_t bad[] = L"not a multi-string";
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote\\QuickFolders", L"History", REG_SZ, bad, sizeof(bad));
    rejected = false;
    try { quick::visit(L"C:\\dont-overwrite"); } catch (const fn::Error &) { rejected = true; }
    check(rejected);
    wchar_t unchanged[100]{}; DWORD bytes = sizeof(unchanged);
    check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote\\QuickFolders", L"History", RRF_RT_REG_SZ, nullptr, unchanged, &bytes) == 0 && !wcscmp(bad, unchanged));
    quick::clearHistory(); check(quick::load().history.empty());
    // An optional helper may only modify its own Run value, never other startup apps.
    constexpr auto run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t other[] = L"existing application";
    RegSetKeyValueW(HKEY_CURRENT_USER, run, L"OtherProgram", REG_SZ, other, sizeof(other));
    quick::configureStartup(L"C:\\Program Files\\FolderNote\\FolderNote.exe", true);
    wchar_t command[256]{}; bytes = sizeof(command);
    check(RegGetValueW(HKEY_CURRENT_USER, run, L"FolderNote.QuickFolders", RRF_RT_REG_SZ, nullptr, command, &bytes) == 0 &&
          !wcscmp(command, L"\"C:\\Program Files\\FolderNote\\FolderNote.exe\" --quick-helper"));
    quick::configureStartup({}, false); quick::configureStartup({}, false);
    bytes = sizeof(command);
    check(RegGetValueW(HKEY_CURRENT_USER, run, L"FolderNote.QuickFolders", RRF_RT_ANY, nullptr, command, &bytes) == ERROR_FILE_NOT_FOUND);
    bytes = sizeof(command);
    check(RegGetValueW(HKEY_CURRENT_USER, run, L"OtherProgram", RRF_RT_REG_SZ, nullptr, command, &bytes) == 0 && !wcscmp(command, other));
    // Navigation guard must reject filename fields and unrelated dialogs.
    HWND dialog = CreateWindowW(L"#32770", L"fixture", WS_OVERLAPPED, 0, 0, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(dialog != nullptr && !quick::isFileDialog(dialog));
    HWND filename = CreateWindowW(L"EDIT", L"keep-me.txt", WS_CHILD, 0, 0, 100, 20, dialog, reinterpret_cast<HMENU>(1148), GetModuleHandleW(nullptr), nullptr);
    HWND address = CreateWindowW(L"EDIT", L"C:\\", WS_CHILD, 0, 30, 100, 20, dialog, reinterpret_cast<HMENU>(41477), GetModuleHandleW(nullptr), nullptr);
    check(!quick::addressEdit(filename, dialog)); check(quick::addressEdit(address, dialog));
    check(!quick::addressEdit(address, filename));
    DestroyWindow(dialog);
    std::cout << "PASS " << checks << " quick folder checks: MRU/dedup, pins/order/limits, concurrent updates, Unicode/UNC, corrupt data, opt-in startup, filename protection\n";
  } catch (const fn::Error &error) { std::wcerr << error.message << L'\n'; return 1; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  CoUninitialize(); return 0;
}
