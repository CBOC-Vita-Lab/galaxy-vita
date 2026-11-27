// GXV_FE_PARALLEL: the vertex decode on the finishing thread (see vgx_fepar.h). Included at the end of vgx_fesplit.cpp: the decodes ride
// on the split's operations (split::Call), and every build and host test that links the split links this.
#include <memory>
#include <type_traits>

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));

namespace vgx {
namespace fepar {

bool g_pending = false;

namespace {
// ---- switches (read once)
int ModeOf() {
    const long v = EnvInt("GXV_FE_PARALLEL", 0);
    return v == 1 || v == 2 ? static_cast<int>(v) : 0;
}
uint32_t MinVerts() {
    const uint32_t n = static_cast<uint32_t>(VGX_KNOB([] {
        const long v = EnvInt("GXV_FE_PARALLEL_MIN", 17);
        return static_cast<int32_t>(v < 1 ? 1 : v > 65535 ? 65535 : v);
    }()));
    return n;
}
bool Prof() {
    const bool on = VGX_KNOB(EnvInt("GXV_FE_PARALLEL_PROF", 0) != 0) != 0;
    return on;
}

// ---- what a decode needs besides its source: the plan and the vertex arrays as they were when the draw was parsed
static_assert(std::is_trivially_copyable<Plan>::value && std::is_trivially_copyable<ArrayRef>::value, "plans and arrays are copied as bytes");
// (vgx_state.cpp's attribute and mode numbers: POS = 9 .. TEX7 = 20; direct = 1)
constexpr int kPos = 9, kAttrs = 21, kDirect = 1; // (arrays: the attributes POS .. TEX7)
struct Desc {
    Plan plan;
    ArrayRef arrays[kAttrs];
};
// Blocks of descriptions, kept for the process; a frame's descriptions are reused from the next frame on (its operations are all done at
// its split::FinishFrame, before the frame number moves on). A draw whose plan and arrays are the last description's takes that one.
constexpr uint32_t kBlock = 64;
std::vector<std::unique_ptr<Desc[]>> g_blocks;
uint32_t g_used = 0;
uint64_t g_descFrame = ~0ull;
Desc* g_last = nullptr;

bool SameArrays(const Desc& d, const Plan& p) {
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        if (a.attr < kPos || a.mode == kDirect) continue;
        const ArrayRef& x = d.arrays[a.attr];
        const ArrayRef& y = g.arrays[a.attr];
        if (x.data != y.data || x.size != y.size || x.stride != y.stride || x.le != y.le) return false;
    }
    return true;
}
Desc* DescFor(const Plan& p) {
    const uint64_t frame = CurrentFrame();
    if (frame != g_descFrame) {
        g_descFrame = frame;
        g_used = 0;
        g_last = nullptr;
    }
    if (g_last && std::memcmp(&g_last->plan, &p, sizeof(Plan)) == 0 && SameArrays(*g_last, p)) return g_last;
    if (g_used == g_blocks.size() * kBlock) g_blocks.emplace_back(new Desc[kBlock]);
    Desc* d = &g_blocks[g_used / kBlock][g_used % kBlock];
    ++g_used;
    std::memcpy(static_cast<void*>(&d->plan), &p, sizeof(Plan));
    for (int i = 0; i < kAttrs; ++i) d->arrays[i] = ArrayRef{};
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        if (a.attr >= kPos && a.attr < kAttrs && a.mode != kDirect) d->arrays[a.attr] = g.arrays[a.attr];
    }
    g_last = d;
    return d;
}

// ---- the draw whose decode waits (player)
struct Pend {
    const Desc* d = nullptr;
    const uint8_t* src = nullptr;
    uint32_t count = 0;
    size_t off = 0; // in the frame's arena
} g_p;
uint32_t g_ticket = 0;   // split::Issued() after the last decode sent
bool g_ticketOn = false; // ... and it may not be done yet

// ---- counters. Player: g_pl (this picture), g_plTotal. Whoever runs the operations (the finishing thread, or the player at a Drain;
// never both: split::g_busy): g_fin, read by the player in FrameFinished, which holds the reader.
struct PlayerCtr {
    uint64_t sent = 0, sentVerts = 0, placed = 0, placedVerts = 0, cancelled = 0, settles = 0, settleWaits = 0, settleNs = 0, stray = 0;
    uint64_t rowChecks = 0, rowBad = 0; // (=2: the highest matrix row)
};
PlayerCtr g_pl, g_plTotal;
struct FinCtr {
    uint64_t decodes = 0, verts = 0, ns = 0, checked = 0, bad = 0, oob = 0;
};
FinCtr g_fin, g_finTotal;
uint64_t g_pictures = 0;
bool g_tablesReady = false;

inline uint8_t* Ptr(uint32_t lo, uint32_t hi) {
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(lo | (static_cast<uint64_t>(hi) << 32)));
}
inline void PutPtr(uint32_t* w, const void* p) {
    const uint64_t v = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    w[0] = static_cast<uint32_t>(v);
    w[1] = static_cast<uint32_t>(v >> 32);
}

