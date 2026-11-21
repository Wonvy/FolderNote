#pragma once
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <memory>
#include <string>

namespace quick::icons {
struct Icon {
  HICON handle;
  explicit Icon(HICON value) : handle(value) {}
  ~Icon() { if (handle) DestroyIcon(handle); }
  Icon(const Icon &) = delete;
};
using Image = std::shared_ptr<Icon>;
struct Source {
  enum class Kind { File, Stock, Known, Child } kind = Kind::File;
  std::wstring path;
  std::wstring displayName;
  SHSTOCKICONID stock = SIID_FOLDER;
  GUID known{};
  static Source file(const std::wstring &value) { Source source; source.path = value; return source; }
  static Source system(SHSTOCKICONID value) { Source source; source.kind = Kind::Stock; source.stock = value; return source; }
  static Source folder(REFGUID value) { Source source; source.kind = Kind::Known; source.known = value; return source; }
  // Resolve a Shell display name, including names with hidden extensions.
  static Source child(const std::wstring &parent, const std::wstring &name) {
    Source source; source.kind = Kind::Child; source.path = parent; source.displayName = name; return source;
  }
};
Image read(const Source &source, int size);
class Cache {
  struct State;
  std::shared_ptr<State> state;
public:
  using Loader = std::function<Image(const Source &, int)>;
  Cache(HWND notify, UINT message, int size, Loader loader = read);
  ~Cache();
  Cache(const Cache &) = delete;
  Image get(const Source &source); // Never reads a folder or waits for extraction.
};
}
