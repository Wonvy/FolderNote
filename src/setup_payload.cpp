#include "setup_payload.h"
#include <setupapi.h>
#include <objbase.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

namespace setup {
namespace {
struct Extraction {
  std::filesystem::path directory;
  bool modern;
  std::array<bool, 8> seen{};
  unsigned long long total = 0;
};

UINT CALLBACK cabinetCallback(PVOID context, UINT notification, UINT_PTR first, UINT_PTR) noexcept {
  try {
    auto &state = *static_cast<Extraction *>(context);
    if (notification == SPFILENOTIFY_NEEDNEWCABINET) return ERROR_INVALID_DATA;
    if (notification == SPFILENOTIFY_FILEEXTRACTED)
      return reinterpret_cast<FILEPATHS_W *>(first)->Win32Error;
    if (notification != SPFILENOTIFY_FILEINCABINET) return NO_ERROR;
    auto &file = *reinterpret_cast<FILE_IN_CABINET_INFO_W *>(first);
    size_t index = 0;
    const wchar_t *name = nullptr;
    for (auto candidate : commonFiles) {
      if (_wcsicmp(file.NameInCabinet, candidate) == 0) { name = candidate; break; }
      ++index;
    }
    if (!name) {
      for (auto candidate : modernFiles) {
        if (state.modern && _wcsicmp(file.NameInCabinet, candidate) == 0) { name = candidate; break; }
        ++index;
      }
    }
    if (!name && _wcsicmp(file.NameInCabinet, bootstrapFile) == 0) name = bootstrapFile;
    if (!name || index >= state.seen.size() || state.seen[index] || !file.FileSize ||
        (file.DosAttribs & FILE_ATTRIBUTE_DIRECTORY) || state.total + file.FileSize > 200ull * 1024 * 1024) {
      SetLastError(ERROR_INVALID_DATA);
      return FILEOP_ABORT;
    }
    const auto target = (state.directory / name).native();
    if (target.size() >= MAX_PATH || GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
      SetLastError(ERROR_INVALID_NAME);
      return FILEOP_ABORT;
    }
    wcscpy_s(file.FullTargetName, target.c_str());
    state.seen[index] = true;
    state.total += file.FileSize;
    return FILEOP_DOIT;
  } catch (...) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FILEOP_ABORT;
  }
}
}

Staging::Staging() {
  GUID guid{};
  wchar_t token[40]{};
  if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, ARRAYSIZE(token)))
    throw std::runtime_error("create staging identifier");
  path = std::filesystem::temp_directory_path() / (std::wstring(L"FolderNote-Setup-") + token);
  if (!CreateDirectoryW(path.c_str(), nullptr)) {
    const auto error = GetLastError();
    path.clear();
    throw std::system_error(static_cast<int>(error), std::system_category(), "create staging directory");
  }
}

Staging::~Staging() {
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

void extract(HMODULE module, const std::filesystem::path &directory, bool modern) {
  const auto resource = FindResourceW(module, MAKEINTRESOURCEW(payloadResource), RT_RCDATA);
  const auto size = resource ? SizeofResource(module, resource) : 0;
  const auto loaded = resource ? LoadResource(module, resource) : nullptr;
  const auto data = loaded ? LockResource(loaded) : nullptr;
  if (!data || !size || size > 100u * 1024 * 1024) throw std::runtime_error("invalid embedded payload");
  const auto cabinet = directory / L"payload.cab";
  {
    std::ofstream stream(cabinet, std::ios::binary | std::ios::trunc);
    stream.write(static_cast<const char *>(data), size);
    stream.close();
    if (!stream) throw std::runtime_error("write embedded payload");
  }
  Extraction state{directory, modern};
  if (!SetupIterateCabinetW(cabinet.c_str(), 0, cabinetCallback, &state))
    throw std::runtime_error("extract embedded payload");
  const size_t required = modern ? 7 : 5;
  for (size_t i = 0; i < required; ++i)
    if (!state.seen[i]) throw std::runtime_error("missing installer component");
  std::filesystem::remove(cabinet);
}
}