// ---- on the finishing thread (or the player at a Drain): words = dst, src, desc (two words each), count
void RunDecode(const uint32_t* w) {
    uint8_t* dst = Ptr(w[0], w[1]);
    const uint8_t* src = Ptr(w[2], w[3]);
    const Desc* d = reinterpret_cast<const Desc*>(Ptr(w[4], w[5]));
    const uint32_t count = w[6];
    const int64_t t0 = Prof() ? split::NowNs() : 0;
    uint32_t oob = 0;
    DecodeVerticesOff(d->plan, src, count, dst, d->arrays, &oob);
#if defined(VGX_FEPAR_NEG) && VGX_FEPAR_NEG == 1
    dst[0] ^= 1; // (the host test's negative controls: 1, one bit of every decode made here is wrong: the command buffers must differ;
#elif defined(VGX_FEPAR_NEG) && VGX_FEPAR_NEG == 2
    ++g_pl.sent; // 2, a counter the player bumps at every Send written here too: ThreadSanitizer must report it)
#endif
    ++g_fin.decodes;
    g_fin.verts += count;
    g_fin.oob += oob;
    if (t0) g_fin.ns += static_cast<uint64_t>(split::NowNs() - t0);
}
// GXV_FE_PARALLEL=2: the player has decoded the vertices at dst itself; the same decode here, off its thread, must give the same bytes.
std::vector<uint8_t>& Scratch() {
    static std::vector<uint8_t>& s = *new std::vector<uint8_t>; // (the operations' runner only; never destroyed: see fesplit)
    return s;
}
void RunCheck(const uint32_t* w) {
    const uint8_t* dst = Ptr(w[0], w[1]);
    const uint8_t* src = Ptr(w[2], w[3]);
    const Desc* d = reinterpret_cast<const Desc*>(Ptr(w[4], w[5]));
    const uint32_t count = w[6];
    const size_t bytes = static_cast<size_t>(count) * d->plan.layout.stride;
    std::vector<uint8_t>& s = Scratch();
    if (s.size() < bytes) s.resize(bytes);
    std::memcpy(s.data(), dst, bytes); // (a byte the decode does not write is the arena's: compared as equal)
    const int64_t t0 = Prof() ? split::NowNs() : 0;
    uint32_t oob = 0;
    DecodeVerticesOff(d->plan, src, count, s.data(), d->arrays, &oob);
#if defined(VGX_FEPAR_NEG) && VGX_FEPAR_NEG == 1
    s[0] ^= 1;
#endif
    if (t0) g_fin.ns += static_cast<uint64_t>(split::NowNs() - t0);
    ++g_fin.checked;
    g_fin.verts += count;
    g_fin.oob += oob;
    if (std::memcmp(s.data(), dst, bytes) != 0) {
        ++g_fin.bad;
        if (g_finTotal.bad + g_fin.bad <= 16) {
            size_t at = 0;
            while (at < bytes && s[at] == dst[at]) ++at;
            split::Log("fepar: MISMATCH: %u vertices of layout %03X (stride %u, %s plan), first at byte %u", count, d->plan.layoutMask,
                static_cast<unsigned>(d->plan.layout.stride), d->plan.direct ? "direct" : "general", static_cast<unsigned>(at));
        }
    }
}

void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((format(printf, 1, 2))) void LogLine(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (port_log_summary) port_log_summary("%s", line);
    else split::Log("%s", line);
}
} // namespace

