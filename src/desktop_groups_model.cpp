#include "desktop_groups_model.h"
#include "core.h"
#include "language.h"
#include "quick_folders.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <roapi.h>
#include <set>
#include <shlobj.h>
#include <shlwapi.h>
#include <span>
#include <string_view>
#include <windows.data.json.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

namespace desktop_groups {
namespace {
using namespace ABI::Windows::Data::Json;
using ABI::Windows::Foundation::Collections::IMap;
using ABI::Windows::Foundation::Collections::IVector;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;

constexpr wchar_t settingsKey[] = L"Software\\FolderNote";
constexpr wchar_t groupsValue[] = L"DesktopGroups";
constexpr wchar_t readError[] = L"无法读取桌面盒子";
constexpr wchar_t writeError[] = L"无法保存桌面盒子";
constexpr DWORD legacyVersion = 3;
constexpr LONG jsonVersion = 1;
constexpr DWORD legacySizeLimit = 1024 * 1024, jsonSizeLimit = 4 * 1024 * 1024;
constexpr size_t pathLimit = 32760;
constexpr std::array viewNames{L"icons", L"list"};
constexpr std::array sortNames{L"manual", L"name", L"type", L"modified"};
constexpr std::array displayModeNames{L"scroll", L"pages"};
static_assert(sizeof(DWORD) == 4 && sizeof(RECT) == 16 && sizeof(wchar_t) == 2);

struct PathOrder {
  bool operator()(std::wstring_view a, std::wstring_view b) const {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  }
};

bool validBounds(const RECT &bounds) {
  const auto width = std::int64_t(bounds.right) - bounds.left;
  const auto height = std::int64_t(bounds.bottom) - bounds.top;
  return width > 0 && height > 0 && width <= std::numeric_limits<int>::max() &&
         height <= std::numeric_limits<int>::max();
}

bool unsetRect(const RECT &bounds) {
  return bounds.left == 0 && bounds.top == 0 && bounds.right == 0 && bounds.bottom == 0;
}

using PathSet = std::set<std::wstring_view, PathOrder>;

void validateGroups(const std::vector<Group> &groups, PathSet &paths, const wchar_t *message) {
  const auto validName = [](const std::wstring &name) {
    return !name.empty() && name.size() <= groupNameLimit && name.find(L'\0') == std::wstring::npos;
  };
  const auto validDetachedTint = [](const std::optional<std::optional<COLORREF>> &tint) {
    return !tint || !*tint || **tint <= 0x00ffffff;
  };
  const auto validatePaths = [&](const std::vector<std::wstring> &entries) {
    for (const auto &path : entries) {
      if (path.empty() || (quick::normalize(path) != path && !isDesktopShellItem(path)) || !paths.insert(path).second)
        fn::fail(lang::text(message), ERROR_INVALID_DATA);
    }
  };
  for (const auto &group : groups) {
    if (!validName(group.name) || std::find(iconSizes.begin(), iconSizes.end(), group.iconSize) == iconSizes.end() ||
        (group.opacity && (*group.opacity < 0 || *group.opacity > 100)) ||
        group.blurStrength < 0 || group.blurStrength > 100 ||
        (group.tint && *group.tint > 0x00ffffff) || !validDetachedTint(group.primaryDetachedTint) ||
        group.titleHeight < 28 || group.titleHeight > 80 ||
        group.titleStyle < TitleStyle::Text || group.titleStyle > TitleStyle::Hidden ||
        group.titleIcon.find(L'\0') != std::wstring::npos ||
        group.autoRule.size() > 512 || group.autoRule.find(L'\0') != std::wstring::npos ||
        (!unsetRect(group.referenceWorkArea) && !validBounds(group.referenceWorkArea)) ||
        group.view < View::Icons || group.view > View::List ||
        group.displayMode < DisplayMode::Scroll || group.displayMode > DisplayMode::Pages ||
        group.itemSpacing < 0 || group.itemSpacing > maxItemSpacing ||
        (!group.folder.empty() && (quick::normalize(group.folder) != group.folder || !group.paths.empty())) ||
        group.sort < Sort::Manual || group.sort > Sort::Modified || !validBounds(group.bounds))
      fn::fail(lang::text(message), ERROR_INVALID_DATA);
    validatePaths(group.paths);
    for (const auto &tab : group.tabs) {
      if (!validName(tab.name) || !validDetachedTint(tab.detachedTint) ||
          (!tab.folder.empty() && (quick::normalize(tab.folder) != tab.folder || !tab.paths.empty())))
        fn::fail(lang::text(message), ERROR_INVALID_DATA);
      validatePaths(tab.paths);
    }
  }
}

void validate(const Layout &layout, const wchar_t *message) {
  PathSet paths;
  validateGroups(layout.groups, paths, message);
  for (const auto &item : layout.looseItems) {
    if (item.path.empty() || (quick::normalize(item.path) != item.path && !isDesktopShellItem(item.path)) ||
        !paths.insert(item.path).second)
      fn::fail(lang::text(message), ERROR_INVALID_DATA);
  }
}

struct Reader {
  std::span<const BYTE> remaining;

  void read(void *destination, size_t size) {
    if (size > remaining.size()) fn::fail(lang::text(readError), ERROR_INVALID_DATA);
    if (size) std::memcpy(destination, remaining.data(), size);
    remaining = remaining.subspan(size);
  }

  DWORD number() {
    DWORD value = 0;
    read(&value, sizeof(value));
    return value;
  }

  std::wstring string(size_t limit) {
    const auto count = number();
    if (count > limit || count > remaining.size() / sizeof(wchar_t))
      fn::fail(lang::text(readError), ERROR_INVALID_DATA);
    std::wstring value(count, L'\0');
    read(value.data(), count * sizeof(wchar_t));
    return value;
  }

  DWORD count(size_t minimumSize) {
    const auto value = number();
    if (value > remaining.size() / minimumSize) fn::fail(lang::text(readError), ERROR_INVALID_DATA);
    return value;
  }

