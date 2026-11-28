#include "../src/core.h"
#include <fstream>
#include <iostream>
#include <shlobj.h>
#include <propkey.h>
#include <propsys.h>
static int checks = 0;
static void expect(bool ok, const char *name) {
  ++checks;
  if (!ok)
    throw std::runtime_error(name);
}
static void fixture(const fn::fs::path &p, const std::wstring &s) {
  std::ofstream f(p, std::ios::binary);
  wchar_t bom = 0xFEFF;
  f.write((char *)&bom, 2);
  f.write((const char *)s.data(), s.size() * 2);
}
static std::wstring shellNote(const fn::fs::path &p) {
  IShellItem2 *item = nullptr;
  auto hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (FAILED(hr))
    throw std::runtime_error("shell item");
  PWSTR text = nullptr;
  hr = item->GetString(PKEY_Comment, &text);
  std::wstring s = text ? text : L"";
  CoTaskMemFree(text);
  item->Release();
  return s;
}
static std::vector<char> bytes(const fn::fs::path &p) {
  std::ifstream stream(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
int wmain(int argc, wchar_t **argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (argc == 6 && std::wstring(argv[1]) == L"--warm-comment") {
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(argv[5]) + L".ready").c_str());
    HANDLE saved = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(argv[5]) + L".saved").c_str());
    bool valid = ready && saved && shellNote(argv[2]) == argv[3];
    if (ready) SetEvent(ready);
    DWORD index = 0;
    if (valid) valid = SUCCEEDED(CoWaitForMultipleHandles(
        COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, 10000, 1, &saved, &index));
    // Explorer pumps its UI messages while receiving the Shell notifications.
    const auto deadline = GetTickCount64() + 500;
    while (valid && GetTickCount64() < deadline) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (valid) valid = shellNote(argv[2]) == argv[4];
    if (ready) CloseHandle(ready);
    if (saved) CloseHandle(saved);
    CoUninitialize();
    return valid ? 0 : 1;
  }
  if (argc == 4 && std::wstring(argv[1]) == L"--cold-comment") {
    bool equal = shellNote(argv[2]) == argv[3];
    CoUninitialize();
    return equal ? 0 : 1;
  }
  auto root = fn::fs::temp_directory_path() /
              (L"ShibaKit-Test-" + std::to_wstring(GetCurrentProcessId()));
  fn::fs::create_directories(root);
  try {
    auto p = root / L"中文 文件夹";
    fn::fs::create_directory(p);
    expect(fn::repair(p) == fn::Status::MissingFile, "missing file");
    expect(shellNote(p).empty(), "prime empty comment cache");
    fn::save(p, L"中文 English 😀");
    expect(fn::note(p) == L"中文 English 😀", "unicode roundtrip");
    expect(shellNote(p) == L"中文 English 😀", "Explorer comment");
    expect(fn::readText(p / L"desktop.ini") == L"[.ShellClassInfo]\r\nInfoTip=中文 English 😀\r\n",
           "cache update does not leave compatibility sections");
    const std::wstring expected = L"\xFEFF[.ShellClassInfo]\r\nInfoTip=中文 English 😀\r\n";
    const auto raw = bytes(p / L"desktop.ini");
    expect(raw.size() == expected.size() * sizeof(wchar_t) &&
           !memcmp(raw.data(), expected.data(), raw.size()), "exact UTF16 byte preservation after cache sync");
    expect(fn::repair(p) == fn::Status::Normal, "normal");
    auto ini = p / L"desktop.ini";
    auto original = fn::readText(ini);
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    expect(fn::repair(p) == fn::Status::Repaired, "repair lost attributes");
    expect(fn::readText(ini) == original, "repair preserves bytes decoded");
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    fixture(ini, L"; "
                 L"comment\r\n[.ShellClassInfo]\r\nIconResource=C:\\icons.ico,"
                 L"0\r\nInfoTip=old\r\nCustomKey=keep\r\n[Other]\r\nKey=Value\r\n");
    fn::save(p, L"新备注");
    expect(shellNote(p) == L"新备注", "edited Explorer comment cache");
    auto s = fn::readText(ini);
    expect(s.find(L"; comment\r\n") != s.npos &&
               s.find(L"IconResource=C:\\icons.ico,0") != s.npos &&
               s.find(L"CustomKey=keep") != s.npos && s.find(L"[Other]\r\nKey=Value") != s.npos,
           "preserve unrelated settings");
    fn::save(p, L"");
    expect(shellNote(p).empty(), "cleared Explorer comment cache");
    expect(fn::note(p).empty(), "clear");
    expect(fn::readText(ini).find(L"IconResource=") != std::wstring::npos, "clear preserves icon");
    expect(fn::repair(p) == fn::Status::MissingNote, "no infotip");
    auto empty = root / L"empty-note";
    fn::fs::create_directory(empty);
    fn::save(empty, L"old");
    expect(shellNote(empty) == L"old", "prime simple note");
    fn::save(empty, L"");
    expect(fn::fs::exists(empty / L"desktop.ini") &&
           fn::readText(empty / L"desktop.ini") == L"[.ShellClassInfo]\r\n" && shellNote(empty).empty(),
           "clear restores exact empty configuration after Shell removes it");
    expect(fn::normalize(L"  a\nb\t ") == L"a b", "normalize");
    bool rejected = false;
    try {
      fn::normalize(std::wstring(501, L'a'));
    } catch (...) {
      rejected = true;
    }
    expect(rejected, "length limit");
    std::wstring emoji;
    for (int i = 0; i < 500; ++i)
      emoji += L"😀";
    expect(fn::normalize(emoji) == emoji, "500 unicode scalars");
    fn::save(p, L"keep");
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_READONLY);
    rejected = false;
    try {
      fn::save(p, L"overwrite");
    } catch (...) {
      rejected = true;
    }
    expect(rejected && fn::note(p) == L"keep", "readonly preserves original");
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    {
      std::ofstream f(ini, std::ios::binary);
      f << "[.ShellClassInfo]\r\nInfoTip=ANSI ASCII\r\n; kept\r\n";
    }
    fn::save(p, L"转换");
    expect(fn::note(p) == L"转换" && fn::readText(ini).find(L"; kept") != std::wstring::npos,
           "legacy encoding");
    auto stable = bytes(ini);
    fn::save(p, L"转换");
    expect(bytes(ini) == stable, "repeat Unicode save preserves all bytes");
    size_t files = 0;
    for (const auto &entry : fn::fs::directory_iterator(p)) { (void)entry; ++files; }
    expect(files == 1, "successful save leaves no cache snapshot");
    auto compatibility = root / L"compatibility";
    fn::fs::create_directory(compatibility);
    fixture(compatibility / L"desktop.ini", L"[.ShellClassInfo]\r\nInfoTip=old\r\n"
            L"[.ShellClassInfo.A]\r\nInfoTip=old\r\nOtherA=keep\r\n"
            L"[.ShellClassInfo.W]\r\nInfoTip=old\r\nOtherW=keep\r\n");
    fn::save(compatibility, L"更新 😀");
    auto compatText = fn::readText(compatibility / L"desktop.ini");
    expect(shellNote(compatibility) == L"更新 😀" && compatText.find(L"InfoTip=old") == compatText.npos &&
           compatText.find(L"OtherA=keep") != compatText.npos && compatText.find(L"OtherW=keep") != compatText.npos,
           "old compatibility note removed; other settings preserved");
    auto command = L"\"" + fn::modulePath().native() + L"\" --cold-comment \"" +
                   compatibility.native() + L"\" \"更新 😀\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    expect(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &child) != FALSE, "start cold Shell reader");
    DWORD code = 1;
    WaitForSingleObject(child.hProcess, 10000);
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    expect(code == 0, "cold process reads restored Unicode comment correctly");
    const auto crossProcess = root / L"cross-process-cache";
    fn::fs::create_directory(crossProcess);
    fn::save(crossProcess, L"before");
    const auto event = L"Local\\ShibaKit.CacheTest." + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (event + L".ready").c_str());
    HANDLE saved = CreateEventW(nullptr, TRUE, FALSE, (event + L".saved").c_str());
    command = L"\"" + fn::modulePath().native() + L"\" --warm-comment \"" + crossProcess.native() +
              L"\" before after \"" + event + L"\"";
    expect(ready && saved && CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "start warm independent reader");
    expect(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "independent reader primed old cache");
    fn::save(crossProcess, L"after");
    SetEvent(saved);
    code = 1;
    WaitForSingleObject(child.hProcess, 10000);
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    CloseHandle(ready); CloseHandle(saved);
    expect(code == 0, "independent warm process receives updated Shell comment");
    auto deep = root;
    auto quoted = root / L"quoted";
    fn::fs::create_directory(quoted);
    fn::save(quoted, L"\"引号测试\"");
    expect(shellNote(quoted) == L"\"引号测试\"", "literal quotes match Explorer");
    auto broken = root / L"broken";
    fn::fs::create_directory(broken);
    {
      std::ofstream f(broken / L"desktop.ini", std::ios::binary);
      unsigned char bytes[] = {255, 254, 'x'};
      f.write(reinterpret_cast<const char*>(bytes), 3);
    }
    rejected = false;
    try {
      fn::save(broken, L"replacement");
    } catch (...) {
      rejected = true;
    }
    expect(rejected && fn::fs::file_size(broken / L"desktop.ini") == 3, "malformed file untouched");
    auto copy = root / L"copied";
    fn::fs::create_directory(copy);
    fn::fs::copy_file(ini, copy / L"desktop.ini");
    SetFileAttributesW((copy / L"desktop.ini").c_str(), FILE_ATTRIBUTE_NORMAL);
    expect(fn::repair(copy) == fn::Status::Repaired && shellNote(copy) == L"转换",
           "copied folder recovery");
    for (int i = 0; i < 8; ++i)
      deep /= L"long-path-segment-abcdefghijklmnopqrstuvwxyz";
    fn::fs::create_directories(deep);
    fn::save(deep, L"长路径");
    expect(fn::note(deep) == L"长路径", "long path");
    std::vector<std::wstring> paths;
    for (int i = 0; i < 1000; ++i)
      paths.push_back((root / std::to_wstring(i)).native());
    auto token = fn::makeRequest(2, paths);
    int op = -1;
    std::vector<std::wstring> decoded;
    fn::readRequest(token, op, decoded);
    expect(op == 2 && decoded == paths, "1000 selection request");
    if (argc == 2 && std::wstring(argv[1]) == L"--cache-only") {
      std::cout << "PASS " << checks << " save/cache/format assertions\n";
      CoUninitialize();
      return 0;
    }
    for (int i = 0; i < 1000; ++i) {
      auto d = root / std::to_wstring(i);
      fn::fs::create_directory(d);
      fixture(d / L"desktop.ini", L"[.ShellClassInfo]\r\nInfoTip=batch\r\n");
      expect(fn::repair(d) == fn::Status::Repaired, "batch repair");
    }
    std::cout << "PASS " << checks << " checks\n";
    std::wcout << L"Test fixtures: " << root.native() << L"\n";
  } catch (const fn::Error &e) {
    std::wcerr << e.message << L"\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  CoUninitialize();
  return 0;
}
