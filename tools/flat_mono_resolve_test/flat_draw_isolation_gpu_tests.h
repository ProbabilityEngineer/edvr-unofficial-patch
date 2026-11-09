#pragma once
#include "../../src/d3d11/flat_draw_isolation.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

inline bool isolationFixtureQuerySafe=true;
namespace edvr {
// The production count-bearing-query observer is exercised elsewhere. This
// seam tests that refusal reaches the issue gate; predicate/UAV/SO checks are real.
inline bool flatRuntimeIsolationQueriesSafe(ID3D11DeviceContext*){return isolationFixtureQuerySafe;}
}
inline uint64_t isolationFixtureTime=10000;
inline bool isolationFixtureEscape=false;
inline uint64_t isolationFixtureClock(){return isolationFixtureTime;}
inline bool isolationFixtureKey(){return isolationFixtureEscape;}
inline void STDMETHODCALLTYPE isolationFixtureBlend(ID3D11DeviceContext* c,ID3D11BlendState* b,const FLOAT f[4],UINT m){c->OMSetBlendState(b,f,m);}
inline void STDMETHODCALLTYPE isolationFixtureDepth(ID3D11DeviceContext* c,ID3D11DepthStencilState* d,UINT r){c->OMSetDepthStencilState(d,r);}

inline int flatDrawIsolationGpuTests(ID3D11Device* dev,ID3D11DeviceContext* ctx) {
    using Microsoft::WRL::ComPtr;namespace fs=std::filesystem;
    int bad=0,checks=0;
    auto ck=[&](bool ok,const char* why){++checks;if(!ok){++bad;std::printf("FAIL: isolation %s\n",why);}return ok;};
    edvr::FlatIsolationRequest parsed;
    const std::string token(32,'a');
    const auto prefix="EDVR_ISOLATE_V1 1000 "+token+" ";
    ck(edvr::FlatIsolationRequest::parse(prefix+"discover - 0 0",1005,parsed),"fresh request");
    ck(!edvr::FlatIsolationRequest::parse(prefix+"discover - 0 0",1006,parsed),"stale request refused");
    ck(!edvr::FlatIsolationRequest::parse(prefix+"discover - 0 0",999,parsed),"future request refused");
    ck(!edvr::FlatIsolationRequest::parse(prefix+"test "+token+" 0 1",1000,parsed),"zero range refused");
    ck(!edvr::FlatIsolationRequest::parse(prefix+"test "+token+" 2 1",1000,parsed),"inverted range refused");
    ck(!edvr::FlatIsolationRequest::parse(prefix+"discover - 0 0 trailing",1000,parsed),"trailing input refused");
    ck(!edvr::FlatIsolationRequest::parse(prefix+"test "+token+" -4294967295 1",1000,parsed),"negative unsigned wrap refused");
    ck(!edvr::FlatIsolationRequest::parse("EDVR_ISOLATE_V1 18446744073709552616 "+token+" discover - 0 0",1000,parsed),"overflow epoch refused");

    const auto saved=edvr::Config::get().logDir();
    const fs::path root=fs::path(saved)/(L"isolation_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    edvr::FlatDrawIsolation controller(isolationFixtureClock,isolationFixtureKey,root.wstring());
    struct Cleanup {edvr::FlatDrawIsolation& c;ID3D11DeviceContext* ctx;~Cleanup(){c.cancel("fixture-end");ctx->ClearState();}} cleanup{controller,ctx};
    ctx->ClearState();isolationFixtureTime=10000;
    unsigned serial=0;uint64_t frame=0;
    auto request=[&](const char* command,const std::string& dataset="-",unsigned first=0,unsigned last=0){
        std::ostringstream id;id<<std::hex<<std::setw(32)<<std::setfill('0')<<++serial;
        std::ofstream f(root/"flat_draw_isolation.request");
        f<<"EDVR_ISOLATE_V1 "<<std::time(nullptr)<<' '<<id.str()<<' '<<command<<' '<<dataset<<' '<<first<<' '<<last<<'\n';
        f.close();isolationFixtureTime+=300;return id.str();
    };
    const HWND window=CreateWindowExW(0,L"STATIC",L"EDVR isolation fixture",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    struct Window {HWND h;~Window(){if(h)DestroyWindow(h);}} closeWindow{window};
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;ComPtr<IDXGISwapChain> swap;
    dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    if(!ck(dxgi&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))),"DXGI factory"))return bad;
    DXGI_ADAPTER_DESC adapterDesc{};adapter->GetDesc(&adapterDesc);
    std::printf("Draw isolation fixture adapter=%ls feature-level=0x%x\n",adapterDesc.Description,unsigned(dev->GetFeatureLevel()));
    DXGI_SWAP_CHAIN_DESC sc{};sc.BufferDesc.Width=sc.BufferDesc.Height=64;sc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;sc.OutputWindow=window;sc.Windowed=TRUE;
    if(!ck(SUCCEEDED(factory->CreateSwapChain(dev,&sc,&swap)),"hidden fixture swapchain"))return bad;
    ComPtr<ID3D11Texture2D> output,aux,mrt1,mrt6,depth;
    ComPtr<ID3D11RenderTargetView> outputView,auxView,view1,view6;
    swap->GetBuffer(0,IID_PPV_ARGS(&output));dev->CreateRenderTargetView(output.Get(),nullptr,&outputView);
    D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);td.BindFlags=D3D11_BIND_RENDER_TARGET;td.MiscFlags=0;
    dev->CreateTexture2D(&td,nullptr,&aux);dev->CreateRenderTargetView(aux.Get(),nullptr,&auxView);
    dev->CreateTexture2D(&td,nullptr,&mrt1);dev->CreateRenderTargetView(mrt1.Get(),nullptr,&view1);
    td.Format=DXGI_FORMAT_R32G32_FLOAT;dev->CreateTexture2D(&td,nullptr,&mrt6);dev->CreateRenderTargetView(mrt6.Get(),nullptr,&view6);
    td.Format=DXGI_FORMAT_R24G8_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    dev->CreateTexture2D(&td,nullptr,&depth);
    ComPtr<ID3D11DepthStencilView> dsv;D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    vd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;dev->CreateDepthStencilView(depth.Get(),&vd,&dsv);
    if(!ck(outputView&&auxView&&view1&&view6&&dsv,"colour, depth, stencil and private MRT fixtures"))return bad;
    const char* source=
        "float4 vs(float3 p:POSITION):SV_Position{return float4(p,1);}"
        "struct O{float4 a:SV_Target0;float4 b:SV_Target1;float2 c:SV_Target6;};"
        "O ps(){O o;o.a=float4(0,1,0,1);o.b=float4(0,0,1,1);o.c=float2(3,4);return o;}";
    ComPtr<ID3DBlob> vbCode,pbCode,errors;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vbCode,&errors);
    D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&pbCode,&errors);
    if(!ck(vbCode&&pbCode,"fixture shaders compiled"))return bad;
    dev->CreateVertexShader(vbCode->GetBufferPointer(),vbCode->GetBufferSize(),nullptr,&vs);
    dev->CreatePixelShader(pbCode->GetBufferPointer(),pbCode->GetBufferSize(),nullptr,&ps);
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;dev->CreateInputLayout(&element,1,vbCode->GetBufferPointer(),vbCode->GetBufferSize(),&layout);
    const float vertices[]={-1,-1,.5f,-1,1,.5f,1,-1,.5f,1,-1,.5f,-1,1,.5f,1,1,.5f};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof vertices;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.Usage=D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA initial{vertices,0,0};ComPtr<ID3D11Buffer> vb;dev->CreateBuffer(&bd,&initial,&vb);
    D3D11_BLEND_DESC blendDesc=edvr::stationmask::blendDesc();for(auto& rt:blendDesc.RenderTarget)rt.RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> blend;dev->CreateBlendState(&blendDesc,&blend);
    auto dsDesc=edvr::stationmask::defaultDepthStencilDesc();dsDesc.DepthFunc=D3D11_COMPARISON_ALWAYS;dsDesc.StencilEnable=TRUE;
    dsDesc.FrontFace.StencilPassOp=dsDesc.BackFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;
    ComPtr<ID3D11DepthStencilState> ds;dev->CreateDepthStencilState(&dsDesc,&ds);
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rs;dev->CreateRasterizerState(&raster,&rs);
    if(!ck(vs&&ps&&vb&&layout&&blend&&ds&&rs,"fixture pipeline"))return bad;
    const FLOAT factor[4]={.1f,.2f,.3f,.4f};const UINT stride=12,offset=0;auto* buffer=vb.Get();
    ctx->IASetVertexBuffers(0,1,&buffer,&stride,&offset);ctx->IASetInputLayout(layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
    ctx->RSSetState(rs.Get());const D3D11_VIEWPORT viewport{0,0,64,64,0,1};ctx->RSSetViewports(1,&viewport);
    ctx->OMSetBlendState(blend.Get(),factor,0xffffffff);ctx->OMSetDepthStencilState(ds.Get(),7);
    auto bind=[&](bool other=false){ID3D11RenderTargetView* views[7]={other?auxView.Get():outputView.Get(),view1.Get(),nullptr,nullptr,nullptr,nullptr,view6.Get()};
        ctx->OMSetRenderTargets(7,views,dsv.Get());};
    auto clear=[&]{
        const FLOAT red[4]={1,0,0,1},motion[4]={.25f,.5f,0,0};
        ctx->ClearRenderTargetView(outputView.Get(),red);ctx->ClearRenderTargetView(auxView.Get(),red);
        ctx->ClearRenderTargetView(view1.Get(),red);ctx->ClearRenderTargetView(view6.Get(),motion);
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,3);
    };
    auto read=[&](ID3D11Texture2D* texture,unsigned bpp){
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;std::vector<uint8_t> bytes;
        if(FAILED(dev->CreateTexture2D(&desc,nullptr,&stage)))return bytes;
        ctx->CopyResource(stage.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped)))return bytes;
        bytes.resize(size_t(desc.Width)*desc.Height*bpp);
        for(UINT y=0;y<desc.Height;++y)std::memcpy(bytes.data()+size_t(y)*desc.Width*bpp,
            static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,desc.Width*bpp);
        ctx->Unmap(stage.Get(),0);return bytes;
    };
    const edvr::FlatIsolationArgs args{'D',6,0,1,0,0};
    auto draw=[&](){auto ticket=controller.observe(ctx,args,0x1234,0x5678);
        return controller.issue(ticket,ctx,isolationFixtureBlend,isolationFixtureDepth,[&]{ctx->Draw(6,0);});};
    auto present=[&](UINT flags=0){ctx->Flush();controller.present(swap.Get(),dev,++frame,flags,false);ctx->Flush();Sleep(1);};
    auto drain=[&]{for(unsigned i=0;i<100&&controller.mode()==edvr::FlatDrawIsolation::Mode::Drain;++i)present();};
    bind();clear();const auto emptyDepth=read(depth.Get(),4);ctx->Draw(6,0);
    const auto green=read(output.Get(),4);
    ck(!green.empty()&&green[0]==0&&green[1]==255&&read(depth.Get(),4)!=emptyDepth,"unmasked positive control writes colour/depth/stencil");
    ck(!controller.observe(nullptr,args,1,2).group,"dormant observer does not dereference context");
    const auto dataset=request("discover");present(DXGI_PRESENT_TEST);
    ck(controller.mode()==edvr::FlatDrawIsolation::Mode::Idle,"TEST Present never accepts request");
    present();ck(controller.mode()==edvr::FlatDrawIsolation::Mode::Discover,"explicit discovery armed");
    draw();draw();bind(true);draw();bind();present();drain();
    ck(controller.mode()==edvr::FlatDrawIsolation::Mode::Ready,"count-only discovery completed");
    auto trial=request("test",dataset,1,1);present();
    ck(controller.mode()==edvr::FlatDrawIsolation::Mode::Mask,"baseline captured before suppression");
    clear();bind();auto ticket=controller.observe(ctx,args,0x1234,0x5678);
    ck(ticket.group==1,"frozen group identity matched");
    const bool masked=controller.issue(ticket,ctx,isolationFixtureBlend,isolationFixtureDepth,[&]{ctx->Draw(6,0);});
    auto red=read(output.Get(),4);
    ck(masked&&!red.empty()&&red[0]==255&&red[1]==0,"selected draw colour suppressed");
    auto blue=read(mrt1.Get(),4);auto motion=read(mrt6.Get(),8);float motion0=0;if(!motion.empty())std::memcpy(&motion0,motion.data(),4);
    ck(!blue.empty()&&blue[0]==255&&motion0==.25f,"all MRT writes including private target6 suppressed");
    ck(!emptyDepth.empty()&&read(depth.Get(),4)==emptyDepth,"depth AND stencil unchanged");
    ComPtr<ID3D11BlendState> restoredBlend;ComPtr<ID3D11DepthStencilState> restoredDepth;FLOAT gotFactor[4]{};UINT sample=0,ref=0;
    ctx->OMGetBlendState(&restoredBlend,gotFactor,&sample);ctx->OMGetDepthStencilState(&restoredDepth,&ref);
    ck(restoredBlend==blend&&restoredDepth==ds&&sample==0xffffffff&&ref==7&&!std::memcmp(factor,gotFactor,sizeof factor),"exact OM state restored");
    bind(true);ck(!draw(),"other half not suppressed despite reordered calls");bind();
    try {controller.issue(ticket,ctx,isolationFixtureBlend,isolationFixtureDepth,[]{throw std::runtime_error("fixture");});}
    catch(const std::runtime_error&){}
    ctx->OMGetBlendState(&restoredBlend,gotFactor,&sample);ck(restoredBlend==blend,"exception restores blend");
    for(unsigned i=0;i<30;++i){isolationFixtureTime+=100;clear();bind();draw();present();}
    drain();ck(controller.mode()==edvr::FlatDrawIsolation::Mode::Ready,"automatic restoration completes");
    ck(read(output.Get(),4)==green,"normal pixels restored without settings/history reset");
    // Test the other half with the same frozen IDs, then cancel on Escape.
    trial=request("test",dataset,2,2);present();bind(true);
    ck(!controller.issue(ticket,ctx,isolationFixtureBlend,isolationFixtureDepth,[]{}),"ticket cannot escape its original experiment");
    ck(draw(),"other half is independently selectable");bind();ck(!draw(),"first half now unaffected");
    isolationFixtureEscape=true;isolationFixtureTime+=20;ck(!draw(),"Escape stops masking at next issue");
    isolationFixtureEscape=false;isolationFixtureTime+=600;present();drain();
    trial=request("test",dataset,1,1);present();
    ticket=controller.observe(ctx,args,0x1234,0x5678);isolationFixtureQuerySafe=false;
    ck(!controller.issue(ticket,ctx,isolationFixtureBlend,isolationFixtureDepth,[&]{ctx->Draw(6,0);}),"unsafe query refuses masking and issues normally");
    isolationFixtureQuerySafe=true;controller.cancel("fixture-query-refusal");
    // Fresh discovery is required after cancellation. No old selector can run.
    request("test",dataset,1,1);present();ck(controller.mode()==edvr::FlatDrawIsolation::Mode::Idle,"cancel invalidates dataset");
    auto rediscover=[&]{
        const auto id=request("discover");present();bind();draw();present();drain();return id;
    };
    const auto fresh=rediscover();request("test",fresh,1,1);present();bind();
    ComPtr<ID3D11Predicate> predicate;D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};
    dev->CreatePredicate(&qd,&predicate);
    if(ck(predicate!=nullptr,"predicate fixture")){
        ctx->Begin(predicate.Get());ctx->End(predicate.Get());ctx->SetPredication(predicate.Get(),TRUE);
        ck(!controller.observe(ctx,args,0x1234,0x5678).group,"bound predication refused");
        ctx->SetPredication(nullptr,FALSE);
    }
    D3D11_BUFFER_DESC streamDesc{};streamDesc.ByteWidth=64;streamDesc.BindFlags=D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> stream;dev->CreateBuffer(&streamDesc,nullptr,&stream);
    if(ck(stream!=nullptr,"stream-output fixture")){
        auto* raw=stream.Get();UINT position=0;ctx->SOSetTargets(1,&raw,&position);
        ck(!controller.observe(ctx,args,0x1234,0x5678).group,"stream output refused");ctx->SOSetTargets(0,nullptr,nullptr);
    }
    streamDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;streamDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> storage;ComPtr<ID3D11UnorderedAccessView> uav;dev->CreateBuffer(&streamDesc,nullptr,&storage);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32_TYPELESS;uv.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements=16;uv.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;dev->CreateUnorderedAccessView(storage.Get(),&uv,&uav);
    if(ck(uav!=nullptr,"pixel UAV fixture")){
        auto* raw=uav.Get();ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,7,1,&raw,nullptr);
        ck(!controller.observe(ctx,args,0x1234,0x5678).group,"pixel UAV refused");
        raw=nullptr;ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,7,1,&raw,nullptr);
    }
    // Hard timeout is checked at the draw boundary, even without another Present.
    isolationFixtureTime+=edvr::FlatDrawIsolation::kHardMs+1;clear();
    ck(!draw()&&read(output.Get(),4)==green,"hard deadline restores without Present");
    present();drain();
    request("test",fresh,1,1);present();
    // A high-rate renderer must not extend suppression beyond its frame cap.
    for(unsigned i=0;i<edvr::FlatDrawIsolation::kFrameCap;++i){draw();present();}
    ck(!draw()&&controller.mode()==edvr::FlatDrawIsolation::Mode::Recover,"frame limit restores without wall-clock advance");
    isolationFixtureTime+=600;present();drain();
    controller.cancel("fixture-cleanup");
    std::printf("Draw isolation GPU tests: %d checks, %d failures; artifacts=%ls\n",checks,bad,root.c_str());
    return bad;
}
