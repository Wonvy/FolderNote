#pragma once
#include <windows.h>

namespace desktop_groups {
class DesktopHold {
public:
  DesktopHold() = default;
  DesktopHold(const DesktopHold &) = delete;
  DesktopHold &operator=(const DesktopHold &) = delete;
  ~DesktopHold();
  bool start(HWND receiver, UINT holdMessage);
  void stop();

private:
  struct State;
  State *state = nullptr;
};
}
