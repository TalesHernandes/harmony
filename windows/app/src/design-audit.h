#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>

inline winrt::Microsoft::UI::Xaml::FrameworkElement FindDesignPart(winrt::Microsoft::UI::Xaml::DependencyObject node, std::wstring const& name) {
  using namespace winrt::Microsoft::UI::Xaml;
  if (auto element=node.try_as<FrameworkElement>(); element && element.Name()==name) return element;
  for(int i=0;i<Media::VisualTreeHelper::GetChildrenCount(node);++i)
    if(auto part=FindDesignPart(Media::VisualTreeHelper::GetChild(node,i),name)) return part;
  return nullptr;
}
inline uint32_t DesignColor(winrt::Microsoft::UI::Xaml::Media::Brush brush) {
  if(auto solid=brush.try_as<winrt::Microsoft::UI::Xaml::Media::SolidColorBrush>()) {
    auto color=solid.Color(); return (color.R<<16)|(color.G<<8)|color.B;
  }
  return 0;
}

// Read actual laid-out controls, not constants from the drawing code. Input
// contents are deliberately excluded from the JSON (especially passwords).
inline void WriteDesignTree(std::ostream& out, winrt::Microsoft::UI::Xaml::DependencyObject const& node,
                            winrt::Microsoft::UI::Xaml::UIElement const& root, bool& first) {
  using namespace winrt;
  using namespace Microsoft::UI::Xaml;
  using namespace Microsoft::UI::Xaml::Controls;
  using namespace Microsoft::UI::Xaml::Media;
  if (auto element = node.try_as<FrameworkElement>(); element && element.ActualWidth()>0 && element.ActualHeight()>0) {
    auto position = element.TransformToVisual(root).TransformPoint({0,0});
    if (!first) out << ','; first=false;
    out << "{\"type\":\"" << to_string(get_class_name(node)) << "\",\"name\":\"" << to_string(element.Name())
        << "\",\"x\":" << position.X << ",\"y\":" << position.Y << ",\"width\":" << element.ActualWidth()
        << ",\"height\":" << element.ActualHeight() << ",\"opacity\":" << element.Opacity();
    auto margin=element.Margin();
    out << ",\"margin\":[" << margin.Left << ',' << margin.Top << ',' << margin.Right << ',' << margin.Bottom << ']';
    if (auto control=node.try_as<Control>()) {
      auto pad=control.Padding(); auto radius=control.CornerRadius();
      out << ",\"enabled\":" << (control.IsEnabled()?"true":"false");
      out << ",\"size\":" << control.FontSize() << ",\"weight\":" << control.FontWeight().Weight
          << ",\"font\":\"" << to_string(control.FontFamily().Source()) << "\",\"spacing\":" << control.CharacterSpacing()
          << ",\"foreground\":" << DesignColor(control.Foreground()) << ",\"background\":" << DesignColor(control.Background())
          << ",\"padding\":[" << pad.Left << ',' << pad.Top << ',' << pad.Right << ',' << pad.Bottom << ']'
          << ",\"radius\":[" << radius.TopLeft << ',' << radius.TopRight << ',' << radius.BottomRight << ',' << radius.BottomLeft << ']';
    }
    if (auto text=node.try_as<TextBlock>()) out << ",\"text\":" << json::Value(to_string(text.Text())).Dump() << ",\"font\":" << json::Value(to_string(text.FontFamily().Source())).Dump() << ",\"size\":" << text.FontSize() << ",\"weight\":" << text.FontWeight().Weight << ",\"lineHeight\":" << text.LineHeight() << ",\"spacing\":" << text.CharacterSpacing() << ",\"foreground\":" << DesignColor(text.Foreground());
    if (auto border=node.try_as<Border>()) {
      auto radius=border.CornerRadius();
      out << ",\"radius\":[" << radius.TopLeft << ',' << radius.TopRight << ',' << radius.BottomRight << ',' << radius.BottomLeft << ']';
      out << ",\"background\":" << DesignColor(border.Background());
    }
    out << '}';
  }
  for(int i=0;i<VisualTreeHelper::GetChildrenCount(node);++i) WriteDesignTree(out,VisualTreeHelper::GetChild(node,i),root,first);
}

