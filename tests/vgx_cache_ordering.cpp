#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

void CacheNotifyBoundary(int stage);

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
alignas(4096) uint8_t ram[16384] = {};
std::mutex gate;
std::condition_variable cv;
std::thread::id writerId;
bool startWriter = false, parked = false, releaseWriter = false;
bool armDecode = false, disjoint = false, completedNoise = false;
bool samePage = false, editList = false, textureAba = false, overflow = false, mixedFormats = false;
unsigned buildStage = 0;
int boundary = 2;
float observed[3]{};
std::vector<uint8_t> indexedGeometry[3];
unsigned pictures = 0;
unsigned planLookups = 0;
unsigned stageHits[5] = {};
const vgx::Plan* retainedPlans[8] = {};
uint16_t retainedStrides[8] = {};

void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}
void Observe(vgx::CmdBuf& b) {
    Require(pictures < 3 && !b.draws.empty(), "three pictures contain geometry");
    const auto& d = b.draws.front();
    Require(d.vbuf == 0, "fixture disables static promotion");
    const size_t base = (b.vtxChunks.empty() ? 0 : b.vtxChunks.at(d.vchunk)) + d.vtxOffset;
    Require(base + 4 <= b.vtx.size(), "decoded first vertex is in bounds");
    std::memcpy(&observed[pictures], b.vtx.data() + base, 4);
    for (const auto& c : b.cmds) {
        if (c.type != vgx::CMD_DRAW) continue;
        const auto& draw = b.draws.at(c.arg);
        Require(draw.vbuf == 0, "canonical fixture stays in the vertex arena");
        const size_t chunk = b.vtxChunks.empty() ? 0 : b.vtxChunks.at(draw.vchunk);
        const size_t stride = vgx::MakeLayout(draw.layoutMask).stride;
        Require(size_t(draw.idxOffset) + draw.idxCount <= b.idx.size(), "canonical index offsets remain in bounds");
        for (unsigned i = 0; i < draw.idxCount; ++i) {
            const size_t offset = chunk + draw.vtxOffset + size_t(b.idx[draw.idxOffset + i]) * stride;
            Require(offset + stride <= b.vtx.size(), "every canonical indexed vertex is in bounds");
            indexedGeometry[pictures].insert(indexedGeometry[pictures].end(), b.vtx.data() + offset, b.vtx.data() + offset + stride);
        }
    }
    Require(!indexedGeometry[pictures].empty(), "canonical picture has indexed vertices");
    ++pictures;
    vgx::backend::Execute(b);
}
void Start() {
    std::unique_lock<std::mutex> lock(gate);
    startWriter = true;
    cv.notify_all();
    cv.wait(lock, [] { return parked; });
}
void Finish(std::thread& writer) {
    { std::lock_guard<std::mutex> lock(gate); releaseWriter = true; cv.notify_all(); }
    writer.join();
}
std::thread Writer(bool texture, bool edit = true) {
    return std::thread([texture, edit] {
        {
            std::unique_lock<std::mutex> lock(gate);
            writerId = std::this_thread::get_id();
            cv.wait(lock, [] { return startWriter; });
        }
        uint8_t* const p = ram + (editList ? 0 : samePage ? 4096 + 3904 : disjoint ? 8192 : 4096);
        if (edit) {
            if (texture) std::memset(p, 2, 64);
            else if (editList) ram[5] = 1;
            else Put32(p, vgx::f2u(9.0f));
        }
        // Also exercise a source edit whose notification has not started yet.
        CacheNotifyBoundary(0);
        vgx::hle::NotifyGuestWrite(p, texture ? 64 : editList ? 6 : 4);
        { std::lock_guard<std::mutex> lock(gate); parked = true; cv.notify_all(); }
    });
}
void Geometry() {
    vgx::SetVcd(0, 1); vgx::SetVcd(9, 3);
    for (unsigned f = 0; f < (mixedFormats ? 8u : 1u); ++f) vgx::SetVat(f, 9, 1, 4, 0);
    vgx::g.arrays[9] = {ram + 4096, 320 * 12, 12, false};
    for (unsigned i = 0; i < 320; ++i) {
        Put32(ram + 4096 + i * 12, vgx::f2u(1.0f + float(i) / 16));
        Put32(ram + 4100 + i * 12, vgx::f2u(2.0f + float(i & 1)));
        Put32(ram + 4104 + i * 12, vgx::f2u(3.0f));
    }
    unsigned n = 0;
    for (unsigned s = 0; s < 80; ++s) {
        ram[n++] = static_cast<uint8_t>(0x98 | (mixedFormats ? s & 7 : 0)); ram[n++] = 0; ram[n++] = 4;
        for (unsigned v = 0; v < 4; ++v) {
            const unsigned i = s * 4 + v;
            ram[n++] = 0; ram[n++] = static_cast<uint8_t>(i >> 8); ram[n++] = static_cast<uint8_t>(i);
        }
    }
    vgx::SetFrameSink(Observe);
    armDecode = true;
    auto writer = Writer(false);
    vgx::CallList(ram, n);
    Finish(writer);
    Require(planLookups == (mixedFormats ? 8u : 1u), "pure-list build resolves each immutable vertex plan only once");
    Require(!buildStage || stageHits[buildStage] == 1, "selected in-build edit boundary must actually be reached once");
    vgx::FrontPresent(true, nullptr);
    vgx::CallList(ram, n);
    if (disjoint) Require(vgx::stats::Cur().dlBuild == 0 &&
                          (samePage ? vgx::stats::Cur().dlRehash == 1 : vgx::stats::Cur().dlHit == 1),
                          "unrelated concurrent flush preserves decoded cache reuse, including same-page rehash");
    vgx::FrontPresent(true, nullptr);
    vgx::ProcessStream(ram, n, true);
    vgx::FrontPresent(true, nullptr);
    const float edited = editList ? 1.0625f : disjoint ? 1 : 9;
    Require(pictures == 3 && observed[0] == (buildStage || overflow ? edited : 1) && observed[2] == edited,
            "cached and raw pictures bracket the synchronized source edit");
    Require(observed[1] == observed[2], "reused geometry must not retain the pre-write decode");
    Require(indexedGeometry[1] == indexedGeometry[2], "all cached vertex bytes and index addressing match raw decoding");
    std::printf("geometry: first=%g reused=%g raw=%g\n", observed[0], observed[1], observed[2]);
}
void Texture() {
    std::memset(ram + 4096, 1, 64);
    auto& slot = vgx::g.tex[0]; slot.valid = true; slot.avail = 64;
    slot.obj.data = ram + 4096; slot.obj.width = slot.obj.height = 8; slot.obj.format = 1;
    slot.obj.minFilter = vgx::FILT_LINEAR; slot.obj.magFilter = 1;
    vgx::TexBind bind; uint16_t w = 0, h = 0;
    Require(vgx::ResolveTexture(0, bind, w, h), "first texture upload");
    Require(vgx::ResolveTexture(0, bind, w, h) && vgx::Pending().uploads.size() == 1,
            "texture contents validated and memoized");
    if (textureAba) {
        Require(vgx::Pending().uploads[0].levels[0][0] == 2, "initial upload uses the notified intermediate pixels");
        std::memset(ram + 4096, 1, 64);
        vgx::hle::NotifyGuestWrite(ram + 4096, 64);
        Require(vgx::ResolveTexture(0, bind, w, h), "resolve after returning to original pixels");
        const auto& uploads = vgx::Pending().uploads;
        Require(uploads.size() == 2 && uploads.back().levels[0][0] == 1,
                "first upload must not pair a newer generation with an older hash");
        std::printf("texture: notified first-upload edit and later restoration refresh correctly\n");
        return;
    }
    auto writer = Writer(true); Start();
    Require(vgx::ResolveTexture(0, bind, w, h), "texture resolve during unfinished notification");
    Finish(writer);
    Require(vgx::ResolveTexture(0, bind, w, h), "texture resolve after notification");
    const auto& uploads = vgx::Pending().uploads;
    Require(uploads.size() == 2 && uploads.back().levels[0][0] == 2,
            "completion epoch must invalidate memoized texture contents");
    std::printf("texture: uploads=%zu final-pixel=%u\n", uploads.size(), unsigned(uploads.back().levels[0][0]));
}
void Ring(bool lapped) {
    const uint64_t first = vgx::GuestWriteEpoch();
    boundary = lapped ? 4 : 3;
    auto writer = Writer(false, false); Start();
    for (unsigned i = 0; i < 2048; ++i) vgx::hle::NotifyGuestWrite(ram + 8192, 4);
    const vgx::HostRange oldRange{ram + 4096, 4};
    const bool rejected = vgx::GuestWritesTouch(first + 2048, first + 2049, &oldRange, 1);
    Finish(writer);
    Require(rejected != lapped, "busy slot rejects; unclaimed lapped slot is precise before the older writer resumes");
    Require(vgx::GuestWritesTouch(first + 2048, first + 2049, &oldRange, 1),
            "a skipped or overwritten newer record must reject after the older writer finishes");
    const uint64_t next = vgx::GuestWriteEpoch();
    Require(next == first + 2049, "every overlapping writer publishes completion");
    vgx::hle::NotifyGuestWrite(ram + 8192, 4);
    const vgx::HostRange newRange{ram + 8192, 4};
    Require(!vgx::GuestWritesTouch(next, next + 1, &oldRange, 1), "fresh disjoint ring record stays precise");
    Require(vgx::GuestWritesTouch(next, next + 1, &newRange, 1), "fresh matching ring record invalidates");
    Require(vgx::GuestWritesTouch(first, next + 1, &oldRange, 1), "lost ring history invalidates conservatively");
    std::printf("ring: busy reuse rejected, fresh range precision preserved\n");
}
void Stress() {
    constexpr unsigned writers = 3, turns = 12000;
    std::atomic<bool> go{false};
    std::atomic<unsigned> done{0};
    std::thread threads[writers];
    const uint64_t first = vgx::GuestWriteEpoch();
    const uint64_t gen = vgx::GuestWriteGeneration(ram, sizeof ram);
    for (unsigned i = 0; i < writers; ++i) threads[i] = std::thread([&, i] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned j = 0; j < turns; ++j) vgx::hle::NotifyGuestWrite(ram + 4096 * i, 4);
        done.fetch_add(1, std::memory_order_release);
    });
    go.store(true, std::memory_order_release);
    const vgx::HostRange all{ram, sizeof ram};
    unsigned checks = 0;
    while (done.load(std::memory_order_acquire) != writers) {
        const uint64_t end = vgx::GuestWriteEpoch();
        if (end > first) {
            Require(vgx::GuestWritesTouch(end - 1, end, &all, 1), "concurrent record cannot falsely exclude whole RAM");
            ++checks;
        }
    }
    for (auto& t : threads) t.join();
    Require(vgx::GuestWriteEpoch() == first + writers * turns, "concurrent completion count");
    Require(vgx::GuestWriteGeneration(ram, sizeof ram) == gen + writers * turns, "concurrent page generations");
    std::printf("stress: %u notifications, %u range checks\n", writers * turns, checks);
}
void Spans() {
    constexpr unsigned indices[] = {0, 2, 1, 255, 65535};
    constexpr unsigned colorBytes[] = {2, 3, 4, 2, 3, 4};
    unsigned checks = 0;
    for (unsigned attr : {9u, 10u, 11u, 12u, 13u, 20u})
    for (unsigned mode : {2u, 3u})
    for (unsigned type = 0; type < ((attr == 11 || attr == 12) ? 6u : 5u); ++type)
    for (unsigned cnt = 0; cnt < (attr == 10 ? 3u : 2u); ++cnt)
    for (unsigned stride : {0u, 1u, 16u, 65536u}) {
        vgx::ResetState(); vgx::g.cpArrayStale = 0;
        vgx::SetVcd(0, 1); vgx::SetVcd(attr, mode); vgx::SetVat(0, attr, cnt, type, 0);
        const auto& plan = vgx::CurrentPlan(0);
        const vgx::AttrPlan* a = nullptr;
        for (int i = 0; i < plan.n; ++i) if (plan.a[i].attr == attr) a = &plan.a[i];
        Require(a != nullptr, "indexed attribute exists in plan");
        std::vector<uint8_t> source(plan.streamSize * 5, 0);
        for (unsigned i = 0; i < 5; ++i) {
            uint8_t* s = source.data() + i * plan.streamSize + a->srcOff;
            if (mode == 2) s[0] = static_cast<uint8_t>(indices[i]);
            else { s[0] = static_cast<uint8_t>(indices[i] >> 8); s[1] = static_cast<uint8_t>(indices[i]); }
        }
        const unsigned component = type == 4 ? 4 : (type >= 2 ? 2 : 1);
        const unsigned elem = (attr == 11 || attr == 12) ? colorBytes[type] :
                              component * (attr == 9 ? (cnt ? 3 : 2) : attr == 10 ? (cnt == 1 ? 9 : 3) : (cnt ? 2 : 1));
        for (unsigned size : {0u, elem - 1, elem, elem + 2 * stride, 0xFFFFFFFFu})
        for (bool present : {false, true}) {
            vgx::g.arrays[attr] = {present ? ram : nullptr, size, stride, false};
            const auto spans = vgx::MeasureArraySpans(plan, source.data(), 5);
            uint32_t low = 0xFFFFFFFFu, high = 0;
            bool used = false;
            for (unsigned index : indices) {
                if (mode == 2) index &= 255;
                const uint64_t offset = uint64_t(index) * stride;
                if (!present || offset + elem > size) continue;
                used = true;
                if (offset < low) low = static_cast<uint32_t>(offset);
                if (offset + elem > high) high = static_cast<uint32_t>(offset + elem);
            }
            Require(spans.mask == (used ? 1u << attr : 0), "range scan selects exactly the readable indexed attribute");
            if (used) Require(spans.lo[attr - 9] == low && spans.hi[attr - 9] == high,
                              "range bounds match independent offsets, including NBT3 and wide multiplication");
            Require(vgx::MeasureArraySpans(plan, source.data(), 0).mask == 0, "empty vertex range reads no arrays");
            ++checks;
        }
    }
    for (unsigned fmt = 0; fmt < 8; ++fmt) {
        vgx::ResetState(); vgx::g.cpArrayStale = 0;
        vgx::SetVcd(0, 1);
        for (unsigned a = 1; a <= 8; ++a) vgx::SetVcd(a, 1);
        for (unsigned a = 9; a <= 20; ++a) {
            vgx::SetVcd(a, 1 + a % 3);
            vgx::SetVat(fmt, a, a == 10 ? fmt % 3 : fmt % 2, a == 11 || a == 12 ? 5 : 4, 0);
            vgx::g.arrays[a] = {ram + 1024 + (a - 9) * 256, 192, 64, false};
        }
        const auto& plan = vgx::CurrentPlan(fmt);
        std::vector<uint8_t> source(plan.streamSize * 3, 0);
        vgx::ArraySpans expected;
        unsigned offset = 0;
        for (int i = 0; i < plan.n; ++i) {
            const auto& a = plan.a[i];
            Require(a.srcOff == offset, "plan source offsets match independent cumulative attribute sizes");
            if (a.attr >= 9 && a.mode != 1) {
                for (unsigned v = 0; v < 3; ++v) {
                    const unsigned index = v == 0 ? 0 : v == 1 ? 2 : 1;
                    uint8_t* s = source.data() + v * plan.streamSize + offset;
                    if (a.mode == 2) s[0] = static_cast<uint8_t>(index);
                    else s[1] = static_cast<uint8_t>(index);
                }
                const unsigned elem = a.attr == 11 || a.attr == 12 ? 4 :
                    4 * (a.attr == 9 ? (fmt % 2 ? 3 : 2) : a.attr == 10 ? (fmt % 3 == 1 ? 9 : 3) : (fmt % 2 ? 2 : 1));
                expected.mask |= 1u << a.attr;
                expected.hi[a.attr - 9] = 128 + elem;
            }
            offset += a.srcSize;
        }
        Require(offset == plan.streamSize, "mixed direct/indexed plan consumes exact stream stride");
        const auto spans = vgx::MeasureArraySpans(plan, source.data(), 3);
        Require(spans.mask == expected.mask && std::memcmp(spans.lo, expected.lo, sizeof spans.lo) == 0 &&
                std::memcmp(spans.hi, expected.hi, sizeof spans.hi) == 0,
                "multi-attribute spans agree with cumulative-offset oracle across all formats");
        ++checks;
    }
    std::printf("spans: %u independent range checks\n", checks);
}
}

