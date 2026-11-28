#pragma once
#include "explorer_tabs.h"
#include "navigation.h"
#include <shlobj.h>
#include <cstring>
#include <vector>

namespace tabs::routing {
inline constexpr wchar_t helperClass[] = L"ShibaKit.QuickFolders.Helper.v1";
// Only the helper requests a transfer. A source returns 0 while busy, 1 when
// accepted, or 2 for deliberate retention. Completion is asynchronous.
inline constexpr UINT routeMessage = WM_APP + 127;
inline constexpr UINT importMessage = WM_APP + 128;
inline constexpr UINT completedMessage = WM_APP + 129;
inline constexpr UINT holdMessage = WM_APP + 130;
inline UINT releaseGateMessage() {
  static const UINT message = RegisterWindowMessageW(L"ShibaKit.ExplorerTabs.ReleaseGate.v1");
  return message;
}
inline constexpr UINT capabilityMessage = WM_APP + 132;
inline constexpr wchar_t gateProperty[] = L"ShibaKit.ExplorerTabs.Held.v1";
inline constexpr wchar_t gateOriginProperty[] = L"ShibaKit.ExplorerTabs.HeldForeground.v1";
inline constexpr ULONG_PTR importData = 0x464e5401;
inline constexpr ULONG_PTR bypassAdd = 0x464e5402;
inline constexpr ULONG_PTR bypassCheck = 0x464e5403;
inline constexpr ULONG_PTR bypassCancel = 0x464e5404;
inline constexpr size_t maxPidlBytes = 65536;
inline constexpr size_t maxPacketBytes = 1024 * 1024, maxSelection = 256;
struct PacketHeader { ULONG_PTR token; DWORD folderBytes; DWORD selectedCount; };
struct Packet { ULONG_PTR token = 0; std::vector<BYTE> folder; std::vector<std::vector<BYTE>> selected; };
inline bool enabled() { return navigation::tabsSupported() && navigation::tabsEnabled() && navigation::preferNewTabEnabled(); }
inline bool validPidl(const void *data, size_t bytes) {
  if (!data || bytes < sizeof(USHORT) || bytes > maxPidlBytes) return false;
  auto p = static_cast<const BYTE *>(data);
  for (size_t offset = 0; offset + sizeof(USHORT) <= bytes;) {
    USHORT count = 0; memcpy(&count, p + offset, sizeof(count));
    if (!count) return offset + sizeof(count) == bytes;
    if (count < sizeof(count) || count > bytes - offset - sizeof(USHORT)) return false;
    offset += count;
  }
  return false;
}
inline bool bandWindow(HWND band) {
  if (!IsWindow(band)) return false;
  auto frame = GetAncestor(band, GA_ROOT);
  wchar_t name[64]{}; GetClassNameW(band, name, ARRAYSIZE(name));
  return !wcscmp(name, L"ShibaKit.ExplorerTabs.Band.v1") &&
      GetPropW(frame, windowProperty) == band;
}
inline std::vector<BYTE> encode(const Packet &packet) {
  if (!packet.token || !validPidl(packet.folder.data(), packet.folder.size()) || packet.selected.size() > maxSelection) return {};
  PacketHeader header{packet.token, DWORD(packet.folder.size()), DWORD(packet.selected.size())};
  std::vector<BYTE> bytes(sizeof(header)); memcpy(bytes.data(), &header, sizeof(header));
  bytes.insert(bytes.end(), packet.folder.begin(), packet.folder.end());
  for (const auto &item : packet.selected) {
    if (!validPidl(item.data(), item.size()) || bytes.size() + sizeof(DWORD) + item.size() > maxPacketBytes) return {};
    DWORD count = DWORD(item.size()); auto start = bytes.size(); bytes.resize(start + sizeof(count));
    memcpy(bytes.data() + start, &count, sizeof(count)); bytes.insert(bytes.end(), item.begin(), item.end());
  }
  return bytes;
}
inline bool decode(const COPYDATASTRUCT *data, Packet &packet) {
  if (!data || data->dwData != importData || !data->lpData || data->cbData < sizeof(PacketHeader) || data->cbData > maxPacketBytes) return false;
  PacketHeader header{}; memcpy(&header, data->lpData, sizeof(header));
  auto bytes = static_cast<const BYTE *>(data->lpData); size_t at = sizeof(header), total = data->cbData;
  if (!header.token || header.selectedCount > maxSelection || header.folderBytes > total - at ||
      !validPidl(bytes + at, header.folderBytes)) return false;
  Packet value; value.token = header.token; value.folder.assign(bytes + at, bytes + at + header.folderBytes); at += header.folderBytes;
  for (DWORD index = 0; index < header.selectedCount; ++index) {
    if (total - at < sizeof(DWORD)) return false;
    DWORD count = 0; memcpy(&count, bytes + at, sizeof(count)); at += sizeof(count);
    if (count > total - at || !validPidl(bytes + at, count)) return false;
    value.selected.emplace_back(bytes + at, bytes + at + count); at += count;
  }
  if (at != total) return false;
  packet = std::move(value); return true;
}
inline HWND helperWindow() { return FindWindowExW(HWND_MESSAGE, nullptr, helperClass, nullptr); }
inline ULONG_PTR bypass(HWND sender, ULONG_PTR kind, PCIDLIST_ABSOLUTE id = nullptr, ULONG_PTR token = 0) {
  auto helper = helperWindow(); if (!helper) return 0;
  COPYDATASTRUCT data{kind, id ? ILGetSize(id) : DWORD(sizeof(token)), id ? static_cast<void *>(const_cast<ITEMIDLIST *>(id)) : &token};
  DWORD_PTR reply = 0;
  return SendMessageTimeoutW(helper, WM_COPYDATA, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&data),
      SMTO_ABORTIFHUNG, 500, &reply) ? reply : 0;
}
// Helper-owned watcher. Existing windows are snapshotted before event intake;
// enabling the option never merges windows that were already open.
bool start(HWND helper);
void stop();
bool message(HWND helper, UINT message, WPARAM w, LPARAM l, LRESULT &result);
}
