// Exercise actual front-end diagnostics, files and command buffers, not a model.
#include "../platform/vgx/src/vgx_draw.cpp"
#include "../platform/vgx/src/vgx_stats.cpp"
#include <new>
#include <string>
#include <unistd.h>

namespace {
constexpr bool diagnostic = TEST_DIAGNOSTICS != 0;
#if !TEST_DIAGNOSTICS
static_assert(!vgx::diag::kEnabled && vgx::diag::Env("unused") == nullptr && vgx::diag::Int("unused",9) == 9,
              "quiet selectors are constant expressions, with no initialization guard");
#endif
bool countAllocations = false;
unsigned allocations = 0, diagnosticReads = 0, failures = 0, pictures = 0;
uint64_t geometry = 14695981039346656037ull;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}
void Mix(const void* p, size_t size) {
    for (const auto* b = static_cast<const uint8_t*>(p); size; --size, ++b)
        geometry = (geometry ^ *b) * 1099511628211ull;
}
void Observe(vgx::CmdBuf& b) {
    ++pictures;
    Require(b.draws.size() == 1 && b.draws[0].idxCount == 6 && b.draws[0].vtxCount == 6,
            "two triangles still merge into one complete draw");
    Require(b.vtx.size() == 96 && b.idx.size() == 6, "all six vertex/index records reach the sink");
    const uint16_t expected[] = {0,1,2,3,4,5};
    Require(std::memcmp(b.idx.data(), expected, sizeof expected) == 0, "independent triangle indices");
    Mix(b.vtx.data(), b.vtx.size()); Mix(b.idx.data(), b.idx.size() * 2);
    Mix(b.uni.data(), b.uni.size() * 4);
    Mix(b.vtxChunks.data(), b.vtxChunks.size() * 4);
    for (const auto& c : b.cmds) {
        const uint32_t fields[] = {c.type,c.arg}; Mix(fields,sizeof fields);
    }
    for (const auto* shader : b.shaders) {
        Mix(shader->text.data(),shader->text.size()); Mix(&shader->fragment,sizeof shader->fragment);
    }
    const auto& d = b.draws[0];
    const uint32_t state[] = {d.layoutMask,d.prim,d.vsId,d.fsId,d.texMask,d.idxCount,d.vtxCount};
    Mix(state, sizeof state); Mix(d.ublock, sizeof d.ublock);
    const uint32_t pipeline[] = {d.rs.blend,d.rs.blendSrc,d.rs.blendDst,d.rs.blendEq,d.rs.depthTest,d.rs.depthFunc,
                                d.rs.depthWrite,d.rs.cull,d.rs.colorWrite,d.rs.alphaWrite,d.rs.dstAlphaPass,d.rs.lineWidth6};
    const int16_t scissor[] = {d.rs.scX,d.rs.scY,d.rs.scW,d.rs.scH};
    const float viewport[] = {d.rs.vpX,d.rs.vpY,d.rs.vpW,d.rs.vpH,d.rs.zNear,d.rs.zFar};
    Mix(pipeline,sizeof pipeline); Mix(scissor,sizeof scissor); Mix(viewport,sizeof viewport);
    Require(b.uploads.empty() && b.copyTex.empty() && b.copyDisp.empty() && b.targets.empty() &&
            b.bufUploads.empty() && b.passes.empty() && b.variants.empty() && b.warm.empty(),
            "diagnostic selectors insert no extra rendering or upload commands");
    vgx::backend::Execute(b);
}
void Stats() {
    vgx::stats::SetDetailed(true);
    Require(vgx::stats::Detailed() == diagnostic, "quiet SetDetailed cannot enable expensive per-draw statistics");
    // Even an old caller writing the public state cannot bypass the build contract.
    vgx::stats::g_detailed = true;
    Require(vgx::stats::Detailed() == diagnostic, "quiet Detailed rejects stale direct state");
    vgx::FsKey fs{}; vgx::VsKey vs{}; vgx::VtxAttrFmt vat{};
    countAllocations = true;
    for (unsigned i = 0; i < 128; ++i) {
        vgx::stats::TexFormat(i % 16, 64, 32, true);
        vgx::stats::CopyFormat(i % 16, (i & 1) != 0);
        vgx::stats::Note("a synthetic diagnostic note longer than a small-string buffer");
        vgx::stats::VertexFormat(9, 1, vat);
        vgx::stats::Draw(fs, vs, i % 16, 3);
        vgx::stats::ShaderCreated((i & 1) != 0, i);
        vgx::stats::TexBound(i, 1024);
        vgx::stats::ProgramUsed(i, i + 1);
        vgx::stats::TexLive(1, 1024);
    }
    countAllocations = false;
    Require(diagnostic ? allocations > 0 : allocations == 0, "diagnostic-only bookkeeping allocates nothing in quiet builds");
    Require(vgx::stats::g_created.size() == (diagnostic ? 128u : 0u), "quiet shader timeline does not grow");
    Require(vgx::stats::g_texFmt.empty() == !diagnostic && vgx::stats::g_notes.empty() == !diagnostic,
            "quiet texture and note census does not collect unused data");
    Require(vgx::stats::Cur().shadersVs == 64 && vgx::stats::Cur().shadersFs == 64,
            "scalar shader counters survive diagnostics removal");
    vgx::stats::Cur().prims = 37; vgx::stats::Cur().vertsDL = 91;
    vgx::stats::FrameEnd(256);
    Require(vgx::stats::Cur().prims == 0 && vgx::stats::Cur().vertsDL == 0 &&
            vgx::stats::Cur().shadersVs == 0 && vgx::stats::Cur().shadersFs == 0,
            "every frame resets counters, including quiet frames");
    Require(vgx::stats::g_frameTex.empty() && vgx::stats::g_framePrograms.empty(), "per-frame distinct sets still reset");
}
void Files(const char* csv, const char* report, bool ram = false) {
    if (ram) setenv("GXV_STATS_RAM", "1", 1);
    setenv("GXV_FRAMESTATS", csv, 1);
    for (unsigned f = 1; f <= 600; ++f) {
        vgx::stats::Cur().prims = 3;
        vgx::stats::FrameEnd(f);
    }
    if (ram) Require(access(csv, F_OK) != 0, "RAM diagnostic frames perform no file I/O during play");
    vgx::stats::CloseFrameLog();
    vgx::stats::Dump(report);
    Require((access(csv, F_OK) == 0) == diagnostic, "quiet frames never open or write diagnostic CSV");
    Require((access(report, F_OK) == 0) == diagnostic, "quiet report never opens or writes an ordinary file");
    if (diagnostic) {
        FILE* f = std::fopen(csv, "r"); char line[2048]{};
        Require(f && std::fgets(line, sizeof line, f), "CSV header exists in opt-in/default diagnostic builds");
        unsigned columns = 1; for (const char* c = line; *c; ++c) columns += *c == ',';
        Require(columns == 54 && std::strncmp(line, "frame,prims,primsDL,verts,tris,drawCalls,", 38) == 0,
                "diagnostic CSV keeps its existing 54-column format");
        unsigned rows = 0;
        while (f && std::fgets(line, sizeof line, f)) { ++rows; Require(std::strstr(line, ",3,0,0,") != nullptr, "CSV contains each frame's scalar counters"); }
        Require(rows == 600, "all requested diagnostic frames are written");
        if (f) std::fclose(f);
    }
}
void PassCheck() {
    setenv("GXV_PASSCHECK", "1", 1);
    vgx::CopyDispCmd copy; copy.src = {0,0,16,16}; copy.clear.enable = true;
    vgx::PassCmd pass; pass.rect = copy.src;
    std::memcpy(pass.rgba, copy.clear.rgba, sizeof pass.rgba);
    vgx::g_buf.copyDisp.push_back(copy); vgx::g_buf.passes.push_back(pass);
    vgx::g_buf.cmds.push_back({vgx::CMD_COPY_DISP,0});
    vgx::g_buf.cmds.push_back({vgx::CMD_PASS_BEGIN,0});
    vgx::VerifyPasses();
    Require(vgx::g_chk.px[0].empty() == !diagnostic, "quiet pass verifier allocates no per-pixel planes");
    Require(vgx::g_chk.checks == (diagnostic ? 1u : 0u) && vgx::g_chk.failures == 0 &&
            vgx::g_chk.pixelsChecked == (diagnostic ? 768u : 0u), "only diagnostic builds execute pixel-plane validation");
}
void Repeat() {
    setenv("GXV_PASSREPEAT", "1", 1);
    vgx::PassCmd pass; pass.rect = {0,0,16,16};
    vgx::CopyTexCmd copy; copy.texId = 7;
    vgx::g_buf.passes.push_back(pass); vgx::g_buf.copyTex.push_back(copy);
    vgx::g_buf.cmds.push_back({vgx::CMD_PASS_BEGIN,0});
    vgx::g_buf.cmds.push_back({vgx::CMD_COPY_TEX,0});
    vgx::MeasurePassRepeats(); vgx::MeasurePassRepeats();
    Require(vgx::g_passRepeat.passes == (diagnostic ? 2u : 0u) &&
            vgx::g_passRepeat.sameAsLastFrame == (diagnostic ? 1u : 0u), "quiet pass-repeat census performs no hashing or history growth");
}
void Light() {
    vgx::VsKey vs{}; vs.numChans = 1; vs.chan[0].enable = 1; vs.chan[0].lightMask = 1;
    vgx::g.xfLight[0][13] = vgx::f2u(42.0f);
    vgx::BuildLight(vs);
    const int mode = std::atoi(std::getenv("GXV_LIGHTNORM"));
    Require(vgx::g_lnc.blocks == (diagnostic && mode == 2 ? 1u : 0u), "only requested diagnostic light census runs");
    const bool normalise = mode != 0;
    Require(vgx::g_buf.uni.size() >= 24 && vgx::g_buf.uni[20] == (normalise ? 0.0f : 42.0f),
            "functional light normalization selector remains effective in every build");
}
void Front(const char* csv, bool stale, bool full) {
    if (stale) {
        setenv("GXV_FRAMESTATS", csv, 1);
        setenv("GXV_HASH", "2", 1); setenv("GXV_PASSES", "1,2", 1);
        setenv("GXV_PASSLOG", "1", 1); setenv("GXV_TRACE_DRAWS", "1", 1);
        setenv("GXV_TRACE", "1", 1); setenv("GXV_DLTRACE", "1", 1);
        setenv("GXV_RESOLVEFAST", "2", 1); setenv("GXV_MERGEFAST", "2", 1);
        vgx::stats::SetDetailed(true);
    } else {
        setenv("GXV_LIGHTNORM","1",1);
        setenv("GXV_RESOLVEFAST",full ? "0" : "1",1);
        setenv("GXV_MERGEFAST",full ? "0" : "1",1);
    }
    vgx::SetFrameSink(Observe);
    const float triangle[3][4] = {{0,0,0,0},{1,0,0,0},{0,1,0,0}};
    for (unsigned f = 0; f < 3; ++f) {
        const uint8_t noop[] = {0};
        Require(vgx::ProcessStream(noop, sizeof noop, false) == sizeof noop, "actual stream parser still consumes commands");
        for (unsigned i = 0; i < 2; ++i) vgx::DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(triangle), 3);
        vgx::FrontPresent(true, nullptr);
    }
    vgx::stats::CloseFrameLog();
    Require(pictures == 3, "every real frame is submitted");
    Require(diagnostic && stale ? vgx::g_traceFrame != ~0ull : vgx::g_traceFrame == ~0ull, "only requested diagnostic draw tracing schedules a frame");
    if (diagnostic && stale) {
        FILE* f = std::fopen(csv, "r"); char line[2048]{};
        Require(f && std::fgets(line,sizeof line,f) && std::fgets(line,sizeof line,f), "frame CSV contains actual hashed frame");
        const char* last = std::strrchr(line, ',');
        Require(last && std::strtoull(last + 1, nullptr, 16) != 0, "diagnostic frame hashing still executes");
        if (f) std::fclose(f);
    } else Require(access(csv,F_OK) != 0, "quiet frame submission creates no diagnostic file");
    std::printf("geometry=%016llx\n", static_cast<unsigned long long>(geometry));
}
void Decode(bool specialised) {
    setenv("GXV_DECODE",specialised ? "2" : "0",1);
    vgx::SetVcd(9,1); vgx::SetVat(0,9,1,4,0);
    const vgx::Plan& plan = vgx::CurrentPlan(0);
    const uint8_t input[] = {0x3f,0x80,0,0, 0x40,0,0,0, 0x40,0x40,0,0};
    alignas(16) uint8_t out[64]{};
    countAllocations = true;
    vgx::DecodeVertices(plan,input,1,out,nullptr);
    countAllocations = false;
    const float expected[4] = {1,2,3,0};
    Require(plan.layout.stride == sizeof expected && std::memcmp(out,expected,sizeof expected) == 0,
            "specialised decoder retains hand-derived vertex bytes");
    Require(diagnostic && specialised ? allocations >= 2 : allocations == 0,
            "quiet mixed decode mode does not allocate or run differential decoders");
}
void Fold(bool enabled) {
    setenv("GXV_SOFTXFORM","128",1); setenv("GXV_FOLDAHEAD",enabled ? "2" : "0",1);
    vgx::cur.valid = true; vgx::cur.foldOk = true; vgx::cur.vcdGen = vgx::g.vcdGen;
    vgx::g.bpKeyDirty = vgx::g.xfKeyDirty = vgx::g.xfRegDirty = false;
    const uint32_t mask = vgx::FoldGuess(0,3);
    Require(mask == (enabled ? (vgx::VL_COL0 | vgx::VL_COL1) : 0),
            "functional fold-ahead selector remains effective in quiet builds");
}
void Soft(bool neon) {
    setenv("GXV_SOFTNEON",neon ? "2" : "0",1);
    vgx::g.matIdxA = 0;
    vgx::g.xfPos[0][3] = 4; vgx::g.xfPos[1][3] = 5; vgx::g.xfPos[2][3] = 6;
    alignas(16) float vertex[4] = {1,2,3,7};
    countAllocations = true;
    vgx::SoftTransform(reinterpret_cast<uint8_t*>(vertex),1,vgx::MakeLayout(0));
    countAllocations = false;
    const float expected[4] = {5,7,9,7};
    Require(std::memcmp(vertex,expected,sizeof expected) == 0, "NEON/scalar modes preserve hand-derived transformed vertex");
    Require(diagnostic && neon ? allocations > 0 : allocations == 0,
            "quiet NEON mode does not allocate a scalar-comparison copy");
}
void MixedDraw(bool folded, bool check) {
    setenv("GXV_RESOLVEFAST",check ? "2" : "1",1); setenv("GXV_MERGEFAST",check ? "2" : "1",1);
    setenv("GXV_DECODE","1",1); setenv("GXV_SOFTNEON","1",1);
    setenv("GXV_SOFTXFORM",folded ? "128" : "0",1); setenv("GXV_FOLDAHEAD",check ? "2" : "1",1);
    vgx::g_buf.vtx.reserve(4096); vgx::g_buf.idx.reserve(1024);
    vgx::g_buf.uni.reserve(4096); vgx::g_buf.draws.reserve(16); vgx::g_buf.cmds.reserve(64);
    vgx::SetVcd(9,1); vgx::SetVat(0,9,1,4,0);
    if (folded) vgx::WriteBP(0xC0000002u); // TEV reads a foldable colour register.
    const uint8_t input[36] = {0x3f,0x80,0,0, 0x40,0,0,0, 0x40,0x40,0,0};
    vgx::DrawStream(0x90,0,input,3,sizeof input);
    if (folded) Require(vgx::FoldGuess(0,3) == (vgx::VL_COL0 | vgx::VL_COL1), "stream fixture reaches genuine fold-ahead decode");
    countAllocations = true;
    vgx::DrawStream(0x90,0,input,3,sizeof input);
    countAllocations = false;
    Require(vgx::g_buf.draws.size() == 1 && vgx::g_buf.draws[0].idxCount == 6,
            "mixed diagnostic selectors preserve merging and both triangles");
}
}

