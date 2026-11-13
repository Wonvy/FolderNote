#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fn {
// Call on a COM-initialized worker after committing all changed folders. Errors
// are intentionally best effort: a view refresh cannot undo a successful save.
void refreshOpenViews(const std::vector<std::wstring> &changedFolders) noexcept;
// Refresh the active view of each Explorer window at the given Shell location.
// A null location refreshes all Explorer windows. The optional desktop pass
// also refreshes the desktop icon view, which is not an Explorer frame.
void refreshShellViews(PCIDLIST_ABSOLUTE location, bool includeDesktop = false) noexcept;

namespace refresh_detail {
struct FreePidl {
  using pointer = PIDLIST_ABSOLUTE;
  void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, FreePidl>;
// Also exposed to isolated tests; the Shell identities, not path spelling, are
// used when deciding which open view is affected.
std::vector<Pidl> affectedParents(const std::vector<std::wstring> &changedFolders);
HRESULT refreshMatchingView(IShellView *view,
                            std::span<const PCIDLIST_ABSOLUTE> parents) noexcept;
} // namespace refresh_detail
} // namespace fn
