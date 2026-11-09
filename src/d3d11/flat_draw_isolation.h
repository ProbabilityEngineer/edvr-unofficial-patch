#pragma once
// Explicit, short-lived flat draw WRITE suppression, not a rendering fix.
// Discovery is independent of temporal admission and guessed shader families.
#include "flat_output_reference.h"
#include "station_draw_mask.h"
#include "flat_compute_readback.h"
#include "../common/config.h"
#include "../common/iniedit.h"
#include "../common/log.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>
#include <string>

namespace edvr {
bool flatRuntimeIsolationQueriesSafe(ID3D11DeviceContext*);
bool flatRuntimeIsolationCaptureBusy();

struct FlatIsolationArgs {
    char kind='?';
    uint32_t count=0,start=0,instances=0,startInstance=0;
    int32_t base=0;
};
struct FlatIsolationKey {
    // Resource lifetime IDs, not bare addresses. Constant-buffer contents and
    // object identity are NOT inferred: repeated signatures remain a group.
    std::array<uint64_t,18> words{};
    bool operator<(const FlatIsolationKey& b) const {return words<b.words;}
};
struct FlatIsolationRequest {
    uint64_t epoch=0;
    std::string id,command,dataset;
    unsigned first=0,last=0;
    static bool token(const std::string& s) {
        return s.size()==32&&s.find_first_not_of("0123456789abcdef")==std::string::npos;
    }
    static bool parse(const std::string& text,uint64_t now,FlatIsolationRequest& r) {
        if(text.size()>256)return false;
        std::istringstream in(text);std::string version,extra,epoch,first,last;
        if(!(in>>version>>epoch>>r.id>>r.command>>r.dataset>>first>>last)||(in>>extra))return false;
        auto number=[](const std::string& s,uint64_t maximum,uint64_t& value){
            value=0;if(s.empty())return false;
            for(char c:s){
                if(c<'0'||c>'9')return false;
                const unsigned digit=unsigned(c-'0');
                if(value>(maximum-digit)/10)return false;
                value=value*10+digit;
            }
            return true;
        };
        uint64_t a=0,b=0;
        if(!number(epoch,UINT64_MAX,r.epoch)||!number(first,8192,a)||!number(last,8192,b))return false;
        r.first=unsigned(a);r.last=unsigned(b);
        if(version!="EDVR_ISOLATE_V1"||!token(r.id)||r.epoch>now||now-r.epoch>5)return false;
        if(r.command=="discover"||r.command=="cancel")return r.dataset=="-"&&!r.first&&!r.last;
        return r.command=="test"&&token(r.dataset)&&r.first&&r.first<=r.last&&r.last<=8192;
    }
};

class FlatDrawIsolation {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    static uint64_t systemClock(){return GetTickCount64();}
    static bool systemEscape(){return (GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;}
public:
    explicit FlatDrawIsolation(uint64_t(*clock)()=&systemClock,bool(*escape)()=&systemEscape,const std::wstring& logDirectory={}):
        clock_(clock),escape_(escape),logDirectory_(logDirectory){}
    enum class Mode { Idle, Discover, Ready, Baseline, Mask, Recover, Drain };
    static constexpr unsigned kKeyCap=8192,kFrameCap=240;
    static constexpr uint64_t kMaskMs=2000,kHardMs=5000,kDatasetMs=300000,kByteCap=8ull*1024*1024;
    struct Group {FlatIsolationKey key;unsigned discovered=0,seen=0,selected=0,masked=0,refused=0;};
    struct Ticket {unsigned group=0;uint64_t generation=0;};
private:
    uint64_t(*clock_)();
    bool(*escape_)();
    std::wstring logDirectory_;
    const std::wstring& logRoot() const {return logDirectory_.empty()?Config::get().logDir():logDirectory_;}
    std::atomic<Mode> mode_{Mode::Idle};
    std::atomic<DWORD> thread_{0};
    std::atomic<ID3D11DeviceContext*> context_{nullptr}; // identity only
    std::atomic<const char*> cancelRequested_{nullptr};
    std::atomic_flag presenting_=ATOMIC_FLAG_INIT;
    uint64_t start_=0,maskStart_=0,recoverStart_=0,deadline_=0,datasetUntil_=0,pollAt_=0,keyAt_=0,bytes_=0;
    uint64_t generation_=0;
    unsigned frames_=0,first_=0,last_=0,unsupported_=0,unsafe_=0,unknown_=0,omittedPairs_=0;
    unsigned outputW_=0,outputH_=0;
    bool discoveredThisPresent_=false;
    std::string dataset_,request_,command_,reason_="idle";
    std::wstring directory_;
    std::vector<Group> groups_;
    std::map<FlatIsolationKey,unsigned> index_;
    std::map<std::array<uint64_t,3>,unsigned> excluded_;
    std::array<FlatOutputReference,3> images_;
    std::array<uint64_t,3> imageFrames_{};
    stationmask::Mask mask_;
    void exclude(uint64_t vs,uint64_t ps,unsigned reason) {
        const std::array<uint64_t,3> key{vs,ps,reason};
        auto it=excluded_.find(key);
        if(it!=excluded_.end())++it->second;
        else if(excluded_.size()<128)excluded_.emplace(key,1);
        else ++omittedPairs_;
    }

    static uint64_t identity(ID3D11DeviceChild* object) {
        if(!object)return 0;
        // Unique to this diagnostic; attaching a scalar does not retain objects.
        static const GUID guid={0x884af7bd,0x639d,0x47b3,{0x9d,0xf4,0x58,0x77,0x5a,0x11,0x6b,0xe9}};
        static std::atomic<uint64_t> serial{0};
        uint64_t value=0;UINT size=sizeof value;
        if(SUCCEEDED(object->GetPrivateData(guid,&size,&value))&&size==sizeof value&&value)return value;
        value=++serial;
        return SUCCEEDED(object->SetPrivateData(guid,sizeof value,&value))?value:0;
    }
    static bool safe(ID3D11DeviceContext* ctx) {
        if(!flatRuntimeIsolationQueriesSafe(ctx))return false;
        Ptr<ID3D11Predicate> predicate;BOOL value=FALSE;ctx->GetPredication(&predicate,&value);
        if(predicate)return false;
        bool clean=true;ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);
        for(auto* p:so)if(p){clean=false;p->Release();}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        ID3D11UnorderedAccessView* uavs[64]{};
        const UINT count=dev->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,count,uavs);
        for(auto* p:uavs)if(p){clean=false;p->Release();}
        return clean;
    }
    static bool write(const std::wstring& path,const void* data,size_t bytes) {
        FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"wb")||!f)return false;
        const bool ok=std::fwrite(data,1,bytes,f)==bytes;
        return std::fclose(f)==0&&ok;
    }
    bool newDirectory(const std::string& id) {
        const auto root=logRoot()+L"\\flat_draw_isolation";
        if(!ensureDirectory(root))return false;
        directory_=root+L"\\"+std::wstring(id.begin(),id.end());
        return CreateDirectoryW(directory_.c_str(),nullptr)!=0; // never replace an experiment
    }
    void recover(uint64_t now,const char* why) {
        reason_=why;mode_=Mode::Recover;recoverStart_=now;frames_=0;
    }
    void tick(uint64_t now) {
        if(mask_.active())return; // never release state or groups under an in-flight issue
        if(const char* why=cancelRequested_.exchange(nullptr)){cancel(why);return;}
        if(mode_==Mode::Idle||mode_==Mode::Ready)return;
        if(now>=deadline_){reason_="hard-timeout";mode_=Mode::Drain;return;}
        if(now>=keyAt_) {
            keyAt_=now+16;
            if((mode_==Mode::Discover||mode_==Mode::Baseline||mode_==Mode::Mask)&&escape_()){
                recover(now,"escape-cancelled");return;
            }
        }
        if(mode_==Mode::Mask&&(now-maskStart_>=kMaskMs||frames_>=kFrameCap))
            recover(now,"test-ended");
    }
    void snapshot(ID3D11DeviceContext* ctx,ID3D11Texture2D* output,unsigned slot,uint64_t frame) {
        imageFrames_[slot]=frame;
        images_[slot].capture(ctx,output,[&](uint64_t n){
            if(n>kByteCap-bytes_)return false;bytes_+=n;return true;
        },[&](uint64_t n){bytes_-=n;});
    }
    void finish() {
        unsigned masked=0,selected=0;
        for(const auto& g:groups_){masked+=g.masked;selected+=g.selected;}
        for(auto& im:images_)if(!std::strcmp(im.status,"queued"))im.status="timeout";
        const bool pictures=std::all_of(images_.begin(),images_.end(),[](const FlatOutputReference& i){
            return !std::strcmp(i.status,"ok");});
        std::ostringstream out;
        out<<"{\"schema\":\"edvr-flat-draw-isolation-v1\",\"request\":\""<<request_
           <<"\",\"dataset\":\""<<dataset_<<"\",\"command\":\""<<command_<<"\",\"reason\":\""<<reason_
           <<"\",\"test_complete\":"<<(command_=="test"&&reason_=="test-ended"&&masked&&pictures?"true":"false")
           <<",\"visual_effect\":\"unassessed\",\"duration_ms\":"<<clock_()-start_
           <<",\"first\":"<<first_<<",\"last\":"<<last_<<",\"selected_calls\":"<<selected<<",\"masked_issues\":"<<masked
           <<",\"unsupported_calls\":"<<unsupported_<<",\"unsafe_calls\":"<<unsafe_<<",\"unknown_signatures\":"<<unknown_
           <<",\"signature_semantics\":\"shader/arguments/bindings group, not unique object identity\",\"groups\":[";
        for(size_t i=0;i<groups_.size();++i){
            if(i)out<<',';const auto& g=groups_[i];
            out<<"{\"id\":"<<i+1<<",\"discovered\":"<<g.discovered<<",\"seen\":"<<g.seen<<",\"selected\":"<<g.selected
               <<",\"masked\":"<<g.masked<<",\"refused\":"<<g.refused<<",\"key\":[";
            for(unsigned j=0;j<g.key.words.size();++j){if(j)out<<',';out<<'"'<<std::hex<<g.key.words[j]<<std::dec<<'"';}
            out<<"]}";
        }
        out<<"],\"excluded_pair_overflow\":"<<omittedPairs_<<",\"excluded_pairs\":[";
        bool comma=false;
        const char* exclusions[]={"unsupported-shaders-or-arguments","no-geometry-bindings","unsafe-query-predicate-uav-or-so","identity-unavailable"};
        for(const auto& e:excluded_){
            if(comma)out<<',';comma=true;
            out<<"{\"vs\":\""<<std::hex<<e.first[0]<<"\",\"ps\":\""<<e.first[1]<<std::dec
               <<"\",\"reason\":\""<<exclusions[e.first[2]]<<"\",\"calls\":"<<e.second<<"}";
        }
        out<<"],\"images\":[";
        for(unsigned i=0;i<3;++i){
            if(i)out<<',';const auto& im=images_[i];
            out<<"{\"label\":\""<<(i==0?"normal":i==1?"suppressed":"restored")<<"\",\"status\":\""<<im.status
               <<"\",\"frame\":"<<imageFrames_[i]<<",\"file\":\"image_"<<i<<".bin\",\"width\":"<<im.width
               <<",\"height\":"<<im.height<<",\"output_width\":"<<im.outputWidth<<",\"output_height\":"<<im.outputHeight
               <<",\"x\":"<<im.x<<",\"y\":"<<im.y<<",\"format\":"<<im.format<<",\"bytes\":"<<im.bytes<<"}";
        }
        out<<"]}";
        const auto text=out.str();
        const auto path=directory_+L"\\manifest.json";
        const bool saved=!directory_.empty()&&write(path+L".tmp",text.data(),text.size())&&replaceFileAtomic(path+L".tmp",path);
        Log::get().note("flat draw isolation: command=%s reason=%s groups=%u selected=%u masked-issued=%u pictures=%u manifest=%u directory=%ls; rendering restored, signatures are not object identity",
            command_.c_str(),reason_.c_str(),unsigned(groups_.size()),selected,masked,pictures?1:0,saved?1:0,directory_.c_str());
        images_={};bytes_=0;mask_.clear();mode_=dataset_.empty()?Mode::Idle:Mode::Ready;
        directory_.clear();
    }
    void poll(ID3D11DeviceContext* ctx,uint64_t now,bool conflict) {
        if(now<pollAt_)return;pollAt_=now+250;
        const auto path=logRoot()+L"\\flat_draw_isolation.request";
        FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"rb")||!f)return;
        char text[258]{};const size_t n=std::fread(text,1,257,f);std::fclose(f);
        if(!DeleteFileW(path.c_str()))return;
        FlatIsolationRequest r;
        if(!FlatIsolationRequest::parse(std::string(text,n),uint64_t(std::time(nullptr)),r)) {
            Log::get().note("flat draw isolation: refused malformed or stale request");return;
        }
        if(r.command=="cancel"){cancel("command-cancelled");return;}
        if(conflict||(mode_!=Mode::Idle&&mode_!=Mode::Ready)){
            Log::get().note("flat draw isolation: refused busy/capture-active request=%s",r.id.c_str());return;
        }
        if(r.command=="test"&&(dataset_!=r.dataset||now>datasetUntil_||r.last>groups_.size())){
            Log::get().note("flat draw isolation: refused stale dataset or group range request=%s",r.id.c_str());return;
        }
        if(!newDirectory(r.id)){Log::get().note("flat draw isolation: refused output directory");return;}
        request_=r.id;command_=r.command;reason_="armed";thread_=GetCurrentThreadId();context_=ctx;
        ++generation_;
        start_=now;deadline_=now+kHardMs;frames_=0;unsupported_=unsafe_=unknown_=omittedPairs_=0;excluded_.clear();
        first_=r.first;last_=r.last;images_={};imageFrames_={};bytes_=0;
        if(r.command=="discover"){
            groups_.clear();index_.clear();dataset_.clear();mode_=Mode::Discover;discoveredThisPresent_=true;
        } else {
            for(auto& g:groups_)g.seen=g.selected=g.masked=g.refused=0;
            mode_=Mode::Baseline;
        }
        Log::get().note("flat draw isolation: armed request=%s command=%s groups=%u..%u automatic-restore=2s/240frames hard-limit=5s Escape=cancels; no rendering config changed",
            request_.c_str(),command_.c_str(),first_,last_);
    }
