#pragma once
#include <filesystem>
#include <initializer_list>
#include <string>

namespace lang {
enum class Choice : unsigned { Chinese, English, Custom };
struct Settings { Choice choice; std::wstring customName; };
const wchar_t *text(const wchar_t *source) noexcept;
std::wstring format(const wchar_t *source, std::initializer_list<std::wstring> values);
Settings settings();
void select(Choice choice);
void importFile(const std::filesystem::path &path);
void exportFile(const std::filesystem::path &path);
}
