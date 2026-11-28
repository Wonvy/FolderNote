// Optional isolated cache experiment; operates only in a new temporary folder.
#include "../src/core.h"
#include <shlobj.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
using Microsoft::WRL::ComPtr;
static std::vector<char> bytes(const fn::fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void fixture(const fn::fs::path &p, const std::wstring &s) {
  std::ofstream f(p, std::ios::binary);
  const wchar_t bom = 0xfeff;
  f.write(reinterpret_cast<const char *>(&bom), sizeof(bom));
  f.write(reinterpret_cast<const char *>(s.data()), s.size() * sizeof(wchar_t));
}
static std::wstring ascii(const std::wstring &s) {
  std::wstring out;
  for (wchar_t ch : s) {
    if (ch >= 32 && ch < 127) out += ch;
    else { wchar_t text[8]; swprintf_s(text, L"\\u%04x", ch); out += text; }
  }
  return out;
}
static void probeSave(const fn::fs::path &p, const std::wstring &value) {
  std::wstring old = fn::fs::exists(p / L"desktop.ini") ? fn::readText(p / L"desktop.ini") : L"[.ShellClassInfo]\r\n";
  auto start = old.find(L"InfoTip=");
  if (start != std::wstring::npos) {
    auto end = old.find(L'\n', start);
    old.erase(start, end == std::wstring::npos ? old.size() - start : end + 1 - start);
  }
  auto header = old.find(L"[.ShellClassInfo]\r\n");
  if (!value.empty()) old.insert(header + 19, L"InfoTip=" + value + L"\r\n");
  fixture(p / L"pending.ini", old);
  SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_NORMAL);
  if (!MoveFileExW((p / L"pending.ini").c_str(), (p / L"desktop.ini").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw fn::Error{L"replace failed"};
  SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
  PathMakeSystemFolderW(p.c_str());
}
static ULONGLONG modified(const fn::fs::path &p) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data);
  return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}
