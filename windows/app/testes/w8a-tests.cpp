#include "../src/window-state.h"
#include "../src/tray-menu.h"
#include <iostream>
#include <stdexcept>
void Check(bool ok,char const* why) {if(!ok)throw std::runtime_error(why);std::cout<<"PASS: "<<why<<'\n';}
int main() {
  try {
    using namespace app;
    WindowState s{L"\\\\.\\DISPLAY2",120,80,960,680,true};
    auto path=std::filesystem::current_path()/L"test-output/window-state-fixture.txt";
    Check(WriteWindowState(path,s),"window state writes atomically");WindowState read;
    Check(ReadWindowState(path,read) && read.monitor==s.monitor && read.width==960 && read.x==120 && read.maximized,"cold start retains normal bounds, monitor and maximized");
    RECT work{1920,40,4480,1440};
    for(UINT dpi:{96u,120u,144u,192u}) {
      auto r=FitWindowState(read,work,dpi,true);
      Check(r.left==1920+MulDiv(120,dpi,96) && r.top==std::min(40L+MulDiv(80,dpi,96),work.bottom-MulDiv(680,dpi,96)) && r.right-r.left==MulDiv(960,dpi,96),"restore adapts logical bounds to destination DPI");
    }
    s.x=-500;s.y=10000;s.width=32000;s.height=32000;auto r=FitWindowState(s,work,192,true);
    Check(EqualRect(&r,&work),"oversized or offscreen bounds fit the current work area");
    s={L"removed",90000,90000,640,480,false};r=FitWindowState(s,work,96,false);
    Check(r.left==2880 && r.top==500,"removed monitor recenters without reusing stale coordinates");
    std::ofstream(path)<<"1 \"monitor\" 0 0 -1 600 1";
    Check(!ReadWindowState(path,read),"corrupt sizes do not reach Win32");
    std::ofstream(path)<<"1 \"monitor\" 0 0 800 600 1 trailing";
    Check(!ReadWindowState(path,read),"trailing corrupt state is refused");
    std::ofstream(path)<<"9 \"monitor\" 0 0 800 600 1";
    Check(!ReadWindowState(path,read),"unknown state format keeps safe defaults");
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MONITORINFO target{sizeof target};GetMonitorInfoW(MonitorFromPoint({2060,600},MONITOR_DEFAULTTONEAREST),&target);
    HWND window=CreateWindowExW(0,L"STATIC",L"W8a hidden placement fixture",WS_OVERLAPPEDWINDOW,target.rcWork.left+140,target.rcWork.top+60,960,680,nullptr,nullptr,nullptr,nullptr);
    Check(window!=nullptr,"hidden placement fixture exists on second monitor");
    s=CaptureWindowState(window);Check(WriteWindowState(path,s),"real normal placement serializes");DestroyWindow(window);
    window=CreateWindowExW(0,L"STATIC",L"W8a reopened hidden fixture",WS_OVERLAPPEDWINDOW,target.rcWork.left+200,target.rcWork.top+100,400,300,nullptr,nullptr,nullptr,nullptr);
    ReadWindowState(path,read);RestoreWindowState(window,read);auto restored=CaptureWindowState(window);
    std::wcout<<L"geometry saved "<<s.monitor<<L' '<<s.x<<L','<<s.y<<L' '<<s.width<<L'x'<<s.height<<L" restored "<<restored.monitor<<L' '<<restored.x<<L','<<restored.y<<L' '<<restored.width<<L'x'<<restored.height<<L'\n';
    Check(restored.monitor==s.monitor && std::abs(restored.x-s.x)<2 && std::abs(restored.y-s.y)<2 && std::abs(restored.width-s.width)<2 && std::abs(restored.height-s.height)<2,"real HWND recreation restores normal geometry without activation");
    Check(!IsWindowVisible(window),"restoration keeps a tray startup hidden");DestroyWindow(window);
    auto items=TrayItems({});Check(items.size()==2 && items[0].action==TrayAction::Open && items[1].action==TrayAction::Quit && items[1].text==L"Fechar programa" && items[1].danger,"tray outside a call offers only open and close the program");
    Check(TrayTop(items,1)==44 && TrayHeight(items)==84,"the close button sits 4px under open");
    for(bool direct:{false,true}) {
      items=TrayItems({true,direct,false,false});
      Check(items.size()==5 && items[1].text==L"Mutar" && items[2].text==L"Ensurdecer" && items[3].action==TrayAction::Leave,"room and direct call expose mute, deafen and leave only");
      items=TrayItems({true,direct,true,false});Check(items[1].text==L"Desmutar" && items[1].enabled,"muted call offers unmute");
      items=TrayItems({true,direct,true,true});Check(!items[1].enabled && items[2].text==L"Desensurdecer","deafened call preserves microphone lock and offers hearing restore");
      Check(items[3].danger && items[4].danger,"leave and quit have dangerous action semantics");
      Check(TrayTop(items,1)==40 && TrayTop(items,3)==116 && TrayTop(items,4)==156 && TrayHeight(items)==196 && TrayWidth==184,"36px rows without dividers; the red buttons sit 4px apart");
    }
    return 0;
  } catch(std::exception const& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