int Mode() {
    const int m = static_cast<int>(VGX_KNOB(ModeOf()));
    return m;
}

bool Want(const Plan& p, uint32_t count) {
    if (__builtin_expect(Mode() == 0, 1)) return false;
    if (count < MinVerts() || !split::OffThreadOk()) return false;
    if (!DecodeOffThreadOk(p)) return false;
    if (!g_tablesReady) {
        DecodeTablesReady(); // (before the first operation that decodes: published to the finishing thread with it)
        g_tablesReady = true;
    }
    return true;
}

void Defer(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out) {
    if (g_pending) { ++g_pl.stray; Place(); } // (cannot happen: every EmitDraw gives its pending decode a way out)
    g_p.d = DescFor(p);
    g_p.src = src;
    g_p.count = count;
    g_p.off = static_cast<size_t>(out - vgx::Pending().vtx.data());
    g_pending = true;
    if (Mode() == 2) { // (verify: the player's own decode, as with the switch off; the highest row read from the stream must be its)
        DecodeVertices(p, src, count, out, nullptr);
        const uint32_t mx = LastDecodedPosMtxMax(), scan = PosMtxMaxOf(p, src, count);
        ++g_pl.rowChecks;
        if (mx != scan) {
            ++g_pl.rowBad;
            VGX_LOG_N(16, "fepar: MISMATCH: highest matrix row %u read from the stream, %u decoded (%u vertices)\n", scan, mx, count);
        }
    }
}

void Place() {
    g_pending = false;
    ++g_pl.placed;
    g_pl.placedVerts += g_p.count;
    if (Mode() == 2) return; // (decoded at Defer)
    uint32_t oob = 0;
    DecodeVerticesOff(g_p.d->plan, g_p.src, g_p.count, vgx::Pending().vtx.data() + g_p.off, g_p.d->arrays, &oob);
    if (oob) VGX_LOG_N(16, "vertex array read out of range (%u attributes of a draw of %u vertices)\n", oob, g_p.count);
}

void Send() {
    g_pending = false;
    uint32_t w[7];
    PutPtr(w, vgx::Pending().vtx.data() + g_p.off);
    PutPtr(w + 2, g_p.src);
    PutPtr(w + 4, g_p.d);
    w[6] = g_p.count;
    split::Call(Mode() == 2 ? &RunCheck : &RunDecode, w, 7);
    g_ticket = split::Issued();
    g_ticketOn = true;
    ++g_pl.sent;
    g_pl.sentVerts += g_p.count;
}

void Cancel() {
    g_pending = false;
    ++g_pl.cancelled;
}

void SettleSources() {
    if (!g_ticketOn) return;
    ++g_pl.settles;
    g_ticketOn = false;
    // (the operations up to the ticket are done once split::g_done has reached it: the finishing thread only moves it forward)
    if (static_cast<int32_t>(split::g_done.load(std::memory_order_acquire) - g_ticket) >= 0) return;
    ++g_pl.settleWaits;
    const int64_t t0 = split::NowNs();
    split::Wake(false); // (it may be asleep with the decode published or not)
    // hitch30: spin GXV_BOUND_SPIN_US (default 100; clock read every 16 polls), then the player runs what is left itself (split::Drain:
    // it takes the reader as soon as the finishing thread lets go, and sleeps on a condition meanwhile). The old loop gave up after 20000
    // polls (about a millisecond or more, with yield() empty on the console). GXV_BOUND_SPIN_US=0 keeps the old loop.
    const int64_t bound = static_cast<int64_t>(split::BoundSpinUs()) * 1000;
    bool tookOver = false;
    for (unsigned spins = 0; static_cast<int32_t>(split::g_done.load(std::memory_order_acquire) - g_ticket) < 0; ++spins) {
        if (bound == 0) {
            if (spins > 20000) { split::Drain(); break; } // (it is not getting there: the player runs what is left itself)
            if (spins > 64) std::this_thread::yield();
        } else if ((spins & 15u) == 15u && split::NowNs() - t0 >= bound) {
            tookOver = true;
            split::Drain();
            break;
        }
    }
    split::NoteSpin(1, split::NowNs() - t0, tookOver, tookOver);
    g_pl.settleNs += static_cast<uint64_t>(split::NowNs() - t0);
}

