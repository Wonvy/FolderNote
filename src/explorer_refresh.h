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
