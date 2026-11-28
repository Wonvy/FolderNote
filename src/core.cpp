#include "core.h"
#include "language.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <shlobj.h>
#include <shlwapi.h>
namespace fn {
void fail(const std::wstring &what, DWORD code) {
  wchar_t *p = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, (wchar_t *)&p, 0, nullptr);
  std::wstring detail = p ? p : lang::text(L"操作失败");
  if (p)
    LocalFree(p);
  throw Error{lang::format(L"{0}：{1}", {lang::text(what.c_str()), detail})};
}
struct Handle {
  HANDLE h;
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
};
static std::wstring trim(std::wstring s) {
  auto a = s.find_first_not_of(L" \t\r\n");
  if (a == s.npos)
    return {};
  return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}
static void checkFolder(const fs::path &p) {
  DWORD a = GetFileAttributesW(p.c_str());
  if (a == INVALID_FILE_ATTRIBUTES)
    fail(L"无法读取文件夹");
  if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))
    throw Error{lang::text(L"请选择普通文件夹；不处理目录链接或云占位目录。")};
}
std::wstring readText(const fs::path &p) {
  Handle f{
      CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
  if (f.h == INVALID_HANDLE_VALUE)
    fail(L"无法读取文件");
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(f.h, &sz))
    fail(L"无法读取大小");
  if (sz.QuadPart > 4 * 1024 * 1024)
    throw Error{lang::text(L"配置文件过大，已停止处理。")};
  std::vector<char> b((size_t)sz.QuadPart);
  DWORD n = 0;
  if (!ReadFile(f.h, b.data(), (DWORD)b.size(), &n, nullptr) || n != b.size())
    fail(L"读取不完整");
  std::wstring s;
  if (b.size() >= 2 && (unsigned char)b[0] == 255 && (unsigned char)b[1] == 254) {
    if (b.size() % 2)
      throw Error{lang::text(L"UTF-16 文件损坏。")};
    s.resize((b.size() - 2) / 2);
    memcpy(s.data(), b.data() + 2, b.size() - 2);
  } else {
    size_t off = 0;
    UINT cp = CP_ACP;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (b.size() >= 3 && (unsigned char)b[0] == 239 && (unsigned char)b[1] == 187 &&
        (unsigned char)b[2] == 191) {
      off = 3;
      cp = CP_UTF8;
      flags = MB_ERR_INVALID_CHARS;
    }
    if (b.size() >= 2 && (unsigned char)b[0] == 254 && (unsigned char)b[1] == 255)
      throw Error{lang::text(L"暂不支持 UTF-16 BE 文件，请先转换编码。")};
    int len = MultiByteToWideChar(cp, flags, b.data() + off, (int)(b.size() - off), nullptr, 0);
    if (!len && b.size() > off)
      fail(L"无法解码文件");
    s.resize(len);
    if (len) {
      MultiByteToWideChar(cp, flags, b.data() + off, (int)(b.size() - off), s.data(), len);
      if (off == 0 && cp == CP_ACP) {
        bool nonAscii = false;
        for (char ch : b) if (static_cast<unsigned char>(ch) >= 0x80) { nonAscii = true; break; }
        if (nonAscii) {
          BOOL usedDefault = FALSE;
          const int rtLen = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.data(), len,
                                                nullptr, 0, nullptr, &usedDefault);
          std::string rt(rtLen > 0 ? rtLen : 0, '\0');
          if (rtLen <= 0 || usedDefault ||
              !WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.data(), len,
                                   rt.data(), rtLen, nullptr, &usedDefault) ||
              usedDefault || rt.size() != b.size() || memcmp(rt.data(), b.data(), b.size()) != 0) {
            throw Error{lang::text(L"配置文件编码与当前系统不匹配，已取消修改以防乱码。")};
          }
        }
      }
    }
  }
  for (size_t i = 0; i < s.size(); ++i) {
    auto c = s[i];
    if (!c)
      throw Error{lang::text(L"配置文件包含空字符。")};
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF)
        throw Error{lang::text(L"配置文件包含无效 Unicode。")};
      ++i;
    } else if (c >= 0xDC00 && c <= 0xDFFF)
      throw Error{lang::text(L"配置文件包含无效 Unicode。")};
  }
  return s;
}
struct Line {
  std::wstring text, end;
};
static std::vector<Line> lines(const std::wstring &s) {
  std::vector<Line> v;
  size_t a = 0;
  while (a < s.size()) {
    auto b = s.find_first_of(L"\r\n", a);
    if (b == s.npos) {
      v.push_back({s.substr(a), L""});
      break;
    }
    size_t e = b + 1;
    if (s[b] == L'\r' && e < s.size() && s[e] == L'\n')
      ++e;
    v.push_back({s.substr(a, b - a), s.substr(b, e - b)});
    a = e;
  }
  return v;
}
static bool section(const std::wstring &t) {
  return t.size() > 1 && t.front() == L'[' && t.back() == L']';
}
static bool key(const std::wstring &t) {
  auto e = t.find(L'=');
  return e != t.npos && _wcsicmp(trim(t.substr(0, e)).c_str(), L"InfoTip") == 0;
}
static std::wstring extract(const std::wstring &s) {
  bool active = false;
  for (auto &l : lines(s)) {
    auto t = trim(l.text);
    if (section(t))
      active = _wcsicmp(t.c_str(), L"[.ShellClassInfo]") == 0;
    else if (active && key(t)) {
      auto value = trim(t.substr(t.find(L'=') + 1));
      return value;
    }
  }
  return {};
}
std::wstring note(const fs::path &p) {
  checkFolder(p);
  auto f = p / L"desktop.ini";
  DWORD a = GetFileAttributesW(f.c_str());
  if (a == INVALID_FILE_ATTRIBUTES) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
      return {};
    fail(L"无法检查备注文件");
  }
  if (a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
    throw Error{lang::text(L"备注文件不是普通文件。")};
  return extract(readText(f));
}
std::wstring normalize(std::wstring s) {
  for (auto &c : s) {
    if (c == L'\r' || c == L'\n' || c == L'\t')
      c = L' ';
    else if (c < 32)
      throw Error{lang::text(L"备注包含不支持的控制字符。")};
  }
  s = trim(s);
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++i, ++n) {
    if (s[i] >= 0xD800 && s[i] <= 0xDBFF) {
      if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF)
        throw Error{lang::text(L"无效 Unicode 字符。")};
      ++i;
    } else if (s[i] >= 0xDC00 && s[i] <= 0xDFFF)
      throw Error{lang::text(L"无效 Unicode 字符。")};
  }
  if (n > 500)
    throw Error{lang::text(L"备注最多 500 个字符。")};
  if (!s.empty() && s[0] == L'@')
    throw Error{lang::text(L"备注不能以 @ 开头（Windows 会将其解释为资源引用）。")};
  return s;
}
static std::wstring guid() {
  GUID g{};
  CoCreateGuid(&g);
  wchar_t b[40];
  StringFromGUID2(g, b, 40);
  return b;
}
static void writeAtomic(const fs::path &p, const std::wstring &s) {
  auto tmp = p;
  tmp += L"." + guid() + L".tmp";
  DWORD old = GetFileAttributesW(p.c_str());
  if (old != INVALID_FILE_ATTRIBUTES &&
      (old & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    throw Error{lang::text(L"备注文件只读或不是普通文件，未修改。")};
  try {
    {
      Handle f{CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr)};
      if (f.h == INVALID_HANDLE_VALUE)
        fail(L"无法创建临时文件");
      std::wstring data = L"\xFEFF" + s;
      DWORD n = 0;
      DWORD bytes = (DWORD)(data.size() * 2);
      if (!WriteFile(f.h, data.data(), bytes, &n, nullptr) || n != bytes || !FlushFileBuffers(f.h))
        fail(L"无法完成写入");
    }
    BOOL ok = old == INVALID_FILE_ATTRIBUTES
                  ? MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_WRITE_THROUGH)
                  : ReplaceFileW(p.c_str(), tmp.c_str(), nullptr, 0, nullptr, nullptr);
    if (!ok)
      fail(L"无法替换备注文件");
  } catch (...) {
    DeleteFileW(tmp.c_str());
    throw;
  }
}
static void notifyPath(const fs::path &path, LONG event, UINT delivery) {
  if (path.native().size() < MAX_PATH) {
    SHChangeNotify(event, SHCNF_PATHW | delivery, path.c_str(), nullptr);
    return;
  }
  PIDLIST_ABSOLUTE id = nullptr;
  if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr))) {
    SHChangeNotify(event, SHCNF_IDLIST | delivery, id, nullptr);
    CoTaskMemFree(id);
  }
}
void notify(const fs::path &p) {
  // The cached customization comes from desktop.ini, not just the folder item.
  notifyPath(p / L"desktop.ini", SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES, SHCNF_FLUSHNOWAIT);
  notifyPath(p, SHCNE_UPDATEITEM | SHCNE_ATTRIBUTES, SHCNF_FLUSHNOWAIT);
  auto parent = p.parent_path();
  if (!parent.empty())
    notifyPath(parent, SHCNE_UPDATEDIR, SHCNF_FLUSHNOWAIT);
}
static bool attributes(const fs::path &p, bool sendNotification = true) {
  auto f = p / L"desktop.ini";
  DWORD a = GetFileAttributesW(f.c_str()), d = GetFileAttributesW(p.c_str());
  if (a == INVALID_FILE_ATTRIBUTES || d == INVALID_FILE_ATTRIBUTES)
    fail(L"无法读取属性");
  bool normal = (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ==
                    (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) &&
                (d & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM));
  if (!normal) {
    if (!SetFileAttributesW(f.c_str(), a | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
      fail(L"无法恢复备注文件属性");
    if (p.native().size() < MAX_PATH) {
      if (!PathMakeSystemFolderW(p.c_str()))
        fail(L"无法设置文件夹属性");
    } else if (!SetFileAttributesW(p.c_str(), d | FILE_ATTRIBUTE_READONLY))
      fail(L"无法设置文件夹属性");
  }
  if (sendNotification)
    notify(p);
  return normal;
}
static bool sameBytes(const fs::path &first, const fs::path &second) {
  Handle a{CreateFileW(first.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
  Handle b{CreateFileW(second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
  if (a.h == INVALID_HANDLE_VALUE || b.h == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER sa{}, sb{};
  if (!GetFileSizeEx(a.h, &sa) || !GetFileSizeEx(b.h, &sb) || sa.QuadPart != sb.QuadPart)
    return false;
  char ba[8192], bb[8192];
  for (;;) {
    DWORD na = 0, nb = 0;
    if (!ReadFile(a.h, ba, sizeof(ba), &na, nullptr) ||
        !ReadFile(b.h, bb, sizeof(bb), &nb, nullptr) || na != nb || memcmp(ba, bb, na))
      return false;
    if (!na)
      return true;
  }
}
static void syncCommentCache(const fs::path &folder, const std::wstring &comment) {
  // This legacy Shell API only supports MAX_PATH. Notifications remain the
  // fallback for paths it cannot represent.
  if (folder.native().size() >= MAX_PATH)
    return;
  const auto ini = folder / L"desktop.ini";
  auto snapshot = ini;
  snapshot += L"." + guid() + L".cache-backup";
  // The Shell call really writes the INI (and may add compatibility sections or
  // delete an empty file). Preserve the exact committed file before invoking it.
  // If the snapshot cannot be made, keep the successful save and skip this step.
  if (!CopyFileW(ini.c_str(), snapshot.c_str(), TRUE))
    return;
  SHFOLDERCUSTOMSETTINGS settings{};
  settings.dwSize = sizeof(settings);
  settings.dwMask = FCSM_INFOTIP;
  settings.pszInfoTip = comment.empty() ? nullptr : const_cast<LPWSTR>(comment.c_str());
  SHGetSetFolderCustomSettings(&settings, folder.c_str(), FCS_FORCEWRITE);
  if (sameBytes(ini, snapshot)) {
    if (!DeleteFileW(snapshot.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
      throw Error{lang::format(L"备注已保存，但清理配置备份失败：{0}", {snapshot.native()})};
    return;
  }
  // Keep the cache update but restore our UTF-16 contents atomically, including
  // every unrelated setting and comment. Never apply the writing API in repair().
  DWORD current = GetFileAttributesW(ini.c_str());
  BOOL restored = current == INVALID_FILE_ATTRIBUTES
      ? MoveFileExW(snapshot.c_str(), ini.c_str(), MOVEFILE_WRITE_THROUGH)
      : ReplaceFileW(ini.c_str(), snapshot.c_str(), nullptr, 0, nullptr, nullptr);
  if (!restored)
    throw Error{lang::format(L"备注已写入，但恢复原配置格式失败。完整配置备份保留在：{0}", {snapshot.native()})};
}
void save(const fs::path &p, const std::wstring &value) {
  checkFolder(p);
  auto val = normalize(value);
  auto f = p / L"desktop.ini";
  auto a = GetFileAttributesW(f.c_str());
  std::wstring s;
  if (a != INVALID_FILE_ATTRIBUTES)
    s = readText(f);
  else if (GetLastError() != ERROR_FILE_NOT_FOUND)
    fail(L"无法检查配置文件");
  if (val.empty() && s.empty())
    return;
  auto v = lines(s);
  bool active = false, found = false, inserted = false;
  std::wstring out;
  for (auto &l : v) {
    auto t = trim(l.text);
    if (section(t)) {
      const bool primary = _wcsicmp(t.c_str(), L"[.ShellClassInfo]") == 0;
      // Windows may have stored older copies of the same note in these
      // compatibility sections. Remove only InfoTip there, preserving other keys.
      active = primary || _wcsicmp(t.c_str(), L"[.ShellClassInfo.A]") == 0 ||
               _wcsicmp(t.c_str(), L"[.ShellClassInfo.W]") == 0;
      if (primary && !found) {
        found = true;
        out += l.text + (l.end.empty() ? L"\r\n" : l.end);
        if (!val.empty()) {
          out += L"InfoTip=" + val + L"\r\n";
          inserted = true;
        }
        continue;
      }
    }
    if (active && key(t))
      continue;
    out += l.text + l.end;
  }
  if (!inserted && !val.empty()) {
    if (!out.empty() && out.back() != L'\n' && out.back() != L'\r')
      out += L"\r\n";
    out += L"[.ShellClassInfo]\r\nInfoTip=" + val + L"\r\n";
  }
  writeAtomic(f, out);
  if (!val.empty())
    attributes(p, false);
  syncCommentCache(p, val);
  notify(p);
}
Status repair(const fs::path &p) {
  checkFolder(p);
  auto f = p / L"desktop.ini";
  if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
      return Status::MissingFile;
    fail(L"无法检查备注文件");
  }
  if (note(p).empty())
    return Status::MissingNote;
  return attributes(p) ? Status::Normal : Status::Repaired;
}
fs::path modulePath(HMODULE m) {
  std::wstring s(32768, L'\0');
  auto n = GetModuleFileNameW(m, s.data(), (DWORD)s.size());
  if (!n || n >= s.size())
    fail(L"无法获取程序路径");
  s.resize(n);
  return s;
}
fs::path localRoot() {
  PWSTR p = nullptr;
  auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p);
  if (FAILED(hr))
    fail(L"无法获取用户目录", (DWORD)hr);
  fs::path r = p;
  CoTaskMemFree(p);
  return r / L"Programs" / L"ShibaKit";
}
static fs::path requestPath(const std::wstring &t) {
  if (t.size() != 38 || t.front() != L'{' || t.back() != L'}' ||
      t.find_first_not_of(L"{}0123456789abcdefABCDEF-") != t.npos)
    throw Error{lang::text(L"无效请求标识。")};
  return localRoot() / L"Requests" / (t + L".txt");
}
std::wstring makeRequest(int op, const std::vector<std::wstring> &paths) {
  auto t = guid();
  auto p = requestPath(t);
  fs::create_directories(p.parent_path());
  std::wstring s = std::to_wstring(op) + L"\n";
  for (auto &v : paths) {
    if (v.find_first_of(L"\r\n") != v.npos)
      throw Error{lang::text(L"路径包含换行。")};
    s += v + L"\n";
  }
  writeAtomic(p, s);
  return t;
}
void readRequest(const std::wstring &t, int &op, std::vector<std::wstring> &paths) {
  auto p = requestPath(t);
  auto s = readText(p);
  auto v = lines(s);
  if (v.empty() || v[0].text.size() != 1 || v[0].text[0] < L'0' || v[0].text[0] > L'2')
    throw Error{lang::text(L"无效请求。")};
  op = v[0].text[0] - L'0';
  for (size_t i = 1; i < v.size(); ++i)
    if (!v[i].text.empty())
      paths.push_back(v[i].text);
  DeleteFileW(p.c_str());
}
} // namespace fn
