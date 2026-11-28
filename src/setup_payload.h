#pragma once
#include <windows.h>
#include <filesystem>

namespace setup {
inline constexpr int payloadResource = 301;
inline constexpr const wchar_t *commonFiles[] = {
    L"Install.exe", L"Uninstall.exe", L"ShibaKit.exe", L"ShibaKitShell.dll", L"ShibaKitNavigation.dll"};
inline constexpr const wchar_t *modernFiles[] = {L"Install-Win11.exe", L"ShibaKit.msix"};
inline constexpr auto bootstrapFile = L"Microsoft.WindowsAppRuntime.Bootstrap.dll";

struct Staging {
  std::filesystem::path path;
  Staging();
  ~Staging();
  Staging(const Staging &) = delete;
  Staging &operator=(const Staging &) = delete;
};

// The caller owns a fresh, empty staging directory. Only fixed payload names are accepted.
void extract(HMODULE module, const std::filesystem::path &directory, bool modern);
}
