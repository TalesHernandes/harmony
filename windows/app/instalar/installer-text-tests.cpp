#include "installer-text.h"
#include <iostream>
#include <vector>
class GlyphProbe final : public IDWriteTextRenderer {
 public:
  int expected{}, runs{};bool correct=true;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** value) override {
    if(id==__uuidof(IUnknown)||id==__uuidof(IDWritePixelSnapping)||id==__uuidof(IDWriteTextRenderer)) {*value=static_cast<IDWriteTextRenderer*>(this);return S_OK;}
    *value=nullptr;return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
  ULONG STDMETHODCALLTYPE Release() override {return 1;}
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value) override {*value=FALSE;return S_OK;}
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value) override {*value={1,0,0,1,0,0};return S_OK;}
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override {*value=1;return S_OK;}
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT,FLOAT,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) override {
    installerui::ComPtr<IDWriteFontFace5> face;
    if(FAILED(run->fontFace->QueryInterface(IID_PPV_ARGS(&face)))) {correct=false;return E_FAIL;}
    std::vector<DWRITE_FONT_AXIS_VALUE> axes(face->GetFontAxisValueCount());
    installerui::Check(face->GetFontAxisValues(axes.data(),UINT32(axes.size())));
    installerui::ComPtr<IDWriteLocalizedStrings> names;installerui::Check(face->GetFamilyNames(&names));
    UINT32 length{};installerui::Check(names->GetStringLength(0,&length));std::wstring family(length+1,L'\0');
    installerui::Check(names->GetString(0,family.data(),length+1));family.resize(length);
    correct=correct&&family==L"Inter";std::wcout<<L"actual glyph family="<<family<<L'\n';
    bool found=false;
    for(auto axis:axes) if(axis.axisTag==DWRITE_FONT_AXIS_TAG_WEIGHT) {found=axis.value==expected;std::cout<<"actual glyph weight="<<axis.value<<" expected="<<expected<<'\n';}
    correct=correct&&found&&face->GetSimulations()==DWRITE_FONT_SIMULATIONS_NONE;++runs;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return S_OK;}
};
int wmain(int argc,wchar_t** argv) {
  try {
    if(argc!=2)return 2;
    installerui::InitializeText(argv[1]);
    for(int weight:{500,600}) {
      auto layout=installerui::Layout(L"Instalação do harmony — versão e arquivos",560,80,15,weight);
      GlyphProbe probe;probe.expected=weight;installerui::Check(layout->Draw(nullptr,&probe,0,0));
      if(!probe.correct||!probe.runs)return 3;
    }
    bool refused=false;try {installerui::Layout(L"invalid",100,30,15,400);}catch(...) {refused=true;}
    if(!refused)return 4;
    std::cout<<"PASS: installer uses actual Inter variable glyph weights 500/600 without synthetic bold; weights below 500 refused.\n";
    return 0;
  } catch(...) {return 1;}
}
