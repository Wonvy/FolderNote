#include "update.h"
#include "language.h"
#include "core.h"
#include <array>
#include <bcrypt.h>
#include <fstream>
#include <roapi.h>
#include <string_view>
#include <windows.data.json.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

namespace updater {
namespace {
using namespace ABI::Windows::Data::Json;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
constexpr auto invalidRelease = L"GitHub 发布信息格式无效。";
constexpr size_t metadataLimit = 2 * 1024 * 1024, packageLimit = 100 * 1024 * 1024;

const wchar_t *packageName(bool win11Package, bool standaloneInstaller) {
  if (standaloneInstaller)
    return win11Package ? L"FolderNote-Setup-Win11-x64.exe" : L"FolderNote-Setup-x64.exe";
  return win11Package ? L"FolderNote-win11-test.zip" : L"FolderNote-win-x64.zip";
}

bool repositoryName(std::wstring_view name) {
  const auto split = name.find(L'/');
  if (split == name.npos || split == 0 || split + 1 == name.size() || name.size() > 200 ||
      name.find(L'/', split + 1) != name.npos) return false;
  const auto owner = name.substr(0, split), repo = name.substr(split + 1);
  return owner.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == name.npos &&
      repo.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == name.npos &&
      repo != L"." && repo != L"..";
}

std::array<unsigned, 3> versionParts(std::wstring_view version) {
  std::array<unsigned, 3> parts{};
  for (size_t i = 0; i < parts.size(); ++i) {
    auto end = version.find(L'.');
    if ((end == version.npos) != (i == parts.size() - 1)) throw fn::Error{lang::text(invalidRelease)};
    auto part = version.substr(0, end);
    if (part.empty() || part.size() > 5 || part.find_first_not_of(L"0123456789") != part.npos)
      throw fn::Error{lang::text(invalidRelease)};
    for (auto digit : part) parts[i] = parts[i] * 10 + static_cast<unsigned>(digit - L'0');
    if (parts[i] > 65535) throw fn::Error{lang::text(invalidRelease)};
    version.remove_prefix(part.size() + (end != version.npos ? 1 : 0));
  }
  return parts;
}

struct HttpHandle {
  HINTERNET value = nullptr;
  ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};

template <typename Read>
void get(const std::wstring &url, size_t limit, DWORD deadlineMs, Read read) {
  URL_COMPONENTSW parts{sizeof(parts)};
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
  parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
      parts.nPort != INTERNET_DEFAULT_HTTPS_PORT || parts.dwUserNameLength || parts.dwPasswordLength ||
      !parts.dwHostNameLength || !parts.dwUrlPathLength)
    throw fn::Error{lang::text(L"更新下载地址无效。")};

  auto deadline = GetTickCount64() + deadlineMs;
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  const wchar_t *headers = host == L"api.github.com"
      ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
  HttpHandle session{WinHttpOpen(L"FolderNote Update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000))
    fn::fail(lang::text(L"无法准备 GitHub 连接"));
  HttpHandle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
  if (!connection.value) fn::fail(lang::text(L"无法连接 GitHub"));
  HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
  if (!request.value) fn::fail(lang::text(L"无法准备 GitHub 请求"));
  DWORD redirects = 5, policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
      !WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects)) ||
      !WinHttpSendRequest(request.value, headers, headers ? DWORD(-1) : 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) fn::fail(lang::text(L"无法从 GitHub 获取更新"));
  DWORD status = 0, bytes = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
    fn::fail(lang::text(L"无法读取 GitHub 响应"));
  if (status == 404) throw fn::Error{lang::text(L"GitHub 上找不到公开发布或对应的安装包。")};
  if (status != 200) throw fn::Error{lang::format(L"GitHub 请求失败（HTTP {0}）。", {std::to_wstring(status)})};

  std::array<char, 16384> buffer;
  size_t total = 0;
  for (;;) {
    if (GetTickCount64() >= deadline) throw fn::Error{lang::text(L"下载 GitHub 更新超时。")};
    DWORD received = 0;
    if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received))
      fn::fail(lang::text(L"下载 GitHub 更新时连接中断"));
    if (!received) break;
    if (received > limit - total) throw fn::Error{lang::text(L"GitHub 返回的更新文件过大。")};
    read(buffer.data(), received);
    total += received;
  }
}

struct Runtime {
  HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
  ~Runtime() { if (SUCCEEDED(status)) RoUninitialize(); }
};

std::wstring jsonString(IJsonObject *object, const wchar_t *name) {
  HString value;
  if (FAILED(object->GetNamedString(HStringReference(name).Get(), value.GetAddressOf())))
    throw fn::Error{lang::text(invalidRelease)};
  unsigned length = 0;
  const auto data = value.GetRawBuffer(&length);
  return std::wstring(data, length);
}

