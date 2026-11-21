#include "setup_payload.h"
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
#ifdef FOLDERNOTE_MODERN_SETUP
  constexpr bool modern = true;
#else
  constexpr bool modern = false;
#endif
  bool quiet = false;
  try {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return ERROR_NOT_ENOUGH_MEMORY;
    std::wstring forwarded;
    bool valid = true;
    if constexpr (modern) {
      if (count == 3 && std::wstring_view(arguments[1]) == L"--parent") {
        const std::wstring_view id(arguments[2]);
        valid = !id.empty() && id.size() <= 10 && id.find_first_not_of(L"0123456789") == id.npos;
        if (valid) forwarded = L"--parent " + std::wstring(id);
      } else if (count != 1) valid = false;
    } else {
      if (count == 2 && (std::wstring_view(arguments[1]) == L"--automatic-update" ||
                        std::wstring_view(arguments[1]) == L"--quiet")) {
        quiet = true;
        forwarded = arguments[1];
      } else if (count != 1) valid = false;
    }
    LocalFree(arguments);
    if (!valid) return ERROR_INVALID_PARAMETER;

    // Prevent DLL search through the download directory. The payload is extracted
    // exclusively into a new directory before the existing installer is launched.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    setup::Staging staging;
    setup::extract(instance, staging.path, modern);
    const auto installer = staging.path / (modern ? L"Install-Win11.exe" : L"Install.exe");
    auto command = L"\"" + installer.native() + L"\" " + forwarded;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(installer.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        staging.path.c_str(), &startup, &process)) throw GetLastError();
    CloseHandle(process.hThread);
    AllowSetForegroundWindow(process.dwProcessId);
    const auto completed = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    if (completed == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
  } catch (...) {
    if (!quiet) {
      const bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
      MessageBoxW(nullptr, chinese ? L"无法解包或启动安装。请重新下载安装包，并检查临时文件夹是否可写。"
                                 : L"Could not unpack or start setup. Download the installer again and check that your temporary folder is writable.",
                  L"FolderNote", MB_OK | MB_ICONERROR);
    }
    return 1;
  }
}
