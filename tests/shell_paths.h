#pragma once

#include <windows.h>

#include <string>

// The Shell expands 8.3 short paths when a shortcut is saved, so the raw
// target string can differ from the path used to create the link. That
// happens whenever the temporary directory itself uses a short name, such as
// C:\Users\RUNNER~1\AppData\Local\Temp on GitHub Actions runners. Compare the
// long forms instead of the exact spelling.
inline std::wstring shellTestLongPath(const std::wstring &path) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetLongPathNameW(path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return path;
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    buffer.resize(length + 1);
  }
}

inline bool sameShellTestPath(const std::wstring &left, const std::wstring &right) {
  const auto first = shellTestLongPath(left), second = shellTestLongPath(right);
  return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}
