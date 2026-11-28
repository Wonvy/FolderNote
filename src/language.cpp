#include "language.h"
#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <charconv>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

namespace lang {
namespace {
using Microsoft::WRL::ComPtr;
using namespace std::literals;
constexpr auto registry = L"Software\\ShibaKit";
constexpr DWORD sizeLimit = 1024 * 1024;
constexpr std::pair<const wchar_t *, const wchar_t *> translations[] = {
#include "language_settings.inc"
#include "language_quick.inc"
#include "language_other.inc"
#include "language_context.inc"
#include "language_context_commands.inc"
#include "language_explorer_entries.inc"
};
using Strings = std::map<std::wstring, std::wstring, std::less<>>;
struct Pack { std::wstring name; Strings messages; };

std::wstring key(std::wstring_view source) {
  unsigned long long hash = 14695981039346656037ULL;
  for (wchar_t character : source) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  char buffer[16];
  auto end = std::to_chars(buffer, buffer + sizeof(buffer), hash, 16).ptr;
  return std::wstring(buffer, end);
}
const auto &catalog() {
  static const Strings items = [] {
    Strings result;
    for (auto [source, english] : translations) result.try_emplace(source, english);
    return result;
  }();
  return items;
}
[[noreturn]] void invalid() { throw std::runtime_error("Invalid language XML"); }
std::wstring_view nodeName(IXmlReader *reader) {
  const wchar_t *name = nullptr; UINT size = 0;
  if (FAILED(reader->GetQualifiedName(&name, &size))) invalid();
  return {name, size};
}
std::wstring nodeValue(IXmlReader *reader) {
  const wchar_t *value = nullptr; UINT size = 0;
  if (FAILED(reader->GetValue(&value, &size))) invalid();
  return std::wstring(value, size);
}
std::wstring attribute(IXmlReader *reader, const wchar_t *name) {
  if (reader->MoveToAttributeByName(name, nullptr) != S_OK) invalid();
  auto value = nodeValue(reader);
  if (reader->MoveToElement() != S_OK) invalid();
  return value;
}
Pack parse(const std::string &xml) {
  if (xml.empty() || xml.size() > sizeLimit) invalid();
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(xml.data()), static_cast<UINT>(xml.size())));
  ComPtr<IXmlReader> reader;
  if (!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void **>(reader.GetAddressOf()), nullptr)) ||
      FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
      FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 3)) ||
      FAILED(reader->SetInput(stream.Get()))) invalid();
  Pack pack;
  bool root = false, closed = false;
  std::wstring id, value;
  HRESULT status;
  XmlNodeType node;
  while ((status = reader->Read(&node)) == S_OK) {
    UINT depth = 0;
    if (FAILED(reader->GetDepth(&depth))) invalid();
    if (node == XmlNodeType_Element) {
      if (!root && depth == 0 && nodeName(reader.Get()) == L"shibakit-language"sv) {
        root = true;
        if (attribute(reader.Get(), L"version") != L"1") invalid();
        pack.name = attribute(reader.Get(), L"name");
        if (pack.name.empty() || pack.name.size() > 48 || reader->IsEmptyElement()) invalid();
      } else if (root && !closed && depth == 1 && nodeName(reader.Get()) == L"message"sv && id.empty()) {
        id = attribute(reader.Get(), L"id");
        if (id.empty() || id.size() > 16 || id.find_first_not_of(L"0123456789abcdef") != id.npos) invalid();
        if (reader->IsEmptyElement()) {
          if (!pack.messages.emplace(std::move(id), L"").second) invalid();
          id.clear();
        }
      } else invalid();
    } else if (node == XmlNodeType_Text || node == XmlNodeType_CDATA || node == XmlNodeType_Whitespace) {
      if (!id.empty()) value += nodeValue(reader.Get());
      else if (node != XmlNodeType_Whitespace) invalid();
    } else if (node == XmlNodeType_EndElement) {
      if (nodeName(reader.Get()) == L"message"sv && !id.empty()) {
        if (value.size() > 4096 || !pack.messages.emplace(std::move(id), std::move(value)).second) invalid();
        id.clear(); value.clear();
      } else if (nodeName(reader.Get()) == L"shibakit-language"sv && root && !closed && id.empty()) closed = true;
      else invalid();
    } else if (node != XmlNodeType_XmlDeclaration && node != XmlNodeType_Comment) invalid();
  }
  if (status != S_FALSE || !closed || pack.messages.empty()) invalid();
  return pack;
}
struct Cache {
  std::mutex mutex;
  ULONGLONG expires = 0;
  Choice choice = Choice::Chinese;
  Pack pack;
  // Explorer callers may retain pointers after the active pack changes.
  std::set<std::wstring, std::less<>> customStrings;
  void refresh(bool force = false) {
    auto now = GetTickCount64();
    if (!force && now < expires) return;
    expires = now + 1000;
    DWORD bytes = sizeof(DWORD), stored = 0;
    RegGetValueW(HKEY_CURRENT_USER, registry, L"Language", RRF_RT_REG_DWORD, nullptr, &stored, &bytes);
    choice = stored <= static_cast<DWORD>(Choice::Custom) ? static_cast<Choice>(stored) : Choice::Chinese;
    if (choice != Choice::Custom && !force) return;
    bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, registry, L"LanguageXml", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) ||
        bytes == 0 || bytes > sizeLimit) {
      pack = {};
      loadedXml.clear();
      return;
    }
    std::string xml(bytes, '\0');
    if (RegGetValueW(HKEY_CURRENT_USER, registry, L"LanguageXml", RRF_RT_REG_BINARY, nullptr, xml.data(), &bytes)) {
      pack = {};
      loadedXml.clear();
      return;
    }
    xml.resize(bytes);
    if (xml == loadedXml) return;
    try { pack = parse(xml); loadedXml = std::move(xml); }
    catch (...) { pack = {}; loadedXml.clear(); }
  }
  std::string loadedXml;
};
Cache &cache() { static Cache value; return value; }
void save(const wchar_t *name, DWORD type, const void *data, DWORD bytes) {
  if (RegSetKeyValueW(HKEY_CURRENT_USER, registry, name, type, data, bytes) != ERROR_SUCCESS)
    throw std::runtime_error("Could not save language settings");
}
std::wstring escaped(std::wstring_view value) {
  std::wstring output;
  for (wchar_t ch : value) {
    switch (ch) {
    case L'&': output += L"&amp;"; break;
    case L'<': output += L"&lt;"; break;
    case L'>': output += L"&gt;"; break;
    case L'\r': output += L"&#13;"; break;
    default: output += ch; break;
    }
  }
  return output;
}
}
const wchar_t *text(const wchar_t *source) noexcept try {
  auto &state = cache();
  std::lock_guard lock(state.mutex);
  state.refresh();
  if (state.choice == Choice::Chinese) return source;
  if (state.choice == Choice::Custom) {
    auto custom = state.pack.messages.find(key(source));
    if (custom != state.pack.messages.end() && !custom->second.empty())
      return state.customStrings.insert(custom->second).first->c_str();
  }
  auto english = catalog().find(source);
  return english == catalog().end() ? source : english->second.c_str();
} catch (...) { return source; }
std::wstring format(const wchar_t *source, std::initializer_list<std::wstring> values) {
  std::wstring_view pattern = text(source);
  std::wstring output;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 2] == L'}' &&
        pattern[i + 1] >= L'0' && size_t(pattern[i + 1] - L'0') < values.size()) {
      output += values.begin()[pattern[i + 1] - L'0'];
      i += 2;
    } else output += pattern[i];
  }
  return output;
}
Settings settings() {
  auto &state = cache(); std::lock_guard lock(state.mutex); state.refresh(true);
  return {state.choice, state.pack.name};
}
void select(Choice choice) {
  auto &state = cache(); std::lock_guard lock(state.mutex);
  auto value = static_cast<DWORD>(choice);
  save(L"Language", REG_DWORD, &value, sizeof(value));
  state.refresh(true);
}
void importFile(const std::filesystem::path &path) {
  auto bytes = std::filesystem::file_size(path);
  if (!bytes || bytes > sizeLimit) invalid();
  std::ifstream input(path, std::ios::binary);
  std::string xml(static_cast<size_t>(bytes), '\0');
  if (!input.read(xml.data(), static_cast<std::streamsize>(xml.size())) || input.peek() != EOF) invalid();
  auto pack = parse(xml);
  bool compatible = false;
  for (const auto &[source, english] : catalog()) {
    auto entry = pack.messages.find(key(source));
    if (entry != pack.messages.end() && !entry->second.empty()) { compatible = true; break; }
  }
  if (!compatible) invalid();
  save(L"LanguageXml", REG_BINARY, xml.data(), static_cast<DWORD>(xml.size()));
  select(Choice::Custom);
}
void exportFile(const std::filesystem::path &path) {
  std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      L"<shibakit-language version=\"1\" name=\"My language\">\n";
  for (const auto &[source, english] : catalog())
    xml += L"  <message id=\"" + key(source) + L"\">" + escaped(english) + L"</message>\n";
  xml += L"</shibakit-language>\n";
  auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(), static_cast<int>(xml.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) throw std::runtime_error("Could not encode language template");
  std::string bytes(size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(), static_cast<int>(xml.size()), bytes.data(), size, nullptr, nullptr))
    throw std::runtime_error("Could not encode language template");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), size);
  output.close();
  if (!output) throw std::runtime_error("Could not save language template");
}
}