struct DesignPixels {
  int width = 0, height = 0;
  std::vector<uint8_t> bytes;
};
// TextBox's caret is a laid-out rectangle in the visual tree. Unlike
// GetRectFromCharacterIndex this includes the template's padding/translation.
inline winrt::Microsoft::UI::Xaml::FrameworkElement FindDesignCaret(winrt::Microsoft::UI::Xaml::DependencyObject node) {
  using namespace winrt::Microsoft::UI::Xaml;
  if (auto rect = node.try_as<Shapes::Rectangle>(); rect && rect.ActualWidth() > 0 && rect.ActualWidth() <= 3 && rect.ActualHeight() >= 8)
    return rect;
  for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
    if (auto caret = FindDesignCaret(Media::VisualTreeHelper::GetChild(node, i))) return caret;
  return nullptr;
}
inline bool DesignInk(DesignPixels const& pixels, int x, int y, uint32_t background) {
  auto i = size_t(y * pixels.width + x) * 4;
  for (int c = 0; c < 3; ++c) {
    int bg = (background >> (c * 8)) & 255;
    // RenderTargetBitmap is premultiplied BGRA. Composite any transparent
    // pixels over the real surface before measuring contrast, never white.
    int value = pixels.bytes[i + c] + (bg * (255 - pixels.bytes[i + 3]) + 127) / 255;
    if (std::abs(value - bg) > 24) return true;
  }
  return false;
}
inline winrt::Windows::Foundation::IAsyncAction ReadDesignPixels(
    winrt::Microsoft::UI::Xaml::FrameworkElement element, std::shared_ptr<DesignPixels> result) {
  winrt::Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
  element.UpdateLayout();
  co_await bitmap.RenderAsync(element);
  auto buffer = co_await bitmap.GetPixelsAsync();
  result->width = bitmap.PixelWidth(); result->height = bitmap.PixelHeight();
  result->bytes.resize(buffer.Length());
  winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer).ReadBytes(result->bytes);
}
inline winrt::Windows::Foundation::IAsyncAction ReadDesignRegion(
    winrt::Microsoft::UI::Xaml::FrameworkElement surface,
    winrt::Microsoft::UI::Xaml::FrameworkElement region, std::shared_ptr<DesignPixels> result) {
  auto full = std::make_shared<DesignPixels>(); co_await ReadDesignPixels(surface, full);
  auto at = region.TransformToVisual(surface).TransformPoint({0, 0});
  double sx = full->width / surface.ActualWidth(), sy = full->height / surface.ActualHeight();
  int left = int(std::floor(at.X * sx)), top = int(std::floor(at.Y * sy));
  result->width = int(std::ceil((at.X + region.ActualWidth()) * sx)) - left;
  result->height = int(std::ceil((at.Y + region.ActualHeight()) * sy)) - top;
  winrt::check_bool(left >= 0 && top >= 0 && left + result->width <= full->width && top + result->height <= full->height);
  result->bytes.resize(size_t(result->width * result->height) * 4);
  for (int y = 0; y < result->height; ++y) for (int x = 0; x < result->width; ++x)
    for (int c = 0; c < 4; ++c) result->bytes[size_t(y * result->width + x) * 4 + c] = full->bytes[size_t((y + top) * full->width + x + left) * 4 + c];
}

inline winrt::Windows::Foundation::IAsyncAction SaveDesignSurface(
    winrt::Microsoft::UI::Xaml::FrameworkElement root, std::filesystem::path directory, std::string name) {
  using namespace winrt;
  using namespace Windows::Storage;
  using namespace Windows::Graphics::Imaging;
  root.UpdateLayout();
  { std::ofstream out(directory/(name+".json")); out << '['; bool first=true; WriteDesignTree(out,root,root,first); out << ']'; }
  Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
  co_await bitmap.RenderAsync(root);
  auto pixels=co_await bitmap.GetPixelsAsync();
  auto folder=co_await StorageFolder::GetFolderFromPathAsync(directory.wstring());
  auto file=co_await folder.CreateFileAsync(to_hstring(name+".png"),CreationCollisionOption::ReplaceExisting);
  auto stream=co_await file.OpenAsync(FileAccessMode::ReadWrite);
  auto encoder=co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(),stream);
  Windows::Storage::Streams::DataReader reader=Windows::Storage::Streams::DataReader::FromBuffer(pixels);
  std::vector<uint8_t> bytes(pixels.Length()); reader.ReadBytes(bytes);
  encoder.SetPixelData(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Premultiplied,bitmap.PixelWidth(),bitmap.PixelHeight(),96,96,bytes);
  co_await encoder.FlushAsync();
}

// Capture this test window only, including Windows' caption buttons. This
// does not read the owner's desktop or use an external automation helper.
inline winrt::Windows::Foundation::IAsyncAction SaveDesignWindow(HWND window, std::filesystem::path directory, std::wstring name = L"nativo-janela-inteira.png") {
  using namespace winrt;
  using namespace Windows::Storage;
  using namespace Windows::Graphics::Imaging;
  RECT rect{}; check_bool(GetWindowRect(window,&rect));
  int width=rect.right-rect.left, height=rect.bottom-rect.top;
  HDC dc=GetWindowDC(window), copy=CreateCompatibleDC(dc);
  HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);
  auto old=SelectObject(copy,bitmap);
  BOOL captured=PrintWindow(window,copy,2 /* PW_RENDERFULLCONTENT */);
  SelectObject(copy,old);
  BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=width;
  info.bmiHeader.biHeight=-height; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
  std::vector<uint8_t> bytes(size_t(width)*height*4);
  int rows=GetDIBits(copy,bitmap,0,height,bytes.data(),&info,DIB_RGB_COLORS);
  DeleteObject(bitmap); DeleteDC(copy); ReleaseDC(window,dc);
  check_bool(captured && rows==height);
  for(size_t i=3;i<bytes.size();i+=4) bytes[i]=255;
  auto folder=co_await StorageFolder::GetFolderFromPathAsync(directory.wstring());
  auto file=co_await folder.CreateFileAsync(name,CreationCollisionOption::ReplaceExisting);
  auto stream=co_await file.OpenAsync(FileAccessMode::ReadWrite);
  auto encoder=co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(),stream);
  encoder.SetPixelData(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Ignore,width,height,96,96,bytes);
  co_await encoder.FlushAsync();
}
