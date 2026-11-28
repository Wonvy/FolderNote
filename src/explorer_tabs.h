#pragma once
#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <memory>
struct IShellBrowser;
namespace tabs {
inline constexpr CLSID clsid = {0x8f95be22,0xd37a,0x4a22,{0x8c,0xf8,0x46,0xa6,0x03,0xc1,0x05,0x23}};
inline constexpr wchar_t clsidText[] = L"{8F95BE22-D37A-4A22-8CF8-46A603C10523}";
inline constexpr wchar_t userKey[] = L"Software\\Classes\\CLSID\\{8F95BE22-D37A-4A22-8CF8-46A603C10523}";
inline constexpr wchar_t toolbarKey[] = L"Software\\Microsoft\\Internet Explorer\\Toolbar";
inline constexpr wchar_t windowProperty[] = L"ShibaKit.ExplorerTabs.Window.v1";
// Forward wheel input from a focused file-view child. The band returns TRUE
// only when the supplied screen point is over one of its tabs.
inline constexpr UINT wheelInputMessage = WM_APP + 126;
HRESULT create(REFIID, void **);
long activeObjects();
// Owned by one Explorer BHO on its UI thread. Hosts the band directly in the
// current frame's native rebar; no process-wide input hooks or polling worker.
class Host {
  struct State;
  std::shared_ptr<State> state;
public:
  Host();
  ~Host();
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;
  bool update(HWND frame, IShellBrowser *, IWebBrowser2 *);
  void clear();
  void shutdown() { clear(); }
};
}