void* operator new(size_t n) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void* operator new(size_t n, std::align_val_t alignment) {
    if (countAllocations) ++allocations;
    void* p = nullptr;
    if (posix_memalign(&p,static_cast<size_t>(alignment),n ? n : 1) != 0) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n, std::align_val_t a) { return ::operator new(n,a); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return ::operator new(n,std::nothrow); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n,a); } catch (...) { return nullptr; } }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return ::operator new(n,a,std::nothrow); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
static void HeapCounter() {
    const auto a = std::align_val_t(64);
    countAllocations = true;
    void* p[] = {::operator new(8),::operator new[](8),::operator new(8,std::nothrow),::operator new[](8,std::nothrow),
                 ::operator new(64,a),::operator new[](64,a),::operator new(64,a,std::nothrow),::operator new[](64,a,std::nothrow)};
    countAllocations = false;
    Require(allocations == 8, "allocation observer counts every ordinary/aligned/nothrow overload once");
    for (unsigned i = 0; i < 8; ++i) Require(p[i] && (i < 4 || (reinterpret_cast<uintptr_t>(p[i]) & 63u) == 0),
                                              "allocation observer preserves over-alignment");
    ::operator delete(p[0]); ::operator delete[](p[1]);
    ::operator delete(p[2],std::nothrow); ::operator delete[](p[3],std::nothrow);
    ::operator delete(p[4],a); ::operator delete[](p[5],a);
    ::operator delete(p[6],a,std::nothrow); ::operator delete[](p[7],a,std::nothrow);
}
extern "C" char* __real_getenv(const char*);
extern "C" char* __wrap_getenv(const char* name) {
    for (const char* key : {"GXV_FRAMESTATS","GXV_HASH","GXV_PASSCHECK","GXV_PASSREPEAT","GXV_PASSLOG",
                           "GXV_TRACE","GXV_TRACE_DRAWS","GXV_PASSES","GXV_DLTRACE","GXV_STATS"})
        if (std::strcmp(name,key) == 0) ++diagnosticReads;
    return __real_getenv(name);
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
int main(int argc, char** argv) {
    Require(argc == 2, "supply one scenario"); if (argc != 2) return 1;
    const std::string mode = argv[1];
    const char* csv = "/tmp/frontend-quiet.csv"; const char* report = "/tmp/frontend-quiet.txt";
    std::remove(csv); std::remove(report);
    setenv("GXV_SOFTXFORM","0",1); setenv("GXV_ATLAS","0",1);
    setenv("GXV_LIGHTNORM","2",1); vgx::SetShaderListFile(nullptr); vgx::ResetState();
    if (mode == "stats") Stats();
    else if (mode == "files" || mode == "files-ram") Files(csv,report,mode == "files-ram");
    else if (mode == "passcheck") PassCheck();
    else if (mode == "repeat") Repeat();
    else if (mode == "light" || mode == "light-off") {
        if (mode == "light-off") setenv("GXV_LIGHTNORM","0",1);
        Light();
    } else if (mode == "trace") {
        setenv("GXV_TRACE","1",1);
        Require(vgx::TraceFrame() == diagnostic, "quiet trace selector cannot enable per-draw formatting");
    } else if (mode == "front" || mode == "front-baseline" || mode == "front-full") Front(csv,mode == "front",mode == "front-full");
    else if (mode == "decode" || mode == "decode-plain") Decode(mode == "decode");
    else if (mode == "fold" || mode == "fold-off") Fold(mode == "fold");
    else if (mode == "soft" || mode == "soft-scalar") Soft(mode == "soft");
    else if (mode == "resolve" || mode == "fold-stream" || mode == "resolve-base" || mode == "fold-stream-base")
        MixedDraw(mode == "fold-stream" || mode == "fold-stream-base", mode == "resolve" || mode == "fold-stream");
    else if (mode == "heap-counter") HeapCounter();
    else { Require(false, "unknown scenario"); }
    Require(diagnostic || diagnosticReads == 0, "quiet build does not even read pure diagnostic environment selectors");
    std::printf("mode=%s diagnostics=%u allocations=%u diagnostic-reads=%u failures=%u\n",
                mode.c_str(), diagnostic, allocations, diagnosticReads, failures);
    return failures ? 1 : 0;
}
