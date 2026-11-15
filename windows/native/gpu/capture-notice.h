#pragma once
#include "internal.h"
#include "capture-hold.h"
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <filesystem>
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dwrite.lib")

namespace gpu {
// Called only when entering a hold state. The clean retained image and every
// published texture stay immutable. Text, dimming and composition stay on GPU.
inline ComPtr<ID3D11Texture2D> CaptureNotice(ID3D11Texture2D* clean,HoldReason reason) {
  if(reason!=HoldReason::Minimized&&reason!=HoldReason::Closed)return clean;
  auto measured=perf::Get("capture.notice").Begin();
  std::lock_guard lock(ContextMutex());
  D3D11_TEXTURE2D_DESC desc{};clean->GetDesc(&desc);
  auto output=Texture(desc.Width,desc.Height,DXGI_FORMAT_B8G8R8A8_UNORM);
  Context()->CopyResource(output.Get(),clean);
  ComPtr<ID2D1Factory1> factory;
  Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,IID_PPV_ARGS(&factory)),"capture D2D factory");
  ComPtr<IDXGIDevice> dxgi;Check(Device()->QueryInterface(IID_PPV_ARGS(&dxgi)),"capture DXGI");
  ComPtr<ID2D1Device> device;Check(factory->CreateDevice(dxgi.Get(),&device),"capture D2D device");
  ComPtr<ID2D1DeviceContext> dc;Check(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc),"capture D2D context");
  ComPtr<IDXGISurface> surface;Check(output.As(&surface),"capture notice surface");
  D2D1_BITMAP_PROPERTIES1 properties{};properties.pixelFormat={DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE};properties.dpiX=properties.dpiY=96;properties.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET;
  ComPtr<ID2D1Bitmap1> bitmap;Check(dc->CreateBitmapFromDxgiSurface(surface.Get(),&properties,&bitmap),"capture notice bitmap");dc->SetTarget(bitmap.Get());
  ComPtr<IDWriteFactory> write;Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.GetAddressOf())),"capture DirectWrite");
  ComPtr<IDWriteFontCollection1> fonts;
  wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);auto path=std::filesystem::path(exe).parent_path()/L"assets/Inter.ttf";
  ComPtr<IDWriteFactory5> modern;
  if(SUCCEEDED(write.As(&modern))) {
    ComPtr<IDWriteFontFile> file;
    if(SUCCEEDED(write->CreateFontFileReference(path.c_str(),nullptr,&file))) {
      ComPtr<IDWriteFontSetBuilder1> builder;Check(modern->CreateFontSetBuilder(&builder),"capture font builder");Check(builder->AddFontFile(file.Get()),"capture Inter font");
      ComPtr<IDWriteFontSet> set;Check(builder->CreateFontSet(&set),"capture font set");Check(modern->CreateFontCollectionFromFontSet(set.Get(),&fonts),"capture Inter collection");
    }
  }
  const float width=float(desc.Width),height=float(desc.Height),scale=std::min(width/1280.f,height/720.f);
  const float band=std::min(height,100.f*scale),top=height-band;
  auto format=[&](DWRITE_FONT_WEIGHT weight,float size){ComPtr<IDWriteTextFormat> f;Check(write->CreateTextFormat(fonts?L"Inter":L"Segoe UI",fonts.Get(),weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size*scale,L"pt-BR",&f),"capture notice format");f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);return f;};
  auto title=format(DWRITE_FONT_WEIGHT_SEMI_BOLD,24),subtitle=format(DWRITE_FONT_WEIGHT_NORMAL,17);
  ComPtr<ID2D1SolidColorBrush> dim,black,white,gray;
  Check(dc->CreateSolidColorBrush(D2D1::ColorF(0,0,0,.5f),&dim),"capture dim brush");Check(dc->CreateSolidColorBrush(D2D1::ColorF(0,0,0,.6f),&black),"capture band brush");Check(dc->CreateSolidColorBrush(D2D1::ColorF(1,1,1),&white),"capture title brush");Check(dc->CreateSolidColorBrush(D2D1::ColorF(.83f,.83f,.85f),&gray),"capture subtitle brush");
  dc->BeginDraw();dc->FillRectangle(D2D1::RectF(0,0,width,height),dim.Get());dc->FillRectangle(D2D1::RectF(0,top,width,height),black.Get());
  const bool minimized=reason==HoldReason::Minimized;const wchar_t* text=minimized?L"Janela minimizada":L"A janela foi fechada";
  dc->DrawTextW(text,UINT32(wcslen(text)),title.Get(),D2D1::RectF(12*scale,top+(minimized?8:0)*scale,width-12*scale,minimized?top+54*scale:height),white.Get());
  if(minimized){const wchar_t* sub=L"A imagem volta quando a janela for aberta";dc->DrawTextW(sub,UINT32(wcslen(sub)),subtitle.Get(),D2D1::RectF(12*scale,top+52*scale,width-12*scale,height-10*scale),gray.Get());}
  Check(dc->EndDraw(),"capture notice draw");measured.Done();return output;
}
}