  std::vector<std::wstring> paths() {
    const auto itemCount = count(sizeof(DWORD));
    std::vector<std::wstring> result;
    result.reserve(itemCount);
    for (DWORD i = 0; i < itemCount; ++i) result.push_back(string(pathLimit));
    return result;
  }
};

std::optional<Layout> loadLegacy() {
  DWORD size = 0;
  auto result = RegGetValueW(HKEY_CURRENT_USER, settingsKey, groupsValue, RRF_RT_REG_BINARY, nullptr, nullptr, &size);
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
  if (result != ERROR_SUCCESS) fn::fail(lang::text(readError), result);
  if (size > legacySizeLimit || size < 2 * sizeof(DWORD)) fn::fail(lang::text(readError), ERROR_INVALID_DATA);

  std::vector<BYTE> bytes(size);
  result = RegGetValueW(HKEY_CURRENT_USER, settingsKey, groupsValue, RRF_RT_REG_BINARY, nullptr, bytes.data(), &size);
  if (result != ERROR_SUCCESS) fn::fail(lang::text(readError), result);
  Reader reader{std::span<const BYTE>(bytes.data(), size)};
  const auto version = reader.number();
  if (version == 0 || version > legacyVersion)
    fn::fail(lang::text(readError), ERROR_REVISION_MISMATCH);
  const bool hasCollapseState = version >= 2;
  const bool hasTabs = version >= 3;
  const auto minimumGroupSize = sizeof(RECT) + (2 + hasCollapseState + (hasTabs ? 3 : 0)) * sizeof(DWORD);
  const auto count = reader.count(minimumGroupSize);

  Layout layout;
  auto &groups = layout.groups;
  groups.reserve(count);
  for (DWORD i = 0; i < count; ++i) {
    Group group;
    reader.read(&group.bounds, sizeof(group.bounds));
    if (hasCollapseState) {
      const auto collapsed = reader.number();
      if (collapsed > 1) fn::fail(lang::text(readError), ERROR_INVALID_DATA);
      group.collapsed = collapsed == 1;
    }
    group.name = reader.string(groupNameLimit);
    group.paths = reader.paths();
    if (hasTabs) {
      const auto view = reader.number(), sort = reader.number();
      if (view > DWORD(View::List) || sort > DWORD(Sort::Modified))
        fn::fail(lang::text(readError), ERROR_INVALID_DATA);
      group.view = View(view);
      group.sort = Sort(sort);
      const auto tabCount = reader.count(3 * sizeof(DWORD));
      group.tabs.reserve(tabCount);
      for (DWORD j = 0; j < tabCount; ++j) {
        group.tabs.push_back({reader.string(groupNameLimit), reader.string(pathLimit), reader.paths()});
      }
    }
    groups.push_back(std::move(group));
  }
  if (!reader.remaining.empty()) fn::fail(lang::text(readError), ERROR_INVALID_DATA);
  validate(layout, readError);
  return layout;
}


struct File {
  HANDLE handle;
  ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

void checkJson(HRESULT result) {
  if (FAILED(result)) fn::fail(lang::text(readError), static_cast<DWORD>(result));
}

struct Runtime {
  HRESULT result = RoInitialize(RO_INIT_SINGLETHREADED);
  Runtime() { if (result != RPC_E_CHANGED_MODE) checkJson(result); }
  ~Runtime() { if (SUCCEEDED(result)) RoUninitialize(); }
};

std::wstring wide(const HString &value) {
  unsigned length = 0;
  const auto data = value.GetRawBuffer(&length);
  return std::wstring(data, length);
}

std::wstring stringField(IJsonObject *object, const wchar_t *name) {
  HString value;
  checkJson(object->GetNamedString(HStringReference(name).Get(), value.GetAddressOf()));
  return wide(value);
}

bool hasField(const ComPtr<IJsonObject> &object, const wchar_t *name) {
  ComPtr<IMap<HSTRING, IJsonValue *>> fields;
  checkJson(object.As(&fields));
  boolean exists = false;
  checkJson(fields->HasKey(HStringReference(name).Get(), &exists));
  return exists != 0;
}

LONG numberField(IJsonObject *object, const wchar_t *name) {
  double number = 0;
  checkJson(object->GetNamedNumber(HStringReference(name).Get(), &number));
  if (!std::isfinite(number) || number != std::trunc(number) ||
      number < std::numeric_limits<LONG>::min() || number > std::numeric_limits<LONG>::max())
    fn::fail(lang::text(readError), ERROR_INVALID_DATA);
  return static_cast<LONG>(number);
}

std::optional<std::optional<COLORREF>> detachedTintField(const ComPtr<IJsonObject> &object, const wchar_t *name) {
  if (!hasField(object, name)) return std::nullopt;
  ComPtr<IJsonValue> tint;
  checkJson(object->GetNamedValue(HStringReference(name).Get(), &tint));
  JsonValueType type{};
  checkJson(tint->get_ValueType(&type));
  if (type == JsonValueType_Null) return std::optional<std::optional<COLORREF>>(std::in_place);
  return std::optional<std::optional<COLORREF>>(std::in_place, static_cast<COLORREF>(numberField(object.Get(), name)));
}

template<typename Enum, size_t N>
Enum enumField(IJsonObject *object, const wchar_t *name, const std::array<const wchar_t *, N> &values) {
  const auto value = stringField(object, name);
  const auto match = std::find(values.begin(), values.end(), value);
  if (match == values.end()) fn::fail(lang::text(readError), ERROR_INVALID_DATA);
  return static_cast<Enum>(match - values.begin());
}

struct JsonArrayReader {
  ComPtr<IJsonArray> array;
  unsigned size = 0;

  JsonArrayReader(IJsonObject *object, const wchar_t *name) {
    checkJson(object->GetNamedArray(HStringReference(name).Get(), &array));
    ComPtr<IVector<IJsonValue *>> values;
    checkJson(array.As(&values));
    checkJson(values->get_Size(&size));
  }

