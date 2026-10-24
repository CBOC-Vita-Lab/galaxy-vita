#include <cstdio>
#if __has_include("../platform/vgx/src/vgx_coldtrace_report.h")
#include "../platform/vgx/src/vgx_coldtrace_report.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <thread>

namespace {
using namespace vgx::coldtrace;
std::atomic<uint64_t> now{0};thread_local uint32_t tid=1;
bool countNew=false;unsigned allocations=0;
uint64_t Clock() noexcept{return now.load();}
uint32_t Thread() noexcept{return tid;}
void Require(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
struct Sink {
    char data[2u*1024u*1024u];size_t used=0,calls=0,chunk=SIZE_MAX;int fail=0;
    static int64_t Write(const char* data,size_t size,void* opaque) noexcept {
        auto& s=*static_cast<Sink*>(opaque);++s.calls;
        if(s.fail==1)return -77;if(s.fail==2)return 0;if(s.fail==3)return static_cast<int64_t>(size)+1;
        const auto n=std::min(size,s.chunk);Require(n<=sizeof(s.data)-s.used,"test sink is bounded");
        std::memcpy(s.data+s.used,data,n);s.used+=n;return static_cast<int64_t>(n);
    }
    void Reset(){used=calls=0;chunk=SIZE_MAX;fail=0;}
    ReportSink Target(){return {Write,this};}
} sink;
void Golden(Recorder& recorder) {
    now.store(0);Require(recorder.Arm(Clock,Thread,1),"arm real report fixture");
    std::atomic<bool> begin{false},started{false},finish{false},done{false};
    std::thread worker([&]{tid=2;while(!begin.load())std::this_thread::yield();
        {Scope read(recorder,Phase::Read,IoClass::Read);started.store(true);while(!finish.load())std::this_thread::yield();read.Result(700,900);}done.store(true);});
    {SpanScope execute(recorder,SpanKind::Execute,17);SpanScope slice(recorder,SpanKind::WarmSlice);
     Scope root(recorder,Key{17,1,9,2,7,11},Origin::Queued);now.store(1000);
     {Scope compile(recorder,Phase::Compile);now.store(2000);
      {Scope read(recorder,Phase::Read,IoClass::Read);now.store(20000);begin.store(true);while(!started.load())std::this_thread::yield();read.Result(7,9);now.store(52000);}
      {Scope shark(recorder,Phase::Shark);now.store(80000);finish.store(true);while(!done.load())std::this_thread::yield();now.store(152000);}
      {Scope seek(recorder,Phase::Seek,IoClass::Meta);seek.Result(-0x100000002ll);}
      recorder.MarkShader(3,2,UINT64_MAX);now.store(171000);}
     now.store(180000);{Scope wait(recorder,Phase::Finish);now.store(200000);}now.store(208000);}
    worker.join();recorder.RequestFreeze();Require(recorder.Frozen().data,"fixture is frozen before formatting");
}
}
void* operator new(std::size_t n){if(countNew)++allocations;if(auto* p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
int main(int argc,char** argv) {
    using namespace vgx::coldtrace;
    if(argc>1 && std::strcmp(argv[1],"upload")==0){
        Recorder recorder;now.store(0);Require(recorder.Arm(Clock,Thread,1),"arm upload-selected frame fixture");
        {SpanScope execute(recorder,SpanKind::Execute,404);
         for(unsigned i=0;i<4;++i){Scope texture(recorder,Key{404,4,i},Origin::Texture);now.fetch_add(250);}
         now.store(2000);}
        {SpanScope execute(recorder,SpanKind::Execute,405);
         Scope texture(recorder,Key{405,4,5},Origin::Texture);now.fetch_add(750);}
        for(unsigned i=0;i<20;++i){SpanScope execute(recorder,SpanKind::Execute,500+i);now.fetch_add(5000);}
        recorder.RequestFreeze();const auto view=recorder.Frozen();
        sink.Reset();const auto result=WriteReport(view,sink.Target());
        Require(result.error==0,"upload-selected report serializes after drain");
        Require(std::fwrite(sink.data,1,sink.used,stdout)==sink.used,"dump upload-selected report");return 0;
    }
    Recorder recorder;Golden(recorder);const auto view=recorder.Frozen();
    sink.chunk=13;countNew=true;const auto result=WriteReport(view,sink.Target());countNew=false;
    Require(result.error==0 && result.bytes==sink.used && allocations==0,"bounded report formatting preserves bytes and uses no C++ allocation");
    const std::string_view text(sink.data,sink.used);
    Require(text.find("FFFFFFFFFFFFFFFF")!=text.npos && text.find("-4294967298")!=text.npos,"source hashes and signed 64-bit errors survive serialization");
    if(argc>1 && std::strcmp(argv[1],"full")!=0){Require(std::fwrite(sink.data,1,sink.used,stdout)==sink.used,"dump complete synthetic JSON");return 0;}
    for(int failure=1;failure<=3;++failure){sink.Reset();sink.fail=failure;const auto bad=WriteReport(view,sink.Target());
        Require(bad.error==(failure==1?-77:kReportSinkError) && sink.calls==1,"failed/zero/impossible write results stop without an endless retry");}
    sink.Reset();Require(WriteReport({},sink.Target()).error==kReportInvalid && sink.calls==0,"unpublished reports perform no writes");
    Data invalid{};invalid.version=VGX_COLDTRACE_DETAIL_VERSION;invalid.worstCount=kWorstItems+1;
    Require(WriteReport({&invalid},sink.Target()).error==kReportInvalid && sink.calls==0,"invalid worst-item bounds cannot read or write past storage");
    invalid.worstCount=0;Require(WriteReport({&invalid,kIoIntervals+1},sink.Target()).error==kReportInvalid && sink.calls==0,"invalid I/O bounds are rejected before formatting");
    invalid.sliceCount=kWorstSpans+1;
    Require(WriteReport({&invalid},sink.Target()).error==kReportInvalid && sink.calls==0,"invalid retained slice bounds are rejected before writes");
    Recorder full;now.store(0);Require(full.Arm(Clock,Thread,1),"arm full worker interval report");tid=2;
    for(unsigned i=0;i<kIoIntervals+11;++i){now.store(i*2);Scope read(full,Phase::Read,IoClass::Read);read.Result(1,1);now.store(i*2+1);}
    tid=1;now.store(10000);{Scope root(full,Key{});now.store(11000);}full.RequestFreeze();const auto capped=full.Frozen();
    Require(capped.ioCount==kIoIntervals && capped.ioDropped==11,"real Recorder publishes a capped count at I/O saturation");
    sink.Reset();const auto capacity=WriteReport(capped,sink.Target());Require(capacity.error==0 && capacity.bytes==sink.used,"a saturated I/O ring still produces a valid report");
    if(argc>1){Require(std::fwrite(sink.data,1,sink.used,stdout)==sink.used,"dump full interval JSON");return 0;}
    std::puts("PASS: frozen JSON report, exact 64-bit fields, short writes, failure bounds and zero allocations");
}
#else
int main(){std::fputs("FAIL: frozen coldtrace report serializer is not implemented\n",stderr);return 1;}
#endif
