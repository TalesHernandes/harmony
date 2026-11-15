#pragma once
#include <algorithm>
#include <winrt/Microsoft.UI.Input.h>
#include "harmony-controls.h"

// Hook the content, before the wheel bubbles into ScrollViewer's animated
// default handler. Keep the viewer (and ItemsStackPanel virtualization) intact.
// Precision touchpads use direct manipulation, not this wheel event.
inline constexpr bool kImmediateWheel = true;

inline bool HarmonyWheelStep(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer viewer,
    int delta, bool horizontal, uint64_t timestamp) {
  using namespace winrt;
  using namespace Microsoft::UI::Xaml;
  auto key=box_value(L"HarmonyWheelTimestamp");
  if(viewer.Resources().HasKey(key) && unbox_value<uint64_t>(viewer.Resources().Lookup(key))==timestamp) return true;
  UINT lines{};
  if(!SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES,0,&lines,0)) return false;
  double viewport=horizontal ? viewer.ViewportWidth() : viewer.ViewportHeight();
  double step=lines==WHEEL_PAGESCROLL ? viewport : double(lines)*24.0;
  double offset=horizontal ? viewer.HorizontalOffset() : viewer.VerticalOffset();
  double limit=horizontal ? viewer.ScrollableWidth() : viewer.ScrollableHeight();
  double target=std::clamp(offset+(horizontal ? 1 : -1)*delta*step/WHEEL_DELTA,0.0,limit);
  if(target==offset && lines!=0) return false;
  bool changed=lines==0 || viewer.ChangeView(horizontal ? box_value(target).as<Windows::Foundation::IReference<double>>() : nullptr,
    horizontal ? nullptr : box_value(target).as<Windows::Foundation::IReference<double>>(),nullptr,true);
  if(changed) viewer.Resources().Insert(key,box_value(timestamp));
  return changed;
}