  ComPtr<IJsonObject> objectAt(unsigned index) const {
    ComPtr<IJsonObject> object;
    checkJson(array->GetObjectAt(index, &object));
    return object;
  }
};

std::vector<std::wstring> pathsField(IJsonObject *object) {
  JsonArrayReader paths(object, L"paths");
  std::vector<std::wstring> result;
  result.reserve(paths.size);
  for (unsigned i = 0; i < paths.size; ++i) {
    HString item;
    checkJson(paths.array->GetStringAt(i, item.GetAddressOf()));
    result.push_back(wide(item));
  }
  return result;
}

Layout parseLayoutObject(const ComPtr<IJsonObject> &root) {
  if (numberField(root.Get(), L"version") != jsonVersion)
    fn::fail(lang::text(readError), ERROR_REVISION_MISMATCH);

  JsonArrayReader entries(root.Get(), L"groups");
  Layout layout;
  auto &groups = layout.groups;
  groups.reserve(entries.size);
  for (unsigned i = 0; i < entries.size; ++i) {
    const auto entry = entries.objectAt(i);
    Group group;
    group.name = stringField(entry.Get(), L"name");
    ComPtr<IJsonObject> bounds;
    checkJson(entry->GetNamedObject(HStringReference(L"bounds").Get(), &bounds));
    group.bounds = {numberField(bounds.Get(), L"left"), numberField(bounds.Get(), L"top"),
                    numberField(bounds.Get(), L"right"), numberField(bounds.Get(), L"bottom")};
    if (hasField(entry, L"workArea")) {
      ComPtr<IJsonObject> work;
      checkJson(entry->GetNamedObject(HStringReference(L"workArea").Get(), &work));
      group.referenceWorkArea = {numberField(work.Get(), L"left"), numberField(work.Get(), L"top"),
                                 numberField(work.Get(), L"right"), numberField(work.Get(), L"bottom")};
    }
    boolean collapsed = false;
    checkJson(entry->GetNamedBoolean(HStringReference(L"collapsed").Get(), &collapsed));
    group.collapsed = collapsed != 0;
    group.view = enumField<View>(entry.Get(), L"view", viewNames);
    group.sort = enumField<Sort>(entry.Get(), L"sort", sortNames);
    if (hasField(entry, L"displayMode")) group.displayMode = enumField<DisplayMode>(entry.Get(), L"displayMode", displayModeNames);
    if (hasField(entry, L"itemSpacing")) group.itemSpacing = numberField(entry.Get(), L"itemSpacing");
    if (hasField(entry, L"iconSize")) group.iconSize = numberField(entry.Get(), L"iconSize");
    if (hasField(entry, L"opacity")) group.opacity = numberField(entry.Get(), L"opacity");
    if (hasField(entry, L"blurStrength")) group.blurStrength = numberField(entry.Get(), L"blurStrength");
    if (hasField(entry, L"glass")) {
      boolean glass = false;
      checkJson(entry->GetNamedBoolean(HStringReference(L"glass").Get(), &glass));
      group.glass = glass != 0;
    }
    if (hasField(entry, L"autoCollapse")) {
      boolean autoCollapse = false;
      checkJson(entry->GetNamedBoolean(HStringReference(L"autoCollapse").Get(), &autoCollapse));
      group.autoCollapse = autoCollapse != 0;
    }
    if (hasField(entry, L"hoverMenu")) {
      boolean hoverMenu = true;
      checkJson(entry->GetNamedBoolean(HStringReference(L"hoverMenu").Get(), &hoverMenu));
      group.hoverMenu = hoverMenu != 0;
    }
    if (hasField(entry, L"autoRule")) group.autoRule = stringField(entry.Get(), L"autoRule");
    if (hasField(entry, L"tint")) group.tint = static_cast<COLORREF>(numberField(entry.Get(), L"tint"));
    if (hasField(entry, L"titleHeight")) group.titleHeight = numberField(entry.Get(), L"titleHeight");
    if (hasField(entry, L"titleStyle")) group.titleStyle = static_cast<TitleStyle>(numberField(entry.Get(), L"titleStyle"));
    if (hasField(entry, L"titleIcon")) group.titleIcon = stringField(entry.Get(), L"titleIcon");
    if (hasField(entry, L"folder")) group.folder = stringField(entry.Get(), L"folder");
    group.primaryDetachedTint = detachedTintField(entry, L"primaryDetachedTint");
    group.paths = pathsField(entry.Get());
    JsonArrayReader tabs(entry.Get(), L"tabs");
    group.tabs.reserve(tabs.size);
    for (unsigned j = 0; j < tabs.size; ++j) {
      const auto tab = tabs.objectAt(j);
      Tab item{stringField(tab.Get(), L"name"), stringField(tab.Get(), L"folder"), pathsField(tab.Get()),
               detachedTintField(tab, L"detachedTint")};
      group.tabs.push_back(std::move(item));
    }
    groups.push_back(std::move(group));
  }
  if (hasField(root, L"looseItems")) {
    JsonArrayReader loose(root.Get(), L"looseItems");
    layout.looseItems.reserve(loose.size);
    for (unsigned i = 0; i < loose.size; ++i) {
      const auto item = loose.objectAt(i);
      LooseItem entry{stringField(item.Get(), L"path"),
                      {numberField(item.Get(), L"x"), numberField(item.Get(), L"y")}};
      if (hasField(item, L"columnSpacing") && hasField(item, L"rowSpacing")) {
        const SIZE spacing{numberField(item.Get(), L"columnSpacing"), numberField(item.Get(), L"rowSpacing")};
        if (spacing.cx > 0 && spacing.cx <= 2048 && spacing.cy > 0 && spacing.cy <= 2048) entry.spacing = spacing;
      }
      layout.looseItems.push_back(std::move(entry));
    }
  }
  validate(layout, readError);
  for (auto &group : groups) {
    if (!group.folder.empty() || !group.paths.empty() || group.primaryDetachedTint || group.tabs.size() != 1) continue;
    auto &tab = group.tabs.front();
    // Earlier releases detached a mapped tab into a box with an empty main tab and a duplicate title.
    if (tab.name != group.name || tab.folder.empty() || tab.detachedTint) continue;
    group.folder = std::move(tab.folder);
    group.tabs.clear();
  }
  return layout;
}

ComPtr<IJsonObject> parseJsonRoot(std::string_view utf8) {
  if (utf8.starts_with("\xef\xbb\xbf")) utf8.remove_prefix(3);
  const auto size = static_cast<int>(utf8.size());
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, nullptr, 0);
  if (!length) fn::fail(lang::text(readError), ERROR_INVALID_DATA);
  std::wstring content(length, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, content.data(), length))
    fn::fail(lang::text(readError));
  if (content.find(L'\0') != content.npos) fn::fail(lang::text(readError), ERROR_INVALID_DATA);