std::wstring optionalJsonString(IJsonObject *object, const wchar_t *name) {
  ComPtr<IJsonValue> val;
  if (!object || FAILED(object->GetNamedValue(HStringReference(name).Get(), &val)) || !val) return {};
  JsonValueType type = JsonValueType_Null;
  if (FAILED(val->get_ValueType(&type)) || type != JsonValueType_String) return {};
  HString str;
  if (FAILED(val->GetString(str.GetAddressOf()))) return {};
  unsigned length = 0;
  const auto data = str.GetRawBuffer(&length);
  return data ? std::wstring(data, length) : std::wstring{};
}

std::wstring wideUtf8(const std::string &value) {
  const auto size = static_cast<int>(value.size());
  const auto chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, nullptr, 0);
  if (!chars) throw fn::Error{lang::text(invalidRelease)};
  std::wstring wide(chars, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, wide.data(), chars))
    throw fn::Error{lang::text(invalidRelease)};
  return wide;
}

bool validDigest(std::wstring_view value) {
  return value.size() == 64 && value.find_first_not_of(L"0123456789abcdefABCDEF") == value.npos;
}

struct Sha256 {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  ~Sha256() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};

std::wstring downloadDigest(const std::wstring &url, const fn::fs::path &path) {
  Sha256 digest;
  if (BCryptOpenAlgorithmProvider(&digest.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) ||
      BCryptCreateHash(digest.algorithm, &digest.hash, nullptr, 0, nullptr, 0, 0))
    throw fn::Error{lang::text(L"无法初始化更新校验。")};
  std::ofstream file(path, std::ios::binary);
  if (!file) throw fn::Error{lang::text(L"无法写入临时安装包。")};
  get(url, packageLimit, 180000, [&](char *buffer, DWORD length) {
    if (!file.write(buffer, length)) throw fn::Error{lang::text(L"无法保存临时安装包。")};
    if (BCryptHashData(digest.hash, reinterpret_cast<PUCHAR>(buffer), length, 0))
      throw fn::Error{lang::text(L"无法校验更新安装包。")};
  });
  file.close();
  if (!file) throw fn::Error{lang::text(L"无法保存临时安装包。")};
  std::array<UCHAR, 32> hash{};
  if (BCryptFinishHash(digest.hash, hash.data(), static_cast<ULONG>(hash.size()), 0))
    throw fn::Error{lang::text(L"无法校验更新安装包。")};
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring encoded;
  for (auto byte : hash) { encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
  return encoded;
}

struct Staging {
  fn::fs::path path;
  Staging() {
    GUID guid{};
    wchar_t token[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, ARRAYSIZE(token)))
      throw fn::Error{lang::text(L"无法准备更新目录。")};
    path = fn::fs::temp_directory_path() / (std::wstring(L"FolderNote-Update-") + token);
    if (!CreateDirectoryW(path.c_str(), nullptr)) fn::fail(lang::text(L"无法创建更新目录"));
  }
  ~Staging() {
    if (path.empty()) return;
    std::error_code error;
    fn::fs::remove_all(path, error);
  }
};

struct InstallLock {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FolderNote.Update.Install");

  InstallLock() {
    if (mutex) {
      const auto status = WaitForSingleObject(mutex, 0);
      if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) return;
      CloseHandle(mutex);
    }
    throw fn::Error{lang::text(L"更新失败，请稍后重试。")};
  }

  ~InstallLock() {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
};

void run(const fn::fs::path &program, const fn::fs::path &workingDirectory, const std::wstring &arguments,
         const wchar_t *error, bool console = false, bool wait = true) {
  auto command = L"\"" + program.native() + L"\" " + arguments;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, console ? CREATE_NO_WINDOW : 0,
                      nullptr, workingDirectory.c_str(), &startup, &process)) fn::fail(error);
  CloseHandle(process.hThread);
  if (!console) AllowSetForegroundWindow(process.dwProcessId);
  if (!wait) {
    CloseHandle(process.hProcess);
    return;
  }
  const auto completed = WaitForSingleObject(process.hProcess, console ? 60000 : INFINITE);
  DWORD exitCode = 1;
  if (completed == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
  else if (console) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 2000);
  }
  CloseHandle(process.hProcess);
  if (completed != WAIT_OBJECT_0 || exitCode) throw fn::Error{error};
}

