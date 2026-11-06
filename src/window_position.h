#pragma once
#include <windows.h>
#include <algorithm>
#include <cwchar>

namespace ui {
inline RECT mouseWorkArea() {
  POINT mouse{};
  if(!GetCursorPos(&mouse))mouse={0,0};
  MONITORINFO info{sizeof(info)};
  if(GetMonitorInfoW(MonitorFromPoint(mouse,MONITOR_DEFAULTTONEAREST),&info))return info.rcWork;
  RECT fallback{};
  if(!SystemParametersInfoW(SPI_GETWORKAREA,0,&fallback,0))fallback={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
  return fallback;
}
inline RECT centeredRect(RECT work,int width,int height) {
  width=std::clamp<int>(width,1,std::max(1L,work.right-work.left));
  height=std::clamp<int>(height,1,std::max(1L,work.bottom-work.top));
  int left=work.left+(work.right-work.left-width)/2;
  int top=work.top+(work.bottom-work.top-height)/2;
  return {left,top,left+width,top+height};
}
inline void place(HWND window,RECT work,int width,int height) {
  auto r=centeredRect(work,width,height);
  SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
}
// Create the hidden window on the target monitor first, so WM_CREATE uses its DPI.
inline void centerLogical(HWND window,RECT work,int width,int height) {
  UINT dpi=GetDpiForWindow(window);if(!dpi)dpi=96;
  place(window,work,MulDiv(width,dpi,96),MulDiv(height,dpi,96));
}
struct DialogPlacement { RECT work; bool placed=false; DialogPlacement* previous=nullptr; };
inline thread_local DialogPlacement* dialog=nullptr;
inline LRESULT CALLBACK centerDialogHook(int code,WPARAM w,LPARAM l) {
  if(code==HCBT_ACTIVATE&&dialog&&!dialog->placed) {
    HWND window=reinterpret_cast<HWND>(w);wchar_t cls[32]{};GetClassNameW(window,cls,32);
    if(!wcscmp(cls,L"#32770")) {
      dialog->placed=true;RECT r{};GetWindowRect(window,&r);
      place(window,dialog->work,r.right-r.left,r.bottom-r.top);
      // Moving a per-monitor dialog can change its DPI and dimensions.
      GetWindowRect(window,&r);place(window,dialog->work,r.right-r.left,r.bottom-r.top);
    }
  }
  return CallNextHookEx(nullptr,code,w,l);
}
inline int message(HWND owner,LPCWSTR text,LPCWSTR title,UINT flags) {
  DialogPlacement current{mouseWorkArea(),false,dialog};dialog=&current;
  HHOOK hook=SetWindowsHookExW(WH_CBT,centerDialogHook,nullptr,GetCurrentThreadId());
  int result=MessageBoxW(owner,text,title,flags);
  if(hook)UnhookWindowsHookEx(hook);dialog=current.previous;
  return result;
}
}
