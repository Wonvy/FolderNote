#pragma once
#include "core.h"

// Only the BHO discovery entry is machine-wide. COM registration and DLL paths
// belong to the original (unelevated) user, never the administrator supplying UAC.
namespace navreg {
inline constexpr wchar_t clsid[] = L"{87F11DB4-74C9-4B54-9379-E26D75D18B82}";
inline constexpr wchar_t machineKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects\\"
    L"{87F11DB4-74C9-4B54-9379-E26D75D18B82}";
inline constexpr wchar_t userKey[] =
    L"Software\\Classes\\CLSID\\{87F11DB4-74C9-4B54-9379-E26D75D18B82}";

std::wstring currentUserSid();
bool validSid(const std::wstring &sid);
bool machineRegistered(const std::wstring &sid);
void registerMachine(const std::wstring &sid);
void unregisterMachine(const std::wstring &sid);
void ensureMachine(const fn::fs::path &helper);
void removeMachine(const fn::fs::path &helper);
void registerUser(const fn::fs::path &directory);
void unregisterUser();
// Handles only these exact, private elevation modes. The helper never writes
// HKCU or copies files, so over-the-shoulder administrator credentials are safe.
bool handleHelper(int count, wchar_t **arguments, int &result);
} // namespace navreg
