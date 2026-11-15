#pragma once
#include <string>
#include <vector>
namespace app {
enum class TrayAction { Open, Mute, Deafen, Leave, Quit };
struct TrayCall {bool active=false,direct=false,muted=false,deafened=false;};
struct TrayItem {TrayAction action;std::wstring text;bool enabled=true,danger=false;};
// Owner's spec (22/10/2026): no dividing lines; mute, deafen and leave only
// while in a call; "Fechar programa" says what it does (a bare "Sair" read
// as leaving the call); the exits are solid red buttons like every other red
// button, 4 px apart from the row above.
inline constexpr int TrayWidth=184;
inline bool TrayButton(TrayItem const& item) {return item.danger;}
inline int TrayTop(std::vector<TrayItem> const& items,int index) {
  int y=4;for(int i=0;i<index;++i)y+=36+(TrayButton(items[i+1])?4:0);return y;
}
inline int TrayHeight(std::vector<TrayItem> const& items) {return TrayTop(items,int(items.size())-1)+40;}
inline std::string TrayIcon(TrayItem const& item) {
  switch(item.action) {
    case TrayAction::Open:return "chevrons-expand-up-right";
    case TrayAction::Mute:return item.text==L"Mutar"?"microphone":"microphone-slash";
    case TrayAction::Deafen:return item.text==L"Ensurdecer"?"headphones":"volume-xmark";
    case TrayAction::Leave:return "handset-arrow-out";
    default:return "arrow-right-from-square";
  }
}
inline std::vector<TrayItem> TrayItems(TrayCall call) {
  std::vector<TrayItem> items{{TrayAction::Open,L"Abrir"}};
  if(call.active) {
    items.push_back({TrayAction::Mute,call.muted||call.deafened?L"Desmutar":L"Mutar",!call.deafened});
    items.push_back({TrayAction::Deafen,call.deafened?L"Desensurdecer":L"Ensurdecer"});
    items.push_back({TrayAction::Leave,L"Sair da chamada",true,true});
  }
  items.push_back({TrayAction::Quit,L"Fechar programa",true,true});
  return items;
}
}
