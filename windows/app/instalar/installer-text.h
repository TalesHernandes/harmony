#pragma once
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
namespace installerui {
using Microsoft::WRL::ComPtr;
inline ComPtr<IDWriteFactory6> writing;
inline ComPtr<IDWriteFontCollection1> fonts;
inline ComPtr<ID2D1Factory> drawing;
inline void Check(HRESULT result) {if(FAILED(result))throw std::runtime_error("installer text rendering failed");}
inline void InitializeText(const wchar_t* path) {
  Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory6),reinterpret_cast<IUnknown**>(writing.GetAddressOf())));
  ComPtr<IDWriteFontFile> file;Check(writing->CreateFontFileReference(path,nullptr,&file));
  ComPtr<IDWriteFontSetBuilder1> builder;Check(writing->CreateFontSetBuilder(&builder));Check(builder->AddFontFile(file.Get()));
  ComPtr<IDWriteFontSet> set;Check(builder->CreateFontSet(&set));
  Check(static_cast<IDWriteFactory3*>(writing.Get())->CreateFontCollectionFromFontSet(set.Get(),&fonts));
  Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,drawing.GetAddressOf()));
}
inline ComPtr<IDWriteTextLayout> Layout(const std::wstring& text,float width,float height,int size,int weight,bool centered=false) {
  if(weight<500)throw std::runtime_error("installer text weight below 500 refused");
  // GDI maps the variable Inter file back to weight 400. Explicit DirectWrite
  // axes select the actual 500/600 outlines, as WinUI does for the native app.
  DWRITE_FONT_AXIS_VALUE axis{DWRITE_FONT_AXIS_TAG_WEIGHT,float(weight)};
  ComPtr<IDWriteTextFormat3> format;Check(writing->CreateTextFormat(L"Inter",fonts.Get(),&axis,1,float(size),L"pt-BR",&format));
  Check(format->SetWordWrapping(text.find(L'\\')==std::wstring::npos?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_CHARACTER));
  float line=size==13?18.f:size==15?22.f:size==16?24.f:size==20?28.f:16.f;
  Check(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,float(size)));
  if(centered)Check(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
  DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
  ComPtr<IDWriteInlineObject> ellipsis;Check(writing->CreateEllipsisTrimmingSign(format.Get(),&ellipsis));
  Check(format->SetTrimming(&trimming,ellipsis.Get()));
  ComPtr<IDWriteTextLayout> layout;Check(writing->CreateTextLayout(text.c_str(),UINT32(text.size()),format.Get(),width,height,&layout));
  ComPtr<IDWriteTextLayout1> spaced;Check(layout.As(&spaced));
  float spacing=size==12?.06f:size==13?-.02f:size==16?-.01f:-.015f;
  Check(spaced->SetCharacterSpacing(0,spacing*size,0,DWRITE_TEXT_RANGE{0,UINT32(text.size())}));
  return layout;
}
inline void PaintText(HDC dc,float scale,const std::wstring& text,int x,int y,int width,int height,int size,int weight,COLORREF color,bool centered=false) {
  int saved=SaveDC(dc);SetMapMode(dc,MM_TEXT);
  auto layout=Layout(text,float(width),float(height),size,weight,centered);
  ComPtr<ID2D1DCRenderTarget> target;
  auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96,D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
  Check(drawing->CreateDCRenderTarget(&properties,&target));
  BITMAP bitmap{};GetObjectW(GetCurrentObject(dc,OBJ_BITMAP),sizeof(bitmap),&bitmap);
  RECT bounds{0,0,bitmap.bmWidth,bitmap.bmHeight};Check(target->BindDC(dc,&bounds));
  target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  ComPtr<ID2D1SolidColorBrush> brush;Check(target->CreateSolidColorBrush(D2D1::ColorF(GetRValue(color)/255.f,GetGValue(color)/255.f,GetBValue(color)/255.f),&brush));
  target->DrawTextLayout(D2D1::Point2F(float(x),float(y)),layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
  Check(target->EndDraw());
  RestoreDC(dc,saved);
}
}