  ComPtr<IJsonObjectStatics> parser;
  checkJson(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonObject).Get(),
                                   IID_PPV_ARGS(&parser)));
  ComPtr<IJsonObject> root;
  checkJson(parser->Parse(HStringReference(content.c_str(), static_cast<unsigned>(content.size())).Get(), &root));
  return root;
}

Layout parseJson(std::string_view utf8) {
  const Runtime runtime;
  return parseLayoutObject(parseJsonRoot(utf8));
}

void quote(std::wstring &output, std::wstring_view value) {
  constexpr wchar_t hex[] = L"0123456789abcdef";
  output += L'"';
  for (const auto ch : value) {
    if (ch < 32) {
      output += L"\\u00";
      output += hex[(ch >> 4) & 15];
      output += hex[ch & 15];
    } else {
      if (ch == L'\\' || ch == L'"') output += L'\\';
      output += ch;
    }
  }
  output += L'"';
}

void appendPaths(std::wstring &output, const std::vector<std::wstring> &paths) {
  output += L'[';
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) output += L", ";
    quote(output, paths[i]);
  }
  output += L']';
}

std::string formatJson(const Layout &layout) {
  const auto &groups = layout.groups;
  std::wstring content = L"{\n  \"version\": " + std::to_wstring(jsonVersion) + L",\n  \"groups\": [";
  for (size_t i = 0; i < groups.size(); ++i) {
    const auto &group = groups[i];
    if (i) content += L',';
    content += L"\n    {\n      \"name\": ";
    quote(content, group.name);
    content += L",\n      \"bounds\": {\"left\": " + std::to_wstring(group.bounds.left) +
               L", \"top\": " + std::to_wstring(group.bounds.top) +
               L", \"right\": " + std::to_wstring(group.bounds.right) +
               L", \"bottom\": " + std::to_wstring(group.bounds.bottom) + L"}";
    if (!unsetRect(group.referenceWorkArea)) {
      content += L",\n      \"workArea\": {\"left\": " + std::to_wstring(group.referenceWorkArea.left) +
                 L", \"top\": " + std::to_wstring(group.referenceWorkArea.top) +
                 L", \"right\": " + std::to_wstring(group.referenceWorkArea.right) +
                 L", \"bottom\": " + std::to_wstring(group.referenceWorkArea.bottom) + L"}";
    }
    content += L",\n      \"collapsed\": ";
    content += group.collapsed ? L"true" : L"false";
    content += L",\n      \"view\": ";
    quote(content, viewNames[static_cast<size_t>(group.view)]);
    content += L",\n      \"sort\": ";
    quote(content, sortNames[static_cast<size_t>(group.sort)]);
    if (group.displayMode != DisplayMode::Scroll) {
      content += L",\n      \"displayMode\": ";
      quote(content, displayModeNames[static_cast<size_t>(group.displayMode)]);
    }
    content += L",\n      \"iconSize\": " + std::to_wstring(group.iconSize);
    if (group.itemSpacing > 0) content += L",\n      \"itemSpacing\": " + std::to_wstring(group.itemSpacing);
    if (group.opacity) content += L",\n      \"opacity\": " + std::to_wstring(*group.opacity);
    if (group.blurStrength != defaultBlurStrength) content += L",\n      \"blurStrength\": " + std::to_wstring(group.blurStrength);
    if (group.glass) {
      content += L",\n      \"glass\": ";
      content += *group.glass ? L"true" : L"false";
    }
    if (group.autoCollapse) content += L",\n      \"autoCollapse\": true";
    if (!group.hoverMenu) content += L",\n      \"hoverMenu\": false";
    if (!group.autoRule.empty()) {
      content += L",\n      \"autoRule\": ";
      quote(content, group.autoRule);
    }
    if (group.tint) content += L",\n      \"tint\": " + std::to_wstring(*group.tint);
    if (group.primaryDetachedTint) {
      content += L",\n      \"primaryDetachedTint\": ";
      content += *group.primaryDetachedTint ? std::to_wstring(**group.primaryDetachedTint) : L"null";
    }
    if (group.titleHeight != 36) content += L",\n      \"titleHeight\": " + std::to_wstring(group.titleHeight);
    if (group.titleStyle != TitleStyle::Text)
      content += L",\n      \"titleStyle\": " + std::to_wstring(static_cast<int>(group.titleStyle));
    if (!group.titleIcon.empty()) {
      content += L",\n      \"titleIcon\": ";
      quote(content, group.titleIcon);
    }
    if (!group.folder.empty()) {
      content += L",\n      \"folder\": ";
      quote(content, group.folder);
    }
    content += L",\n      \"paths\": ";
    appendPaths(content, group.paths);
    content += L",\n      \"tabs\": [";
    for (size_t j = 0; j < group.tabs.size(); ++j) {
      const auto &tab = group.tabs[j];
      if (j) content += L',';
      content += L"\n        {\"name\": ";
      quote(content, tab.name);
      content += L", \"folder\": ";
      quote(content, tab.folder);
      content += L", \"paths\": ";
      appendPaths(content, tab.paths);
      if (tab.detachedTint) {
        content += L", \"detachedTint\": ";
        content += *tab.detachedTint ? std::to_wstring(**tab.detachedTint) : L"null";
      }
      content += L'}';
    }
    if (!group.tabs.empty()) content += L"\n      ";
    content += L"]\n    }";
  }
  if (!groups.empty()) content += L"\n  ";
  content += L"],\n  \"looseItems\": [";
  for (size_t i = 0; i < layout.looseItems.size(); ++i) {
    const auto &item = layout.looseItems[i];
    if (i) content += L',';
    content += L"\n    {\"path\": ";
    quote(content, item.path);
    content += L", \"x\": " + std::to_wstring(item.position.x) + L", \"y\": " +
               std::to_wstring(item.position.y);
    if (item.spacing.cx > 0 && item.spacing.cy > 0)
      content += L", \"columnSpacing\": " + std::to_wstring(item.spacing.cx) +
                 L", \"rowSpacing\": " + std::to_wstring(item.spacing.cy);
    content += L'}';
  }
  if (!layout.looseItems.empty()) content += L"\n  ";
  content += L"]\n}\n";
  if (content.size() > jsonSizeLimit) fn::fail(lang::text(writeError), ERROR_FILE_TOO_LARGE);
  const auto length = static_cast<int>(content.size());
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), length, nullptr, 0, nullptr, nullptr);
  if (!size) fn::fail(lang::text(writeError));
  if (size > static_cast<int>(jsonSizeLimit)) fn::fail(lang::text(writeError), ERROR_FILE_TOO_LARGE);
  std::string bytes(size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), length, bytes.data(), size, nullptr, nullptr))
    fn::fail(lang::text(writeError));
  return bytes;
}

