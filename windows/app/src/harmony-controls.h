#pragma once
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <type_traits>

// The shape the pointer's element asked for, and where the pointer was then.
struct HarmonyCursorState { winrt::Microsoft::UI::Input::InputSystemCursorShape shape=winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow; POINT at{}; };
inline HarmonyCursorState& HarmonyCursor() { static thread_local HarmonyCursorState state; return state; }
inline LPCWSTR HarmonyCursorId(winrt::Microsoft::UI::Input::InputSystemCursorShape shape) {
  using winrt::Microsoft::UI::Input::InputSystemCursorShape;
  switch(shape) {
    case InputSystemCursorShape::Hand: return IDC_HAND;
    case InputSystemCursorShape::IBeam: return IDC_IBEAM;
    case InputSystemCursorShape::SizeWestEast: return IDC_SIZEWE;
    case InputSystemCursorShape::SizeNorthSouth: return IDC_SIZENS;
    case InputSystemCursorShape::SizeNorthwestSoutheast: return IDC_SIZENWSE;
    case InputSystemCursorShape::SizeNortheastSouthwest: return IDC_SIZENESW;
    case InputSystemCursorShape::SizeAll: return IDC_SIZEALL;
    default: return IDC_ARROW;
  }
}
// A tooltip opening makes Windows ask the window for its cursor again, and
// desktop XAML answers with the arrow until the pointer next moves (the
// owner, 22/10/2026: the hand "blinks" when a tip shows). While the pointer
// has not moved since its element chose a shape, that shape is the answer.
inline bool HarmonyHoldCursor() {
  auto& state=HarmonyCursor(); POINT now{};
  if(state.shape==winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow || !GetCursorPos(&now) || now.x!=state.at.x || now.y!=state.at.y) return false;
  ::SetCursor(LoadCursorW(nullptr,HarmonyCursorId(state.shape))); return true;
}
inline void HarmonySystemCursor(winrt::Microsoft::UI::Input::InputSystemCursorShape shape) {
  using winrt::Microsoft::UI::Input::InputSystemCursorShape;
  static thread_local auto desired=InputSystemCursorShape::Arrow;
  static thread_local bool queued=false;
  desired=shape; HarmonyCursor().shape=shape; GetCursorPos(&HarmonyCursor().at);
  auto apply=[] { ::SetCursor(LoadCursorW(nullptr,HarmonyCursorId(desired))); };
  apply();
  // Run after desktop XAML has finished its own cursor update for this event.
  if(!queued) {
    queued=true;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([apply] {
      queued=false; POINT point{}; DWORD process{};
      if(GetCursorPos(&point)) GetWindowThreadProcessId(WindowFromPoint(point),&process);
      if(process==GetCurrentProcessId()) apply();
    });
  }
}

// WinUI exposes the inherited cursor through its protected interface. Apply
// it to the clickable surface so icons and labels inherit the same cursor.
inline void HarmonyPointer(winrt::Microsoft::UI::Xaml::UIElement const& element,
    winrt::Microsoft::UI::Input::InputSystemCursorShape shape = winrt::Microsoft::UI::Input::InputSystemCursorShape::Hand) {
  auto access=element.as<winrt::Microsoft::UI::Xaml::IUIElementProtected>();
  static auto hand=winrt::Microsoft::UI::Input::InputSystemCursor::Create(winrt::Microsoft::UI::Input::InputSystemCursorShape::Hand);
  auto cursor=shape==winrt::Microsoft::UI::Input::InputSystemCursorShape::Hand ? hand : winrt::Microsoft::UI::Input::InputSystemCursor::Create(shape);
  if (access.ProtectedCursor()!=cursor) {
    access.ProtectedCursor(cursor);
    // Desktop XAML may restore its arrow after setting ProtectedCursor.
    // Apply the same shape on actual pointer events, including popup items.
    auto update=[shape](auto const& sender,auto const&) {
      auto control=sender.template try_as<winrt::Microsoft::UI::Xaml::Controls::Control>();
      HarmonySystemCursor(control && !control.IsEnabled() ? winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow : shape);
    };
    element.PointerEntered(update); element.PointerMoved(update);
    element.PointerExited([](auto const&,auto const&) { HarmonySystemCursor(winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow); });
  }
}