void CacheNotifyBoundary(int stage) {
    std::unique_lock<std::mutex> lock(gate);
    if (std::this_thread::get_id() != writerId || stage != boundary) return;
    parked = true; cv.notify_all();
    cv.wait(lock, [] { return releaseWriter; });
}
void CacheDecodedBoundary() {
    for (unsigned f = 0; f < 8; ++f) if (retainedPlans[f]) {
        Require(retainedPlans[f]->valid && retainedPlans[f]->layout.stride == retainedStrides[f],
                "later format lookups preserve retained plan validity and stride");
        Require(retainedPlans[f] == &vgx::CurrentPlan(f), "retained vertex plans use stable per-format storage");
    }
    if (!armDecode) return;
    armDecode = false; Start();
    if (completedNoise) vgx::hle::NotifyGuestWrite(ram + 8192, 4);
}
void CacheBuildBoundary(int stage) {
    Require(stage >= 1 && stage <= 4, "known instrumented build stage");
    ++stageHits[stage];
    if (unsigned(stage) == buildStage) CacheDecodedBoundary();
}
uint64_t CacheStartingVertexBytes() { return overflow ? 0xFFFFFFFFull - 16 : 0; }
void CacheTextureSnapshotBoundary() {
    static bool edited = false;
    if (!textureAba || edited) return;
    edited = true;
    std::memset(ram + 4096, 2, 64);
    vgx::hle::NotifyGuestWrite(ram + 4096, 64);
}
const vgx::Plan& CacheCurrentPlan(int fmt) {
    ++planLookups;
    const auto& plan = vgx::CurrentPlan(fmt);
    Require(!retainedPlans[fmt] || retainedPlans[fmt] == &plan, "vertex-plan address stays stable during a pure-list build");
    retainedPlans[fmt] = &plan;
    retainedStrides[fmt] = plan.layout.stride;
    return plan;
}
int main(int argc, char** argv) {
    Require(argc == 2, "usage: cache-ordering scenario");
    const std::string mode = argv[1];
    setenv("GXV_STATICBUF", "0", 1); setenv("GXV_ATLAS", "0", 1); setenv("GXV_HASH", "0", 1);
    vgx::ResetState(); vgx::g.cpArrayStale = 0; vgx::g.bpTexStale = 0;
    vgx::hle::RegisterGuestRam(ram, sizeof ram);
    if (mode == "dl-before" || mode == "texture-before") boundary = 1;
    if (mode == "dl-preclaim") boundary = 0;
    if (mode == "texture-log") boundary = 3;
    disjoint = mode == "dl-disjoint" || mode == "dl-same-page";
    samePage = mode == "dl-same-page";
    textureAba = mode == "texture-aba";
    overflow = mode == "dl-overflow";
    mixedFormats = mode == "dl-mixed";
    const size_t buildAt = mode.find("build-");
    if (buildAt != std::string::npos) {
        buildStage = unsigned(mode.at(buildAt + 6) - '0');
        Require(buildStage >= 1 && buildStage <= 4, "known in-build snapshot boundary");
        boundary = mode.find("complete") != std::string::npos ? -1 : 0;
        editList = mode.find("list-") != std::string::npos;
    }
    completedNoise = mode == "dl-completed-noise";
    if (mode.rfind("dl-", 0) == 0) Geometry();
    else if (mode.rfind("texture-", 0) == 0) Texture();
    else if (mode == "ring" || mode == "lapped") Ring(mode == "lapped");
    else if (mode == "stress") Stress();
    else if (mode == "spans") Spans();
    else Require(false, "known scenario");
    std::printf("PASS: %s\n", argv[1]);
}