void FrameFinished(uint64_t frame) {
    if (Mode() == 0) return;
    if (g_pending) { ++g_pl.stray; g_pending = false; }
    g_ticketOn = false; // (everything is done: split::FinishFrame drained)
    ++g_pictures;
    const bool prof = Prof();
    if (prof && (g_pl.sent || g_pl.placed || g_fin.checked)) {
        LogLine("fepar: picture %llu: decodes on the finishing thread %llu (%llu vertices, %.3f ms there), made on the player after all %llu (%llu vertices), "
                "given up %llu; settles %llu (waited %llu, %.3f ms)%s",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g_pl.sent), static_cast<unsigned long long>(g_fin.verts),
                g_fin.ns / 1e6, static_cast<unsigned long long>(g_pl.placed), static_cast<unsigned long long>(g_pl.placedVerts),
                static_cast<unsigned long long>(g_pl.cancelled), static_cast<unsigned long long>(g_pl.settles),
                static_cast<unsigned long long>(g_pl.settleWaits), g_pl.settleNs / 1e6, Mode() == 2 ? " [verify]" : "");
    }
    g_plTotal.sent += g_pl.sent; g_plTotal.sentVerts += g_pl.sentVerts; g_plTotal.placed += g_pl.placed; g_plTotal.placedVerts += g_pl.placedVerts;
    g_plTotal.cancelled += g_pl.cancelled; g_plTotal.settles += g_pl.settles; g_plTotal.settleWaits += g_pl.settleWaits;
    g_plTotal.settleNs += g_pl.settleNs; g_plTotal.stray += g_pl.stray; g_plTotal.rowChecks += g_pl.rowChecks; g_plTotal.rowBad += g_pl.rowBad;
    g_finTotal.decodes += g_fin.decodes; g_finTotal.verts += g_fin.verts; g_finTotal.ns += g_fin.ns; g_finTotal.checked += g_fin.checked;
    g_finTotal.bad += g_fin.bad; g_finTotal.oob += g_fin.oob;
    g_pl = PlayerCtr{};
    g_fin = FinCtr{};
    if (g_pictures % 600 == 0) {
        const double n = static_cast<double>(g_pictures);
        LogLine("fepar: GXV_FE_PARALLEL=%d, %llu pictures: per picture %.1f decodes on the finishing thread (%.0f vertices), %.1f made on the player after "
                "all, %.1f given up; settles that waited %llu (%.1f ms in all)%s; verify: %llu checked, %llu mismatching (%llu of %llu matrix rows); "
                "out-of-range indices off the "
                "player %llu; stray %llu",
                Mode(), static_cast<unsigned long long>(g_pictures), g_plTotal.sent / n, g_plTotal.sentVerts / n, g_plTotal.placed / n,
                g_plTotal.cancelled / n, static_cast<unsigned long long>(g_plTotal.settleWaits), g_plTotal.settleNs / 1e6,
                prof ? "" : " (GXV_FE_PARALLEL_PROF=1: per picture, with times)", static_cast<unsigned long long>(g_finTotal.checked),
                static_cast<unsigned long long>(g_finTotal.bad + g_plTotal.rowBad), static_cast<unsigned long long>(g_plTotal.rowBad),
                static_cast<unsigned long long>(g_plTotal.rowChecks), static_cast<unsigned long long>(g_finTotal.oob),
                static_cast<unsigned long long>(g_plTotal.stray));
    }
}

uint64_t SettleNs() { return g_plTotal.settleNs + g_pl.settleNs; }

void Counters(uint64_t out[8]) {
    out[0] = g_plTotal.sent + g_pl.sent;
    out[1] = g_plTotal.sentVerts + g_pl.sentVerts;
    out[2] = g_plTotal.placed + g_pl.placed;
    out[3] = g_plTotal.cancelled + g_pl.cancelled;
    out[4] = g_finTotal.checked;
    out[5] = g_finTotal.bad + g_plTotal.rowBad + g_pl.rowBad;
    out[6] = g_plTotal.settleWaits + g_pl.settleWaits;
    out[7] = g_finTotal.oob;
}

} // namespace fepar
} // namespace vgx
