// Real CallList/StateListCall paths; the wrapper counts only raw parser fallbacks.
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_workload.h"

#include <atomic>
#include <cstdarg>
#include <string>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
uint32_t TinyCountedProcessStream(const uint8_t*, uint32_t, bool = true);
}
#define ProcessStream TinyCountedProcessStream
#include "../platform/vgx/src/vgx_dlcache.cpp"
#undef ProcessStream

namespace {
alignas(4096) uint8_t tinyRam[1024 * 1024] = {};
alignas(32) uint8_t tinyUntracked[64] = {};
unsigned tinyRawCalls = 0, tinyChecks = 0;
std::vector<uint64_t> tinyEvents;
void Require(bool ok, const char* message) {
    ++tinyChecks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24-8*i));
}
void BP(std::vector<uint8_t>& b, unsigned reg, unsigned value) {
    b.push_back(0x61);
    const size_t at = b.size(); b.resize(at+4); Put32(b.data()+at, reg<<24 | value);
}
void CP(std::vector<uint8_t>& b, unsigned reg, unsigned value) {
    b.push_back(0x08); b.push_back(static_cast<uint8_t>(reg));
    const size_t at = b.size(); b.resize(at+4); Put32(b.data()+at, value);
}
void XF(std::vector<uint8_t>& b, unsigned address, std::initializer_list<uint32_t> words) {
    b.push_back(0x10); b.push_back(0); b.push_back(static_cast<uint8_t>(words.size()-1));
    b.push_back(static_cast<uint8_t>(address>>8)); b.push_back(static_cast<uint8_t>(address));
    for (uint32_t word : words) { const size_t at=b.size(); b.resize(at+4); Put32(b.data()+at, word); }
}
void Store(uint8_t* p, const std::vector<uint8_t>& b, unsigned bytes) {
    Require(b.size() <= bytes, "fixture fits padded list");
    std::memset(p, 0, bytes);
    std::memcpy(p, b.data(), b.size());
    vgx::hle::NotifyGuestWrite(p, bytes);
}
const uint8_t* Resolve(uint32_t address, uint32_t* available) {
    *available = 0;
    if (address == 0x1000 || address == 0x1100 || address == 0x2000) {
        tinyEvents.push_back(0x100000000ull | address);
        *available = address == 0x2000 ? 64 : 32;
        return tinyRam + (address == 0x2000 ? 0xf0000 : address == 0x1000 ? 0xf1000 : 0xf1020);
    }
    return nullptr;
}
void Finish() { tinyEvents.push_back(0x200000000ull); }
void Token(uint16_t value, bool irq) { tinyEvents.push_back(0x300000000ull | uint32_t(irq)<<16 | value); }
void DisplayCopy() { tinyEvents.push_back(0x400000000ull); }

