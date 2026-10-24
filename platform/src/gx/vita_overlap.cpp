#include "vita_overlap.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gpu {
namespace {
using Rect=std::array<int32_t,4>;
bool empty(const Rect& r) { return r[0]>=r[2] || r[1]>=r[3]; }
bool overlaps(const Rect& a,const Rect& b) {
    return a[0]<b[2] && b[0]<a[2] && a[1]<b[3] && b[1]<a[3];
}
constexpr size_t trackedLimit=128;
constexpr double minimumW=1e-5,maximumPixel=65536,maximumViewport=32768;
}

bool partitionVitaTriangles(const VitaPreparedDraw& draw,std::span<const uint16_t> indices,
                            std::vector<VitaOverlapGroup>& out,std::string& error) {
    error.clear();
    auto fail=[&](const char* text) { error=text; return false; };
    const auto& r=draw.raster;
    if (draw.topology!=PORT_VITA_GXM_TRIANGLES || indices.size()%3 ||
        indices.size()>std::numeric_limits<uint32_t>::max() || draw.vertices.size()>kIndexChunkMaxVertices)
        return fail("overlap partition requires a complete native-sized triangle list");
    if (!r.width || !r.height || r.width>4096 || r.height>4096)
        return fail("overlap partition has invalid native surface extents");
    bool extremeViewport=false;
    for (float value:r.viewport) {
        if (!std::isfinite(value)) return fail("overlap partition has nonfinite viewport");
        extremeViewport=extremeViewport || std::fabs(double(value))>maximumViewport;
    }
    if (r.viewport[2]<0 || r.viewport[3]<0) return fail("overlap partition has negative viewport extent");
    for (uint16_t index:indices) if (index>=draw.vertices.size())
        return fail("overlap partition index exceeds immutable vertex extent");
    Rect scissor{};
    for (unsigned i=0;i<4;++i)
        scissor[i]=std::clamp(r.scissor[i],0,int32_t((i&1) ? r.height : r.width));
    const bool noCoverage=empty(scissor) || r.viewport[2]==0 || r.viewport[3]==0;
    auto bounds=[&](size_t at) -> Rect {
        if (noCoverage) return {};
        if (extremeViewport) return scissor;
        double lowX=0,lowY=0,highX=0,highY=0;
        for (unsigned corner=0;corner<3;++corner) {
            const auto* c=draw.vertices[indices[at+corner]].clip;
            for (unsigned component=0;component<4;++component) if (!std::isfinite(c[component])) return scissor;
            const double x=c[0],y=c[1],z=c[2],w=c[3];
            if (w<=minimumW || z<-w) return scissor;
            const double nx=x/w,ny=y/w;
            if (std::fabs(nx)>maximumPixel || std::fabs(ny)>maximumPixel) return scissor;
            // Match the native raster's negative-Y viewport, not the host window.
            const double px=r.viewport[0]+(1+nx)*0.5*r.viewport[2];
            const double py=r.viewport[1]+(1-ny)*0.5*r.viewport[3];
            if (std::fabs(px)>maximumPixel || std::fabs(py)>maximumPixel) return scissor;
            if (!corner) { lowX=highX=px; lowY=highY=py; }
            else { lowX=std::min(lowX,px); lowY=std::min(lowY,py); highX=std::max(highX,px); highY=std::max(highY,py); }
        }
        Rect b{int32_t(std::floor(lowX))-1,int32_t(std::floor(lowY))-1,
               int32_t(std::ceil(highX))+1,int32_t(std::ceil(highY))+1};
        b[0]=std::max(b[0],scissor[0]); b[1]=std::max(b[1],scissor[1]);
        b[2]=std::min(b[2],scissor[2]); b[3]=std::min(b[3],scissor[3]);
        return empty(b) ? Rect{} : b;
    };
    std::vector<VitaOverlapGroup> groups;
    std::array<Rect,trackedLimit> tracked{};
    size_t count=0;
    for (size_t at=0;at<indices.size();at+=3) {
        const Rect b=bounds(at);
        const bool covers=!empty(b);
        bool split=groups.empty();
        if (!split && covers) {
            split=count==tracked.size();
            for (size_t i=0;i<count && !split;++i) split=overlaps(b,tracked[i]);
        }
        if (split) { groups.push_back({uint32_t(at),0,{}}); count=0; }
        auto& group=groups.back(); group.indexCount+=3;
        if (covers) {
            if (!count) group.bounds=b;
            else {
                group.bounds[0]=std::min(group.bounds[0],b[0]); group.bounds[1]=std::min(group.bounds[1],b[1]);
                group.bounds[2]=std::max(group.bounds[2],b[2]); group.bounds[3]=std::max(group.bounds[3],b[3]);
            }
            tracked[count++]=b;
        }
    }
    out.swap(groups); return true;
}
}
