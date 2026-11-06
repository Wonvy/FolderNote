#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
namespace fn {
namespace fs = std::filesystem;
struct Error {
  std::wstring message;
};
[[noreturn]] void fail(const std::wstring &what, DWORD code = GetLastError());
std::wstring readText(const fs::path &file);
std::wstring note(const fs::path &folder);
std::wstring normalize(std::wstring value);
void save(const fs::path &folder, const std::wstring &value);
enum class Status { Repaired, Normal, MissingFile, MissingNote };
Status repair(const fs::path &folder);
void notify(const fs::path &folder);
fs::path modulePath(HMODULE module = nullptr);
fs::path localRoot();
std::wstring makeRequest(int op, const std::vector<std::wstring> &paths);
void readRequest(const std::wstring &token, int &op,
                 std::vector<std::wstring> &paths);
inline const wchar_t *ids[] = {L"{7BA54C64-5E92-4C99-AD47-06DD67193401}",
                               L"{7BA54C64-5E92-4C99-AD47-06DD67193402}",
                               L"{7BA54C64-5E92-4C99-AD47-06DD67193403}"};
inline constexpr wchar_t settingsId[] = L"{7BA54C64-5E92-4C99-AD47-06DD67193404}";
inline const wchar_t *verbs[] = {L"FolderNote.Edit", L"FolderNote.Clear",
                                 L"FolderNote.Repair"};
inline const wchar_t *titles[] = {L"添加/编辑备注", L"清除备注",
                                  L"修复文件夹备注"};
} // namespace fn
