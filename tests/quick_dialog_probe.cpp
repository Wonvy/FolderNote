// Manual integration fixture: shows a standard Save dialog, but never writes
// the selected file. Use middle-click to verify navigation preserves its name.
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  {
    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_DONTADDTORECENT);
      dialog->SetTitle(L"FolderNote 快速定位测试（不会写入文件）");
      dialog->SetFileName(L"保留这个文件名.txt");
      dialog->Show(nullptr);
    }
  }
  CoUninitialize(); return 0;
}
