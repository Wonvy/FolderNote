#pragma once
#include "explorer_entries.h"

namespace explorer_entries {
struct CustomEntry {
  std::wstring id, name, target, shortcut;
  Surface surface{};
};

std::vector<CustomEntry> customEntries(Surface surface);
void addCustom(Surface surface, const std::wstring &name, const std::wstring &target);
void removeCustom(const CustomEntry &entry);
// Returns the number of entries that still need to be removed.
size_t removeAllCustom();
}
