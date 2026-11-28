#include "../src/window_position.h"
#include <iostream>
static int failures=0,monitors=0;
static void check(bool valid){if(!valid)++failures;}
static BOOL CALLBACK testMonitor(HMONITOR monitor,HDC,LPRECT,LPARAM){
 MONITORINFO info{sizeof(info)};if(!GetMonitorInfoW(monitor,&info)){++failures;return TRUE;}
 auto work=info.rcWork;
 auto window=CreateWindowExW(0,L"ShibaKit.Position.Test",L"",WS_OVERLAPPEDWINDOW,work.left,work.top,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 if(!window){++failures;return TRUE;}
 ui::centerLogical(window,work,560,305);RECT actual{};GetWindowRect(window,&actual);
 auto dpi=GetDpiForWindow(window);auto expected=ui::centeredRect(work,MulDiv(560,dpi,96),MulDiv(305,dpi,96));
 check(EqualRect(&actual,&expected));check(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)==monitor);
 DestroyWindow(window);++monitors;return TRUE;
}
int main(){
 auto r=ui::centeredRect({0,0,1920,1040},560,300);check(r.left==680&&r.top==370);
 r=ui::centeredRect({-1920,-200,0,840},560,300);check(r.left==-1240&&r.top==170);
 r=ui::centeredRect({60,0,1920,1080},560,300);check(r.left==710&&r.top==390);
 r=ui::centeredRect({0,0,320,200},800,600);check(r.left==0&&r.top==0&&r.right==320&&r.bottom==200);
 WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ShibaKit.Position.Test";RegisterClassW(&wc);
 EnumDisplayMonitors(nullptr,nullptr,testMonitor,0);
 std::cout<<(failures?"FAIL":"PASS")<<" window geometry and "<<monitors<<" real monitor(s), DPI-aware hidden-window placement\n";
 return failures?1:0;
}