void extract(const fn::fs::path &directory, bool win11Package) {
  wchar_t system[MAX_PATH]{};
  auto length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  if (!length || length >= ARRAYSIZE(system)) throw fn::Error{lang::text(L"找不到 Windows 解压组件。")};
  const auto powershell = fn::fs::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
  // Use only fixed filenames. Extracting selected entries prevents paths from
  // the archive from escaping staging or introducing files beside the installer.
  std::wstring script =
      L"-NoProfile -NonInteractive -Command \""
      L"$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
      L"$directory=(Get-Location).ProviderPath; "
      L"$zip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::Combine($directory,'update.zip')); "
      L"try { $total=0; foreach ($name in 'Install.exe','Uninstall.exe','FolderNote.exe',"
      L"'FolderNoteShell.dll','FolderNoteNavigation.dll'";
  if (win11Package) script += L",'Install-Win11.exe','FolderNote.msix'";
  script += L",'Microsoft.WindowsAppRuntime.Bootstrap.dll') { $entry=$zip.GetEntry($name); "
            L"if (!$entry -and $name -eq 'Microsoft.WindowsAppRuntime.Bootstrap.dll') { continue }; "
            L"if (!$entry -or $entry.Length -le 0) { throw 'Missing installer file' }; "
            L"$total+=$entry.Length; if ($total -gt 200MB) { throw 'Installer files are too large' }; "
            L"[IO.Compression.ZipFileExtensions]::ExtractToFile($entry,[IO.Path]::Combine($directory,$name),$false) "
            L"} } finally { $zip.Dispose() }\"";
  run(powershell, directory, script, lang::text(L"无法解压更新安装包。"), true);
}
} // namespace

std::wstring formatReleaseNotes(std::wstring_view markdown) {
  std::wstring result;
  bool lastWasBlank = true;
  size_t pos = 0;
  while (pos < markdown.size()) {
    size_t end = markdown.find(L'\n', pos);
    if (end == std::wstring_view::npos) end = markdown.size();
    std::wstring_view rawLine = markdown.substr(pos, end - pos);
    pos = end + 1;
    while (!rawLine.empty() && (rawLine.back() == L'\r' || rawLine.back() == L' ' || rawLine.back() == L'\t')) {
      rawLine.remove_suffix(1);
    }
    size_t indent = 0;
    while (indent < rawLine.size() && (rawLine[indent] == L' ' || rawLine[indent] == L'\t')) ++indent;
    std::wstring_view trimmed = rawLine.substr(indent);
    if (trimmed.empty()) {
      if (!lastWasBlank && !result.empty()) {
        result += L"\r\n";
        lastWasBlank = true;
      }
      continue;
    }

    std::wstring line;
    if (trimmed.starts_with(L"#")) {
      size_t hashes = 0;
      while (hashes < trimmed.size() && trimmed[hashes] == L'#') ++hashes;
      auto heading = trimmed.substr(hashes);
      while (!heading.empty() && (heading.front() == L' ' || heading.front() == L'\t')) heading.remove_prefix(1);
      if (heading.empty()) continue;
      line = std::wstring(heading);
    } else if (trimmed.starts_with(L"- ") || trimmed.starts_with(L"* ")) {
      if (indent >= 2) line += L"  ";
      line += L"• ";
      line.append(trimmed.substr(2));
    } else {
      if (indent >= 2) line += L"  ";
      line.append(trimmed);
    }

    std::wstring cleaned;
    cleaned.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
      if (i + 1 < line.size() && line[i] == L'*' && line[i + 1] == L'*') {
        i += 2;
        continue;
      }
      if (line[i] == L'`') {
        ++i;
        continue;
      }
      cleaned.push_back(line[i]);
      ++i;
    }

    if (cleaned.starts_with(L"下载 FolderNote-Setup") ||
        cleaned == L"ZIP 用于兼容旧版在线更新。") {
      continue;
    }

    if (!result.empty()) result += L"\r\n";
    result += cleaned;
    lastWasBlank = false;
  }
  while (result.size() >= 2 && result.ends_with(L"\r\n")) {
    result.resize(result.size() - 2);
  }
  return result;
}

std::optional<Release> check(const std::wstring &repository, const std::wstring &currentVersion, bool win11Package) {
  if (!repositoryName(repository)) throw fn::Error{lang::text(L"当前构建没有有效的 GitHub 更新仓库。")};
  std::string content;
  get(L"https://api.github.com/repos/" + repository + L"/releases/latest", metadataLimit, 20000,
      [&](const char *buffer, DWORD size) { content.append(buffer, size); });
  return parseRelease(content, repository, currentVersion, win11Package);
}

