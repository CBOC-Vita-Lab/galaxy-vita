#pragma once

#ifndef VGX_COLDTRACE
#define VGX_COLDTRACE 0
#endif

#if VGX_COLDTRACE
#define VGX_COLDTRACE_DETAIL_VERSION 3
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

// Opt-in, data-only wall tracing. No sink, allocation, thread CPU inference or scheduling changes.
namespace vgx::coldtrace {
enum class Phase : uint8_t { Compile, Link, Shark, Mkdir, Open, Seek, Read, Write, Close,
    Register, PatchVertex, PatchFragment, BeginScene, EndScene, Finish, Notification, Draw, TransferFinish, Count };
enum class IoClass : uint8_t { None, Read, Write, Meta, Any };
enum class Origin : uint32_t { Unknown, Queued, DirectShader, DirectProgram, Utility, Texture, Stream, Present, Sdk, Count };
enum class SpanKind : uint32_t { Execute, WarmSlice };
constexpr size_t kPhaseCount=static_cast<size_t>(Phase::Count), kMaxDepth=8, kWorstItems=64, kIoIntervals=4096;
constexpr size_t kOriginCount=static_cast<size_t>(Origin::Count), kWorstSpans=16;
constexpr uint32_t kFrozenMidItem=1u, kMalformedScopes=2u, kShaderMarksDropped=4u;
struct Key { uint64_t frame=0; uint32_t kind=0,a=0,b=0,layout=0,blend=0; };
struct ShaderMark { uint64_t sourceHash=0;uint32_t shader=0,kind=0; };
struct Item {
    Key key{};
    uint64_t startUs=0,totalUs=0,selfUs=0;
    uint64_t executeSerial=0,sliceSerial=0;
    std::array<uint64_t,kPhaseCount> inclusiveUs{},exclusiveUs{},bytes{},requested{};
    std::array<int64_t,kPhaseCount> firstError{};
    std::array<ShaderMark,2> shaders{};
    std::array<uint32_t,kPhaseCount> calls{},failures{},shortResults{};
    uint32_t flags=0,shaderCount=0,shaderMarksDropped=0;
    Origin origin=Origin::Unknown;
};
struct Span {
    uint64_t serial=0,parentSerial=0,frame=0,startUs=0,totalUs=0,rootUs=0,selfUs=0,unclassifiedUs=0,roots=0;
    std::array<uint64_t,kPhaseCount> exclusiveUs{};
    std::array<uint64_t,kOriginCount> originUs{};
    Item maxItem{}; // independent of the bounded worst-root set
    uint32_t flags=0;
};
struct Interval { uint64_t startUs=0,endUs=0; int64_t result=0; uint64_t requested=0; uint32_t thread=0; IoClass kind=IoClass::None;uint8_t resultNoted=0; };
struct Data {
    uint32_t version=0,ioCoverageKnown=0;
    uint64_t captureStartUs=0;
    uint64_t itemsSeen=0,itemsNotKept=0,totalUs=0,counterOverflows=0,depthOverflows=0,unbalancedScopes=0;
    uint64_t inheritedItems=0,phasesOutsideItems=0,orphanShaderMarks=0;
    std::array<uint64_t,kPhaseCount> exclusiveUs{};
    uint32_t worstCount=0;
    std::array<Item,kWorstItems> worst{};
    std::array<Interval,kIoIntervals> io{};
    uint64_t executionsSeen=0,slicesSeen=0,spanErrors=0;
    uint32_t executeCount=0,sliceCount=0;
    std::array<Span,kWorstSpans> worstExecutions{},worstSlices{};
    Span maxUploadExecution{};
    uint32_t hasUploadExecution=0;
};
static_assert(std::is_trivially_copyable<Data>::value);
// All producers accessing Recorder layout require versioned link guards, including wrappers and exporter.
static_assert(sizeof(Data)<320u*1024u);
static_assert(sizeof(Key)==32 && sizeof(Interval)==40);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
struct FrozenView {
    const Data* data=nullptr;
    uint32_t ioCount=0,ioDropped=0,clockErrors=0,foreignDropped=0;
};
class Scope;
class SpanScope;
class Recorder {
public:
    using Clock=uint64_t(*)() noexcept;
    using ThreadId=uint32_t(*)() noexcept;
    // Report consumers must finish before rearm. Scopes must always unwind; a leaked permit blocks publication.
    // ioQuiesced is a caller guarantee: I/O begun before Arm is unobservable. The live default stays a lower bound.
    bool Arm(Clock clock,ThreadId thread,uint32_t owner,bool ioQuiesced=false) noexcept {
        if(!clock || !thread || thread()!=owner)return false;
        uint32_t expected=0;
        if(!gate_.compare_exchange_strong(expected,kOpen|kInitializing,std::memory_order_acq_rel))return false;
        std::memset(&data_,0,sizeof data_);std::memset(&context_,0,sizeof context_);clock_=clock;thread_=thread;owner_=owner;
        ioCount_.store(0,std::memory_order_relaxed);ioDropped_.store(0,std::memory_order_relaxed);
        clockErrors_.store(0,std::memory_order_relaxed);foreignDropped_.store(0,std::memory_order_relaxed);scopeMisuses_.store(0,std::memory_order_relaxed);
        data_.version=VGX_COLDTRACE_DETAIL_VERSION;
        data_.ioCoverageKnown=ioQuiesced?1u:0u;
        context_.last=data_.captureStartUs=Now();
        expected=kOpen|kInitializing;
        if(gate_.compare_exchange_strong(expected,kOpen,std::memory_order_release,std::memory_order_relaxed))return true;
        data_.version=0;
        gate_.store(0,std::memory_order_release); // freeze requested during initialization
        return false;
    }
    void RequestFreeze() noexcept { gate_.fetch_and(~kOpen,std::memory_order_acq_rel); }
    FrozenView Frozen() const noexcept {
        if(gate_.load(std::memory_order_acquire)!=0)return {};
        if(!data_.version)return {};
        return {&data_,ioCount_.load(std::memory_order_relaxed),ioDropped_.load(std::memory_order_relaxed),
            clockErrors_.load(std::memory_order_relaxed),foreignDropped_.load(std::memory_order_relaxed)};
    }
    uint32_t PendingPermits() const noexcept {return gate_.load(std::memory_order_relaxed)&kUsers;}
    uint32_t ScopeMisuses() const noexcept {return scopeMisuses_.load(std::memory_order_relaxed);}
    void MarkShader(uint32_t shader,uint32_t kind,uint64_t hash) noexcept {
        if(!Enter())return;
        if(thread_()!=owner_)Increment(foreignDropped_);
        else if(!context_.active)++data_.orphanShaderMarks;
        else {
            auto& item=context_.item;bool duplicate=false;
            for(uint32_t i=0;i<item.shaderCount;++i)if(item.shaders[i].shader==shader && item.shaders[i].kind==kind && item.shaders[i].sourceHash==hash)duplicate=true;
            if(!duplicate){if(item.shaderCount<item.shaders.size())item.shaders[item.shaderCount++]={hash,shader,kind};
                else {item.flags|=kShaderMarksDropped;++item.shaderMarksDropped;}}
        }
        Leave();
    }
private:
    friend class Scope;
    friend class SpanScope;
    static constexpr uint32_t kOpen=1u<<31,kInitializing=1u<<30,kUsers=kInitializing-1u;
    struct Frame { uint64_t start=0,child=0; Phase phase=Phase::Compile; bool outer=false; };
    struct Context {
        Item item{};
        std::array<Frame,kMaxDepth> stack{};
        struct SpanContext { Span span{}; bool active=false; };
        std::array<SpanContext,2> spans{};
        uint64_t serial=0,last=0,children=0,spanSerial=0;
        size_t depth=0;
        bool active=false;
    };
    struct Ticket {
        uint64_t serial=0,start=0;
        size_t depth=0;
        uint32_t thread=0;
        Phase phase=Phase::Compile;
        IoClass io=IoClass::None;
        bool permit=false,root=false,phaseActive=false,raw=false,resultNoted=false;
        int64_t result=0;
        uint64_t requested=0;
    };
    struct SpanTicket { uint64_t serial=0; uint32_t thread=0; SpanKind kind=SpanKind::Execute; bool permit=false; };
    std::atomic<uint32_t> gate_{0},ioCount_{0},ioDropped_{0},clockErrors_{0},foreignDropped_{0},scopeMisuses_{0};
    Clock clock_=nullptr;
    ThreadId thread_=nullptr;
    uint32_t owner_=0;
    Data data_{};
    Context context_{}; // only the admitted render owner touches this or retained items
    static void Increment(std::atomic<uint32_t>& counter) noexcept {
        auto value=counter.load(std::memory_order_relaxed);
        while(value!=std::numeric_limits<uint32_t>::max() && !counter.compare_exchange_weak(value,value+1,std::memory_order_relaxed)){}
    }
    bool Enter() noexcept {
        auto value=gate_.load(std::memory_order_relaxed);
        while((value&(kOpen|kInitializing))==kOpen && (value&kUsers)!=kUsers)
            if(gate_.compare_exchange_weak(value,value+1,std::memory_order_acquire,std::memory_order_relaxed))return true;
        return false;
    }
    void Leave() noexcept { gate_.fetch_sub(1,std::memory_order_release); }
    uint64_t Now() noexcept {return clock_();}
    uint64_t RenderNow() noexcept {
        uint64_t now=Now();
        if(now<context_.last){Increment(clockErrors_);now=context_.last;}
        context_.last=now;return now;
    }
    void Add(uint64_t& value,uint64_t n) noexcept {
        if(n>std::numeric_limits<uint64_t>::max()-value){value=std::numeric_limits<uint64_t>::max();++data_.counterOverflows;}
        else value+=n;
    }
    void BeginRoot(Key key,uint64_t now,Origin origin) noexcept {
        std::memset(&context_.item,0,sizeof context_.item);context_.item.key=key;context_.item.startUs=now;
        context_.item.origin=origin;
        if(context_.spans[0].active)context_.item.executeSerial=context_.spans[0].span.serial;
        if(context_.spans[1].active)context_.item.sliceSerial=context_.spans[1].span.serial;
        ++context_.serial;context_.depth=0;context_.children=0;context_.active=true;
    }
    Ticket StartItem(Key key,Origin origin) noexcept {
        if(!Enter())return {};
        const uint32_t tid=thread_();
        if(tid!=owner_){Increment(foreignDropped_);Leave();return {};}
        if(context_.active){++data_.inheritedItems;Leave();return {};}
        const auto now=RenderNow();BeginRoot(key,now,origin);
        Ticket t;t.permit=t.root=true;t.thread=tid;t.start=now;t.serial=context_.serial;return t;
    }
    Ticket StartCall(Phase phase,IoClass io,Key ambient,Origin origin) noexcept {
        if(!Enter())return {};
        const uint32_t tid=thread_();
        Ticket t;t.permit=true;t.thread=tid;t.phase=phase;
        if(tid!=owner_){
            if(io==IoClass::None || io==IoClass::Any){
                if(phase>=Phase::Mkdir && phase<=Phase::Close)Increment(ioDropped_);
                Increment(foreignDropped_);Leave();return {};
            }
            t.raw=true;t.io=io;t.start=Now();return t;
        }
        if(static_cast<size_t>(phase)>=kPhaseCount){++data_.unbalancedScopes;Leave();return {};}
        if(!context_.active && !ambient.kind){
            ++data_.phasesOutsideItems;ambient={0,0x10001u,static_cast<uint32_t>(phase)};origin=Origin::Sdk;
        }
        t.start=RenderNow();
        if(!context_.active){BeginRoot(ambient,t.start,origin);t.root=true;}
        t.serial=context_.serial;t.depth=context_.depth;
        ++context_.item.calls[static_cast<size_t>(phase)];
        if(context_.depth==kMaxDepth){++data_.depthOverflows;context_.item.flags|=kMalformedScopes;return t;}
        bool outer=true;
        for(size_t i=0;i<context_.depth;++i)if(context_.stack[i].phase==phase)outer=false;
        context_.stack[context_.depth++]={t.start,0,phase,outer};t.phaseActive=true;return t;
    }
    void Result(Ticket& t,int64_t value,uint64_t requested) noexcept {
        if(!t.permit || t.resultNoted)return;
        t.resultNoted=true;t.result=value;t.requested=requested;
        // Depth overflow flags the whole item malformed; no matching phase can own its result.
        if(t.raw || !t.phaseActive || !context_.active || t.serial!=context_.serial)return;
        const size_t p=static_cast<size_t>(t.phase);
        if(value<0){++context_.item.failures[p];if(!context_.item.firstError[p])context_.item.firstError[p]=value;}
        if(t.phase==Phase::Read || t.phase==Phase::Write){
            Add(context_.item.requested[p],requested);
            if(value>=0){Add(context_.item.bytes[p],static_cast<uint64_t>(value));if(static_cast<uint64_t>(value)<requested)++context_.item.shortResults[p];}
        }
    }
    void ClosePhase(uint64_t now) noexcept {
        const Frame frame=context_.stack[--context_.depth];
        const uint64_t duration=now-frame.start;
        const size_t p=static_cast<size_t>(frame.phase);
        Add(context_.item.exclusiveUs[p],duration-frame.child);
        if(frame.outer)Add(context_.item.inclusiveUs[p],duration);
        if(context_.depth)Add(context_.stack[context_.depth-1].child,duration);
        else Add(context_.children,duration);
    }
    void EndRoot(uint64_t now) noexcept {
        if(context_.depth){++data_.unbalancedScopes;context_.item.flags|=kMalformedScopes;while(context_.depth)ClosePhase(now);}
        auto& item=context_.item;item.totalUs=now-item.startUs;item.selfUs=item.totalUs-context_.children;
        // An RMW snapshot cannot read an older gate value after a freeze already ordered before it.
        if(!(gate_.fetch_or(0,std::memory_order_relaxed)&kOpen))item.flags|=kFrozenMidItem;
        Add(data_.itemsSeen,1);Add(data_.totalUs,item.totalUs);
        for(size_t p=0;p<kPhaseCount;++p)Add(data_.exclusiveUs[p],item.exclusiveUs[p]);
        for(size_t i=0;i<context_.spans.size();++i){auto& active=context_.spans[i];
            if(!active.active || (i==0?item.executeSerial:item.sliceSerial)!=active.span.serial)continue;
            auto& span=active.span;Add(span.roots,1);Add(span.rootUs,item.totalUs);Add(span.selfUs,item.selfUs);
            for(size_t p=0;p<kPhaseCount;++p)Add(span.exclusiveUs[p],item.exclusiveUs[p]);
            const auto origin=static_cast<size_t>(item.origin);
            if(origin<kOriginCount)Add(span.originUs[origin],item.totalUs);else span.flags|=kMalformedScopes;
            if(span.roots==1 || item.totalUs>span.maxItem.totalUs)span.maxItem=item;
            span.flags|=item.flags&(kFrozenMidItem|kMalformedScopes);
        }
        if(data_.worstCount<kWorstItems)data_.worst[data_.worstCount++]=item;
        else {
            const auto least=std::min_element(data_.worst.begin(),data_.worst.end(),[](const Item& a,const Item& b){return a.totalUs<b.totalUs;});
            if(item.totalUs>least->totalUs)*least=item;
            Add(data_.itemsNotKept,1);
        }
        context_.active=false;
    }
    void BadSpan() noexcept {
        Add(data_.spanErrors,1);
        for(auto& active:context_.spans)if(active.active)active.span.flags|=kMalformedScopes;
        if(context_.active)context_.item.flags|=kMalformedScopes;
    }
    SpanTicket StartSpan(SpanKind kind,uint64_t frame) noexcept {
        if(!Enter())return {};
        const auto tid=thread_();
        if(tid!=owner_){Increment(foreignDropped_);Leave();return {};}
        const auto index=static_cast<size_t>(kind);
        if(index>=context_.spans.size() || context_.spans[index].active || context_.active ||
           (kind==SpanKind::Execute && context_.spans[1].active)){
            BadSpan();Leave();return {};
        }
        auto& active=context_.spans[index];std::memset(&active.span,0,sizeof active.span);
        auto& span=active.span;span.serial=++context_.spanSerial;span.startUs=RenderNow();
        if(kind==SpanKind::WarmSlice){
            if(context_.spans[0].active){span.parentSerial=context_.spans[0].span.serial;span.frame=context_.spans[0].span.frame;}
        }else span.frame=frame;
        active.active=true;
        return {span.serial,tid,kind,true};
    }
    void FinishSpan(SpanTicket& ticket) noexcept {
        if(!ticket.permit)return;
        if(thread_()!=ticket.thread){Increment(scopeMisuses_);ticket.permit=false;return;}
        auto& active=context_.spans[static_cast<size_t>(ticket.kind)];
        if(!active.active || active.span.serial!=ticket.serial)BadSpan();
        else {
            auto& span=active.span;span.totalUs=RenderNow()-span.startUs;
            if(context_.active || (ticket.kind==SpanKind::Execute && context_.spans[1].active))BadSpan();
            if(span.rootUs>span.totalUs){BadSpan();span.unclassifiedUs=0;}
            else span.unclassifiedUs=span.totalUs-span.rootUs;
            if(!(gate_.fetch_or(0,std::memory_order_relaxed)&kOpen))span.flags|=kFrozenMidItem;
            auto& seen=ticket.kind==SpanKind::Execute?data_.executionsSeen:data_.slicesSeen;Add(seen,1);
            auto& count=ticket.kind==SpanKind::Execute?data_.executeCount:data_.sliceCount;
            auto& worst=ticket.kind==SpanKind::Execute?data_.worstExecutions:data_.worstSlices;
            const auto texture=static_cast<size_t>(Origin::Texture);
            if(ticket.kind==SpanKind::Execute && span.originUs[texture] &&
               (!data_.hasUploadExecution || span.originUs[texture]>data_.maxUploadExecution.originUs[texture])){
                data_.maxUploadExecution=span;data_.hasUploadExecution=1;
            }
            if(count<kWorstSpans)worst[count++]=span;
            else {
                const auto least=std::min_element(worst.begin(),worst.end(),[](const Span& a,const Span& b){return a.totalUs<b.totalUs;});
                if(span.totalUs>least->totalUs)*least=span;
            }
            active.active=false;
        }
        ticket.permit=false;Leave();
    }
    void EndIo(const Ticket& t,uint64_t now) noexcept {
        if(now<t.start){Increment(clockErrors_);now=t.start;}
        uint32_t slot=ioCount_.load(std::memory_order_relaxed);
        while(slot<kIoIntervals && !ioCount_.compare_exchange_weak(slot,slot+1,std::memory_order_relaxed)){}
        if(slot<kIoIntervals)data_.io[slot]={t.start,now,t.result,t.requested,t.thread,t.io,uint8_t(t.resultNoted)};
        else Increment(ioDropped_);
    }
    void Finish(Ticket& t) noexcept {
        if(!t.permit)return;
        if(thread_()!=t.thread){Increment(scopeMisuses_);t.permit=false;return;} // fail closed; do not race on another owner's context
        if(t.raw)EndIo(t,Now());
        else {
            const auto now=RenderNow();
            if(t.phaseActive){
                if(context_.active && t.serial==context_.serial && context_.depth==t.depth+1)ClosePhase(now);
                else {++data_.unbalancedScopes;if(context_.active)context_.item.flags|=kMalformedScopes;}
            }
            if(t.root && context_.active && t.serial==context_.serial)EndRoot(now);
        }
        t.permit=false;Leave();
    }
};
static_assert(std::is_trivially_destructible<Recorder>::value);
static_assert(sizeof(Recorder)<324u*1024u);
class Scope {
public:
    // Stack scopes stay on their creating thread. No move/copy across worker boundaries.
    Scope(Recorder& recorder,Key key,Origin origin=Origin::Unknown) noexcept:recorder_(recorder),ticket_(recorder.StartItem(key,origin)){}
    Scope(Recorder& recorder,Phase phase,IoClass io=IoClass::None,Key ambient={},Origin origin=Origin::Unknown) noexcept:
        recorder_(recorder),ticket_(recorder.StartCall(phase,io,ambient,origin)){}
    ~Scope(){recorder_.Finish(ticket_);}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void Result(int64_t value,uint64_t requested=0) noexcept { recorder_.Result(ticket_,value,requested); }
private:
    Recorder& recorder_;
    Recorder::Ticket ticket_;
};
class SpanScope {
public:
    SpanScope(Recorder& recorder,SpanKind kind,uint64_t frame=0) noexcept:
        recorder_(recorder),ticket_(recorder.StartSpan(kind,frame)){}
    ~SpanScope(){recorder_.FinishSpan(ticket_);}
    SpanScope(const SpanScope&)=delete;
    SpanScope& operator=(const SpanScope&)=delete;
private:
    Recorder& recorder_;
    Recorder::SpanTicket ticket_;
};
struct OverlapResult { uint64_t us=0;bool complete=false; };
// Worker-thread wall overlap only, not owner I/O time or causal I/O attribution. Never add it to exclusive phases.
// Frozen-only analysis: sort indices on a bounded stack, never allocate or add nested I/O twice.
inline OverlapResult Overlap(const FrozenView& view,const Item& item,IoClass kind) noexcept {
    if(!view.data)return {};
    std::array<uint32_t,kIoIntervals> indices{};size_t n=0;
    const uint64_t end=item.startUs+item.totalUs;
    for(uint32_t i=0;i<view.ioCount;++i){const auto& io=view.data->io[i];
        if((kind==IoClass::Any || kind==io.kind) && io.endUs>item.startUs && io.startUs<end)indices[n++]=i;}
    std::sort(indices.begin(),indices.begin()+n,[&](uint32_t a,uint32_t b){return view.data->io[a].startUs<view.data->io[b].startUs;});
    uint64_t result=0,covered=item.startUs;
    for(size_t j=0;j<n;++j){const auto& io=view.data->io[indices[j]];const auto a=std::max(io.startUs,item.startUs),b=std::min(io.endUs,end);
        if(b>covered){result+=b-std::max(a,covered);covered=b;}}
    return {result,view.data->ioCoverageKnown && view.ioDropped==0 && view.clockErrors==0 && !(item.flags&(kFrozenMidItem|kMalformedScopes))};
}
#if defined(__cpp_constinit) && __cpp_constinit>=201907L
inline constinit Recorder g_recorder;
#else
inline Recorder g_recorder;
#endif
} // namespace vgx::coldtrace
#endif
