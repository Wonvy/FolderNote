#pragma once
#include <windows.h>
#include <string>

namespace quick::detail { class MenuRequest; }

namespace quick::legacy {
inline constexpr wchar_t helperClass[] = L"ShibaKit.QuickFolders.Helper.v1";
inline constexpr wchar_t elevatedHelperClass[] = L"ShibaKit.QuickFolders.ElevatedHelper.v1";
inline constexpr ULONG_PTR copyDataTag = 0x464e4c31; // FNL1, fixed across bitness.

bool isDialog(HWND);
// A true result means the helper accepted a selection request, not that an
// application's root/filter accepted the destination. Never confirms a dialog.
bool navigate(HWND, const std::wstring &, const detail::MenuRequest &);

// Run all broker functions on the helper's message thread. The callback is
// dispatched on that same thread, so remote buffers outlive message delivery.
bool receive(const COPYDATASTRUCT *, bool (*authorized)(HWND, DWORD));
bool reap() noexcept;
void shutdown() noexcept;
}
