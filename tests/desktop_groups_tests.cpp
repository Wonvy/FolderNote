#include "../src/core.h"
#include "../src/desktop_groups_model.h"
#include "../src/desktop_groups_render.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
constexpr wchar_t settings[] = L"Software\\FolderNote";
constexpr wchar_t value[] = L"DesktopGroups";

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void exerciseShadow() {
  using namespace desktop_groups;
  using Region = std::unique_ptr<std::remove_pointer_t<HRGN>, decltype(&DeleteObject)>;
  constexpr int boxWidth = 71, boxHeight = 30, spread = 12;
  constexpr int width = boxWidth + 2 * spread, height = boxHeight + 2 * spread;
  std::vector<DWORD> pixels(width * height);
  for (const int diameter : {0, 19, 60}) {
    const Region box(createBoxRegion(boxWidth, boxHeight, diameter), DeleteObject);
    check(box != nullptr, "create the desktop box region");
    check(PtInRegion(box.get(), boxWidth - 1, boxHeight / 2) &&
          PtInRegion(box.get(), boxWidth / 2, boxHeight - 1) &&
          !PtInRegion(box.get(), boxWidth, boxHeight / 2) &&
          !PtInRegion(box.get(), boxWidth / 2, boxHeight),
          "keep the final row and column inside the window bounds");
    for (const int offset : {0, 3, 20}) {
      check(drawBoxShadow(pixels.data(), width, height, spread, offset, diameter, 180),
            "render the shadow at different offsets");
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          if (PtInRegion(box.get(), x - spread, y - spread + offset))
            check(pixels[y * width + x] == 0, "shadow must leave the box interior transparent");
        }
      }
      const int middle = spread + boxWidth / 2;
      const int bottom = spread + boxHeight;
      check(pixels[bottom * width + middle] > pixels[(height - 1) * width + middle],
            "shadow remains visible outside the box and fades away from its edge");
    }
  }
}

struct TemporaryFiles {
  fn::fs::path root = fn::fs::temp_directory_path() /
      (L"FolderNoteDesktopGroups-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

  TemporaryFiles() { check(fn::fs::create_directory(root), "create temporary files directory"); }
  ~TemporaryFiles() {
    std::error_code error;
    fn::fs::remove_all(root, error);
  }
};

struct TemporaryRegistry {
  HKEY user = nullptr, key = nullptr;
  std::wstring path;
  bool redirected = false;

  ~TemporaryRegistry() {
    if (redirected) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (key) {
      RegCloseKey(key);
      RegDeleteTreeW(user, path.c_str());
    }
    if (user) RegCloseKey(user);
  }

  void redirect(const std::wstring &name) {
    path = L"Software\\" + name;
    DWORD disposition = 0;
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &user) == ERROR_SUCCESS, "open test registry");
    HKEY temporary = nullptr;
    const auto result = RegCreateKeyExW(user, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                                        &temporary, &disposition);
    check(result == ERROR_SUCCESS, "create test registry");
    if (disposition != REG_CREATED_NEW_KEY) {
      RegCloseKey(temporary);
      throw std::runtime_error("test registry already exists");
    }
    key = temporary;
    check(RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS, "redirect test registry");
    redirected = true;
  }
};

