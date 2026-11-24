#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdlib>

namespace quick {
// Pressing a physical wheel moves the pointer more than a left click does.
// Every middle-click path shares this tolerance so behaviour stays consistent.
inline constexpr int middleClickMinimumSlop = 9;

inline int middleClickSlopX() { return std::max(GetSystemMetrics(SM_CXDRAG), middleClickMinimumSlop); }
inline int middleClickSlopY() { return std::max(GetSystemMetrics(SM_CYDRAG), middleClickMinimumSlop); }

inline bool middleClickWithin(POINT down, POINT up) {
  return std::abs(up.x - down.x) <= middleClickSlopX() && std::abs(up.y - down.y) <= middleClickSlopY();
}
}