// Polling keeps both latched and hold-to-scroll modes working outside the list.
// Weak ownership and Unloaded stop the timer when a dialog disappears.
// The scroll cursor needs a cover: desktop XAML gives the pointer the cursor of
// the element under it on every move, so SetCursor alone lasted until the
// mouse moved (the owner, 25/10/2026: the cursor "nao muda"). While scrolling,
// a clear layer over the whole window is the element under it, with the
// scroll shape as its own cursor; stopping takes it away.
inline void HarmonyAutoScroll(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer const& scroll) {
  using namespace winrt;
  using namespace Microsoft::UI::Xaml;
  using namespace Microsoft::UI::Xaml::Controls;
  struct State { DispatcherTimer timer{nullptr}; POINT origin{}; HWND owner=nullptr; ULONGLONG stopped=0; weak_ref<UIElement> keyRoot; Windows::Foundation::IInspectable keyHandler{nullptr}; Border cover{nullptr}; bool active=false, held=false, moved=false, previous=false;
    void Uncover() {
      if(!cover) return;
      if(auto panel=Media::VisualTreeHelper::GetParent(cover).try_as<Panel>()) { uint32_t at{}; if(panel.Children().IndexOf(cover,at)) panel.Children().RemoveAt(at); }
      cover=nullptr; HarmonySystemCursor(Microsoft::UI::Input::InputSystemCursorShape::Arrow);
    }
    void Cover(ScrollViewer const& viewer) {
      Uncover(); auto panel=viewer.XamlRoot() ? viewer.XamlRoot().Content().try_as<Panel>() : nullptr; if(!panel) return;
      cover=Border(); cover.Name(L"AutoScrollCover"); cover.Background(Media::SolidColorBrush(Windows::UI::Color{0,0,0,0}));
      if(auto grid=panel.try_as<Grid>()) { Grid::SetRowSpan(cover,(std::max)(1,int(grid.RowDefinitions().Size()))); Grid::SetColumnSpan(cover,(std::max)(1,int(grid.ColumnDefinitions().Size()))); }
      HarmonyPointer(cover,Microsoft::UI::Input::InputSystemCursorShape::SizeNorthSouth); panel.Children().Append(cover);
      HarmonySystemCursor(Microsoft::UI::Input::InputSystemCursorShape::SizeNorthSouth);
    }
  };
  auto state=std::make_shared<State>();
  state->timer=DispatcherTimer(); state->timer.Interval(std::chrono::milliseconds(16));
  auto stop=[state] { state->stopped=GetTickCount64(); state->active=false; state->timer.Stop(); state->Uncover(); };
  auto weak=make_weak(scroll); std::weak_ptr<State> weakState=state;
  state->timer.Tick([weak,weakState](auto const&,auto const&) {
    auto state=weakState.lock(); auto viewer=weak.get(); if(!state) return;
    POINT point{}; GetCursorPos(&point);
    bool middle=(GetAsyncKeyState(VK_MBUTTON)&0x8000)!=0;
    double distance=(point.y-state->origin.y)/(std::max(96u,GetDpiForWindow(GetActiveWindow()))/96.0);
    if(std::abs(distance)>6) state->moved=true;
    bool stop=GetForegroundWindow()!=state->owner || !viewer || !viewer.IsLoaded() || !IsWindowEnabled(GetActiveWindow()) ||
      (GetAsyncKeyState(VK_LBUTTON)&0x8000) ||
      (GetAsyncKeyState(VK_RBUTTON)&0x8000) || (middle && !state->previous) ||
      (!middle && state->held && state->moved);
    state->previous=middle;
    if(!middle) state->held=false;
    if(stop) { state->stopped=GetTickCount64(); state->active=false; state->timer.Stop(); state->Uncover(); return; }
    double speed=std::copysign(std::max(0.0,std::abs(distance)-6)*0.16,distance);
    viewer.ChangeView(nullptr,box_value(std::clamp(viewer.VerticalOffset()+speed,0.0,viewer.ScrollableHeight())).as<Windows::Foundation::IReference<double>>(),nullptr,true);
  });
  scroll.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([weak,state,stop](auto const&,Input::PointerRoutedEventArgs const& e) {
    auto viewer=weak.get(); if(!viewer || !e.GetCurrentPoint(viewer).Properties().IsMiddleButtonPressed()) return;
    auto node=e.OriginalSource().try_as<DependencyObject>();
    while(node && node!=viewer) {
      // Preserve middle-click semantics on links and tab headers.
      if(node.try_as<Documents::Hyperlink>() || node.try_as<HyperlinkButton>() || node.try_as<TabViewItem>()) return;
      if(auto nested=node.try_as<ScrollViewer>();nested && nested.ScrollableHeight()>0) return;
      node=Media::VisualTreeHelper::GetParent(node);
    }
    if(state->active) {stop();e.Handled(true);return;}
    if(viewer.ScrollableHeight()<=0 || GetTickCount64()-state->stopped<100) return;
    GetCursorPos(&state->origin);state->owner=GetActiveWindow();state->active=true;state->held=true;state->moved=false;state->previous=true;state->Cover(viewer);state->timer.Start();e.Handled(true);
  })),true);
  state->keyHandler=box_value(Input::KeyEventHandler([weakState](auto const&,Input::KeyRoutedEventArgs const& e) {
    auto state=weakState.lock();if(!state || !state->active || e.Key()!=Windows::System::VirtualKey::Escape) return;
    state->stopped=GetTickCount64();state->active=false;state->timer.Stop();state->Uncover();e.Handled(true);
  }));
  auto attach=[weak,state] {
    if(auto previous=state->keyRoot.get()) previous.RemoveHandler(UIElement::PreviewKeyDownEvent(),state->keyHandler);
    if(auto viewer=weak.get();viewer && viewer.XamlRoot()) {
      auto root=viewer.XamlRoot().Content();state->keyRoot=make_weak(root);root.AddHandler(UIElement::PreviewKeyDownEvent(),state->keyHandler,true);
    }
  };
  scroll.Loaded([attach](auto const&,auto const&) {attach();});if(scroll.IsLoaded()) attach();
  scroll.Unloaded([stop,state](auto const&,auto const&) {stop();if(auto root=state->keyRoot.get()) root.RemoveHandler(UIElement::PreviewKeyDownEvent(),state->keyHandler);});
}

