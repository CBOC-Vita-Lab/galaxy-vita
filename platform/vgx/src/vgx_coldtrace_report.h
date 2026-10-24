#pragma once
#include "vgx_coldtrace.h"

#if VGX_COLDTRACE
#include <charconv>
#include <system_error>

namespace vgx::coldtrace {
constexpr int kReportInvalid=-10003,kReportSinkError=-10004,kReportLimit=-10005;
struct ReportSink {int64_t(*write)(const char*,size_t,void*) noexcept=nullptr;void* context=nullptr;};
struct ReportResult {int64_t error=0;size_t bytes=0;};
namespace report_detail {
constexpr size_t kMaxReportBytes=2u*1024u*1024u;
constexpr const char* kNames[]={"compile","link","shark","mkdir","open","seek","read","write","close",
    "register","patch_vertex","patch_fragment","begin_scene","end_scene","finish","notification","draw","transfer_finish"};
constexpr const char* kOrigins[]={"unknown","queued","direct_shader","direct_program","utility","texture","stream","present","sdk"};
static_assert(sizeof(kNames)/sizeof(kNames[0])==kPhaseCount && VGX_COLDTRACE_DETAIL_VERSION==3);
static_assert(sizeof(kOrigins)/sizeof(kOrigins[0])==kOriginCount);
class Writer {
public:
    explicit Writer(ReportSink sink) noexcept:sink_(sink){}
    void Text(const char* text) noexcept {Append(text,std::strlen(text));}
    template<class T> void Number(T number) noexcept {
        char text[32];const auto result=std::to_chars(text,text+sizeof text,number);
        if(result.ec!=std::errc{})error_=kReportInvalid;
        else Append(text,static_cast<size_t>(result.ptr-text));
    }
    template<class T> void Field(const char* name,T value) noexcept {Key(name);Number(value);}
    template<class T> void Array(const char* name,const T& values) noexcept {
        Key(name);Text("[");bool comma=false;for(const auto value:values){if(comma)Text(",");Number(value);comma=true;}Text("]");
    }
    void Key(const char* name) noexcept {Text(",\"");Text(name);Text("\":");}
    void Hash(uint64_t value) noexcept {
        constexpr char digits[]="0123456789ABCDEF";char text[16];
        for(unsigned i=0;i<16;++i)text[i]=digits[(value>>((15u-i)*4u))&15u];
        Append(text,sizeof text);
    }
    ReportResult Finish() noexcept {Flush();return {error_,flushed_};}
private:
    ReportSink sink_;
    std::array<char,4096> buffer_{};
    size_t used_=0,generated_=0,flushed_=0;
    int64_t error_=0;
    void Append(const char* text,size_t size) noexcept {
        if(error_)return;
        if(size>kMaxReportBytes-generated_){error_=kReportLimit;return;}
        generated_+=size;
        while(size){const auto n=std::min(size,buffer_.size()-used_);std::memcpy(buffer_.data()+used_,text,n);
            used_+=n;text+=n;size-=n;if(used_==buffer_.size())Flush();if(error_)return;}
    }
    void Flush() noexcept {
        if(error_ || !used_)return;
        if(!sink_.write){error_=kReportSinkError;return;}
        size_t offset=0;
        while(offset<used_){const auto n=sink_.write(buffer_.data()+offset,used_-offset,sink_.context);
            if(n<0){error_=n;return;}
            if(n==0 || static_cast<uint64_t>(n)>used_-offset){error_=kReportSinkError;return;}
            offset+=static_cast<size_t>(n);flushed_+=static_cast<size_t>(n);}
        used_=0;
    }
};
inline void WriteItem(Writer& out,const Item& item) noexcept {
    out.Text("{\"key\":{\"frame\":");out.Number(item.key.frame);out.Field("kind",item.key.kind);out.Field("a",item.key.a);out.Field("b",item.key.b);
    out.Field("layout",item.key.layout);out.Field("blend",item.key.blend);out.Text("}");
    out.Field("start_us",item.startUs);out.Field("total_us",item.totalUs);out.Field("self_us",item.selfUs);out.Field("flags",item.flags);
    out.Field("origin",static_cast<uint32_t>(item.origin));out.Field("execute_serial",item.executeSerial);out.Field("slice_serial",item.sliceSerial);
    out.Field("shader_marks_dropped",item.shaderMarksDropped);out.Array("inclusive_us",item.inclusiveUs);out.Array("exclusive_us",item.exclusiveUs);
    out.Array("calls",item.calls);out.Array("bytes",item.bytes);out.Array("requested",item.requested);out.Array("first_error",item.firstError);
    out.Array("failures",item.failures);out.Array("short_results",item.shortResults);out.Text(",\"shaders\":[");
    for(uint32_t s=0;s<item.shaderCount;++s){const auto& shader=item.shaders[s];if(s)out.Text(",");
        out.Text("{\"shader\":");out.Number(shader.shader);out.Field("kind",shader.kind);out.Text(",\"source_hash\":\"");out.Hash(shader.sourceHash);out.Text("\"}");}
    out.Text("]}");
}
inline void WriteSpan(Writer& out,const Span& span) noexcept {
        out.Text("{\"serial\":");out.Number(span.serial);out.Field("parent_serial",span.parentSerial);out.Field("frame",span.frame);
        out.Field("start_us",span.startUs);out.Field("total_us",span.totalUs);out.Field("root_us",span.rootUs);out.Field("self_us",span.selfUs);
        out.Field("unclassified_us",span.unclassifiedUs);out.Field("roots",span.roots);out.Field("flags",span.flags);
        out.Array("exclusive_us",span.exclusiveUs);out.Array("origin_us",span.originUs);out.Key("max_item");
        if(span.roots)WriteItem(out,span.maxItem);else out.Text("null");out.Text("}");
}
inline void WriteSpans(Writer& out,const char* name,const std::array<Span,kWorstSpans>& spans,uint32_t count) noexcept {
    out.Key(name);out.Text("[");
    for(uint32_t i=0;i<count;++i){if(i)out.Text(",");WriteSpan(out,spans[i]);}
    out.Text("]");
}
}
// Frozen-only, one consumer and no rearm. Stream scalar data, never shader text, paths or memory padding.
inline ReportResult WriteReport(const FrozenView& view,ReportSink sink) noexcept {
    if(!view.data || view.data->version!=VGX_COLDTRACE_DETAIL_VERSION || view.data->worstCount>kWorstItems || view.ioCount>kIoIntervals ||
       view.data->executeCount>kWorstSpans || view.data->sliceCount>kWorstSpans || view.data->hasUploadExecution>1)return {kReportInvalid,0};
    const auto& data=*view.data;
    for(uint32_t i=0;i<data.worstCount;++i)if(data.worst[i].shaderCount>data.worst[i].shaders.size())return {kReportInvalid,0};
    for(const auto& spans:{&data.worstExecutions,&data.worstSlices})
        for(const auto& span:*spans)if(span.maxItem.shaderCount>span.maxItem.shaders.size())return {kReportInvalid,0};
    if(data.maxUploadExecution.maxItem.shaderCount>data.maxUploadExecution.maxItem.shaders.size())return {kReportInvalid,0};
    report_detail::Writer out(sink);
    out.Text("{\"schema\":\"vgx-coldtrace-1\",\"coverage\":\"owner_roots_and_backend_spans\",\"time_unit\":\"microseconds\"");
    out.Field("layout_version",data.version);out.Field("capture_start_us",data.captureStartUs);
    out.Field("items_seen",data.itemsSeen);out.Field("items_not_kept",data.itemsNotKept);out.Field("total_us",data.totalUs);
    out.Field("counter_overflows",data.counterOverflows);out.Field("depth_overflows",data.depthOverflows);out.Field("unbalanced_scopes",data.unbalancedScopes);
    out.Field("inherited_items",data.inheritedItems);out.Field("phases_outside_items",data.phasesOutsideItems);out.Field("orphan_shader_marks",data.orphanShaderMarks);
    out.Field("executions_seen",data.executionsSeen);out.Field("slices_seen",data.slicesSeen);out.Field("span_errors",data.spanErrors);
    out.Field("io_dropped",view.ioDropped);out.Field("clock_errors",view.clockErrors);out.Field("foreign_dropped",view.foreignDropped);
    out.Text(",\"io_coverage_known\":");out.Text(data.ioCoverageKnown?"true":"false");
    out.Text(",\"phase_names\":[");
    for(size_t p=0;p<kPhaseCount;++p){if(p)out.Text(",");out.Text("\"");out.Text(report_detail::kNames[p]);out.Text("\"");}
    out.Text("],\"origin_names\":[");
    for(size_t i=0;i<kOriginCount;++i){if(i)out.Text(",");out.Text("\"");out.Text(report_detail::kOrigins[i]);out.Text("\"");}
    out.Text("]");out.Array("aggregate_exclusive_us",data.exclusiveUs);out.Text(",\"items\":[");
    for(uint32_t i=0;i<data.worstCount;++i){if(i)out.Text(",");report_detail::WriteItem(out,data.worst[i]);}
    out.Text("]");report_detail::WriteSpans(out,"executions",data.worstExecutions,data.executeCount);
    report_detail::WriteSpans(out,"slices",data.worstSlices,data.sliceCount);
    out.Key("max_upload_execution");
    if(data.hasUploadExecution)report_detail::WriteSpan(out,data.maxUploadExecution);else out.Text("null");
    out.Text(",\"io\":[");
    for(uint32_t i=0;i<view.ioCount;++i){const auto& io=data.io[i];if(i)out.Text(",");
        out.Text("{\"start_us\":");out.Number(io.startUs);out.Field("end_us",io.endUs);out.Field("result",io.result);out.Field("requested",io.requested);
        out.Field("thread",io.thread);out.Field("kind",static_cast<uint32_t>(io.kind));out.Text(",\"result_noted\":");out.Text(io.resultNoted?"true":"false");out.Text("}");}
    out.Text("],\"complete\":true}\n");return out.Finish();
}
} // namespace vgx::coldtrace
#endif