struct Snapshot {
    vgx::State state;
    std::vector<uint64_t> events, commands;
    uint32_t paletteChanges;
    size_t textureCopies, displayCopies;
    bool present;
};
Snapshot Capture(const vgx::State& initial, const uint8_t* list, unsigned bytes, bool cached, bool syncAfter = false) {
    // Prime a different palette so that every reference/candidate load has the same observable effect.
    vgx::WriteBP(0x64000088); vgx::WriteBP(0x65000400);
    std::memcpy(&vgx::g, &initial, sizeof initial);
    vgx::Pending().clear(); tinyEvents.clear();
    const uint32_t palette = vgx::g_tlutLoadGen;
    if (cached) {
        vgx::workload::Scope observation(vgx::workload::FrontSample);
        vgx::CallList(list, bytes);
    } else { vgx::SyncVertexArrays(); vgx::ProcessStream(list, bytes); }
    // (a fast hit leaves the vertex-array sync lazy, while the parser path here syncs first: synced afterwards, the two compare)
    if (syncAfter) vgx::SyncVertexArrays();
    Snapshot s;
    std::memcpy(&s.state, &vgx::g, sizeof s.state);
    s.events = tinyEvents;
    for (const vgx::Cmd& c : vgx::Pending().cmds) s.commands.push_back(uint64_t(c.type)<<32 | c.arg);
    s.paletteChanges = vgx::g_tlutLoadGen-palette;
    s.textureCopies=vgx::Pending().copyTex.size(); s.displayCopies=vgx::Pending().copyDisp.size();
    s.present=vgx::Pending().presentAllowed;
    return s;
}
void Same(const Snapshot& a, const Snapshot& b) {
    Require(std::memcmp(&a.state, &b.state, sizeof a.state)==0, "complete state/dirty/hash/mask equivalence");
    Require(a.events==b.events && a.commands==b.commands, "ordered hooks/commands equivalence");
    Require(a.paletteChanges==b.paletteChanges && a.textureCopies==b.textureCopies
            && a.displayCopies==b.displayCopies && a.present==b.present, "copy/TLUT/presentation effects equivalence");
}
void Compare(uint8_t* list, unsigned bytes, unsigned repeats=3, bool syncAfter=false) {
    for (unsigned i=0;i<repeats;++i) {
        vgx::State initial; vgx::InitState(initial);
        initial.bp[0x41]=i*17; initial.bpMask=i&1 ? 0xffu : 0xffffffu;
        Same(Capture(initial,list,bytes,false,syncAfter), Capture(initial,list,bytes,true,syncAfter));
    }
}
void Population(bool cached) {
    for (unsigned bytes : {32u, 64u}) {
        uint8_t* list = tinyRam+bytes*16;
        std::vector<uint8_t> b; BP(b, 0x41, 3); BP(b, 0xE0, 0x101);
        Store(list, b, bytes);
        uint64_t before[5], after[5]; vgx::StateListCounters(before);
        const unsigned raw = tinyRawCalls;
        vgx::State initial; vgx::InitState(initial);
        std::memcpy(&vgx::g, &initial, sizeof initial);
        vgx::SyncVertexArrays();
        vgx::ProcessStream(list, bytes);
        vgx::State expected; std::memcpy(&expected, &vgx::g, sizeof expected);
        for (unsigned i = 0; i < 3; ++i) {
            std::memcpy(&vgx::g, &initial, sizeof initial);
            vgx::CallList(list, bytes);
            Require(std::memcmp(&vgx::g, &expected, sizeof expected) == 0, "tiny replay matches real parser state");
        }
        vgx::StateListCounters(after);
        Require(after[4] == before[4], "no state replay verification mismatch");
        Require(cached ? after[1] == before[1]+1 && after[0] >= before[0]+2 : after[1] == before[1],
                "tiny CallList populates once then replays existing state cache");
        Require(tinyRawCalls-raw == (cached ? 0u : 3u), "raw fallback count matches eligibility");
    }
}
void Matrix() {
    uint8_t* list=tinyRam+0x1000;
    std::vector<uint8_t> b;
    BP(b,0xfe,0xff); BP(b,0x41,0xaabbcc); CP(b,0xac,0x2000); CP(b,0xbc,16);
    XF(b,0x100a,{0x11223344,0x55667788});
    b.insert(b.end(),{0x20,0,0,0x30,0}); // current array element0 -> four XF words
    BP(b,0xe0,0x001234); BP(b,0x80,0x000640);
    Store(list,b,64);
    for (unsigned i=0;i<8;++i) {
        const float values[]={float(i+1),2,3,4};
        std::memcpy(tinyRam+0xf0000,values,sizeof values);
        vgx::hle::NotifyGuestWrite(tinyRam+0xf0000,sizeof values);
        Compare(list,64);
    }
}
void Effects() {
    uint8_t* list=tinyRam+0x2000;
    std::vector<uint8_t> b;
    BP(b,0x41,3); BP(b,0x45,2); BP(b,0x47,0x1234); BP(b,0x48,0x1234);
    BP(b,0x52,1u<<14); BP(b,0x64,0x80); BP(b,0x65,1u<<10);
    BP(b,0xff,vgx::kPresentSuppressedMarker); BP(b,0xff,1); BP(b,0xff,2);
    Store(list,b,64);
    vgx::State initial; vgx::InitState(initial);
    for (unsigned i=0;i<4;++i) {
        const Snapshot expected=Capture(initial,list,64,false), got=Capture(initial,list,64,true);
        Same(expected,got);
        Require(got.events==std::vector<uint64_t>({0x200000000ull,0x300011234ull,0x300001234ull,
                                                 0x400000000ull,0x100001000ull}), "all effectful writes execute once in order");
        Require(got.paletteChanges==1 && got.displayCopies==1 && !got.present, "repeated same-value effects remain observable");
    }
}
void Invalidations() {
    uint8_t* list=tinyRam+0x3000;
    std::vector<uint8_t> b; BP(b,0x41,3); Store(list,b,32); Compare(list,32);
    // Unrelated write on the same page must reuse exact saved bytes rather than stale values or a forced rebuild.
    uint64_t before[5],after[5]; vgx::StateListCounters(before);
    ++list[1024]; vgx::hle::NotifyGuestWrite(list+1024,1); Compare(list,32);
    vgx::StateListCounters(after);
    Require(after[1]==before[1], "same-page noise keeps byte-identical state entry");
    b.clear(); BP(b,0x41,7); Store(list,b,32); Compare(list,32);
    b.clear(); BP(b,0x41,11); XF(b,0x100a,{0x456}); Store(list,b,64); Compare(list,64);
    b.clear(); BP(b,0x41,13); Store(list,b,32); Compare(list,32);
    for (unsigned i=0;i<12;++i) {
        uint8_t* at=list+(i&1 ? 256 : 0);
        b.clear(); BP(b,0x41,17+(i&1)); Store(at,b,32); Compare(at,32);
    }
}
void Fallbacks() {
    const std::vector<std::vector<uint8_t>> bodies={
        {0x00}, {0x08,0x30,0,0,0,1}, {0x90,0,0},
        {0x61,0x45,0,0,2,0x40,0,0,0,1,0,0,0,1},
        {0x61,0x45,0,0,2,0x7f}, {0x61,0x41,0,0,3,0x90,0,0}};
    unsigned offset=0x4000;
    for (const auto& b:bodies) {
        uint8_t* at=tinyRam+offset; offset+=256; Store(at,b,32);
        const unsigned raw=tinyRawCalls; Compare(at,32);
        Require(tinyRawCalls==raw+3, "unsupported or non-BP-first tiny list keeps raw parser");
    }
    std::vector<uint8_t> b; BP(b,0x41,5);
    for (unsigned bytes:{5u,15u,31u,63u}) {
        uint8_t* at=tinyRam+offset; offset+=256; Store(at,b,bytes);
        const unsigned raw=tinyRawCalls; Compare(at,bytes);
        Require(tinyRawCalls==raw+3, "non-selected tiny size keeps raw parser");
    }
    uint8_t* misaligned=tinyRam+offset+1; Store(misaligned,b,32);
    unsigned raw=tinyRawCalls; Compare(misaligned,32);
    Require(tinyRawCalls==raw+3,"misaligned selected-size list keeps raw parser");
    Store(tinyUntracked,b,32); raw=tinyRawCalls; Compare(tinyUntracked,32);
    Require(tinyRawCalls==raw+3,"untracked selected-size list keeps raw parser");
    uint8_t* large=tinyRam+0x5800; Store(large,b,65); raw=tinyRawCalls;
    // The existing >64 fast hit deliberately leaves unused vertex-array sync lazy.
    for(unsigned i=0;i<3;++i) { vgx::CallList(large,65); Require(vgx::g.bp[0x41]==5,"larger state replay retains value"); }
    Require(tinyRawCalls==raw,"existing greater-than64 state-cache admission remains active");
    // A cached address repurposed for unsupported bytes may not replay its old material state.
    uint8_t* reused=tinyRam+0x6000; Store(reused,b,32); Compare(reused,32);
    Store(reused,bodies[4],32); raw=tinyRawCalls; Compare(reused,32);
    Require(tinyRawCalls==raw+3,"same pointer changed to unsupported body falls back");
    // Truncated XF header or payload after BP: the real parser's partial-prefix behavior remains authoritative.
    for (unsigned start:{29u,25u}) {
        std::vector<uint8_t> truncated; BP(truncated,0x45,2); truncated.resize(start,0);
        truncated.push_back(0x10); while(truncated.size()<32)truncated.push_back(0);
        Store(reused+256,truncated,32); Compare(reused+256,32);
    }
}
void Volatile() {
    uint8_t* list=tinyRam+0x7000; std::vector<uint8_t> b;
    for(unsigned i=0;i<4;++i) { b.clear();BP(b,0x41,i+1);Store(list,b,32);Compare(list,32,1); }
    const unsigned raw=tinyRawCalls;
    uint64_t before[5],after[5]; vgx::StateListCounters(before);
    Compare(list,32,5); vgx::StateListCounters(after);
    Require(tinyRawCalls==raw+5 && before[1]==after[1],"three changed bodies retain existing volatile backoff");
    for(unsigned i=0;i<301;++i)vgx::FrontPresent(true,nullptr);
    Compare(list,32); vgx::StateListCounters(after);
    Require(after[1]>before[1] && after[0]>before[0],"expired volatile backoff rebuilds then reuses");
}
// (GXV_SLFAST) material lists above the tiny size whose front classes collide (in 1,024 classes and in 256 alike), and their
// rewrites with the same bytes: replayed at once, never through the raw parser, and leaving the parser's state.
void WideFront() {
    const char* env = std::getenv("GXV_SLFAST");
    const char* sl = std::getenv("GXV_STATELIST");
    const bool wide = !(env && env[0] == '0') && !(sl && std::strcmp(sl, "1") != 0); // (the fast hit is GXV_STATELIST=1's)
    std::vector<uint8_t*> lists;
    for (unsigned k = 0; k < 4; ++k) {
        uint8_t* at = tinyRam + 0xA0000 + k * 0x8000;
        std::vector<uint8_t> b;
        BP(b, 0x41, 0x20 + k); BP(b, 0x80, 0x000640 + k); XF(b, 0x100a, {0x100u + k, 0x200u + k}); BP(b, 0xe0, 0x001200 + k);
        CP(b, 0xac, 0x2000 + k * 32);
        Store(at, b, 96);
        lists.push_back(at);
    }
    uint64_t f0[3], f1[3];
    vgx::StateListFastCounters(f0);
    unsigned raw = tinyRawCalls;
    for (unsigned round = 0; round < 3; ++round)
        for (uint8_t* at : lists) Compare(at, 96, 1, true);
    vgx::StateListFastCounters(f1);
    Require(tinyRawCalls == raw, "colliding material lists never fall back to the raw parser");
    Require(wide ? f1[1] - f0[1] >= 8 : f1[1] == f0[1], "colliding lists are found through the table only with GXV_SLFAST");
    // The game writes the same bytes again (a write notification over each list).
    for (uint8_t* at : lists) vgx::hle::NotifyGuestWrite(at, 96);
    vgx::StateListFastCounters(f0);
    for (uint8_t* at : lists) Compare(at, 96, 1, true);
    vgx::StateListFastCounters(f1);
    Require(tinyRawCalls == raw, "same-bytes rewrites never fall back to the raw parser");
    Require(wide ? f1[2] - f0[2] == 4 : f1[2] == f0[2], "same-bytes rewrites are replayed by the fast hit only with GXV_SLFAST");
    // A real change: parsed again, the new values win.
    std::vector<uint8_t> b; BP(b, 0x41, 0x77); XF(b, 0x100a, {0x9u, 0xAu});
    Store(lists[0], b, 96);
    vgx::StateListFastCounters(f0);
    Compare(lists[0], 96, 3, true);
    vgx::StateListFastCounters(f1);
    Require(vgx::g.bp[0x41] == 0x77, "a changed list replays its new values");
    // The same address repurposed for a list with a draw: the raw parser, never the old replay.
    std::vector<uint8_t> d; BP(d, 0x41, 0x55); d.insert(d.end(), {0x90, 0, 0});
    Store(lists[1], d, 96);
    raw = tinyRawCalls;
    Compare(lists[1], 96, 2, true);
    Require(tinyRawCalls == raw + 2 && vgx::g.bp[0x41] == 0x55, "a list repurposed with a draw keeps the raw parser");
}
void CollisionLifetime() {
    std::vector<uint8_t> b;
    for(unsigned i=0;i<17000;++i) {
        uint8_t* list=tinyRam+0x10000+i*32;
        b.clear();BP(b,0x41,(i&255)+1);Store(list,b,32);
        vgx::CallList(list,32);
        Require(vgx::g.bp[0x41]==(i&255)+1,"cache-table growth retains each list's state");
    }
    for(unsigned offset:{0u,8192u,0u,8192u})Compare(tinyRam+0x10000+offset,32);
    Require(vgx::DlCacheLiveBytes()==0 && vgx::DlCacheStaticBytes()==0,"state admission creates no geometry buffers");
}
}

namespace vgx {
uint32_t TinyCountedProcessStream(const uint8_t* p, uint32_t n, bool inList) {
    ++tinyRawCalls;
    return ProcessStream(p, n, inList);
}
}

int main(int argc, char** argv) {
    Require(argc == 3, "usage: tiny-state population|all cached|raw");
    std::memset(tinyRam+0xf1000,0x37,32); std::memset(tinyRam+0xf1020,0xa5,32);
    vgx::hle::RegisterGuestRam(tinyRam, sizeof tinyRam);
    vgx::GuestHooks hooks{};hooks.resolve=Resolve;hooks.peFinish=Finish;hooks.peToken=Token;hooks.displayCopy=DisplayCopy;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    Require(std::string(argv[1]) == "population" || std::string(argv[1]) == "all", "known fixture mode");
    Population(std::string(argv[2]) == "cached");
    if(std::string(argv[1])=="all") { Matrix();Effects();Invalidations();Fallbacks();Volatile();WideFront();CollisionLifetime(); }
    std::printf("PASS: tiny-state %s %s checks=%u raw=%u\n", argv[1], argv[2], tinyChecks, tinyRawCalls);
}