public:
    bool observing() const {return mode_==Mode::Discover||mode_==Mode::Mask;}
    Mode mode() const {return mode_;}
    void cancel(const char* why) {
        if(!thread_||GetCurrentThreadId()!=thread_){cancelRequested_=why;return;}
        if(mask_.active()){cancelRequested_=why;return;}
        if(mode_!=Mode::Idle&&mode_!=Mode::Ready){reason_=why;mode_=Mode::Drain;finish();}
        dataset_.clear();groups_.clear();index_.clear();mask_.clear();mode_=Mode::Idle;context_=nullptr;thread_=0;
    }
    Ticket observe(ID3D11DeviceContext* ctx,const FlatIsolationArgs& a,uint64_t vs,uint64_t ps) {
        if(g_flatComputeInternal||!observing()||ctx!=context_||GetCurrentThreadId()!=thread_)return {};
        const auto now=clock_();tick(now);if(!observing())return {};
        if(!vs||!ps||!a.count||!a.instances||(a.kind!='D'&&a.kind!='I'&&a.kind!='N'&&a.kind!='X')){
            ++unsupported_;exclude(vs,ps,0);return {};
        }
        Ptr<ID3D11InputLayout> layout;ctx->IAGetInputLayout(&layout);
        Ptr<ID3D11Buffer> vb,vb1,ib;UINT stride=0,offset=0,stride1=0,offset1=0,ibOffset=0;DXGI_FORMAT ibFormat{};
        ctx->IAGetVertexBuffers(0,1,&vb,&stride,&offset);ctx->IAGetIndexBuffer(&ib,&ibFormat,&ibOffset);
        ctx->IAGetVertexBuffers(1,1,&vb1,&stride1,&offset1);
        D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
        Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(1,&rtv,&dsv);
        // Initial geometry domain: excludes fullscreen SV_VertexID passes and
        // depth-only/shadow draws. No guessed pair, render extent or depth-write gate.
        if(!layout||!vb||!rtv||!dsv||((a.kind=='I'||a.kind=='X')&&!ib)){++unsupported_;exclude(vs,ps,1);return {};}
        if(!safe(ctx)){++unsafe_;exclude(vs,ps,2);return {};}
        FlatIsolationKey key;
        key.words[0]=vs;key.words[1]=ps;key.words[2]=uint64_t(uint8_t(a.kind))|(uint64_t(a.count)<<8)|(uint64_t(topology)<<40);
        key.words[3]=uint64_t(a.start)|(uint64_t(uint32_t(a.base))<<32);
        key.words[4]=uint64_t(a.instances)|(uint64_t(a.startInstance)<<32);
        key.words[5]=identity(vb.Get());key.words[6]=identity(ib.Get());key.words[7]=identity(rtv.Get());key.words[8]=identity(dsv.Get());
        key.words[9]=uint64_t(stride)|(uint64_t(offset)<<32);
        key.words[10]=uint64_t(ibOffset)|(uint64_t(ibFormat)<<32);key.words[11]=identity(layout.Get());
        key.words[16]=identity(vb1.Get());key.words[17]=uint64_t(stride1)|(uint64_t(offset1)<<32);
        ID3D11ShaderResourceView* srvs[4]{};ctx->PSGetShaderResources(0,4,srvs);
        bool identities=key.words[5]&&key.words[7]&&key.words[8]&&key.words[11]&&(!ib||key.words[6])&&(!vb1||key.words[16]);
        for(unsigned i=0;i<4;++i)if(srvs[i]){
            key.words[12+i]=identity(srvs[i]);identities=identities&&key.words[12+i];srvs[i]->Release();
        }
        if(!identities){++unsafe_;exclude(vs,ps,3);return {};}
        auto it=index_.find(key);
        if(mode_==Mode::Discover){
            if(it==index_.end()){
                if(groups_.size()>=kKeyCap){dataset_.clear();reason_="discovery-cap";mode_=Mode::Drain;return {};}
                const unsigned id=unsigned(groups_.size())+1;groups_.push_back({key});it=index_.emplace(key,id).first;
            }
            ++groups_[it->second-1].discovered;return {};
        }
        if(it==index_.end()){++unknown_;return {};}
        auto& g=groups_[it->second-1];++g.seen;
        if(it->second<first_||it->second>last_)return {};
        ++g.selected;return {it->second,generation_};
    }
    template<class Issue>
    bool issue(Ticket ticket,ID3D11DeviceContext* ctx,stationmask::SetBlendFn blend,stationmask::SetDepthFn depth,Issue actual) {
        if(!ticket.group||ctx!=context_||GetCurrentThreadId()!=thread_){actual();return false;}
        tick(clock_());
        if(mode_!=Mode::Mask||ticket.generation!=generation_||ticket.group>groups_.size()){actual();return false;}
        auto& g=groups_[ticket.group-1];
        if(!safe(ctx)||!mask_.begin(ctx,blend,depth)){
            ++g.refused;recover(clock_(),"mask-refused");actual();return false;
        }
        struct Restore {stationmask::Mask& mask;ID3D11DeviceContext* ctx;~Restore(){mask.end(ctx);}} restore{mask_,ctx};
        actual();++g.masked;return true;
    }
    void present(IDXGISwapChain* swap,ID3D11Device* device,uint64_t frame,UINT flags,bool conflict) {
        if(!swap||!device||(flags&DXGI_PRESENT_TEST))return;
        if(presenting_.test_and_set(std::memory_order_acquire)){cancelRequested_="concurrent-present";return;}
        struct Leave {std::atomic_flag& flag;~Leave(){flag.clear(std::memory_order_release);}} leave{presenting_};
        if(thread_&&thread_!=GetCurrentThreadId()){cancelRequested_="present-thread-changed";return;}
        if(mask_.active()){cancelRequested_="reentrant-present";return;}
        if(!thread_)cancelRequested_=nullptr; // no live state before the first accepted request
        FlatComputeInternalScope internal;
        const uint64_t now=clock_();
        if(mode_==Mode::Idle||mode_==Mode::Ready){
            if(now<pollAt_)return;
        }
        Ptr<ID3D11DeviceContext> ctx;device->GetImmediateContext(&ctx);
        if(context_&&context_!=ctx.Get()){cancel("context-changed");}
        if(thread_&&thread_!=GetCurrentThreadId()){return;}
        discoveredThisPresent_=false;poll(ctx.Get(),now,conflict);
        if(mode_==Mode::Idle||mode_==Mode::Ready)return;
        if(conflict){cancel("passive-capture-active");return;}
        tick(now);
        Ptr<ID3D11Texture2D> output;swap->GetBuffer(0,IID_PPV_ARGS(&output));
        D3D11_TEXTURE2D_DESC td{};if(output)output->GetDesc(&td);
        if((mode_==Mode::Mask||mode_==Mode::Recover)&&(td.Width!=outputW_||td.Height!=outputH_)){
            cancel("output-extent-changed");return;
        }
        if(mode_==Mode::Discover){
            if(discoveredThisPresent_)return;
            if(groups_.empty()){reason_="no-eligible-geometry";mode_=Mode::Drain;}
            else {
                outputW_=td.Width;outputH_=td.Height;dataset_=request_;datasetUntil_=now+kDatasetMs;
                snapshot(ctx.Get(),output.Get(),0,frame);reason_="discovered";mode_=Mode::Drain;
            }
        } else if(mode_==Mode::Baseline) {
            if(td.Width!=outputW_||td.Height!=outputH_){cancel("output-extent-changed");return;}
            snapshot(ctx.Get(),output.Get(),0,frame);
            if(std::strcmp(images_[0].status,"queued")){reason_="baseline-unavailable";mode_=Mode::Drain;}
            else {mode_=Mode::Mask;maskStart_=now;frames_=0;}
        } else if(mode_==Mode::Mask) {
            ++frames_;
            if(!imageFrames_[1]&&(now-maskStart_>=1000||frames_>=kFrameCap-1))
                snapshot(ctx.Get(),output.Get(),1,frame);
        } else if(mode_==Mode::Recover) {
            if(now-recoverStart_>=500||++frames_>=120){
                snapshot(ctx.Get(),output.Get(),2,frame);mode_=Mode::Drain;
            }
        }
        bool ready=true;
        for(unsigned i=0;i<3;++i)ready=images_[i].readback(ctx.Get(),[&](const void* p,size_t n){
            return write(directory_+L"\\image_"+std::to_wstring(i)+L".bin",p,n);
        },[&](uint64_t n){bytes_-=n;})&&ready;
        if(mode_==Mode::Mask&&std::strcmp(images_[0].status,"queued")&&std::strcmp(images_[0].status,"ok"))
            recover(now,"baseline-readback-failed");
        if(mode_==Mode::Drain&&(ready||now>=deadline_))finish();
    }
};
inline FlatDrawIsolation& flatDrawIsolation(){static auto* p=new FlatDrawIsolation;return *p;}
}
