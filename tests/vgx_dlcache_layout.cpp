// Count layout reconstruction in dlcache.cpp, not in the rest of the renderer.
// Use the real parser, cache and emitter, with independent expected vertex bytes.
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include <map>
#include <string>

namespace {
unsigned layoutCalls = 0;
}
namespace vgx {
VtxLayout DlTestMakeLayout(uint32_t mask) { ++layoutCalls; return MakeLayout(mask); }
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
#define MakeLayout DlTestMakeLayout
#include "../platform/vgx/src/vgx_dlcache.cpp"
#undef MakeLayout
static_assert(sizeof(vgx::Seg) == 28, "cached stride must not enlarge segment metadata");

namespace {
alignas(4096) uint8_t ram[256 * 1024] = {};
std::map<uint32_t, vgx::BufUpload> buffers;
std::vector<uint8_t> expected;
unsigned failures = 0, pictures = 0, currentMask = 0;
unsigned staticDrawsForMask = 0;
uint64_t allGeometry = 14695981039346656037ull;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: mask=%03X %s\n", currentMask, message); ++failures; }
}
void Put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}
unsigned Stride(unsigned mask) {
    unsigned size = 16;
    if (mask & 1) size += 12;
    if (mask & 2) size += 4;
    if (mask & 4) size += 4;
    for (unsigned i = 0; i < 8; ++i) if (mask & (8u << i)) size += 8;
    if (mask & 2048) size += 8;
    return size;
}
void Observe(vgx::CmdBuf& b) {
    std::vector<uint8_t> actual;
    for (const auto& c : b.cmds) {
        if (c.type == vgx::CMD_BUF_UPLOAD) {
            const auto& up = b.bufUploads.at(c.arg); buffers[up.bufId] = up;
        } else if (c.type == vgx::CMD_BUF_FREE) {
            buffers.erase(c.arg);
        } else if (c.type == vgx::CMD_DRAW) {
            const auto& d = b.draws.at(c.arg);
            Require(d.prim == vgx::PRIM_TRIANGLES && d.layoutMask == currentMask, "draw type and layout unchanged");
            const uint8_t* v = nullptr; const uint16_t* idx = nullptr;
            size_t bytes = 0, elements = 0;
            if (d.vbuf) {
                ++staticDrawsForMask;
                const auto& up = buffers.at(d.vbuf);
                v = up.vtx.data(); bytes = up.vtx.size(); idx = up.idx.data(); elements = up.idx.size();
            } else {
                const size_t chunk = b.vtxChunks.empty() ? 0 : b.vtxChunks.at(d.vchunk);
                v = b.vtx.data() + chunk; bytes = b.vtx.size() - chunk;
                idx = b.idx.data(); elements = b.idx.size();
            }
            Require(d.idxOffset + d.idxCount <= elements, "index range stays valid");
            for (unsigned i = 0; i < d.idxCount; ++i) {
                const size_t off = d.vtxOffset + static_cast<size_t>(idx[d.idxOffset+i]) * Stride(currentMask);
                Require(off + Stride(currentMask) <= bytes, "vertex range stays valid");
                actual.insert(actual.end(), v + off, v + off + Stride(currentMask));
            }
        }
    }
    Require(actual == expected, "every indexed vertex byte matches independently constructed source geometry");
    for (uint8_t byte : actual) allGeometry = (allGeometry ^ byte) * 1099511628211ull;
    ++pictures;
    vgx::backend::Execute(b);
}
void PlanInvariants() {
    vgx::PlanCache plans{};
    uint8_t vcd[26] = {};
    vgx::VtxAttrFmt vat[8][26] = {};
    for (unsigned f=0;f<8;++f) for(unsigned a=9;a<=20;++a) {
        vat[f][a].cnt = a==10 ? 0 : f&1;
        vat[f][a].type = (a==11 || a==12) ? 5 : 4;
    }
    unsigned checked=0;
    for(unsigned mask=0;mask<4096;++mask) {
        currentMask=mask;vcd[0]=1;vcd[9]=3;
        for(unsigned i=0;i<8;++i)vcd[1+i]=(mask&2048)?1:0;
        vcd[10]=(mask&1)?3:0;vcd[11]=(mask&2)?3:0;vcd[12]=(mask&4)?3:0;
        for(unsigned i=0;i<8;++i)vcd[13+i]=(mask&(8u<<i))?3:0;
        bool dirty=true;
        for(unsigned f=0;f<8;++f) {
            const auto& p=vgx::GetPlanIn(plans,dirty,vcd,vat,f,false);
            Require(p.layoutMask==mask && p.layout.mask==mask && p.layout.stride==Stride(mask),
                    "plan mask and stride remain equivalent for every layout and format");
            const auto& folded=vgx::GetFoldedPlanIn(plans,f,p);
            Require(folded.layoutMask==(mask|6) && folded.layout.mask==(mask|6) && folded.layout.stride==Stride(mask|6),
                    "folded plans update mask and stride together");
            checked+=2;
        }
    }
    std::printf("plan-invariants=%u\n",checked);
}
unsigned Fixture(unsigned mask, uint8_t* list, unsigned count) {
    vgx::ResetState(); vgx::g.cpArrayStale = 0;
    vgx::SetVcd(0, 1); vgx::SetVcd(9, 3);
    for (unsigned i = 0; i < 8; ++i) vgx::SetVcd(1+i, (mask & 2048) ? 1 : 0);
    vgx::SetVcd(10, (mask & 1) ? 3 : 0);
    vgx::SetVcd(11, (mask & 2) ? 3 : 0);
    vgx::SetVcd(12, (mask & 4) ? 3 : 0);
    for (unsigned i = 0; i < 8; ++i) vgx::SetVcd(13+i, (mask & (8u << i)) ? 3 : 0);
    for (unsigned a = 9; a <= 20; ++a) {
        uint8_t* data = ram + 128*1024 + a*256;
        for (unsigned i = 0; i < 3; ++i) {
            if (a == 11 || a == 12) {
                for (unsigned c = 0; c < 4; ++c) data[i*16+c] = static_cast<uint8_t>(a*10+i+c);
            } else for (unsigned c = 0; c < 3; ++c) Put32(data+i*16+c*4, vgx::f2u(float(a+i+c)/16.0f));
        }
        vgx::g.arrays[a] = {data,48,16,false};
    }
    expected.clear(); unsigned offset = 0;
    for (unsigned f = 0; f < 8; ++f) {
        vgx::SetVat(f,9,f&1,4,0); vgx::SetVat(f,10,0,4,0);
        vgx::SetVat(f,11,1,5,0); vgx::SetVat(f,12,1,5,0);
        for (unsigned i = 0; i < 8; ++i) vgx::SetVat(f,13+i,f&1,4,0);
        list[offset++] = static_cast<uint8_t>(0x90 | f);
        list[offset++] = static_cast<uint8_t>(count >> 8); list[offset++] = static_cast<uint8_t>(count);
        for (unsigned j = 0; j < count; ++j) {
            const unsigned index = j % 3;
            list[offset++] = static_cast<uint8_t>(f*3);
            if (mask & 2048) for (unsigned i = 0; i < 8; ++i) list[offset++] = static_cast<uint8_t>(i*3);
            for (unsigned a = 9; a <= 20; ++a) if (vgx::g.vcd[a]) { list[offset++] = 0; list[offset++] = static_cast<uint8_t>(index); }
            std::vector<uint8_t> vertex(Stride(mask),0);
            auto store = [&](unsigned at, float value) { std::memcpy(vertex.data()+at,&value,4); };
            store(0,float(9+index)/16); store(4,float(10+index)/16);
            if (f&1) store(8,float(11+index)/16);
            store(12,float(f*3)); unsigned at = 16;
            if (mask & 1) { for (unsigned c=0;c<3;++c) store(at+c*4,float(10+index+c)/16); at+=12; }
            for (unsigned a=11;a<=12;++a) if (mask & (1u << (a-10))) {
                for (unsigned c=0;c<4;++c) vertex[at+c] = static_cast<uint8_t>(a*10+index+c); at+=4;
            }
            for (unsigned t=0;t<8;++t) if (mask & (8u<<t)) {
                store(at,float(13+t+index)/16); if(f&1)store(at+4,float(14+t+index)/16); at+=8;
            }
            if (mask & 2048) for(unsigned i=0;i<8;++i)vertex[at+i]=static_cast<uint8_t>(i*3);
            expected.insert(expected.end(),vertex.begin(),vertex.end());
        }
    }
    return offset;
}
}
int main(int argc,char** argv) {
    Require(argc==2,"usage: layout dynamic|static");
    const bool promote = std::string(argv[1]) == "static";
    setenv("GXV_ATLAS","0",1); setenv("GXV_HASH","0",1);
    setenv("GXV_SOFTXFORM","128",1); setenv("GXV_STATICBUF","1",1);
    setenv("GXV_DLCACHE","1",1); setenv("GXV_DL_TINY","64",1);
    PlanInvariants();
    vgx::hle::RegisterGuestRam(ram,sizeof ram); vgx::SetFrameSink(Observe);
    const unsigned masks[] = {0,1,2,4,8,1024,2048,3,0x555,0xAAA,0x7FF,0xFFF};
    unsigned firstCalls = 0, warmCalls = 0, steadyCalls = 0;
    for (unsigned i=0;i<sizeof masks/sizeof masks[0];++i) {
        currentMask=masks[i]; uint8_t* list=ram+i*8192;
        staticDrawsForMask=0;
        const unsigned bytes=Fixture(currentMask,list,promote?18:3);
        Require(bytes>64,"fixture reaches the real geometry cache");
        Require(bytes<=8192,"synthetic lists occupy separate storage");
        layoutCalls=0; vgx::CallList(list,bytes); firstCalls+=layoutCalls;
        vgx::FrontPresent(true,nullptr);
        Require(staticDrawsForMask==0,"first use decodes before promotion");
        layoutCalls=0; vgx::CallList(list,bytes); warmCalls+=layoutCalls;
        vgx::FrontPresent(true,nullptr);
        Require(promote ? staticDrawsForMask>0 : staticDrawsForMask==0,
                "static fixture actually promotes; dynamic fixture stays in the arena");
        const unsigned promotedDraws=staticDrawsForMask;
        layoutCalls=0; vgx::CallList(list,bytes); steadyCalls+=layoutCalls;
        vgx::FrontPresent(true,nullptr);
        Require(promote ? staticDrawsForMask>promotedDraws : staticDrawsForMask==0,
                "already-promoted hits retain their static buffer");
    }
    Require(firstCalls==0,"new decoded segments reuse their plan stride during emission");
    Require(warmCalls==0,"cache hits and promotion never reconstruct immutable vertex layouts");
    Require(steadyCalls==0,"steady cache hits never reconstruct immutable vertex layouts");
    Require(pictures==36,"all three frames of twelve layouts reach the backend");
    std::printf("layout mode=%s first-layout-calls=%u warm-layout-calls=%u steady-layout-calls=%u pictures=%u geometry=%016llx failures=%u\n",
                argv[1],firstCalls,warmCalls,steadyCalls,pictures,static_cast<unsigned long long>(allGeometry),failures);
    return failures?1:0;
}