// Editing keeps the platform's clipboard implementation and shortcuts, while
// its context and selection menus use the same drawing as the person menus.
template<class T> void HarmonySelection(T const& field) {
  using namespace winrt;
  using namespace Microsoft::UI::Xaml::Controls;
  MenuFlyout menu; auto weak=make_weak(field);
  auto add=[&](wchar_t const* label,auto action) {
    MenuFlyoutItem item; item.Text(label); HarmonyPointer(item);
    item.Click([weak,action](auto const&,auto const&) { if(auto box=weak.get()) action(box); });
    menu.Items().Append(item); return item;
  };
  if constexpr (!std::is_same_v<T,PasswordBox>) {
    add(L"Copiar",[](auto box) { box.CopySelectionToClipboard(); });
    if constexpr (std::is_same_v<T,TextBox>) add(L"Recortar",[](auto box) { if(!box.IsReadOnly()) box.CutSelectionToClipboard(); });
  }
  if constexpr (std::is_same_v<T,TextBox> || std::is_same_v<T,PasswordBox>) {
    add(L"Colar",[](auto box) {
      if constexpr (std::is_same_v<T,TextBox>) { if(!box.IsReadOnly()) box.PasteFromClipboard(); }
      else box.PasteFromClipboard();
    });
  }
  add(L"Selecionar tudo",[](auto box) { box.SelectAll(); });
  field.SelectionFlyout(menu); field.ContextFlyout(menu);
}
inline winrt::Microsoft::UI::Xaml::Controls::TextBox HarmonyTextBox() {
  winrt::Microsoft::UI::Xaml::Controls::TextBox box; HarmonySelection(box); return box;
}
inline winrt::Microsoft::UI::Xaml::Controls::PasswordBox HarmonyPasswordBox() {
  winrt::Microsoft::UI::Xaml::Controls::PasswordBox box; HarmonySelection(box); return box;
}

// Also covers framework-created popup items and controls built in the media
// UI, which has another owner. No visual-tree walk is needed on idle frames.
inline void HarmonyInputAt(winrt::Windows::Foundation::IInspectable const& source) {
  using namespace winrt::Microsoft::UI::Xaml;
  auto node=source.try_as<DependencyObject>();
  while(node) {
    if(auto author=node.try_as<Controls::TextBlock>(); author && author.Name()==L"MessageAuthor") author.TextDecorations(winrt::Windows::UI::Text::TextDecorations::Underline);
    if(node.try_as<Controls::Primitives::ButtonBase>() || node.try_as<Controls::ToggleSwitch>() ||
       node.try_as<Controls::ComboBox>() || node.try_as<Controls::ComboBoxItem>() ||
       node.try_as<Controls::MenuFlyoutItem>() || node.try_as<Controls::Slider>()) {
      auto control=node.as<Controls::Control>(); HarmonyPointer(control);
      HarmonySystemCursor(control.IsEnabled() ? winrt::Microsoft::UI::Input::InputSystemCursorShape::Hand : winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow); return;
    }
    if(node.try_as<Controls::TextBox>() || node.try_as<Controls::PasswordBox>()) { HarmonySystemCursor(winrt::Microsoft::UI::Input::InputSystemCursorShape::IBeam); return; }
    if(auto element=node.try_as<UIElement>()) {
      if(auto cursor=element.as<IUIElementProtected>().ProtectedCursor().try_as<winrt::Microsoft::UI::Input::InputSystemCursor>()) {
        auto shape=cursor.CursorShape();
        if(shape==winrt::Microsoft::UI::Input::InputSystemCursorShape::Hand || shape==winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast) {
          HarmonySystemCursor(shape); return;
        }
      }
    }
    node=Media::VisualTreeHelper::GetParent(node);
  }
  HarmonySystemCursor(winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow);
}