std::optional<std::string> readFileBytes(const fn::fs::path &file, bool missingOk) {
  const File input{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (input.handle == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (missingOk && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return {};
    fn::fail(lang::text(readError), error);
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(input.handle, &info)) fn::fail(lang::text(readError));
  if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    fn::fail(lang::text(readError), ERROR_INVALID_DATA);
  if (info.nFileSizeHigh || info.nFileSizeLow > jsonSizeLimit) fn::fail(lang::text(readError), ERROR_FILE_TOO_LARGE);
  std::string bytes(info.nFileSizeLow, '\0');
  DWORD received = 0;
  if (!ReadFile(input.handle, bytes.data(), info.nFileSizeLow, &received, nullptr))
    fn::fail(lang::text(readError));
  if (received != info.nFileSizeLow) fn::fail(lang::text(readError), ERROR_HANDLE_EOF);
  return bytes;
}

std::optional<Layout> loadJson(const fn::fs::path &file) {
  auto bytes = readFileBytes(file, true);
  if (!bytes) return {};
  return parseJson(*bytes);
}

bool writeAtomicBytes(std::string_view bytes, const fn::fs::path &file, bool replace) {
  const auto attributes = GetFileAttributesW(file.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if (!replace) return false;
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY))
      fn::fail(lang::text(writeError), ERROR_ACCESS_DENIED);
  }
  const auto directory = file.parent_path();
  if (!directory.empty()) {
    std::error_code error;
    fn::fs::create_directories(directory, error);
    if (error) fn::fail(lang::text(writeError), static_cast<DWORD>(error.value()));
  }
  GUID id{};
  if (const auto status = CoCreateGuid(&id); FAILED(status))
    fn::fail(lang::text(writeError), static_cast<DWORD>(status));
  wchar_t suffix[40]{};
  if (!StringFromGUID2(id, suffix, ARRAYSIZE(suffix))) fn::fail(lang::text(writeError), ERROR_INVALID_DATA);
  auto temporary = file;
  temporary += std::wstring(L".") + suffix + L".tmp";
  bool created = false;
  try {
    {
      const File output{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)};
      if (output.handle == INVALID_HANDLE_VALUE) fn::fail(lang::text(writeError));
      created = true;
      DWORD written = 0;
      if (!WriteFile(output.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        fn::fail(lang::text(writeError));
      if (written != bytes.size()) fn::fail(lang::text(writeError), ERROR_WRITE_FAULT);
      if (!FlushFileBuffers(output.handle)) fn::fail(lang::text(writeError));
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (replace && attributes != INVALID_FILE_ATTRIBUTES) {
      auto backup = file;
      backup += L".bak";
      CopyFileW(file.c_str(), backup.c_str(), FALSE);
    }
    if (MoveFileExW(temporary.c_str(), file.c_str(), flags)) return true;
    const auto error = GetLastError();
    if (!replace && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) {
      DeleteFileW(temporary.c_str());
      return false;
    }
    fn::fail(lang::text(writeError), error);
  } catch (...) {
    if (created) DeleteFileW(temporary.c_str());
    throw;
  }
}

bool writeJson(const Layout &layout, const fn::fs::path &file, bool replace) {
  validate(layout, writeError);
  const auto bytes = formatJson(layout);
  return writeAtomicBytes(bytes, file, replace);
}
}

std::filesystem::path layoutPath() { return fn::localRoot() / L"DesktopGroups.json"; }

Layout readLayout(const fn::fs::path &file) { return loadJson(file).value_or(Layout{}); }

Layout loadLayout(const fn::fs::path &file) {
  try {
    if (auto layout = loadJson(file)) return std::move(*layout);
  } catch (const fn::Error &) {
    auto backup = file;
    backup += L".bak";
    try {
      if (auto fallback = loadJson(backup)) return std::move(*fallback);
    } catch (...) {}
    throw;
  }
  if (auto legacy = loadLegacy()) {
    if (writeJson(*legacy, file, false)) return std::move(*legacy);
    if (auto layout = loadJson(file)) return std::move(*layout);
    fn::fail(lang::text(readError), ERROR_FILE_NOT_FOUND);
  }
  return {};
}

void saveLayout(const Layout &layout, const fn::fs::path &file) { writeJson(layout, file, true); }

std::vector<Group> load(const fn::fs::path &file) { return loadLayout(file).groups; }

void save(const std::vector<Group> &groups, const fn::fs::path &file) {
  Layout layout{groups, {}};
  if (auto previous = loadJson(file)) {
    PathSet groupedPaths;
    validateGroups(groups, groupedPaths, writeError);
    std::erase_if(previous->looseItems, [&](const auto &item) { return groupedPaths.contains(item.path); });
    layout.looseItems = std::move(previous->looseItems);
  }
  saveLayout(layout, file);
}

std::vector<std::wstring> desktopRoots() {
  std::vector<std::wstring> roots;
  for (const auto *id : {&FOLDERID_Desktop, &FOLDERID_PublicDesktop}) {
    PWSTR raw = nullptr;
    const auto result = SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
    auto root = SUCCEEDED(result) && path ? quick::normalize(path.get()) : std::wstring{};
    if (roots.empty() && root.empty()) return {};
    if (!root.empty() && (roots.empty() || !samePath(roots.front(), root))) roots.push_back(std::move(root));
  }
  return roots;
}

bool samePath(const std::wstring &a, const std::wstring &b) {
  return quick::same(a, b);
}

bool isDesktopShellItem(const std::wstring &path) {
  if (!path.starts_with(L"::{") || path.size() != 40 || path.back() != L'}') return false;
  GUID id{};
  return SUCCEEDED(CLSIDFromString(path.c_str() + 2, &id));
}

std::wstring desktopItem(const std::vector<std::wstring> &roots, const std::wstring &path) {
  auto normalized = quick::normalize(path);
  if (normalized.empty()) return {};
  const fn::fs::path item(normalized);
  const auto parent = item.parent_path().native();
  if (!item.has_filename() ||
      std::none_of(roots.begin(), roots.end(), [&](const auto &root) { return samePath(root, parent); }))
    return {};
  WIN32_FIND_DATAW data{};
  const auto search = FindFirstFileW(normalized.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) return {};
  FindClose(search);
  if (samePath(data.cFileName, L"desktop.ini")) return {};
  return (item.parent_path() / data.cFileName).native();
}

const std::wstring &folderForTab(const Group &group, size_t tab) {
  static const std::wstring empty;
  if (!tab) return group.folder;
  return tab <= group.tabs.size() ? group.tabs[tab - 1].folder : empty;
}

bool addItem(std::vector<Group> &groups, size_t target, const std::wstring &path, size_t tab) {
  if (target >= groups.size()) return false;
  auto &group = groups[target];
  if (tab > group.tabs.size() || !folderForTab(group, tab).empty()) return false;
  auto normalized = quick::normalize(path);
  if (normalized.empty() && isDesktopShellItem(path)) normalized = path;
  if (normalized.empty()) return false;
  auto &items = tab ? group.tabs[tab - 1].paths : group.paths;
  if (std::any_of(items.begin(), items.end(), [&](const auto &item) { return samePath(item, normalized); }))
    return false;

  items.push_back(std::move(normalized));
  const auto removeFromOtherTab = [&](std::vector<std::wstring> &entries) {
    if (&entries != &items)
      std::erase_if(entries, [&](const auto &item) { return samePath(item, items.back()); });
  };
  for (auto &candidate : groups) {
    removeFromOtherTab(candidate.paths);
    for (auto &otherTab : candidate.tabs) removeFromOtherTab(otherTab.paths);
  }
  return true;
}

namespace {
Tab takePrimaryTab(Group &group) {
  Tab tab{std::move(group.name), std::move(group.folder), std::move(group.paths), group.primaryDetachedTint};
  if (!tab.detachedTint) tab.detachedTint.emplace(group.tint);
  return tab;
}

void setPrimaryTab(Group &group, Tab &&tab) {
  group.name = std::move(tab.name);
  group.folder = std::move(tab.folder);
  group.paths = std::move(tab.paths);
  group.primaryDetachedTint = tab.detachedTint;
}

Tab removeTab(Group &group, size_t index) {
  if (index) {
    const auto position = group.tabs.begin() + (index - 1);
    Tab tab = std::move(*position);
    group.tabs.erase(position);
    return tab;
  }
  auto tab = takePrimaryTab(group);
  setPrimaryTab(group, std::move(group.tabs.front()));
  group.tabs.erase(group.tabs.begin());
  return tab;
}
}

bool mergeGroups(std::vector<Group> &groups, size_t source, size_t target) {
  if (source == target || source >= groups.size() || target >= groups.size()) return false;
  auto &from = groups[source];
  auto &tabs = groups[target].tabs;
  tabs.reserve(tabs.size() + 1 + from.tabs.size());
  tabs.push_back(takePrimaryTab(from));
  for (auto &tab : from.tabs) {
    if (!tab.detachedTint) tab.detachedTint.emplace(from.tint);
    tabs.push_back(std::move(tab));
  }
  groups.erase(groups.begin() + source);
  return true;
}

bool detachTab(std::vector<Group> &groups, size_t group, size_t tabIndex, RECT bounds) {
  if (group >= groups.size() || groups[group].tabs.empty() || tabIndex > groups[group].tabs.size() ||
      !validBounds(bounds)) return false;
  groups.reserve(groups.size() + 1);
  auto &from = groups[group];
  Group detached;
  detached.bounds = bounds;
  detached.view = from.view;
  detached.sort = from.sort;
  detached.iconSize = from.iconSize;
  detached.opacity = from.opacity;
  detached.glass = from.glass;
  detached.blurStrength = from.blurStrength;
  detached.autoCollapse = from.autoCollapse;
  detached.hoverMenu = from.hoverMenu;
  detached.referenceWorkArea = from.referenceWorkArea;
  detached.titleHeight = from.titleHeight;
  detached.titleStyle = from.titleStyle;
  detached.titleIcon = from.titleIcon;
  detached.displayMode = from.displayMode;
  detached.itemSpacing = from.itemSpacing;
  auto tab = removeTab(from, tabIndex);
  detached.tint = tab.detachedTint.value_or(from.tint);
  setPrimaryTab(detached, std::move(tab));
  detached.primaryDetachedTint.reset();
  groups.push_back(std::move(detached));
  return true;
}

std::optional<Tab> deleteTab(std::vector<Group> &groups, size_t group, size_t tabIndex) {
  if (group >= groups.size() || groups[group].tabs.empty() || tabIndex > groups[group].tabs.size())
    return std::nullopt;
  auto &target = groups[group];
  if (tabIndex == 0) target.autoRule.clear();
  auto removed = removeTab(target, tabIndex);
  if (target.tabs.empty() && target.primaryDetachedTint) {
    target.tint = *target.primaryDetachedTint;
    target.primaryDetachedTint.reset();
  }
  return removed;
}

bool moveTab(std::vector<Group> &groups, size_t source, size_t tab, size_t target, size_t insertion) {
  if (source >= groups.size() || target >= groups.size()) return false;
  auto &from = groups[source];
  auto &to = groups[target];
  if (tab > from.tabs.size() || insertion > to.tabs.size() + 1 || from.tabs.empty()) return false;

  size_t destination = insertion;
  if (source == target) {
    if (insertion == tab || insertion == tab + 1) return false;
    if (insertion > tab) --destination;
  } else {
    to.tabs.reserve(to.tabs.size() + 1);
  }
  auto item = removeTab(from, tab);
  if (source != target && !item.detachedTint) item.detachedTint.emplace(from.tint);
  if (destination == 0) {
    to.tabs.insert(to.tabs.begin(), takePrimaryTab(to));
    setPrimaryTab(to, std::move(item));
  } else {
    to.tabs.insert(to.tabs.begin() + (destination - 1), std::move(item));
  }
  return true;
}

namespace {
bool sameCharIgnoreCase(wchar_t a, wchar_t b) {
  return a == b || CompareStringOrdinal(&a, 1, &b, 1, TRUE) == CSTR_EQUAL;
}

bool wildcardMatch(std::wstring_view text, std::wstring_view pattern) {
  size_t ti = 0, pi = 0;
  size_t starIdx = std::wstring_view::npos, matchIdx = 0;
  while (ti < text.size()) {
    if (pi < pattern.size() && (pattern[pi] == L'?' || sameCharIgnoreCase(text[ti], pattern[pi]))) {
      ++ti;
      ++pi;
    } else if (pi < pattern.size() && pattern[pi] == L'*') {
      starIdx = pi++;
      matchIdx = ti;
    } else if (starIdx != std::wstring_view::npos) {
      pi = starIdx + 1;
      ti = ++matchIdx;
    } else {
      return false;
    }
  }
  while (pi < pattern.size() && pattern[pi] == L'*') ++pi;
  return pi == pattern.size();
}
}

bool matchesAutoRule(std::wstring_view fileName, std::wstring_view rule) {
  if (fileName.empty() || rule.empty()) return false;
  size_t pos = 0;
  while (pos < rule.size()) {
    const auto sep = rule.find_first_of(L",;，；\r\n", pos);
    auto token = rule.substr(pos, sep == std::wstring_view::npos ? std::wstring_view::npos : sep - pos);
    pos = sep == std::wstring_view::npos ? rule.size() : sep + 1;
    const auto start = token.find_first_not_of(L" \t");
    if (start == std::wstring_view::npos) continue;
    token = token.substr(start, token.find_last_not_of(L" \t") - start + 1);
    if (token.empty()) continue;

    std::wstring pattern;
    if (token.front() == L'.' && token.find_first_of(L"*?") == std::wstring_view::npos) {
      pattern = L"*" + std::wstring(token);
    } else if (token.find_first_of(L".*?") == std::wstring_view::npos) {
      pattern = L"*" + std::wstring(token) + L"*";
    } else {
      pattern.assign(token);
    }
    if (wildcardMatch(fileName, pattern)) return true;
  }
  return false;
}

RECT scaleBoundsToWorkArea(RECT bounds, RECT fromWork, RECT toWork) {
  if (!validBounds(bounds) || !validBounds(toWork)) return bounds;
  const LONG newW = toWork.right - toWork.left;
  const LONG newH = toWork.bottom - toWork.top;
  const LONG width = std::clamp(bounds.right - bounds.left, 1L, newW);
  const LONG height = std::clamp(bounds.bottom - bounds.top, 1L, newH);
  if (unsetRect(fromWork) || !validBounds(fromWork) || EqualRect(&fromWork, &toWork)) {
    const LONG x = std::clamp(bounds.left, toWork.left, toWork.right - width);
    const LONG y = std::clamp(bounds.top, toWork.top, toWork.bottom - height);
    return {x, y, x + width, y + height};
  }
  const LONG oldW = fromWork.right - fromWork.left;
  const LONG oldH = fromWork.bottom - fromWork.top;
  const LONG oldBoxW = bounds.right - bounds.left;
  const LONG oldBoxH = bounds.bottom - bounds.top;
  const LONG oldSlackX = oldW - oldBoxW;
  const LONG oldSlackY = oldH - oldBoxH;
  const double ratioX = oldSlackX > 0
      ? std::clamp(double(bounds.left - fromWork.left) / double(oldSlackX), 0.0, 1.0)
      : std::clamp(double(bounds.left - fromWork.left) / double(std::max(1L, oldW)), 0.0, 1.0);
  const double ratioY = oldSlackY > 0
      ? std::clamp(double(bounds.top - fromWork.top) / double(oldSlackY), 0.0, 1.0)
      : std::clamp(double(bounds.top - fromWork.top) / double(std::max(1L, oldH)), 0.0, 1.0);
  const LONG newSlackX = std::max(0L, newW - width);
  const LONG newSlackY = std::max(0L, newH - height);
  const LONG x = std::clamp(toWork.left + static_cast<LONG>(std::lround(ratioX * newSlackX)),
                            toWork.left, toWork.right - width);
  const LONG y = std::clamp(toWork.top + static_cast<LONG>(std::lround(ratioY * newSlackY)),
                            toWork.top, toWork.bottom - height);
  return {x, y, x + width, y + height};
}

namespace {
constexpr LONG backupVersion = 1;
constexpr wchar_t quickFoldersSubkey[] = L"Software\\FolderNote\\QuickFolders";

void appendRegistryKeyJson(std::wstring &out, const wchar_t *subkey) {
  out += L'[';
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &key) == ERROR_SUCCESS && key) {
    bool first = true;
    for (DWORD index = 0;; ++index) {
      wchar_t name[256]{};
      DWORD nameLen = ARRAYSIZE(name);
      DWORD type = 0;
      DWORD dataBytes = 0;
      const auto query = RegEnumValueW(key, index, name, &nameLen, nullptr, &type, nullptr, &dataBytes);
      if (query == ERROR_NO_MORE_ITEMS) break;
      if (query != ERROR_SUCCESS || dataBytes > 1024 * 1024) continue;
      if (_wcsicmp(name, groupsValue) == 0 || _wcsicmp(name, L"DesktopGroupsHidden") == 0) continue;
      if (type != REG_DWORD && type != REG_SZ && type != REG_MULTI_SZ) continue;
      std::vector<BYTE> buf(dataBytes + 4, 0);
      nameLen = ARRAYSIZE(name);
      if (RegEnumValueW(key, index, name, &nameLen, nullptr, &type, buf.data(), &dataBytes) != ERROR_SUCCESS)
        continue;
      if (type == REG_DWORD && dataBytes == sizeof(DWORD)) {
        DWORD val = 0;
        std::memcpy(&val, buf.data(), sizeof(val));
        if (!first) out += L',';
        first = false;
        out += L"\n    {\"name\": ";
        quote(out, std::wstring_view(name, nameLen));
        out += L", \"type\": \"dword\", \"value\": " + std::to_wstring(val) + L'}';
      } else if (type == REG_SZ && dataBytes % sizeof(wchar_t) == 0) {
        const auto *chars = reinterpret_cast<const wchar_t *>(buf.data());
        size_t len = dataBytes / sizeof(wchar_t);
        while (len > 0 && chars[len - 1] == L'\0') --len;
        if (!first) out += L',';
        first = false;
        out += L"\n    {\"name\": ";
        quote(out, std::wstring_view(name, nameLen));
        out += L", \"type\": \"sz\", \"value\": ";
        quote(out, std::wstring_view(chars, len));
        out += L'}';
      } else if (type == REG_MULTI_SZ && dataBytes >= 2 && dataBytes % sizeof(wchar_t) == 0) {
        const auto *chars = reinterpret_cast<const wchar_t *>(buf.data());
        const size_t count = dataBytes / sizeof(wchar_t);
        std::vector<std::wstring> items;
        for (size_t i = 0; i < count && chars[i];) {
          const size_t begin = i;
          while (i < count && chars[i]) ++i;
          items.emplace_back(chars + begin, i - begin);
          if (i < count) ++i;
        }
        if (!first) out += L',';
        first = false;
        out += L"\n    {\"name\": ";
        quote(out, std::wstring_view(name, nameLen));
        out += L", \"type\": \"multi_sz\", \"values\": ";
        appendPaths(out, items);
        out += L'}';
      }
    }
    if (!first) out += L"\n  ";
    RegCloseKey(key);
  }
  out += L']';
}

