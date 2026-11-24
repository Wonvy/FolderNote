// Real system picker and UIA integration, confined to generated test files.
#include "../src/quick_find_external.cpp"
#include "quick_find_test_settings.h"
#include "shell_paths.h"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using Microsoft::WRL::ComPtr;
constexpr auto title = L"FolderNote quick-find isolated picker test";
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
ComPtr<IUIAutomationElement> byType(IUIAutomation *ui, IUIAutomationElement *root, CONTROLTYPEID type) {
  VARIANT value{}; value.vt = VT_I4; value.lVal = type;
  ComPtr<IUIAutomationCondition> condition;
  ComPtr<IUIAutomationElement> found;
  ui->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
  root->FindFirst(TreeScope_Descendants, condition.Get(), &found);
  return found;
}
std::wstring selectedName(IUIAutomationElement *list) {
  ComPtr<IUIAutomationSelectionPattern> selection;
  ComPtr<IUIAutomationElementArray> selected;
  if (FAILED(list->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&selection))) ||
      !selection || FAILED(selection->GetCurrentSelection(&selected)) || !selected) return {};
  int count = 0; selected->get_Length(&count);
  if (count != 1) return {};
  ComPtr<IUIAutomationElement> item; selected->GetElement(0, &item);
  BSTR name = nullptr; item->get_CurrentName(&name);
  std::wstring result = name ? name : L""; SysFreeString(name); return result;
}
void shellTests(const std::filesystem::path &directory) {
  using namespace quick_find::external;
  require(std::filesystem::create_directory(directory), "create Shell fixture directory");
  const auto folder = directory / L"素材", file = directory / L"设计.txt", shortcut = directory / L"文件夹链接.lnk";
  std::filesystem::create_directory(folder);
  std::ofstream(file) << "isolated fixture";
  ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
  require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))), "create test shortcut");
  require(SUCCEEDED(link->SetPath(folder.c_str())) && SUCCEEDED(link.As(&persist)) &&
      SUCCEEDED(persist->Save(shortcut.c_str(), TRUE)), "save test shortcut");
  ComPtr<IShellItem> item; ComPtr<IShellFolder> parent;
  require(SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
      SUCCEEDED(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&parent))), "bind test Shell folder");
  const auto entries = shellItems(parent.Get());
  require(entries.size() == 3, "enumerate Shell display names including hidden extensions");
  for (const auto &path : {folder, file, shortcut}) {
    SHFILEINFOW info{};
    require(SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) != 0, "read actual displayed name");
    const auto found = entries.find(info.szDisplayName);
    require(found != entries.end() && found->second.verified && sameShellTestPath(found->second.path, path.native()),
        "display name resolves to its absolute parsing path");
    require(found->second.folder == (path == folder), "folders open while ordinary files and shortcuts follow the file setting");
    require(quick::icons::read(quick::icons::Source::file(found->second.path), 16) != nullptr,
        "resolved Shell entry loads its native icon");
  }
  ShellItems duplicate;
  addShellEntry(duplicate, L"同名", {folder.native(), true, true});
  addShellEntry(duplicate, L"同名", {file.native(), false, true});
  addShellEntry(duplicate, L"同名", {shortcut.native(), false, true});
  require(!duplicate.at(L"同名").verified && duplicate.at(L"同名").path.empty(),
      "duplicate user/public desktop names cannot infer an open action or wrong icon");
  const auto desktop = desktopItems();
  size_t checked = 0;
  for (const auto &[label, entry] : desktop) {
    if (!entry.verified) continue;
    ComPtr<IShellItem> resolved;
    require(SUCCEEDED(SHCreateItemFromParsingName(entry.path.c_str(), nullptr, IID_PPV_ARGS(&resolved))),
        "native desktop entries resolve including public and virtual items");
    ++checked;
  }
  std::cout << "Shell names, absolute paths, folder/file policy, native icons and ambiguity passed; "
            << checked << " native desktop entries verified read-only.\n";
}
}
int main(int count, char **arguments) {
  using namespace quick_find::external;
  const bool save = count > 1 && std::string_view(arguments[1]) == "--save";
  const bool folders = count > 1 && std::string_view(arguments[1]) == "--folders";
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const auto directory = std::filesystem::temp_directory_path() /
      (L"FolderNote-FindTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  int result = 1;
  try {
    if (count > 1 && std::string_view(arguments[1]) == "--shell-only") {
      shellTests(directory); result = 0;
    } else {
    QuickFindTestSettings isolated;
    auto preferences = quick_find::Settings{};
    preferences.openFiles = true; // Even this mode must not submit open/save dialogs.
    require(quick_find::saveSettings(preferences) == ERROR_SUCCESS, "save isolated preferences");
    require(std::filesystem::create_directory(directory), "create isolated directory");
    const auto makeItem = [&](const std::wstring &name) {
      if (folders) std::filesystem::create_directory(directory / name);
      else std::ofstream(directory / name) << "fixture";
    };
    for (int i = 0; i < 180; ++i) makeItem(L"entry-" + std::to_wstring(i) + L".txt");
    for (auto name : {L"设计素材.txt", L"重庆项目.txt", L"视频素材.txt"}) makeItem(name);
    for (auto name : {L"ScriptData", L"scriptprism.exe", L"step", L"素材"})
      std::filesystem::create_directory(directory / name);
    ComPtr<IFileDialog> dialog;
    ComPtr<IShellItem> folder;
    require(SUCCEEDED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))), "create file picker");
    require(SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder))), "create folder item");
    dialog->SetFolder(folder.Get()); dialog->SetTitle(title);
    require(SUCCEEDED(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
        (save ? 0 : FOS_FILEMUSTEXIST) | (folders ? FOS_PICKFOLDERS : 0))), "configure picker");
    std::thread driver([&] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      HWND window = nullptr;
      try {
        for (int i = 0; i < 100 && !window; ++i) { Sleep(50); window = FindWindowW(L"#32770", title); }
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        require(window && pid == GetCurrentProcessId(), "find owned picker");
        SetForegroundWindow(window);
        ComPtr<IUIAutomation> ui; ComPtr<IUIAutomationElement> root;
        require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui))), "create automation");
        require(SUCCEEDED(ui->ElementFromHandle(window, &root)), "picker automation root");
        ComPtr<IUIAutomationElement> list;
        for (int i = 0; i < 40 && !list; ++i) { Sleep(50); list = byType(ui.Get(), root.Get(), UIA_ListControlTypeId); }
        require(list != nullptr, "file list exists");
        require(start(), "start locator worker"); target(window);
        // The dialog can assign its initial filename focus after its list
        // first appears. Wait for actual native AND UIA focus to settle.
        Input input{};
        bool ready = false;
        for (int i = 0; i < 40 && !ready; ++i) {
          SetForegroundWindow(window);
          list->SetFocus(); Sleep(80);
          GUITHREADINFO info{sizeof(info)};
          GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info);
          input = {window, info.hwndFocus, pid, L'S', GetTickCount64(), epoch.load()};
          ready = GetForegroundWindow() == window && fileSurface(info.hwndFocus) && focusedList(ui.Get(), input) != nullptr;
        }
        require(ready, "recognize settled file list focus");
        // The picker can focus its list before Shell finishes populating it.
        // Wait for fixture readiness instead of treating early partial data as
        // a failed search; focus/target validation still applies on every read.
        auto contents = readItems(ui.Get(), list.Get(), input);
        for (int retry = 0; retry < 30 && contents.names.size() < 187 && current(input); ++retry) {
          Sleep(100); input.time = GetTickCount64();
          contents = readItems(ui.Get(), list.Get(), input);
        }
        if (contents.names.size() < 187) {
          GUITHREADINFO info{sizeof(info)}; GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info);
          std::cerr << "fixture count=" << contents.names.size() << " foreground=" << (GetForegroundWindow() == window)
                    << " sameFocus=" << (info.hwndFocus == input.focus) << " current=" << current(input) << '\n';
        }
        require(contents.names.size() >= 187, "enumerate virtualized offscreen items");
        const auto type = [](const char *text) {
          for (; *text; ++text) {
            KBDLLHOOKSTRUCT key{}; key.vkCode = static_cast<DWORD>(*text);
            keyboard(WM_KEYDOWN, key); keyboard(WM_KEYUP, key);
          }
        };
        const auto press = [](DWORD code) {
          KBDLLHOOKSTRUCT key{}; key.vkCode = code;
          keyboard(WM_KEYDOWN, key); keyboard(WM_KEYUP, key);
        };
        const auto candidates = [&](const wchar_t *query) {
          for (int i = 0; i < 150; ++i) {
            const auto popup = candidateWindow.load();
            wchar_t caption[256]{};
            if (popup) GetWindowTextW(GetDlgItem(popup, 1), caption, ARRAYSIZE(caption));
            if (popup && IsWindowVisible(popup) && std::wstring_view(caption).starts_with(query)) return GetDlgItem(popup, 2);
            Sleep(25);
          }
          GUITHREADINFO info{sizeof(info)}; GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info);
          std::string requested; for (auto c = query; *c; ++c) requested += static_cast<char>(*c);
          std::cerr << "query=" << requested << " foreground=" << (GetForegroundWindow() == window)
                    << " sameFocus=" << (info.hwndFocus == input.focus) << " popup=" << candidateWindow.load()
                    << " pending=" << candidatePending.load() << " stopping=" << stopping.load() << '\n';
          throw std::runtime_error("candidate list did not display query");
        };
        const auto before = selectedName(list.Get());
        type("SJSC");
        candidates(L"SJSC");
        require(selectedName(list.Get()) == before, "typing only updates candidates");
        if (folders) { press(VK_ESCAPE); Sleep(80); }
        else {
          press(VK_RETURN);
          for (int i = 0; i < 100 && selectedName(list.Get()).find(L"设计素材") == std::wstring::npos; ++i) Sleep(50);
          require(selectedName(list.Get()).find(L"设计素材") != std::wstring::npos, "first initials select offscreen file");
        }
        cancel(); type("CQXM");
        candidates(L"CQXM");
        if (folders) { press(VK_ESCAPE); Sleep(80); }
        else {
          press(VK_RETURN);
          for (int i = 0; i < 100 && selectedName(list.Get()).find(L"重庆项目") == std::wstring::npos; ++i) Sleep(50);
          if (selectedName(list.Get()).find(L"重庆项目") == std::wstring::npos)
            std::wcerr << L"selection=" << selectedName(list.Get()) << L" foreground=" << (GetForegroundWindow() == window) << std::endl;
          require(selectedName(list.Get()).find(L"重庆项目") != std::wstring::npos, "polyphonic initials select another file");
        }
        const auto beforeEmptyQuery = selectedName(list.Get());
        cancel(); type("ZZZZ"); auto emptyChoices = candidates(L"ZZZZ");
        require(SendMessageW(emptyChoices, LB_GETCOUNT, 0, 0) == 0, "no matches produces empty candidates");
        press(VK_RETURN); Sleep(80);
        require(selectedName(list.Get()) == beforeEmptyQuery, "confirming empty candidates leaves original selection");
        press(VK_ESCAPE); Sleep(80);
        require(!candidateWindow.load(), "Escape dismisses candidates");
        preferences.openFolders = false;
        require(quick_find::saveSettings(preferences) == ERROR_SUCCESS, "switch folders to locate only");
        cancel(); type("SC"); candidates(L"SC"); press(VK_RETURN);
        for (int i = 0; i < 60 && selectedName(list.Get()) != L"素材"; ++i) Sleep(25);
        require(selectedName(list.Get()) == L"素材", "locate-only folder selects the actual item");
        require(sameShellTestPath(quick::dialog::currentPath(window), directory.wstring()), "locate-only folder keeps current directory");
        preferences.openFolders = true;
        require(quick_find::saveSettings(preferences) == ERROR_SUCCESS, "switch folders back to open");
        cancel(); type("S");
        auto choices = candidates(L"S");
        wchar_t first[128]{}; SendMessageW(choices, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(first));
        require(std::wstring_view(first) == L"ScriptData", "first candidate can be English prefix");
        type("C");
        choices = candidates(L"SC");
        SendMessageW(choices, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(first));
        require(std::wstring_view(first) == L"素材", "complete initials rank first");
        require(SendMessageW(choices, LB_GETCOUNT, 0, 0) >= 3, "both Chinese and English candidates remain available");
        RECT item{}; SendMessageW(choices, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item));
        const auto point = MAKELPARAM(item.left + 10, (item.top + item.bottom) / 2);
        SendMessageW(choices, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(choices, WM_LBUTTONUP, 0, point);
        const auto destination = (directory / L"素材").wstring();
        std::wstring actualPath;
        const auto deadline = GetTickCount64() + 3000;
        do { actualPath = quick::dialog::currentPath(window); if (sameShellTestPath(actualPath, destination)) break; Sleep(25); }
        while (GetTickCount64() < deadline);
        if (!sameShellTestPath(actualPath, destination)) {
          for (size_t i = 0; i < contents.labels.size(); ++i) if (contents.labels[i] == L"素材") {
            BSTR itemType = nullptr; contents.items[i].element->get_CurrentItemType(&itemType);
            std::wcerr << L"folder type=" << (itemType ? itemType : L"<null>") << L" path=" << actualPath << std::endl;
            SysFreeString(itemType);
          }
        }
        require(sameShellTestPath(actualPath, destination), "folder candidate navigates inside the existing picker");
        list = byType(ui.Get(), root.Get(), UIA_ListControlTypeId);
        require(list != nullptr, "destination file list remains available");
        require(SUCCEEDED(list->SetFocus()), "focus destination file list after navigation");
        Sleep(100);
        if (!folders) {
        HWND filenameEdit = nullptr;
        EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
          wchar_t cls[80]{}; GetClassNameW(child, cls, ARRAYSIZE(cls));
          if (wcscmp(cls, L"Edit")) return TRUE;
          if (GetDlgCtrlID(child) == 1001) { *reinterpret_cast<HWND *>(data) = child; return FALSE; }
          for (auto parent = GetParent(child); parent; parent = GetParent(parent)) {
            GetClassNameW(parent, cls, ARRAYSIZE(cls));
            if (!wcscmp(cls, L"FileNameControlHost") || GetDlgCtrlID(parent) == 1148) { *reinterpret_cast<HWND *>(data) = child; return FALSE; }
          }
          return TRUE;
        }, reinterpret_cast<LPARAM>(&filenameEdit));
        ComPtr<IUIAutomationElement> edit;
        if (filenameEdit) ui->ElementFromHandle(filenameEdit, &edit);
        require(edit != nullptr && SUCCEEDED(edit->SetFocus()), "focus filename input");
        ComPtr<IUIAutomationValuePattern> value;
        require(SUCCEEDED(edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))), "filename value pattern");
        auto filename = SysAllocString(L"保留输入.txt");
        const auto setFilename = value->SetValue(filename); SysFreeString(filename);
        require(SUCCEEDED(setFilename), "set fixture filename");
        BSTR initial = nullptr; value->get_CurrentValue(&initial);
        const std::wstring initialValue = initial ? initial : L""; SysFreeString(initial);
        type("SJSC"); Sleep(250);
        BSTR text = nullptr; const auto readValue = value->get_CurrentValue(&text);
        const bool unchanged = text && initialValue == text;
        if (!unchanged) std::cerr << "filename read hr=" << std::hex << readValue << std::dec << " initial length=" << initialValue.size() << " final length=" << (text ? SysStringLen(text) : 0) << '\n';
        SysFreeString(text);
        require(unchanged, "filename typing is not intercepted");
        }
        require(IsWindowVisible(window), "search never confirms or closes the picker");
        result = 0;
        std::cout << "Picker: 187 virtualized items, Chinese/English priority, initials, polyphony, filename isolation and no activation passed.\n";
      } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
      stop();
      if (window) PostMessageW(window, WM_CLOSE, 0, 0);
      CoUninitialize();
    });
    dialog->Show(nullptr);
    driver.join();
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  // Only the uniquely created fixture directory is ever removed.
  if (directory.parent_path() == std::filesystem::temp_directory_path() && directory.filename().native().starts_with(L"FolderNote-FindTest-")) {
    std::error_code error; std::filesystem::remove_all(directory, error);
  }
  CoUninitialize();
  return result;
}