std::optional<Release> parseRelease(const std::string &content, const std::wstring &repository,
                                   const std::wstring &currentVersion, bool win11Package) {
  if (!repositoryName(repository) || content.size() > metadataLimit) throw fn::Error{lang::text(invalidRelease)};
  Runtime runtime;
  ComPtr<IJsonObjectStatics> parser;
  if ((FAILED(runtime.status) && runtime.status != RPC_E_CHANGED_MODE) ||
      FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonObject).Get(),
                                    IID_PPV_ARGS(&parser))))
    throw fn::Error{lang::text(L"无法读取 GitHub 发布信息。")};
  const auto wide = wideUtf8(content);
  HString body;
  ComPtr<IJsonObject> object;
  if (FAILED(body.Set(wide.data(), static_cast<unsigned>(wide.size()))) ||
      FAILED(parser->Parse(body.Get(), &object))) throw fn::Error{lang::text(invalidRelease)};
  const auto tag = jsonString(object.Get(), L"tag_name");
  if (!tag.starts_with(L'v')) throw fn::Error{lang::text(invalidRelease)};
  const auto version = tag.substr(1);
  if (versionParts(version) <= versionParts(currentVersion)) return {};
  auto notes = formatReleaseNotes(optionalJsonString(object.Get(), L"body"));
  if (notes.empty()) {
    const auto title = formatReleaseNotes(optionalJsonString(object.Get(), L"name"));
    if (!title.empty() && title != tag && title != version) notes = title;
  }
  ComPtr<IJsonArray> assets;
  ComPtr<ABI::Windows::Foundation::Collections::IVector<IJsonValue *>> entries;
  unsigned count = 0;
  if (FAILED(object->GetNamedArray(HStringReference(L"assets").Get(), &assets)) ||
      FAILED(assets.As(&entries)) || FAILED(entries->get_Size(&count))) throw fn::Error{lang::text(invalidRelease)};
  // Prefer the standalone installer regardless of GitHub's asset order. ZIPs
  // remain supported for older releases and are still published for old clients.
  for (bool standalone : {true, false}) {
   const auto name = packageName(win11Package, standalone);
   for (unsigned i = 0; i < count; ++i) {
    ComPtr<IJsonObject> asset;
    if (FAILED(assets->GetObjectAt(i, &asset))) throw fn::Error{lang::text(invalidRelease)};
    if (jsonString(asset.Get(), L"name") != name) continue;
    auto url = jsonString(asset.Get(), L"browser_download_url");
    const auto expected = L"https://github.com/" + repository + L"/releases/download/" + tag + L"/" + name;
    if (CompareStringOrdinal(url.c_str(), static_cast<int>(url.size()), expected.c_str(),
                             static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL) throw fn::Error{lang::text(invalidRelease)};
    auto digest = jsonString(asset.Get(), L"digest");
    if (!digest.starts_with(L"sha256:") || !validDigest(std::wstring_view(digest).substr(7)))
      throw fn::Error{lang::text(L"GitHub 发布缺少有效的 SHA-256 校验值。")};
    return Release{version, std::move(url), digest.substr(7), win11Package, standalone, std::move(notes)};
   }
  }
  throw fn::Error{lang::text(L"GitHub 发布缺少适用的安装包。")};
}

bool install(const Release &release, bool (*allowAutomaticInstall)()) {
  if (allowAutomaticInstall && release.win11Package) throw fn::Error{lang::text(L"无法启动更新安装。")};
  Staging staging;
  const auto package = staging.path / (release.standaloneInstaller ? L"Setup.exe" : L"update.zip");
  const auto digest = downloadDigest(release.downloadUrl, package);
  if (_wcsicmp(digest.c_str(), release.sha256.c_str()) != 0)
    throw fn::Error{lang::text(L"安装包 SHA-256 校验失败，请重新下载。")};
  if (!release.standaloneInstaller) extract(staging.path, release.win11Package);
  const auto installer = release.standaloneInstaller ? package : staging.path /
      (release.win11Package ? L"Install-Win11.exe" : L"Install.exe");
  if (release.win11Package) {
    run(installer, staging.path, L"--parent " + std::to_wstring(GetCurrentProcessId()),
        lang::text(L"无法启动更新安装。"), false, false);
    // MSIX replacement waits for this process to close. Its detached installer
    // still needs the unpacked files, so leave staging in the temporary directory.
    staging.path.clear();
  } else {
    // Different settings builds can be open at the same time and share the same registration.
    const InstallLock lock;
    if (allowAutomaticInstall && !allowAutomaticInstall()) return false;
    run(installer, staging.path, allowAutomaticInstall ? L"--automatic-update" : L"",
        lang::text(L"更新安装未完成。"));
  }
  return true;
}
}