void applyRegistryKeyJson(IJsonObject *root, const wchar_t *fieldName, const wchar_t *subkey) {
  ComPtr<IJsonObject> rootRef(root);
  if (!hasField(rootRef, fieldName)) return;
  JsonArrayReader entries(root, fieldName);
  if (!entries.size) return;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS || !key)
    return;
  for (unsigned i = 0; i < entries.size; ++i) {
    const auto item = entries.objectAt(i);
    const auto name = stringField(item.Get(), L"name");
    if (name.empty() || name.find(L'\0') != std::wstring::npos || _wcsicmp(name.c_str(), groupsValue) == 0) continue;
    const auto type = stringField(item.Get(), L"type");
    if (type == L"dword") {
      double raw = 0;
      checkJson(item->GetNamedNumber(HStringReference(L"value").Get(), &raw));
      if (std::isfinite(raw) && raw == std::trunc(raw) && raw >= 0 && raw <= double(std::numeric_limits<DWORD>::max())) {
        const DWORD val = static_cast<DWORD>(raw);
        RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&val), sizeof(val));
      }
    } else if (type == L"sz") {
      const auto val = stringField(item.Get(), L"value");
      if (val.find(L'\0') == std::wstring::npos) {
        RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(val.c_str()),
                       static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
      }
    } else if (type == L"multi_sz") {
      JsonArrayReader values(item.Get(), L"values");
      std::vector<wchar_t> multi;
      for (unsigned j = 0; j < values.size; ++j) {
        HString str;
        checkJson(values.array->GetStringAt(j, str.GetAddressOf()));
        const auto wideStr = wide(str);
        if (wideStr.empty() || wideStr.find(L'\0') != std::wstring::npos) continue;
        multi.insert(multi.end(), wideStr.begin(), wideStr.end());
        multi.push_back(L'\0');
      }
      multi.push_back(L'\0');
      if (multi.size() == 1) multi.push_back(L'\0');
      RegSetValueExW(key, name.c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE *>(multi.data()),
                     static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
    }
  }
  RegCloseKey(key);
}
}

