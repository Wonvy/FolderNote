#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace quick::detail {
inline constexpr wchar_t popupMutexName[] = L"Local\\FolderNote.QuickFolders.Popup.v1";

inline PSECURITY_DESCRIPTOR sameUserMediumLabelSecurity() noexcept {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> bytes(size);
  const BOOL ok = size && GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
  CloseHandle(token);
  LPWSTR sid = nullptr;
  if (!ok || !ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(bytes.data())->User.Sid, &sid) || !sid)
    return nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  try {
    const auto sddl = std::wstring(L"D:(A;;GA;;;") + sid + L")S:(ML;;NW;;;ME)";
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
  } catch (...) {
    sd = nullptr;
  }
  LocalFree(sid);
  return sd;
}

// Ownership, not the existence of a named kernel object, defines a live menu.
// The handle may remain open briefly after the previous owner releases it.
class MenuGate {
  HANDLE handle = nullptr;
  bool owned = false;
  DWORD failure = ERROR_SUCCESS;
public:
  explicit MenuGate(const wchar_t *name = popupMutexName) noexcept {
    PSECURITY_DESCRIPTOR sd = sameUserMediumLabelSecurity();
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    handle = CreateMutexW(sd ? &sa : nullptr, FALSE, name);
    if (!handle && sd) handle = CreateMutexW(nullptr, FALSE, name);
    if (!handle) failure = GetLastError();
    if (sd) LocalFree(sd);
  }
  ~MenuGate() { release(); if (handle) CloseHandle(handle); }
  MenuGate(const MenuGate &) = delete;
  MenuGate &operator=(const MenuGate &) = delete;
  bool acquire(DWORD timeout = 200) noexcept {
    if (owned) return true;
    if (!handle) return false;
    DWORD result = WaitForSingleObject(handle, timeout);
    owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    failure = owned ? ERROR_SUCCESS : result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    return owned;
  }
  void release() noexcept { if (owned) { owned = false; ReleaseMutex(handle); } }
  DWORD error() const noexcept { return failure; }
};

// One mapping per target PID/HWND. It stays alive while an old command is
// checking a slow path, even when its popup and all newer popups have closed.
// No disk state, worker or long-lived helper is needed for request ordering.
class MenuRequest {
  HANDLE mapping = nullptr;
  volatile LONG64 *revision = nullptr;
  LONG64 ticket = 0;
  bool started = false;
  DWORD failure = ERROR_SUCCESS;
public:
  explicit MenuRequest(const wchar_t *name) noexcept {
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG64), name);
    if (!mapping) { failure = GetLastError(); return; }
    revision = static_cast<volatile LONG64 *>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG64)));
    if (!revision) failure = GetLastError();
  }
  ~MenuRequest() {
    if (revision) UnmapViewOfFile(const_cast<LONG64 *>(revision));
    if (mapping) CloseHandle(mapping);
  }
  MenuRequest(const MenuRequest &) = delete;
  MenuRequest &operator=(const MenuRequest &) = delete;
  bool valid() const noexcept { return revision != nullptr; }
  DWORD error() const noexcept { return failure; }
  // Call only after acquiring the global popup gate. Rejected duplicate opens
  // must not invalidate the still-visible menu. Zero is a valid wrapped ticket.
  void begin() noexcept { if (revision) { ticket = InterlockedIncrement64(revision); started = true; } }
  bool current() const noexcept { return started && revision && InterlockedCompareExchange64(revision, 0, 0) == ticket; }
};
}
