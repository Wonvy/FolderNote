#include "quick_find.h"
#include <windows.h>
#include <algorithm>
#include <iterator>
#include <limits>

namespace quick_find {
namespace {
struct Initial { char32_t character; std::uint32_t mask; };
constexpr Initial initials[] = {
#include "pinyin_initials.inc"
};
std::uint32_t initialMask(char32_t character) {
  const auto entry = std::lower_bound(std::begin(initials), std::end(initials), character,
      [](const Initial &value, char32_t key) { return value.character < key; });
  return entry != std::end(initials) && entry->character == character ? entry->mask : 0;
}
bool separator(char32_t value) {
  return value == U' ' || value == U'\t' || value == U'_' || value == U'-' || value == U'.' ||
         value == U'(' || value == U')' || value == U'（' || value == U'）';
}
std::u32string normalize(std::wstring_view value) {
  std::wstring lowered(value);
  if (!lowered.empty()) CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
  std::u32string result;
  for (size_t i = 0; i < lowered.size(); ++i) {
    char32_t character = lowered[i];
    if (character >= 0xD800 && character <= 0xDBFF && i + 1 < lowered.size() &&
        lowered[i + 1] >= 0xDC00 && lowered[i + 1] <= 0xDFFF) {
      character = 0x10000 + ((character - 0xD800) << 10) + (lowered[++i] - 0xDC00);
    }
    if (character >= 0xFF01 && character <= 0xFF5E) character -= 0xFEE0;
    if (character >= U'A' && character <= U'Z') character += U'a' - U'A';
    result.push_back(character);
  }
  return result;
}
}
Name::Name(std::wstring_view value) : text(normalize(value)) {
  for (auto character : text)
    if (!separator(character)) compact.push_back({character, initialMask(character)});
}
int Name::rank(std::wstring_view query) const {
  const auto literal = normalize(query);
  if (literal.empty()) return -1;
  const auto literalOffset = text.find(literal);
  if (literal == text) return 0;
  std::u32string needle;
  for (auto character : literal) if (!separator(character)) needle.push_back(character);
  if (needle.empty()) return literalOffset == text.npos ? -1 : literalOffset == 0 ? 2 : 4;
  for (size_t start = 0; start + needle.size() <= compact.size(); ++start) {
    bool matched = true;
    for (size_t i = 0; i < needle.size(); ++i) {
      const auto key = needle[i];
      const auto &character = compact[start + i];
      if (key != character.value && !(key >= U'a' && key <= U'z' && (character.initials & (1u << (key - U'a'))))) {
        matched = false; break;
      }
    }
    if (matched) {
      // A complete name/initial sequence is more specific than a prefix:
      // "sc" selects 素材 before ScriptData; "script" still selects ScriptData.
      if (start == 0 && needle.size() == compact.size()) return 1;
      if (literalOffset == 0) return 2;
      return start == 0 ? 3 : literalOffset != text.npos ? 4 : 5;
    }
  }
  return literalOffset == text.npos ? -1 : literalOffset == 0 ? 2 : 4;
}
bool matches(std::wstring_view name, std::wstring_view query) { return query.empty() || Name(name).rank(query) >= 0; }
bool inputCharacter(wchar_t character) {
  return character >= L'!' && character != 0x7F;
}
bool Session::active(std::uint64_t now) const { return !buffer.empty() && now - last <= timeout; }
void Session::clear() { buffer.clear(); last = 0; }
int Session::choose(const std::vector<Name> &names, int selected, bool cycle, bool backwards) const {
  int best = std::numeric_limits<int>::max(), result = -1;
  const auto size = static_cast<int>(names.size());
  for (int i = 0; i < size; ++i) {
    const int index = cycle ? (selected + (backwards ? size - 1 - i : 1 + i) + size) % size : i;
    const int rank = names[index].rank(buffer);
    if (cycle && rank >= 0) return index;
    if (rank >= 0 && (rank < best || (!cycle && rank == best && index == selected))) {
      best = rank; result = index;
    }
  }
  return result;
}
int Session::type(wchar_t character, std::uint64_t now, const std::vector<Name> &names, int selected) {
  if (!active(now)) clear();
  if (character == L'\b') {
    if (buffer.empty()) return -1;
    buffer.pop_back();
    if (!buffer.empty() && buffer.back() >= 0xD800 && buffer.back() <= 0xDBFF) buffer.pop_back();
    last = now;
    return buffer.empty() ? -1 : choose(names, selected, false);
  }
  if (!inputCharacter(character) || buffer.size() >= 128) return -1;
  const auto previous = buffer;
  buffer += character;
  last = now;
  const auto match = choose(names, selected, false);
  // Repeating one letter cycles only if the longer query has no match.
  if (match < 0 && previous.size() == 1 && normalize(previous) == normalize(std::wstring(1, character))) {
    buffer = previous;
    return choose(names, selected, true);
  }
  return match;
}
int Session::next(const std::vector<Name> &names, int selected, bool backwards) const {
  return buffer.empty() ? -1 : choose(names, selected, true, backwards);
}
}
