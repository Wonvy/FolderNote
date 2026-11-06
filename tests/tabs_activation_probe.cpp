// Manual local probe. Read-only by default; --activate requests only FolderNote's bar.
#include "../src/explorer_tabs.h"
#include "../src/navigation.h"
#include <wrl/client.h>
#include <iostream>
using Microsoft::WRL::ComPtr;
int wmain(int count, wchar_t **args) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (count == 2 && !wcscmp(args[1], L"--activate")) {
    const auto status = navigation::syncTabs();
    std::wcout << L"SyncTabs=0x" << std::hex << status.result << std::dec
               << L" windows=" << status.windows << L" acknowledged=" << status.requested
               << L" visible=" << status.visible << L"\n";
  }
  {
    ComPtr<IShellWindows> windows;
    auto result = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    std::wcout << L"ShellWindows 0x" << std::hex << result << std::dec << L"\n";
    long total = 0; if (windows) windows->get_Count(&total);
    for (long i = 0; i < total; ++i) {
      VARIANT slot{}; slot.vt = VT_I4; slot.lVal = i;
      ComPtr<IDispatch> dispatch; ComPtr<IWebBrowser2> browser;
      if (FAILED(windows->Item(slot, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser))) continue;
      SHANDLE_PTR handle = 0; browser->get_HWND(&handle);
      auto frame = GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT);
      wchar_t cls[128]{}; GetClassNameW(frame, cls, ARRAYSIZE(cls));
      std::wcout << L"Window " << handle << L" class=" << cls;
      auto band = static_cast<HWND>(GetPropW(frame, tabs::windowProperty));
      RECT bounds{}; GetWindowRect(band, &bounds);
      std::wcout << L" band=" << band << L" visible=" << IsWindowVisible(band) << L" size=" << bounds.right-bounds.left << L"x" << bounds.bottom-bounds.top << L"\n";
      if (count == 2 && !wcscmp(args[1], L"--layout")) {
        EnumChildWindows(frame, [](HWND child, LPARAM) -> BOOL {
          wchar_t name[128]{}; GetClassNameW(child, name, ARRAYSIZE(name));
          DWORD pid = 0; auto thread = GetWindowThreadProcessId(child, &pid);
          RECT rect{}; GetWindowRect(child, &rect);
          std::wcout << L"  child=" << child << L" parent=" << GetParent(child) << L" class=" << name
                     << L" tid=" << thread << L" visible=" << IsWindowVisible(child)
                     << L" rect=" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom << L"\n";
          return TRUE;
        }, 0);
      }
    }
  }
  CoUninitialize();
}
