#pragma once

#include <windows.h>
#include <functional>
#include <optional>

namespace desktop_groups {
std::optional<COLORREF> pickBoxColor(HINSTANCE instance, HWND owner, COLORREF initial,
                                    const std::function<void(COLORREF)> &preview);
}
