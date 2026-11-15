#pragma once
#include "internal.h"
#include <d3dcompiler.h>
#pragma comment(lib,"d3dcompiler.lib")

namespace gpu {
// A 16x16 GPU sample with an occlusion query: CPU receives only the count of
// non-black samples, never a captured image. Used only for fullscreen windows.
class CaptureBlackProbe {
  DeviceLease device_lease_;
 public:
  bool Poll(ID3D11Texture2D* texture,int64_t now) {
    std::lock_guard lock(ContextMutex());
    if(pending_) {
      UINT64 visible=0;
      auto hr=Context()->GetData(query_.Get(),&visible,sizeof visible,D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if(hr==S_OK){black_=visible==0;pending_=false;}else if(FAILED(hr)){pending_=false;black_=false;}
    }
    if(pending_||now<next_)return black_;
    if(!query_)Initialize();
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    auto target=Texture(16,16,DXGI_FORMAT_B8G8R8A8_UNORM);
    ComPtr<ID3D11RenderTargetView> rtv;Check(Device()->CreateRenderTargetView(target.Get(),nullptr,&rtv),"capture health target");
    ComPtr<ID3D11ShaderResourceView> srv;Check(Device()->CreateShaderResourceView(texture,nullptr,&srv),"capture health source");
    auto context=Context();auto* rt=rtv.Get();auto* sr=srv.Get();
    D3D11_VIEWPORT viewport{0,0,16,16,0,1};context->RSSetViewports(1,&viewport);context->RSSetState(raster_.Get());context->OMSetRenderTargets(1,&rt,nullptr);context->OMSetBlendState(nullptr,nullptr,~0U);context->OMSetDepthStencilState(nullptr,0);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex_.Get(),nullptr,0);context->PSSetShader(pixel_.Get(),nullptr,0);context->PSSetShaderResources(0,1,&sr);
    context->Begin(query_.Get());context->Draw(3,0);context->End(query_.Get());
    sr=nullptr;context->PSSetShaderResources(0,1,&sr);context->OMSetRenderTargets(0,nullptr,nullptr);
    pending_=true;next_=now+500000;return black_;
  }
 private:
  void Initialize() {
    constexpr char shader[]=R"(
      struct V { float4 pos:SV_Position; float2 uv:TEXCOORD; };
      V vs(uint id:SV_VertexID) { V v;v.uv=float2((id<<1)&2,id&2);v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v; }
      Texture2D<float4> source:register(t0);
      float4 ps(V v):SV_Target { uint w,h;source.GetDimensions(w,h);float3 c=source.Load(int3(min(uint2(v.uv*float2(w,h)),uint2(w-1,h-1)),0)).rgb;clip(max(c.r,max(c.g,c.b))-.025);return float4(1,1,1,1); }
    )";
    ComPtr<ID3DBlob> vs,ps,error;
    Check(D3DCompile(shader,sizeof shader,nullptr,nullptr,nullptr,"vs","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error),"capture health vertex");
    Check(D3DCompile(shader,sizeof shader,nullptr,nullptr,nullptr,"ps","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error),"capture health pixel");
    Check(Device()->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_),"capture health VS");Check(Device()->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_),"capture health PS");
    D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION,0};Check(Device()->CreateQuery(&query,&query_),"capture health query");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;Check(Device()->CreateRasterizerState(&raster,&raster_),"capture health raster");
  }
  ComPtr<ID3D11VertexShader> vertex_;ComPtr<ID3D11PixelShader> pixel_;ComPtr<ID3D11Query> query_;ComPtr<ID3D11RasterizerState> raster_;
  bool pending_=false,black_=false;int64_t next_=0;
};
}
