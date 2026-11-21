#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quick_find {
struct Character { char32_t value; std::uint32_t initials; };
struct Name {
  std::u32string text;
  std::vector<Character> compact;
  explicit Name(std::wstring_view value);
  // -1 means no match. Complete literal/initial names precede prefixes,
  // then substrings. Within partial matches, literals precede initials.
  int rank(std::wstring_view query) const;
};
bool matches(std::wstring_view name, std::wstring_view query);
bool inputCharacter(wchar_t character);

class Session {
public:
  static constexpr std::uint64_t timeout = 1500;
  // Returns the chosen index, or -1. Input never opens or modifies an item.
  int type(wchar_t character, std::uint64_t now, const std::vector<Name> &names, int selected);
  int next(const std::vector<Name> &names, int selected, bool backwards = false) const;
  bool active(std::uint64_t now) const;
  void clear();
  const std::wstring &query() const { return buffer; }
private:
  std::wstring buffer;
  std::uint64_t last = 0;
  int choose(const std::vector<Name> &names, int selected, bool cycle, bool backwards = false) const;
};
}
