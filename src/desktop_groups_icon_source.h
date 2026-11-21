#pragma once
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <string>
#include <vector>

namespace desktop_groups {
// Returns filesystem paths and supported virtual desktop items from a Shell drag data object.
std::vector<std::wstring> desktopShellDropItems(IDataObject *data);
// Loads an aspect-fitted thumbnail centered on a transparent square canvas.
// Requires COM initialization. The caller owns the returned bitmap.
HBITMAP loadDesktopThumbnail(const std::wstring &path, int pixels);
// Adds/replaces a 32-bit thumbnail and resets the slot's previous icon mask.
// Returns the image index, or -1 on failure. A negative index appends an image.
int setDesktopThumbnail(HIMAGELIST images, int index, HBITMAP bitmap);
}
