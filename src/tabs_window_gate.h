#pragma once
#include <windows.h>

namespace tabs::gate {
// Runs only on a still-hidden Explorer frame's UI thread. The optional site
// generation is checked around calls that can dispatch reentrant messages.
bool arm(HWND frame, const unsigned long long *generation = nullptr,
         unsigned long long expected = 0) noexcept;
long activeObjects() noexcept;
}
