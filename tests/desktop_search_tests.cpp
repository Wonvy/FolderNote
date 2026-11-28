#include "../src/desktop_groups_search.h"
#include "../src/core.h"
#include "../src/quick_find.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>

namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::optional<desktop_groups::search::Entry> received;
LRESULT CALLBACK receiver(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message != WM_COPYDATA) return DefWindowProcW(window, message, w, l);
  const auto &data = *reinterpret_cast<const COPYDATASTRUCT *>(l);
  if (!desktop_groups::search::sameApplication(reinterpret_cast<HWND>(w)) ||
      data.dwData != desktop_groups::search::copyDataCommand || data.cbData % sizeof(wchar_t)) return FALSE;
  received = desktop_groups::search::decode({static_cast<const wchar_t *>(data.lpData), data.cbData / sizeof(wchar_t)});
  return received.has_value();
}
}
int main() {
  using namespace desktop_groups;
  namespace fs = std::filesystem;
  const auto parent = fs::temp_directory_path();
  const auto root = parent / (L"ShibaKitSearchTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  int result = 0;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  try {
    check(fs::create_directory(root), "create isolated search fixture");
    const auto a = root / L"A", b = root / L"B", mapped = root / L"映射";
    fs::create_directory(a); fs::create_directory(b); fs::create_directory(mapped);
    fs::create_directory(mapped / L"子目录");
    const auto first = a / L"素材.png", second = b / L"素材.png";
    for (const auto &file : {first, second, mapped / L"海报.jpg", mapped / L"隐藏.txt", mapped / L"子目录" / L"深处.txt"}) std::ofstream(file) << "fixture";
    SetFileAttributesW((mapped / L"隐藏.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
    Group manual{L"工作", {0,0,300,300}, {first.native(), (a / L"missing.txt").native()}, true};
    manual.tabs.push_back({L"收藏", {}, {second.native()}});
    Group mapping{L"图片", {300,0,600,300}}; mapping.folder = mapped.native();
    const std::vector<Group> groups{manual, mapping};
    const auto entries = search::collect(groups);
    check(entries.size() == 4, "collect all manual tabs and direct mapped children, excluding hidden and missing files");
    check(entries[0].path == first.native() && entries[1].path == second.native() && entries[0].label != entries[1].label,
          "same-name files retain distinct locations and source labels");
    check(entries[0].group == 0 && entries[0].tab == 0 && entries[1].group == 0 && entries[1].tab == 1,
          "candidate identity points to the correct box and tab");
    check(quick_find::matches(entries[0].name, L"sc") && quick_find::matches(entries[1].name, L"sc"), "box files support Chinese initials");
    check(std::none_of(entries.begin(), entries.end(), [](const auto &item) { return item.name == L"深处.txt"; }), "search never recursively walks folder contents");
    check(groups[0].collapsed && groups[0].paths.size() == 2 && search::collect(groups, [] { return true; }).empty() &&
          search::collect(groups, {}, 1).size() == 1, "indexing preserves layout and honors cancellation and result limits");
    for (const auto &entry : entries) {
      check(search::matches(groups, entry), "every indexed entry validates against its original box");
      const auto payload = search::encode(entry);
      const auto decoded = search::decode(payload);
      check(decoded && decoded->path == entry.path && decoded->group == entry.group && decoded->tab == entry.tab &&
            search::matches(groups, *decoded), "Unicode paths and candidate identity survive IPC serialization");
      auto truncated = payload; truncated.pop_back();
      check(!search::decode(truncated), "reject a truncated IPC request");
      auto extended = payload; extended.push_back(L'\0');
      check(!search::decode(extended), "reject extra IPC fields");
    }
    auto stale = entries[0]; stale.groupName = L"renamed";
    check(!search::matches(groups, stale), "renaming a box invalidates stale candidates");
    stale = entries[0]; stale.path = second.native();
    check(!search::matches(groups, stale), "a same-name file in another tab cannot be selected accidentally");
    stale = entries[2]; stale.path = (mapped / L"子目录" / L"深处.txt").native();
    check(!search::matches(groups, stale), "mapped selection only accepts a direct child");
    stale.path = (mapped / L".." / L"A" / L"素材.png").native();
    check(!search::matches(groups, stale), "reject traversal outside a mapped folder");
    check(!search::decode(std::wstring(search::requestLimit + 1, L'x')), "bound IPC size");
    const auto layoutFile = root / L"layout.json";
    saveLayout({groups, {}}, layoutFile);
    const auto before = fn::readText(layoutFile);
    check(readLayout(layoutFile).groups.size() == 2 && fn::readText(layoutFile) == before, "reading candidates never rewrites the layout");
    const auto absent = root / L"absent.json";
    check(readLayout(absent).groups.empty() && !fs::exists(absent), "missing layout is not created or migrated by search");
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = receiver; cls.lpszClassName = L"ShibaKit.SearchTests";
    RegisterClassW(&cls);
    const auto window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    check(window && search::locate(entries[1], window) && received && received->path == second.native() && received->tab == 1,
          "actual bounded IPC delivers the requested file to an isolated receiver without contacting the user's boxes");
    DestroyWindow(window); UnregisterClassW(cls.lpszClassName, cls.hInstance);
    std::cout << "PASS desktop box index, matching, tab identity, read-only layout, validation and IPC\n";
  } catch (const fn::Error &) { std::cerr << "Model error\n"; result = 1; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  std::error_code error;
  if (root.parent_path() == parent && root.filename().wstring().starts_with(L"ShibaKitSearchTests-")) fs::remove_all(root, error);
  CoUninitialize();
  return result;
}
