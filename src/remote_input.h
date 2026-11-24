#pragma once
#include <windows.h>
#include <cstdlib>
#include <cwchar>

namespace quick {
namespace detail {
inline bool hasPrefixIgnoreCase(const wchar_t *text, const wchar_t *prefix) noexcept {
  if (!text || !prefix) return false;
  while (*prefix) {
    wchar_t a = *text++, b = *prefix++;
    if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
    if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
    if (a != b) return false;
  }
  return true;
}

inline bool kvmClassName(const wchar_t *cls) noexcept {
  return hasPrefixIgnoreCase(cls, L"Deskflow") ||
         hasPrefixIgnoreCase(cls, L"Synergy") ||
         hasPrefixIgnoreCase(cls, L"Barrier") ||
         hasPrefixIgnoreCase(cls, L"InputLeap") ||
         hasPrefixIgnoreCase(cls, L"MouseWithoutBorders") ||
         hasPrefixIgnoreCase(cls, L"ShareMouse") ||
         hasPrefixIgnoreCase(cls, L"InputDirector");
}

inline bool kvmExecutableName(const wchar_t *fileName) noexcept {
  return hasPrefixIgnoreCase(fileName, L"deskflow") ||
         hasPrefixIgnoreCase(fileName, L"synergy") ||
         hasPrefixIgnoreCase(fileName, L"barrier") ||
         hasPrefixIgnoreCase(fileName, L"input-leap") ||
         hasPrefixIgnoreCase(fileName, L"inputleap") ||
         hasPrefixIgnoreCase(fileName, L"mousewithoutborders") ||
         hasPrefixIgnoreCase(fileName, L"sharemouse") ||
         hasPrefixIgnoreCase(fileName, L"inputdirector");
}

inline bool isKvmProcess(DWORD pid) noexcept {
  if (!pid) return false;
  struct CacheEntry {
    DWORD pid = 0;
    ULONGLONG tick = 0;
    bool kvm = false;
  };
  static CacheEntry cache[8]{};
  const ULONGLONG now = GetTickCount64();
  for (const auto &entry : cache) {
    if (entry.pid == pid && now - entry.tick < 10000) return entry.kvm;
  }
  bool matched = false;
  if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t path[MAX_PATH]{};
    DWORD len = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(proc, 0, path, &len) && len > 0) {
      const wchar_t *base = path;
      for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
      }
      matched = kvmExecutableName(base);
    }
    CloseHandle(proc);
  }
  static unsigned slot = 0;
  cache[slot++ & 7] = {pid, now, matched};
  return matched;
}

inline bool isKvmCaptureWindow(HWND w, POINT cursor, bool haveCursor) noexcept {
  if (!w || !IsWindowVisible(w)) return false;
  const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
  if ((style & WS_CHILD) || (style & WS_CAPTION)) return false;
  const LONG_PTR exStyle = GetWindowLongPtrW(w, GWL_EXSTYLE);
  RECT rect{};
  if (!GetWindowRect(w, &rect)) return false;
  const LONG width = rect.right - rect.left;
  const LONG height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;

  const bool captureShape = (width <= 64 && height <= 64) ||
                            ((style & WS_DISABLED) != 0) ||
                            ((exStyle & WS_EX_TRANSPARENT) != 0);
  if (!captureShape) return false;

  wchar_t cls[128]{};
  if (GetClassNameW(w, cls, ARRAYSIZE(cls)) && kvmClassName(cls)) return true;

  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (isKvmProcess(pid)) return true;

  // Service-launched KVM daemons (running as SYSTEM) deny OpenProcess; recognize
  // their 1x1 disabled transparent cursor-parking window at screen center.
  if (haveCursor && width <= 4 && height <= 4 && rect.left > 16 && rect.top > 16 &&
      (style & WS_POPUP) && (style & WS_DISABLED) && (exStyle & WS_EX_TRANSPARENT) &&
      std::abs(cursor.x - rect.left) <= 8 && std::abs(cursor.y - rect.top) <= 8) {
    return true;
  }
  return false;
}
} // namespace detail

// True when Deskflow, Synergy, Barrier, Input Leap, Mouse Without Borders, etc.
// has redirected local mouse/keyboard input away from this PC to another screen.
inline bool remoteInputCaptured() noexcept {
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (vw > 64 && vh > 64) {
    RECT clip{};
    if (GetClipCursor(&clip)) {
      const LONG cw = clip.right - clip.left;
      const LONG ch = clip.bottom - clip.top;
      if (cw > 0 && ch > 0 && cw <= 32 && ch <= 32) return true;
    }
  }
  POINT cursor{};
  const bool haveCursor = GetCursorPos(&cursor) != FALSE;
  int inspected = 0;
  for (HWND w = GetTopWindow(nullptr); w != nullptr && inspected < 256; w = GetWindow(w, GW_HWNDNEXT), ++inspected) {
    if (!IsWindowVisible(w)) continue;
    if (detail::isKvmCaptureWindow(w, cursor, haveCursor)) return true;
  }
  return false;
}
} // namespace quick