static ComPtr<IShellItem2> item(const fn::fs::path &p) {
  ComPtr<IShellItem2> result;
  HRESULT hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&result));
  if (FAILED(hr)) throw fn::Error{L"create shell item failed"};
  return result;
}
static std::wstring note(IShellItem2 *i) {
  PWSTR text = nullptr;
  HRESULT hr = i->GetString(PKEY_Comment, &text);
  std::wstring result = text ? text : L"";
  CoTaskMemFree(text);
  return SUCCEEDED(hr) ? result : L"<property failed " + std::to_wstring(static_cast<unsigned long>(hr)) + L">";
}
static std::wstring folderNote(const fn::fs::path &p) {
  PIDLIST_ABSOLUTE pidl = nullptr;
  HRESULT hr = SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr);
  if (FAILED(hr)) return L"<parse failed>";
  ComPtr<IShellFolder2> parent;
  PCUITEMID_CHILD child = nullptr;
  hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
  VARIANT result{};
  if (SUCCEEDED(hr)) hr = parent->GetDetailsEx(child, &PKEY_Comment, &result);
  std::wstring text = SUCCEEDED(hr) && result.vt == VT_BSTR && result.bstrVal ? result.bstrVal : L"";
  VariantClear(&result);
  CoTaskMemFree(pidl);
  return text;
}
static void pump(DWORD milliseconds) {
  const ULONGLONG until = GetTickCount64() + milliseconds;
  while (GetTickCount64() < until) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    DWORD remaining = static_cast<DWORD>(until - std::min(until, GetTickCount64()));
    MsgWaitForMultipleObjectsEx(0, nullptr, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
}
int wmain() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  GUID guid{}; CoCreateGuid(&guid); wchar_t token[40]; StringFromGUID2(guid, token, 40);
  auto root = fn::fs::temp_directory_path() / (std::wstring(L"ShibaKit-CacheProbe-") + token);
  fn::fs::create_directory(root);
  const std::wstring original = L"; Keep comment\r\n[.ShellClassInfo]\r\n"
      L"InfoTip=original\r\nIconResource=C:\\icons.ico,0\r\nCustomKey = keep\r\n"
      L"Logo=cover.png\r\n[Other]\r\n; unrelated\r\nKey=Value\r\n";
  std::wcout << L"Fixture: " << root << L"\n";
  try {
    for (int variant = 0; variant != 17; ++variant) {
      auto p = root / std::to_wstring(variant);
      fn::fs::create_directory(p);
      fixture(p / L"desktop.ini", original);
      SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
      PathMakeSystemFolderW(p.c_str());
      auto held = item(p);
      wchar_t profile[1024]{};
      GetPrivateProfileStringW(L".ShellClassInfo", L"InfoTip", L"missing", profile, 1024, (p / L"desktop.ini").c_str());
      std::wcout << L"case=" << variant << L" attrs=" << GetFileAttributesW(p.c_str())
                 << L" iniAttrs=" << GetFileAttributesW((p / L"desktop.ini").c_str())
                 << L" profile=" << profile << L" disk=" << fn::note(p) << L" prime=" << note(held.Get());
      probeSave(p, L"replacement");
      const auto before = bytes(p / L"desktop.ini");
      std::wcout << L" freshAfterSave=" << note(item(p).Get());
      HRESULT hr = S_OK;
      if (variant == 0) {
        hr = held->Update(nullptr);
      } else if (variant == 1) {
        std::wcout << L" newFolderDetails=" << folderNote(p);
      } else if (variant == 2 || variant == 3 || variant == 4) {
        SHFOLDERCUSTOMSETTINGS settings{};
        settings.dwSize = sizeof(settings);
        wchar_t value[] = L"replacement";
        if (variant == 3) { settings.dwMask = FCSM_INFOTIP; settings.pszInfoTip = value; }
        if (variant == 4) { settings.dwMask = FCSM_FLAGS; settings.dwFlags = 0; }
        hr = SHGetSetFolderCustomSettings(&settings, p.c_str(), FCS_FORCEWRITE);
      } else if (variant == 5) {
        auto ini = p / L"desktop.ini";
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHParseDisplayName(ini.c_str(), nullptr, &pidl, 0, nullptr);
        if (SUCCEEDED(hr)) {
          SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_IDLIST | SHCNF_FLUSH, pidl, nullptr);
          CoTaskMemFree(pidl);
        }
      } else if (variant == 7) {
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, (p / L"desktop.ini").c_str());
      } else if (variant == 8) {
        std::thread other([&] {
          CoInitializeEx(nullptr, COINIT_MULTITHREADED);
          std::wcout << L" newThread=" << note(item(p).Get());
          CoUninitialize();
        });
        other.join();
      } else if (variant == 9) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr);
        if (SUCCEEDED(hr)) {
          SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH, pidl, nullptr);
          CoTaskMemFree(pidl);
        }
      } else if (variant == 10 || variant == 11) {
        DWORD attributes = GetFileAttributesW(p.c_str());
        SetFileAttributesW(p.c_str(), attributes & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM));
        if (variant == 11) std::wcout << L" afterUnset=" << note(item(p).Get());
        SetFileAttributesW(p.c_str(), attributes);
      } else if (variant == 12) {
        SHFOLDERCUSTOMSETTINGS fcs{}; fcs.dwSize = sizeof(fcs); fcs.dwMask = FCSM_INFOTIP;
        wchar_t value[512]{}; fcs.pszInfoTip = value; fcs.cchInfoTip = 512;
        hr = SHGetSetFolderCustomSettings(&fcs, p.c_str(), FCS_READ);
        std::wcout << L" readFcs=" << value;
      } else if (variant == 13) {
        held.Reset();
      } else if (variant == 14) {
        WritePrivateProfileStringW(L".ShellClassInfo", L"InfoTip", L"replacement", (p / L"desktop.ini").c_str());
      } else if (variant == 15) {
        HANDLE file = CreateFileW((p / L"desktop.ini").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME now{}; GetSystemTimeAsFileTime(&now); SetFileTime(file, nullptr, nullptr, &now); CloseHandle(file);
      } else if (variant == 16) {
        SHChangeNotify(SHCNE_ATTRIBUTES, SHCNF_PATHW | SHCNF_FLUSH, (p / L"desktop.ini").c_str(), nullptr);
      }
      const auto after = bytes(p / L"desktop.ini");
      std::wcout << L" hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
                 << L" unchanged=" << (before == after) << L" bytes=" << before.size() << L"/" << after.size()
                 << L" held=" << (held ? note(held.Get()) : L"<released>") << L" fresh=" << note(item(p).Get());
      pump(500);
      std::wcout << L" afterPumpHeld=" << (held ? note(held.Get()) : L"<released>") << L" afterPumpFresh=" << note(item(p).Get()) << L"\n";
    }
    // No desktop.ini: a no-mask call must not accidentally create an empty one.
    auto absent = root / L"absent"; fn::fs::create_directory(absent);
    SHFOLDERCUSTOMSETTINGS settings{}; settings.dwSize = sizeof(settings);
    HRESULT hr = SHGetSetFolderCustomSettings(&settings, absent.c_str(), FCS_FORCEWRITE);
    std::wcout << L"absent mask0 hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
               << L" iniExists=" << fn::fs::exists(absent / L"desktop.ini") << L"\n";
    for (int scenario = 0; scenario != 13; ++scenario) {
      auto p = root / (L"infotip-" + std::to_wstring(scenario));
      fn::fs::create_directory(p);
      std::wstring value = scenario == 3 ? L"\"quoted value\"" : scenario >= 5 ? L"new value" : L"中文 English 😀";
      if (scenario == 1 || scenario == 2) value.clear();
      if (scenario != 0) probeSave(p, L"old value");
      if (scenario >= 10) {
        SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_NORMAL);
        fixture(p / L"desktop.ini", original);
        SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
      }
      auto held = item(p);
      std::wcout << L"infotip-scenario=" << scenario << L" prime=" << note(held.Get());
      probeSave(p, value);
      if (scenario >= 10) {
        auto text = fn::readText(p / L"desktop.ini");
        auto index = text.find(L"CustomKey = keep");
        text.replace(index, std::wstring(L"CustomKey = keep").size(), L"CustomKey = newly changed");
        if (scenario == 11) text += L"; new comment\r\n[Unknown New Section]\r\nNewKey = value\r\n";
        if (scenario == 12) {
          index = text.find(L"InfoTip=");
          text.replace(index, std::wstring(L"InfoTip=").size(), L"InfoTip = ");
        }
        SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_NORMAL);
        fixture(p / L"desktop.ini", text);
        SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
      }
      auto before = bytes(p / L"desktop.ini");
      auto beforeTime = modified(p / L"desktop.ini");
      std::wcout << L" afterSave=" << ascii(note(item(p).Get()));
      if (scenario == 5 || scenario == 7) SetFileAttributesW((p / L"desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY);
      SHFOLDERCUSTOMSETTINGS fcs{}; fcs.dwSize = sizeof(fcs); fcs.dwMask = FCSM_INFOTIP;
      fcs.pszInfoTip = scenario == 1 ? nullptr : value.data();
      hr = SHGetSetFolderCustomSettings(&fcs, p.c_str(), scenario == 6 || scenario == 7 ? FCS_WRITE : FCS_FORCEWRITE);
      std::wcout << L" hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
                 << L" unchanged=" << (before == bytes(p / L"desktop.ini"))
                 << L" timeUnchanged=" << (beforeTime == modified(p / L"desktop.ini"))
                 << L" held=" << ascii(note(held.Get())) << L" fresh=" << ascii(note(item(p).Get()))
                 << L" disk=" << ascii(fn::note(p)) << L"\n";
      if (before != bytes(p / L"desktop.ini")) {
        const auto *first = reinterpret_cast<const wchar_t *>(before.data() + 2);
        std::wcout << L"  before=" << ascii(std::wstring(first, (before.size() - 2) / 2)) << L"\n"
                   << L"  after=" << (fn::fs::exists(p / L"desktop.ini") ? ascii(fn::readText(p / L"desktop.ini")) : L"<deleted>") << L"\n";
      }
    }
  } catch (const fn::Error &error) { std::wcerr << error.message << L"\n"; return 1; }
  CoUninitialize();
}
