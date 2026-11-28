#include "../src/core.h"
#include <shlobj.h>
#include <iostream>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Every setting write is redirected to a fresh test subtree. Restore HKCU
// before removing that subtree, including when an assertion throws.
class IsolatedSettings {
  std::wstring path;
  HKEY key = nullptr;
public:
  IsolatedSettings() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw 20;
    wchar_t token[40]{};
    StringFromGUID2(id, token, ARRAYSIZE(token));
    path = std::wstring(L"Software\\ShibaKit.Tests\\Shell\\") + token;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) throw 21;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS) {
      RegCloseKey(key);
      RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
      throw 22;
    }
  }
  ~IsolatedSettings() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
  void notes(bool enabled) {
    HKEY config{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ShibaKit", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &config, nullptr) != ERROR_SUCCESS) throw 23;
    const DWORD value = enabled ? 1 : 0;
    const auto result = RegSetValueExW(config, L"NotesEnabled", 0, REG_DWORD,
                                      reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(config);
    if (result != ERROR_SUCCESS) throw 24;
  }
};

int wmain(int argc, wchar_t **argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  auto dll = LoadLibraryW((fn::modulePath().parent_path() / L"ShibaKitShell.dll").c_str());
  if (!dll) {
    CoUninitialize();
    return 1;
  }
  auto get =
      (HRESULT(__stdcall *)(REFCLSID, REFIID, void **))GetProcAddress(dll, "DllGetClassObject");
  auto unload = (HRESULT(__stdcall *)())GetProcAddress(dll, "DllCanUnloadNow");
  int rc = 0;
  try {
    if (!get || !unload) throw 25;
    for (int op = 0; op < 4; ++op) {
      {
        GUID cls{};
        CLSIDFromString(op == 3 ? fn::settingsId : fn::ids[op], &cls);
        ComPtr<IClassFactory> factory;
        const bool registered = argc > 1 && std::wstring(argv[1]) == L"--registered";
        if (registered && op == 3) continue; // Compatibility class is no longer registered.
        // Resolve an explicitly requested installed factory before redirecting
        // HKCU, so this optional manual test still sees its COM registration.
        const HRESULT factoryResult =
            registered ? CoGetClassObject(cls, CLSCTX_INPROC_SERVER, nullptr,
                                           IID_PPV_ARGS(&factory))
                       : get(cls, IID_PPV_ARGS(&factory));
        if (FAILED(factoryResult)) throw 1;
        IsolatedSettings settings;
        ComPtr<IExecuteCommand> cmd;
        if (FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&cmd)))) throw 2;
        factory.Reset();
        ComPtr<IExplorerCommand> modern;
        if (FAILED(cmd.As(&modern))) throw 10;
        PWSTR title = nullptr;
        if (FAILED(modern->GetTitle(nullptr, &title)) || !title) throw 11;
        const std::wstring titleText(title);
        CoTaskMemFree(title);
        if (titleText != (op == 3 ? L"ShibaKit 设置" : fn::titles[op])) throw 11;
        GUID canonical{};
        if (FAILED(modern->GetCanonicalName(&canonical)) || canonical != cls) throw 26;
        PWSTR icon = nullptr;
        if (FAILED(modern->GetIcon(nullptr, &icon)) || !icon) throw 27;
        const std::wstring iconText(icon);
        CoTaskMemFree(icon);
        if (iconText.find(L"ShibaKit.exe\",-101") == std::wstring::npos) throw 28;
        EXPCMDSTATE state{};
        const EXPCMDSTATE noNotesState = ECS_HIDDEN;
        if (modern->GetState(nullptr, FALSE, nullptr) != E_POINTER) throw 29;
        modern->GetState(nullptr, FALSE, &state);
        if (state != noNotesState) throw 12;
        ComPtr<IObjectWithSelection> selection;
        if (FAILED(cmd.As(&selection))) throw 3;
        for (int count : {1, 10, 100, 1000}) {
          PIDLIST_ABSOLUTE id = nullptr;
          if (FAILED(SHParseDisplayName(fn::modulePath().parent_path().c_str(), nullptr, &id, 0,
                                        nullptr))) throw 4;
          std::vector<PCIDLIST_ABSOLUTE> ids(count, id);
          ComPtr<IShellItemArray> items;
          const auto arrayResult = SHCreateShellItemArrayFromIDLists(count, ids.data(), &items);
          CoTaskMemFree(id);
          if (FAILED(arrayResult)) throw 5;
          selection->SetSelection(items.Get());
          if (count == 1) {
            // Fresh install: no ShibaKit context commands, even with a selection.
            modern->GetState(items.Get(), FALSE, &state);
            if (state != noNotesState) throw 36;
            if (op != 3 && cmd->Execute() != E_ACCESSDENIED) throw 37;
          }
          settings.notes(true);
          const EXPCMDSTATE enabledState = op == 3 || (op == 0 && count > 1) ? ECS_HIDDEN : ECS_ENABLED;
          modern->GetState(items.Get(), FALSE, &state);
          if (state != enabledState) throw 13;
          ComPtr<IShellItemArray> read;
          if (FAILED(selection->GetSelection(IID_PPV_ARGS(&read)))) throw 30;
          DWORD actual = 0;
          read->GetCount(&actual);
          if (actual != static_cast<DWORD>(count)) throw 6;
          if (op == 0 && count > 1 && cmd->Execute() != E_INVALIDARG) throw 7;

          settings.notes(false);
          modern->GetState(items.Get(), FALSE, &state);
          if (state != noNotesState) throw 31;
          modern->GetState(nullptr, FALSE, &state);
          if (state != noNotesState) throw 32;
          // Neither stale legacy nor modern note commands may launch a process.
          if (op != 3 && cmd->Execute() != E_ACCESSDENIED) throw 33;
          if (op != 3 && modern->Invoke(items.Get(), nullptr) != E_ACCESSDENIED) throw 34;
          settings.notes(true);
          modern->GetState(items.Get(), FALSE, &state);
          if (state != enabledState) throw 35;

          if (argc > 1 && std::wstring(argv[1]) == L"--launch" && op == 2 && count == 1000 &&
              FAILED(cmd->Execute())) throw 8;
        }
      }
      if (unload() != S_OK) throw 9;
    }
    std::cout << "PASS four shell commands, title/icon/identity, 1/10/100/1000 selections, "
                 "opt-in note menus, stale command rejection, no settings context entry, DLL unload\n";
  } catch (int n) {
    std::cerr << "FAIL " << n << "\n";
    rc = 1;
  }
  FreeLibrary(dll);
  CoUninitialize();
  return rc;
}