void exercise(const fn::fs::path &root) {
  using namespace desktop_groups;
  const auto layout = root / L"settings" / L"DesktopGroups.json";
  const auto readFile = [](const fn::fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  const auto personal = root / L"Desktop";
  const auto shared = root / L"Public";
  const auto folder = personal / L"文件夹";
  fn::fs::create_directories(folder);
  fn::fs::create_directory(shared);
  const auto document = personal / L"简报.txt";
  const auto sharedDocument = shared / L"Public.txt";
  const auto systemFile = personal / L"desktop.ini";
  const auto child = folder / L"nested.txt";
  const auto outside = root / L"outside.txt";
  for (const auto &path : {document, sharedDocument, systemFile, child, outside})
    std::ofstream(path) << "contents";

  const std::vector<std::wstring> roots{personal.native(), shared.native()};
  const auto documentPath = desktopItem(roots, (personal / L"简报.TXT").native());
  const auto sharedPath = desktopItem(roots, sharedDocument.native());
  check(documentPath == document.native() && sharedPath == sharedDocument.native(), "resolve both desktops");
  check(desktopItem(roots, folder.native()) == folder.native(), "accept desktop folders");
  for (const auto &path : {outside, child, systemFile, personal / L"missing.txt"})
    check(desktopItem(roots, path.native()).empty(), "reject non-desktop or missing items");
  check(desktopItem(roots, (personal / L"简报.txt:stream").native()).empty(), "reject alternate streams");

  check(load(layout).empty() && !fn::fs::exists(layout), "missing settings load as empty");
  std::vector<Group> groups{
      {L"工作 \"😀\"\\", {-450, 10, -110, 320}, {}, true, {{L"常用", {}, {}}, {L"映射", folder.native(), {}}},
       View::List, Sort::Modified, 64, 72, RGB(24, 60, 96), 49, TitleStyle::IconAndText, L"C:\\Box.ico", DisplayMode::Pages},
      {L"共享", {20, 40, 320, 340}, {}, false, {{L"资料", {}, {}}}, View::Icons, Sort::Manual}};
  groups[0].itemSpacing = 14;
  check(addItem(groups, 0, documentPath), "add a document");
  check(!addItem(groups, 0, (personal / L"简报.TXT").native()), "deduplicate case-insensitively");
  check(addItem(groups, 1, sharedPath) && addItem(groups, 1, documentPath), "add and move between groups");
  check(groups[0].paths.empty() && groups[1].paths == std::vector<std::wstring>{sharedPath, documentPath},
        "maintain exclusive membership and order");
  check(addItem(groups, 1, documentPath, 1) && groups[1].paths == std::vector<std::wstring>{sharedPath},
        "move a desktop item into another tab in the same group");
  check(addItem(groups, 0, documentPath, 1) && groups[1].tabs[0].paths.empty(),
        "move a desktop item across groups and tabs");
  check(!addItem(groups, 0, documentPath, 1) && !addItem(groups, 0, documentPath, 2) &&
        !addItem(groups, 0, documentPath, 3), "reject duplicates, mapped tabs and invalid indices");
  save(groups, layout);
  const auto json = readFile(layout);
  check(json.starts_with("{") && json.find("\"version\": 1") != json.npos &&
        json.find("\"iconSize\": 64") != json.npos &&
        json.find("\"sort\": \"modified\"") != json.npos && json.find("工作") != json.npos,
        "write a versioned UTF-8 JSON file");
  constexpr std::string_view displayModeLine = "      \"displayMode\": \"pages\",\n";
  const auto displayModeOffset = json.find(displayModeLine);
  check(displayModeOffset != json.npos && json.find("\"displayMode\"", displayModeOffset + displayModeLine.size()) == json.npos,
        "write display mode only for the paged box");
  constexpr std::string_view spacingLine = "      \"itemSpacing\": 14,\n";
  const auto spacingOffset = json.find(spacingLine);
  check(spacingOffset != json.npos && json.find("\"itemSpacing\"", spacingOffset + spacingLine.size()) == json.npos,
        "write spacing only for the box with extra spacing");
  fn::fs::remove(sharedDocument);
  const auto loaded = load(layout);
  check(loaded.size() == 2 && loaded[0].name == groups[0].name && loaded[1].name == groups[1].name &&
        EqualRect(&loaded[0].bounds, &groups[0].bounds) && EqualRect(&loaded[1].bounds, &groups[1].bounds) &&
        loaded[0].collapsed && !loaded[1].collapsed &&
        loaded[0].view == View::List && loaded[0].sort == Sort::Modified &&
        loaded[1].view == View::Icons && loaded[1].sort == Sort::Manual &&
        loaded[0].displayMode == DisplayMode::Pages && loaded[1].displayMode == DisplayMode::Scroll &&
        loaded[0].itemSpacing == 14 && loaded[1].itemSpacing == 0 &&
        loaded[0].iconSize == 64 && loaded[1].iconSize == defaultIconSize &&
        loaded[0].opacity == 72 && loaded[0].tint == RGB(24, 60, 96) &&
        loaded[0].titleHeight == 49 && loaded[0].titleStyle == TitleStyle::IconAndText &&
        loaded[0].titleIcon == L"C:\\Box.ico" &&
        !loaded[1].opacity && !loaded[1].tint && loaded[1].titleHeight == 36 &&
        loaded[1].titleStyle == TitleStyle::Text && loaded[1].titleIcon.empty() &&
        loaded[0].paths.empty() && loaded[1].paths == groups[1].paths,
        "persist group order, geometry, collapse state, display preferences and missing references");
  check(loaded[0].tabs.size() == 2 && loaded[0].tabs[0].name == L"常用" && loaded[0].tabs[0].folder.empty() &&
        loaded[0].tabs[0].paths == std::vector<std::wstring>{documentPath} &&
        loaded[0].tabs[1].name == L"映射" && loaded[0].tabs[1].folder == folder.native() &&
        loaded[0].tabs[1].paths.empty(), "persist ordered manual and mapped tabs");
  check(fn::fs::exists(document) && fn::fs::exists(folder), "grouping leaves files in place");

  const std::string iconSizeLine = "      \"iconSize\": 64,\n";
  const auto iconSizeOffset = json.find(iconSizeLine);
  check(iconSizeOffset != json.npos, "locate icon size in JSON");
  auto oldJson = json;
  oldJson.erase(spacingOffset, spacingLine.size());
  oldJson.erase(iconSizeOffset, iconSizeLine.size());
  oldJson.erase(displayModeOffset, displayModeLine.size());
  constexpr std::string_view looseItemsLine = ",\n  \"looseItems\": []";
  const auto looseItemsOffset = oldJson.find(looseItemsLine);
  check(looseItemsOffset != oldJson.npos, "locate loose items in JSON");
  oldJson.erase(looseItemsOffset, looseItemsLine.size());
  const auto oldLayout = root / L"old.json";
  std::ofstream(oldLayout, std::ios::binary) << oldJson;
  const auto old = loadLayout(oldLayout);
  check(old.groups[0].iconSize == defaultIconSize && old.groups[0].displayMode == DisplayMode::Scroll &&
        old.groups[0].itemSpacing == 0 && old.looseItems.empty(), "load older JSON with defaults");

  auto badJson = json;
  badJson.replace(iconSizeOffset, iconSizeLine.size(), "      \"iconSize\": 50,\n");
  const auto badSizeLayout = root / L"bad-icon-size.json";
  std::ofstream(badSizeLayout, std::ios::binary) << badJson;
  bool rejected = false;
  try { load(badSizeLayout); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && readFile(badSizeLayout) == badJson, "reject unsupported icon size without rewriting JSON");

  groups[0].paths = {documentPath};
  rejected = false;
  try { save(groups, layout); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && readFile(layout) == json, "reject duplicate membership across tabs without overwriting layout");

  groups = loaded;
  groups[0].tabs[0].paths.clear();
  groups[0].tabs[1].paths = {documentPath};
  rejected = false;
  try { save(groups, layout); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && load(layout)[0].tabs[1].paths.empty(), "reject manual membership in mapped tab");

  groups = loaded;
  groups[0].bounds = {std::numeric_limits<LONG>::min(), 0, std::numeric_limits<LONG>::max(), 200};
  rejected = false;
  try { save(groups, layout); }
  catch (const fn::Error &) { rejected = true; }
  const auto unchangedBounds = load(layout)[0].bounds;
  check(rejected && EqualRect(&unchangedBounds, &loaded[0].bounds), "reject overflowing geometry");

  // A version 1 group uses the same geometry and strings, with no collapse field.
  const std::uint16_t legacy[] = {1, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 1, 0, L'A', 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, legacy, sizeof(legacy)) == ERROR_SUCCESS,
        "seed a version 1 layout");
  check(load(layout)[0].name == loaded[0].name, "prefer existing JSON over legacy registry");
  const auto legacyFile = root / L"version1.json";
  const auto legacyLayout = load(legacyFile);
  const RECT legacyBounds{5, 7, 405, 307};
  check(legacyLayout.size() == 1 && legacyLayout[0].name == L"A" && !legacyLayout[0].collapsed &&
        legacyLayout[0].tabs.empty() && legacyLayout[0].view == View::Icons && legacyLayout[0].sort == Sort::Manual &&
        EqualRect(&legacyLayout[0].bounds, &legacyBounds) &&
        load(legacyFile)[0].name == L"A", "migrate version 1 groups into JSON with default settings");
  std::uint16_t oldRegistry[ARRAYSIZE(legacy)]{};
  DWORD oldBytes = sizeof(oldRegistry);
  check(RegGetValueW(HKEY_CURRENT_USER, settings, value, RRF_RT_REG_BINARY, nullptr,
                     oldRegistry, &oldBytes) == ERROR_SUCCESS && oldBytes == sizeof(legacy) &&
        std::equal(std::begin(oldRegistry), std::end(oldRegistry), std::begin(legacy)),
        "retain the original legacy registry data after migration");

  const std::uint16_t version2[] = {2, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 1, 0, 1, 0, L'B', 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, version2, sizeof(version2)) == ERROR_SUCCESS,
        "seed a version 2 layout");
  const auto version2Layout = load(root / L"version2.json");
  check(version2Layout.size() == 1 && version2Layout[0].name == L"B" && version2Layout[0].collapsed &&
        version2Layout[0].tabs.empty() && version2Layout[0].view == View::Icons &&
        version2Layout[0].sort == Sort::Manual, "preserve version 2 collapse state with default tabs and display");
  const std::uint16_t version3[] = {
      3, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 1, 0, 1, 0, L'C', 0, 0,
      1, 0, 3, 0, 1, 0, 1, 0, L'D', 0, 0, 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, version3, sizeof(version3)) == ERROR_SUCCESS,
        "seed a version 3 layout");
  const auto version3File = root / L"version3.json";
  const auto version3Layout = load(version3File);
  check(version3Layout.size() == 1 && version3Layout[0].name == L"C" && version3Layout[0].collapsed &&
        version3Layout[0].tabs.size() == 1 && version3Layout[0].tabs[0].name == L"D" &&
        version3Layout[0].view == View::List && version3Layout[0].sort == Sort::Modified,
        "preserve version 3 tabs and display options");
  check(load(legacyFile)[0].name == L"A", "do not overwrite migrated files with later registry data");
  const auto broken = root / L"broken.json";
  std::ofstream(broken, std::ios::binary) << "{\"version\":99,\"groups\":[]}";
  rejected = false;
  try { load(broken); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && readFile(broken) == "{\"version\":99,\"groups\":[]}",
        "leave unsupported JSON intact and do not import legacy over it");

  auto changed = loaded;
  changed[0].name = L"Edited";
  changed[0].opacity = 0;
  save(changed, layout);
  const auto edited = load(layout);
  check(edited[0].name == L"Edited" && readFile(layout) != json,
        "atomically replace the layout file");
  check(edited[0].opacity == 0 && !edited[1].opacity,
        "preserve explicit zero background opacity separately from the default");

  const auto looseFile = root / L"loose.json";
  Layout withLoose{loaded, {{folder.native(), {-820, 320}}, {(personal / L"其他.txt").native(), {800, -100}}}};
  saveLayout(withLoose, looseFile);
  const auto looseLayout = loadLayout(looseFile);
  check(looseLayout.groups.size() == loaded.size() && looseLayout.looseItems.size() == 2 &&
        looseLayout.looseItems[0].path == folder.native() && looseLayout.looseItems[0].position.x == -820 &&
        looseLayout.looseItems[0].position.y == 320 && looseLayout.looseItems[1].position.y == -100,
        "preserve loose desktop paths and virtual screen coordinates");
  withLoose.looseItems.push_back({documentPath, {}});
  rejected = false;
  try { saveLayout(withLoose, looseFile); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && loadLayout(looseFile).looseItems.size() == 2, "reject items both loose and grouped");
  withLoose.looseItems.back().path = (personal / L"其他.TXT").native();
  rejected = false;
  try { saveLayout(withLoose, looseFile); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && loadLayout(looseFile).looseItems.size() == 2, "reject duplicate loose paths");
  auto oldApiGroups = looseLayout.groups;
  oldApiGroups[0].paths.push_back(folder.native());
  save(oldApiGroups, looseFile);
  const auto updatedLoose = loadLayout(looseFile);
  check(updatedLoose.groups[0].paths == std::vector<std::wstring>{folder.native()} &&
        updatedLoose.looseItems.size() == 1 && updatedLoose.looseItems[0].path == looseLayout.looseItems[1].path,
        "preserve unaffected loose placements when saving through the group-only API");

  std::vector<Group> stack = loaded;
  stack.insert(stack.begin(), Group{});
  stack[1].paths.push_back(folder.native());
  check(!mergeGroups(stack, 0, 2) && !mergeGroups(stack, 1, 0) && !mergeGroups(stack, 1, 1) &&
        !mergeGroups(stack, 1, 3) && stack.size() == 3, "reject invalid merge and synthetic desktop");
  check(mergeGroups(stack, 1, 2) && stack.size() == 2 &&
        stack[1].name == loaded[1].name && stack[1].paths == loaded[1].paths && stack[1].tabs.size() == 4 &&
        stack[1].tabs[1].name == loaded[0].name && stack[1].tabs[1].paths == std::vector<std::wstring>{folder.native()} &&
        stack[1].tabs[2].paths == std::vector<std::wstring>{documentPath} &&
        stack[1].tabs[3].folder == folder.native(), "merge default, manual and mapped tabs in order");
  stack[1].view = View::List;
  stack[1].sort = Sort::Modified;
  stack[1].iconSize = 64;
  stack[1].displayMode = DisplayMode::Pages;
  stack[1].itemSpacing = maxItemSpacing;
  const RECT detachedBounds{-500, 40, -110, 370};
  check(!detachTab(stack, 0, 1, detachedBounds) && !detachTab(stack, 1, 0, detachedBounds) &&
        !detachTab(stack, 1, 5, detachedBounds) && !detachTab(stack, 1, 1, {}),
        "reject invalid detach and synthetic desktop");
  check(detachTab(stack, 1, 4, detachedBounds) && stack.size() == 3 && stack[1].tabs.size() == 3 &&
        stack.back().name == L"映射" && stack.back().paths.empty() && stack.back().tabs.size() == 1 &&
        stack.back().tabs[0].folder == folder.native() && EqualRect(&stack.back().bounds, &detachedBounds),
        "detach mapped tab without flattening its contents");
  check(detachTab(stack, 1, 3, detachedBounds) && stack.size() == 4 && stack[1].tabs.size() == 2 &&
        stack.back().name == L"常用" && stack.back().paths == std::vector<std::wstring>{documentPath} &&
        stack.back().tabs.empty() && stack.back().view == View::List && stack.back().sort == Sort::Modified &&
        stack.back().iconSize == 64 && stack.back().displayMode == DisplayMode::Pages &&
        stack.back().itemSpacing == maxItemSpacing && !stack.back().collapsed,
        "detach manual tab and retain display preferences");
  stack.erase(stack.begin());
  const auto stackFile = root / L"stack.json";
  save(stack, stackFile);
  check(load(stackFile).size() == 3, "roundtrip resulting group layout");

  const auto sourceTint = RGB(0, 0, 0), hostTint = RGB(70, 92, 114);
  std::vector<Group> nested{
      {},
      {L"Original", detachedBounds, {}, false, {{L"Original extra", {}, {}}, {L"Mapped", folder.native(), {}}}},
      {L"Default", detachedBounds, {}, false, {{L"Default extra", {}, {}}}},
      {L"Host", detachedBounds, {}, false, {{L"Host extra", {}, {}}}}};
  nested[1].tint = sourceTint;
  nested[3].tint = hostTint;
  check(mergeGroups(nested, 1, 2) && mergeGroups(nested, 1, 2) && nested[1].tint == hostTint,
        "merging keeps the host color with nested source tabs attached");
  const auto nestedFile = root / L"nested-tints.json";
  save({nested[1]}, nestedFile);
  const auto nestedJson = readFile(nestedFile);
  check(nestedJson.find("\"detachedTint\": null") != nestedJson.npos &&
        nestedJson.find("\"detachedTint\": 0") != nestedJson.npos,
        "persist theme default and custom black distinctly");
  nested[1] = load(nestedFile).front();
  nested[1].tint = RGB(105, 85, 64);
  const auto checkDetachedTint = [&](size_t tab, const wchar_t *name, std::optional<COLORREF> tint) {
    check(detachTab(nested, 1, tab, detachedBounds) && nested.back().name == name && nested.back().tint == tint,
          "detaching restores original tint or inherits the host color for a regular tab");
  };
  checkDetachedTint(6, L"Mapped", sourceTint);
  check(nested.back().tabs[0].folder == folder.native() && !nested.back().tabs[0].detachedTint,
        "detaching a mapped tab consumes its original color");
  checkDetachedTint(5, L"Original extra", sourceTint);
  checkDetachedTint(4, L"Original", sourceTint);
  checkDetachedTint(3, L"Default extra", std::nullopt);
  checkDetachedTint(2, L"Default", std::nullopt);
  checkDetachedTint(1, L"Host extra", nested[1].tint);

  const std::uint16_t invalidState[] = {2, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 2, 0, 1, 0, L'A', 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, invalidState,
                        sizeof(invalidState)) == ERROR_SUCCESS, "seed an invalid collapse state");
  rejected = false;
  try { load(root / L"invalid-state.json"); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected, "reject invalid collapse state");

  const DWORD unsupported[] = {0xFFFFFFFF, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, unsupported,
                        sizeof(unsupported)) == ERROR_SUCCESS, "seed an unsupported layout version");
  rejected = false;
  try { load(root / L"unsupported-legacy.json"); }
  catch (const fn::Error &) { rejected = true; }
  DWORD unchanged[2]{}, bytes = sizeof(unchanged);
  check(rejected && RegGetValueW(HKEY_CURRENT_USER, settings, value, RRF_RT_REG_BINARY, nullptr,
                                unchanged, &bytes) == ERROR_SUCCESS &&
        unchanged[0] == unsupported[0], "reject unsupported layout without clearing it");
}
}

int main() {
  try {
    const TemporaryFiles files;
    TemporaryRegistry registry;
    registry.redirect(files.root.filename().native());
    exercise(files.root);
    exerciseShadow();
  } catch (const fn::Error &error) {
    std::wcerr << error.message << L'\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
