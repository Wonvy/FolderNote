#include "../src/setup_payload.cpp"
#include "../src/resource.h"
#include <iostream>
#include <vector>

static void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static std::vector<char> bytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  check(stream.good(), "open extracted file");
  return {std::istreambuf_iterator<char>(stream), {}};
}

static std::vector<char> resource(HMODULE module, LPCWSTR type, LPCWSTR name) {
  const auto found = FindResourceW(module, name, type);
  const auto loaded = found ? LoadResource(module, found) : nullptr;
  const auto data = loaded ? static_cast<const char *>(LockResource(loaded)) : nullptr;
  check(data != nullptr, "icon resource exists");
  return {data, data + SizeofResource(module, found)};
}

static BOOL CALLBACK sameIcon(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR application) {
  return resource(module, type, name) == resource(reinterpret_cast<HMODULE>(application), type, name);
}

static void policyTests() {
  setup::Staging staging;
  setup::Extraction state{staging.path, false};
  FILE_IN_CABINET_INFO_W file{};
  file.FileSize = 100;
  auto accept = [&](const wchar_t *name) {
    file.NameInCabinet = name;
    return setup::cabinetCallback(&state, SPFILENOTIFY_FILEINCABINET,
                                  reinterpret_cast<UINT_PTR>(&file), 0);
  };
  check(accept(L"../FolderNote.exe") == FILEOP_ABORT, "reject parent traversal");
  check(accept(L"C:\\FolderNote.exe") == FILEOP_ABORT, "reject absolute path");
  check(accept(L"FolderNote.exe:stream") == FILEOP_ABORT, "reject alternate stream");
  check(accept(L"Install-Win11.exe") == FILEOP_ABORT, "reject unexpected edition");
  check(accept(L"FolderNote.exe") == FILEOP_DOIT, "accept application");
  check(std::filesystem::path(file.FullTargetName) == staging.path / L"FolderNote.exe", "fixed output path");
  check(accept(L"foldernote.EXE") == FILEOP_ABORT, "reject duplicate ignoring case");
  file.FileSize = 0;
  check(accept(L"Install.exe") == FILEOP_ABORT, "reject empty component");
  file.FileSize = 201 * 1024 * 1024;
  check(accept(L"Install.exe") == FILEOP_ABORT, "bound expanded size");
  check(setup::cabinetCallback(&state, SPFILENOTIFY_NEEDNEWCABINET, 0, 0) == ERROR_INVALID_DATA,
        "never load another cabinet");
  FILEPATHS_W extracted{};
  extracted.Win32Error = ERROR_DISK_FULL;
  check(setup::cabinetCallback(&state, SPFILENOTIFY_FILEEXTRACTED,
                               reinterpret_cast<UINT_PTR>(&extracted), 0) == ERROR_DISK_FULL,
        "propagate disk failure");
  bool rejected = false;
  try { setup::extract(GetModuleHandleW(nullptr), staging.path, false); }
  catch (...) { rejected = true; }
  check(rejected, "reject absent payload resource");
}

int wmain(int count, wchar_t **arguments) {
  try {
    policyTests();
    if (count == 4) {
      const std::filesystem::path source(arguments[2]);
      const bool modern = std::wstring_view(arguments[3]) == L"win11";
      setup::Staging staging;
      const auto copiedInstaller = staging.path / L"Setup.exe";
      std::filesystem::copy_file(arguments[1], copiedInstaller);
      const auto module = LoadLibraryExW(copiedInstaller.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
      check(module != nullptr, "load standalone setup resource");
      struct Module { HMODULE value; ~Module() { if (value) FreeLibrary(value); } } cleanup{module};
      const auto extracted = staging.path / L"unpacked";
      std::filesystem::create_directory(extracted);
      setup::extract(module, extracted, modern);
      for (auto name : setup::commonFiles) check(bytes(extracted / name) == bytes(source / name), "payload bytes match");
      if (modern)
        for (auto name : setup::modernFiles) check(bytes(extracted / name) == bytes(source / name), "MSIX payload bytes match");
      if (std::filesystem::exists(source / setup::bootstrapFile))
        check(bytes(extracted / setup::bootstrapFile) == bytes(source / setup::bootstrapFile), "WinUI bootstrap bytes match");
      const auto app = LoadLibraryExW((source / L"FolderNote.exe").c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
      check(app != nullptr, "load application icon");
      Module appCleanup{app};
      check(resource(module, RT_GROUP_ICON, MAKEINTRESOURCEW(IDI_FOLDERNOTE)) ==
            resource(app, RT_GROUP_ICON, MAKEINTRESOURCEW(IDI_FOLDERNOTE)), "setup uses application icon group");
      check(EnumResourceNamesW(module, RT_ICON, sameIcon, reinterpret_cast<LONG_PTR>(app)) != FALSE,
            "all icon sizes match the application");
    } else check(count == 1, "usage: SetupPackageTests [setup.exe source-directory standard|win11]");
    std::cout << "Standalone installer payload and icon checks passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
