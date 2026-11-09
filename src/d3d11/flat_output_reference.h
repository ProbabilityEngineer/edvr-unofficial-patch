#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace edvr {
// Shared bounded, asynchronous pre-Present SDR crop. No render-state setters.
struct FlatOutputReference {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
    unsigned outputWidth=0,outputHeight=0,x=0,y=0,width=0,height=0,format=0;
    uint64_t allocation=0,bytes=0;
    const char* status="not-requested";

    template<class Reserve,class Release>
    void capture(ID3D11DeviceContext* ctx,ID3D11Texture2D* output,Reserve reserve,Release release) {
        if(!output||!ctx){status="output-unavailable";return;}
        D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);
        outputWidth=td.Width;outputHeight=td.Height;format=td.Format;
        if(td.SampleDesc.Count!=1||td.ArraySize!=1||td.MipLevels!=1||
           (td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&
            td.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&td.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            status="unsupported-output";return;
        }
        width=std::min(512u,td.Width);height=std::min(512u,td.Height);
        x=td.Width/2-width/2;y=td.Height/2-height/2;
        allocation=uint64_t(width)*height*8; // staging plus packed readback
        if(!reserve(allocation)){allocation=0;status="budget-cap";return;}
        td.Width=width;td.Height=height;td.Usage=D3D11_USAGE_STAGING;
        td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&stage))) {
            release(allocation);allocation=0;status="create-failed";return;
        }
        const D3D11_BOX box{x,y,0,x+width,y+height,1};
        ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,output,0,&box);status="queued";
    }
    template<class Write,class Release>
    bool readback(ID3D11DeviceContext* ctx,Write write,Release release) {
        if(!stage||std::strcmp(status,"queued"))return true;
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT hr=ctx->Map(stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
        if(FAILED(hr))status="map-failed";
        else {
            std::vector<uint8_t> pixels(size_t(width)*height*4);
            for(unsigned row=0;row<height;++row)std::memcpy(pixels.data()+size_t(row)*width*4,
                static_cast<const uint8_t*>(map.pData)+size_t(row)*map.RowPitch,width*4);
            ctx->Unmap(stage.Get(),0);
            if(write(pixels.data(),pixels.size())){status="ok";bytes=pixels.size();}
            else status="write-failed";
        }
        stage.Reset();release(allocation);allocation=0;return true;
    }
};
}
