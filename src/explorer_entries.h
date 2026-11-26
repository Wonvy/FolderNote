#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace explorer_entries {
enum class Surface { ThisPc, Navigation, Desktop };

struct Entry {
  std::wstring id, name;
  Surface surface{};
  bool hidden = false, changed = false, canChange = true;
  std::wstring target, iconLocation;
};

std::vector<Entry> inventory(Surface surface);
void setHidden(const Entry &entry, bool hidden);
void restore(const Entry &entry);
// Best-effort restoration. Returns the number of records still requiring attention.
size_t restoreAll();
std::vector<std::wstring> pendingMachineMountIds();
bool restoreMachineMountById(const std::wstring &id);
void refresh(Surface surface) noexcept;
// Call after the visibility change has committed and released its registry lock.
void refreshOpen(Surface surface, bool policyChanged = false) noexcept;
void refreshOpen(const Entry &entry) noexcept;
}