// The scroll bar's lane only while there is something to scroll: a permanent
// lane left the channel list's highlight 16 px short of the island's edge
// (the owner, 22/10/2026). For short lists whose content is a plain panel.
// Without a lane, the bar still lay over the content's right edge, unseen,
// and took the clicks there: the profile's x missed its right side (the
// owner, 02/11/2026: "clique na borda falha"). It takes clicks only while
// it has its lane.
inline void ScrollLaneWhenNeeded(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer const& scroll) {
  auto lane=[weak=winrt::make_weak(scroll)] {
    auto viewer=weak.get(); if(!viewer) return;
    auto padding=viewer.Padding(); double right=viewer.ExtentHeight()>viewer.ViewportHeight()+0.5?16.0:0.0;
    if(padding.Right!=right) {padding.Right=right;viewer.Padding(padding);}
    std::vector<winrt::Microsoft::UI::Xaml::DependencyObject> todo{viewer};
    while(!todo.empty()) {
      auto at=todo.back(); todo.pop_back();
      if(auto bar=at.try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar>(); bar && bar.Name()==L"VerticalScrollBar") {bar.IsHitTestVisible(right>0);break;}
      if(at.try_as<winrt::Microsoft::UI::Xaml::Controls::ScrollContentPresenter>()) continue;   // the content is not the bar's
      for(int i=0,n=winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(at);i<n;++i) todo.push_back(winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(at,i));
    }
  };
  scroll.Loaded([lane](auto const&,auto const&) {lane();});
  scroll.SizeChanged([lane](auto const&,auto const&) {lane();});
  if(auto element=scroll.Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) element.SizeChanged([lane](auto const&,auto const&) {lane();});
  lane();
}

inline void ImmediateMouseWheel(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer const& scroll) {
  if (!kImmediateWheel) return;
  auto installed=winrt::box_value(L"HarmonyScrollInstalled");
  if(scroll.Resources().HasKey(installed)) return;
  scroll.Resources().Insert(installed,winrt::box_value(true));
  if(scroll.VerticalScrollBarVisibility()!=winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Hidden && scroll.VerticalScrollBarVisibility()!=winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled) {
    auto padding=scroll.Padding(); padding.Right=std::max(padding.Right,16.0); scroll.Padding(padding);
  }
  HarmonyAutoScroll(scroll);
  using namespace winrt;
  using namespace Microsoft::UI::Xaml;
  using namespace Microsoft::UI::Xaml::Controls;
  auto content = scroll.Content().try_as<UIElement>();
  if (!content) return;
  // Empty space between recycled rows is hit-tested against the presenter,
  // rather than the ItemsPresenter. Cover that space before ScrollViewer's
  // default animated handler as well as the rows themselves.
  auto findPresenter = [&](auto&& self, DependencyObject node) -> UIElement {
    if (auto presenter = node.try_as<ScrollContentPresenter>()) return presenter;
    for (int i=0; i<Media::VisualTreeHelper::GetChildrenCount(node); ++i)
      if (auto found=self(self,Media::VisualTreeHelper::GetChild(node,i))) return found;
    return nullptr;
  };
  if (auto presenter=findPresenter(findPresenter,scroll)) content=presenter;
  auto weak = make_weak(scroll);
  auto handler=box_value(Input::PointerEventHandler(
    [weak](auto const&, Input::PointerRoutedEventArgs const& e) {
      auto viewer = weak.get();
      if (!viewer) return;
      auto point=e.GetCurrentPoint(viewer);
      auto properties = point.Properties();
      bool horizontal = properties.IsHorizontalMouseWheel();
      auto node=e.OriginalSource().try_as<DependencyObject>();
      while(node && node!=viewer) {
        if(auto nested=node.try_as<ScrollViewer>(); nested && (horizontal ? nested.ScrollableWidth() : nested.ScrollableHeight())>0) return;
        node=Media::VisualTreeHelper::GetParent(node);
      }
      if(HarmonyWheelStep(viewer,properties.MouseWheelDelta(),horizontal,point.Timestamp())) e.Handled(true);
    }));
  content.AddHandler(UIElement::PointerWheelChangedEvent(),handler,true);
  // Blank pixels can be hit on ScrollViewer itself. Its class handler runs
  // first there; replace that pending animation with the same immediate step.
  // The timestamp prevents processing a row's event twice.
  scroll.AddHandler(UIElement::PointerWheelChangedEvent(),handler,true);
  // ListView's item gaps can target the list itself, outside the viewer's
  // routed-event branch. Give those pixels the identical wheel treatment.
  auto ancestor=Media::VisualTreeHelper::GetParent(scroll);
  while(ancestor) {
    if(auto list=ancestor.try_as<ListView>()) { list.AddHandler(UIElement::PointerWheelChangedEvent(),handler,true); break; }
    if(ancestor.try_as<ScrollViewer>()) break;
    ancestor=Media::VisualTreeHelper::GetParent(ancestor);
  }
}
