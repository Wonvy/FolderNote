#pragma once
#include <objidl.h>
#include <string>
#include <vector>

namespace desktop_groups {
// Returns filesystem paths and supported virtual desktop items from a Shell drag data object.
std::vector<std::wstring> desktopShellDropItems(IDataObject *data);
}