void exportDesktopBackup(const fn::fs::path &backupFile, const fn::fs::path &file) {
  const auto layout = readLayout(file);
  validate(layout, writeError);
  const auto layoutUtf8 = formatJson(layout);
  const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, layoutUtf8.data(),
                                       static_cast<int>(layoutUtf8.size()), nullptr, 0);
  if (!wlen) fn::fail(lang::text(writeError));
  std::wstring layoutWide(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, layoutUtf8.data(),
                      static_cast<int>(layoutUtf8.size()), layoutWide.data(), wlen);
  while (!layoutWide.empty() && (layoutWide.back() == L'\n' || layoutWide.back() == L'\r')) layoutWide.pop_back();

  std::wstring content = L"{\n  \"backupVersion\": " + std::to_wstring(backupVersion) + L",\n  \"layout\": " + layoutWide;
  content += L",\n  \"settings\": ";
  appendRegistryKeyJson(content, settingsKey);
  content += L",\n  \"quickFolders\": ";
  appendRegistryKeyJson(content, quickFoldersSubkey);
  content += L"\n}\n";

  if (content.size() > jsonSizeLimit) fn::fail(lang::text(writeError), ERROR_FILE_TOO_LARGE);
  const int length = static_cast<int>(content.size());
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), length, nullptr, 0, nullptr, nullptr);
  if (!size || size > static_cast<int>(jsonSizeLimit)) fn::fail(lang::text(writeError), ERROR_FILE_TOO_LARGE);
  std::string bytes(size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), length, bytes.data(), size, nullptr, nullptr))
    fn::fail(lang::text(writeError));
  writeAtomicBytes(bytes, backupFile, true);
}

Layout importDesktopBackup(const fn::fs::path &backupFile, const fn::fs::path &file) {
  const auto bytes = readFileBytes(backupFile, false);
  const Runtime runtime;
  const auto root = parseJsonRoot(*bytes);
  if (hasField(root, L"backupVersion")) {
    if (numberField(root.Get(), L"backupVersion") != backupVersion)
      fn::fail(lang::text(readError), ERROR_REVISION_MISMATCH);
    ComPtr<IJsonObject> layoutObj;
    checkJson(root->GetNamedObject(HStringReference(L"layout").Get(), &layoutObj));
    auto layout = parseLayoutObject(layoutObj);
    saveLayout(layout, file);
    applyRegistryKeyJson(root.Get(), L"settings", settingsKey);
    applyRegistryKeyJson(root.Get(), L"quickFolders", quickFoldersSubkey);
    return layout;
  }
  auto layout = parseLayoutObject(root);
  saveLayout(layout, file);
  return layout;
}
}
