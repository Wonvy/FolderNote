#pragma once
#include <windows.h>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace tabs {
struct Location {
  std::vector<BYTE> id;
  std::wstring name, path;
  bool operator==(const Location &other) const {
    return id == other.id || (!path.empty() && !other.path.empty() &&
        CompareStringOrdinal(path.c_str(), -1, other.path.c_str(), -1, TRUE) == CSTR_EQUAL);
  }
};
struct Model {
  static constexpr size_t limit = 32, closedLimit = 20;
  std::vector<Location> items, closed;
  size_t active = 0;
  void remember(Location value) {
    if (value.id.empty()) return;
    if (items.empty()) items.push_back(std::move(value)); else items[active] = std::move(value);
  }
  bool add(const Location &value) {
    if (value.id.empty() || items.size() >= limit) return false;
    auto copy = value; items.insert(items.begin() + (items.empty() ? 0 : active + 1), std::move(copy));
    if (items.size() > 1) ++active; return true;
  }
  std::optional<size_t> find(const Location &value) const {
    if (value.id.empty()) return std::nullopt;
    if (active < items.size() && items[active] == value) return active;
    for (size_t i = 0; i < items.size(); ++i)
      if (items[i] == value) return i;
    return std::nullopt;
  }
  bool select(size_t index) { if (index >= items.size()) return false; active = index; return true; }
  bool close(size_t index) {
    if (index >= items.size() || items.size() == 1) return false;
    closed.push_back(items[index]); if (closed.size() > closedLimit) closed.erase(closed.begin());
    items.erase(items.begin() + index);
    if (index < active) --active; else active = std::min(active, items.size() - 1);
    return true;
  }
  bool closeOthers(size_t index) {
    if (index >= items.size()) return false;
    select(index);
    for (size_t i = items.size(); i-- > 0;) if (i != active) close(i);
    return true;
  }
  bool restore() {
    if (closed.empty() || items.size() >= limit) return false;
    auto value = closed.back(); if (!add(value)) return false; closed.pop_back(); return true;
  }
};
}
