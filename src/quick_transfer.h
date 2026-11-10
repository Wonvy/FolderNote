#pragma once
#include "quick_folders.h"
#include <functional>
#include <wrl/client.h>

namespace quick::transfer {
// Capture the active view's selection, independent of the pointer location.
// Never fall back to the containing folder or to the clipboard. Count also
// describes unsupported selections whose transferable snapshot is empty.
Microsoft::WRL::ComPtr<IShellItemArray> selectedItems(IShellView *, DWORD &count);
bool supports(IShellItemArray *, MenuMode);
HRESULT perform(HWND owner, IShellItemArray *, IShellItem *destination, MenuMode, const std::function<bool()> &current);
}
