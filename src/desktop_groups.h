#pragma once
#include <windows.h>
#include <string>

namespace desktop_groups {
struct Appearance {
  bool glass = true;
  DWORD radius = 10;
  DWORD opacity = 55;
  bool shadow = false;
  DWORD shadowOpacity = 36;
  DWORD shadowBlur = 12;
  DWORD shadowOffset = 3;
  bool operator==(const Appearance &) const = default;
};
Appearance configuredAppearance();
void previewAppearance(const Appearance &appearance);
void restoreAppearance();
bool enabled();
bool sync(const std::wstring &installedExecutable = {});
bool stop();
bool showManager();
int run(HINSTANCE instance, bool manage, bool create = false);
}
