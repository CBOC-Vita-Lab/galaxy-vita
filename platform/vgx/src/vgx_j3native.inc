// GXV_J3DNATIVE (j3n1; included at the end of the state-list code of vgx_state.cpp, namespace vgx, so it sees the state-list store).
//
// What the game sends (decomp/src/Game/System/DrawBuffer.cpp, decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_bracketed, top-level stream only):
//     0x5A  id  verA  verB  ext  flags   0x40 addr size   0x5B          (five big-endian u32, then the call's own nine bytes, then one byte)
// around every material-list application (flags bit 0 clear) and every differed-list call (bit 0 set). The game still writes the call, so a
// front end that ignores the markers sees the stream it always saw. The fields:
//   id     a number the list's holder (the J3DDisplayListObj, the drawer's merged list) got when it was made: never another's, never 0
//   verA   the holder's list version: moved by every writer of the list's bytes that stores different bytes (J3DDisplayListObj: endDL,
//          endPatch, a differed list that changed, a new buffer)
//   verB   the drawer's membership version: its own serial (packets added / removed / swapped, light sort) and which packets are hidden
//   ext    a mix of the game's external version classes (scene, fog, fade; J3DPortExtSerial) when the bracket was sent. Lights are not one of
//          them: a light load is XF light memory the front end watches itself (the census's light hash), never part of a list's bytes
//   flags  bit 0: a differed list; bit 1: a merged list (material + first differed);
//          bits 8..: the J3DDiffFlag kinds that moved verA since the last bracket of this holder (BTK 0x200, BRK TevReg, BPK/MR colour 1,
//          BTP 0x20000, fog 0x10000000, ...) (a mask the game keeps in the holder: J3DDisplayListObj::mPortKinds, low 24 bits of the flag)
//
// STAGE 0 (GXV_J3DNATIVE >= 1): the census. Per bracket: static (versions, list address and the external writes between this bracket and the
// one before are those of the same bracket's last picture), new, animated (the list moved: by which kind), membership changed, external
// version changed, history-dependent (the external writes before it differ, or its register delta has masked writes: the result depends on
// bits it does not write). A drawer run (a material bracket and the differed brackets up to the next one) takes its worst bracket's class,
// and its J3D shape draws (the 0x59 calls in it) count under it. Calls outside any run are counted as class fallbacks (unbracketed shape
// calls, dynamic draws, unbracketed lists, particle commands). A sampled timer (one picture in GXV_J3DNATIVE_SAMPLE, a clock read at each
// change of class, the read's cost taken off) gives the front end's time per class.
//
// STAGE 1 (GXV_J3DNATIVE >= 2): a bracket whose record matches (id, verA, verB, ext, list, size, the store's epoch)
// is a candidate. The record holds the state-list entry the list's last application resolved to. What the skip replaces is exactly
// CallList -> CallListBody -> StateListFastHit's front and table lookup, GuestWriteGeneration and (when only the 4 KiB page's generation moved)
// SameBytes against the entry's copy; the register state is then restored by the entry's compiled delta, StateListFastHit's SlRun, so the
// registers, dirty flags and key hash are what the normal call leaves in every case (the delta is applied live, masked writes included:
// the result does not depend on a recorded predecessor state). Two tiers:
//   A  the list's write generation is the entry's: no write was notified to its bytes: the same test StateListFastHit makes. Exact whatever
//      the game's versions say.
//   B  the generation moved (a neighbour in the same 4 KiB page was rewritten, which J3D does every drawn turn) but the game's version did not:
//      the game vouches for the bytes and SameBytes is left out. Trusted only after the probation, and cross-checked by the scrub.
// =2: every candidate runs both ways (skip on a copy of the register file, then the normal call) and the two register files are compared byte
// for byte (and the entry the normal call used must be the record's); GXV_J3DNATIVE_PROBATION agreeing runs (default 3) make a record trusted;
// the checks are limited to GXV_J3DNATIVE_VBUDGET per picture (default 8, the copies of the register file are not free).
// =3: a trusted record skips; one skip in GXV_J3DNATIVE_SCRUB (default 32; tier B: GXV_J3DNATIVE_SCRUB_B, default 8) is first checked by
// SameBytes against the entry's bytes: a bracket whose version is the same and whose bytes are not is a missed writer in the game: the
// bracket takes the normal call, its kind (material / differed) is distrusted for the session, and a line is logged.
// Records are the player thread's (the front end on core 2): the finishing thread, the back end and the second worker never read them. With
// GXV_FE_TWOCORE (vgx2_mode != 0) the markers are only consumed. The store's drop at a scene's destruction (GXV_SL_SCENEDROP) and its own
// clear at 16,384 entries move g_slEpoch: every record, and the census history, dies with the entries it points at.
// Relation to the state-list store's fast hit and its delta (SLFAST, SLDELTA): the skip sits above them. It does not replace the delta replay
// (the registers still have to be written, and the replay is already one cache line of entry and the delta's own lines); it replaces the
// lookups and the generation test in front of it. A skipped call is counted here (skip A / B) and not in the fast-hit counters; the draw
// replay's trace gets no record of a skipped call (a state list has no entry there; the trace's lookup of the calls after it is unchanged).
// Estimated saving: calls skipped x (the sampled time of a normal bracket call - the sampled time of a skipped one).
#if defined(VGX_CLONE_B)
int g_j3nMode = 0;
int J3nModeSlow() { return 0; }
void J3nCall(const uint8_t* list, uint32_t nbytes, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { CallList(list, nbytes); }
void J3nNoteBp(uint32_t) {}
void J3nNoteXf(uint32_t, uint32_t, const uint8_t*) {}
void J3nNoteDraw() {}
void J3nNoteFx() {}
void J3nNoteList(uint32_t) {}
void J3nNoteShape() {}
void J3nPictureEnd() {}
void J3nSceneGone() {}
void J3nCounters(uint64_t out[24]) { for (int i = 0; i < 24; ++i) out[i] = 0; }
#else
int g_j3nMode = -1;
int J3nModeSlow() {
    const long v = EnvInt("GXV_J3DNATIVE", 0);
    g_j3nMode = v < 0 || v > 5 ? 0 : static_cast<int>(v);
    return g_j3nMode;
}
namespace {
constexpr uint32_t kJ3nRecs = 4096;
constexpr uint32_t kFlDiff = 1u;
enum : uint8_t { kStNew, kStProb, kStTrusted, kStBad };
// Run classes (the worst of a run's brackets wins; the order is the precedence).
enum : unsigned { kCStatic, kCNew, kCHist, kCExt, kCMember, kCAnim, kCRunClasses };
enum : unsigned { kTRunPending, kTDyn, kTPart, kTNone };
constexpr const char* kCNames[kCRunClasses] = {"static", "new", "history", "ext-version", "members", "animated"};
struct alignas(32) J3nRec {
    uint32_t id = 0, verA = 0, verB = 0, ext = 0;
    const uint8_t* list = nullptr;
    uint32_t nbytes = 0, epoch = 0;
    SlEntry* e = nullptr;
    uint64_t gen = 0;
    uint32_t eh[4] = {};
    uint32_t pred = 0, skips = 0;
    uint8_t state = kStNew, ok = 0, flags = 0, pad = 0;
};
struct J3nCtr {
    // per window (reset at the log line)
    uint64_t pics = 0, matBr = 0, diffBr = 0, newBr = 0, newEvict = 0, maskedLists = 0, maskedRuns = 0, maskedStaticDraws = 0, unbrShape = 0, dynDraws = 0, unbrList = 0, unbrTiny = 0, dynInRun = 0, unbrListInRun = 0, unbrTinyInRun = 0, fxCmds = 0, extHist[4] = {};
    uint64_t runs[kCRunClasses] = {}, draws[kCRunClasses] = {};
    uint64_t kindCnt[24] = {}, kindUnknown = 0;
    uint64_t ns[kCRunClasses] = {}, nsDyn = 0, nsPart = 0, cpuUs = 0, sampled = 0, events = 0;
    uint64_t seen = 0, cand = 0, skipA = 0, skipB = 0, verified = 0, bad = 0, scrubs = 0, scrubBad = 0, declined = 0, vbudgetOut = 0;
    uint64_t normN = 0, normNs = 0, skipN = 0, skipNs = 0;
};
struct J3nState {
    J3nRec* recs = nullptr;
    J3nCtr w, tot;                 // window, since start
    uint32_t eh[4] = {};           // external writes since the previous bracket (light, fog, projection / view, other)
    uint32_t pred = 0;             // the previous bracket's id
    // the open run
    bool runOpen = false;
    uint8_t runWorst = kCStatic;
    bool runMasked = false;        // the run applied a list with masked BP writes (an attribute, not a class)
    uint32_t runDraws = 0;
    uint64_t runNs = 0;
    // knobs (read once)
    bool init = false;
    int probation = 3, vbudgetMax = 24, scrub = 32, scrubB = 8, strict = 1, sampleEvery = 8;
    int vbudget = 0;
    bool kindBad[2] = {false, false};
    // sampling
    bool sampling = false;
    unsigned cur = kTNone;
    int64_t tAt = 0;
    double tcUs = 1.0;
    int64_t cpuLast = 0;
    uint32_t callTick = 0;
    uint64_t lastLogPic = 0;
    uint32_t mismatchLogged = 0;
} g_j3n;
inline uint32_t J3nMix(uint32_t h, uint32_t v) { h = (h ^ v) * 0x9E3779B1u; return h ^ (h >> 15); }

void J3nInit() {
    J3nState& j = g_j3n;
    j.init = true;
    j.recs = new J3nRec[kJ3nRecs];
    auto knob = [](const char* n, long d, long lo, long hi) { const long v = EnvInt(n, d); return static_cast<int>(v < lo ? lo : (v > hi ? hi : v)); };
    j.probation = knob("GXV_J3DNATIVE_PROBATION", 3, 1, 250);
    j.vbudgetMax = knob("GXV_J3DNATIVE_VBUDGET", 8, 0, 100000);
    j.scrub = knob("GXV_J3DNATIVE_SCRUB", 32, 1, 1000000);
    j.scrubB = knob("GXV_J3DNATIVE_SCRUB_B", 8, 1, 1000000);
    j.strict = knob("GXV_J3DNATIVE_STRICT", 1, 0, 1);
    j.sampleEvery = knob("GXV_J3DNATIVE_SAMPLE", 8, 1, 100000);
    j.vbudget = j.vbudgetMax;
    if (port_host_time_ns) {
        const int64_t a = port_host_time_ns();
        for (int i = 0; i < 400; ++i) (void)port_host_time_ns();
        const int64_t b = port_host_time_ns();
        j.tcUs = (b - a) / 401.0 / 1000.0;
    }
    if (port_log_summary)
        port_log_summary("j3n: GXV_J3DNATIVE=%d (1 census, 2 verify / probation, 3 skip); probation %d runs, verify budget %d a picture, scrub 1 in %d (tier B 1 in %d), "
                         "strict %d, timer sample 1 picture in %d, one clock read %.2f us",
                         g_j3nMode, j.probation, j.vbudgetMax, j.scrub, j.scrubB, j.strict, j.sampleEvery, j.tcUs);
}
inline J3nState& J3n() {
    J3nState& j = g_j3n;
    if (__builtin_expect(!j.init, 0)) J3nInit();
    return j;
}

// ---- sampled class timer
inline void J3nTick(J3nState& j, unsigned next) {
    if (!j.sampling) return;
    const int64_t now = port_host_time_ns();
    ++j.w.events;
    if (j.tAt) {
        const int64_t dt = now - j.tAt - static_cast<int64_t>(j.tcUs * 1000.0);
        const uint64_t d = dt > 0 ? static_cast<uint64_t>(dt) : 0;
        if (j.cur == kTRunPending) j.runNs += d;
        else if (j.cur == kTDyn) j.w.nsDyn += d;
        else if (j.cur == kTPart) j.w.nsPart += d;
    }
    j.tAt = now;
    j.cur = next;
}
void J3nFinishRun(J3nState& j) {
    if (!j.runOpen) return;
    J3nCtr& w = j.w;
    ++w.runs[j.runWorst];
    w.draws[j.runWorst] += j.runDraws;
    w.ns[j.runWorst] += j.runNs;
    if (j.runMasked) { ++w.maskedRuns; if (j.runWorst == kCStatic) w.maskedStaticDraws += j.runDraws; }
    j.runMasked = false;
    j.runOpen = false;
    j.runDraws = 0;
    j.runNs = 0;
}
} // namespace

// ---- stream hooks (ProcessStream's top level; only when GXV_J3DNATIVE >= 1)
void J3nNoteBp(uint32_t v) {
    J3nState& j = J3n();
    const uint32_t reg = v >> 24;
    if (reg >= 0xEE && reg <= 0xF2) j.eh[1] = J3nMix(j.eh[1], v);
    else if (reg == 0x20 || reg == 0x21 || reg == 0x22 || reg == 0x59) j.eh[3] = J3nMix(j.eh[3], v);
}
void J3nNoteXf(uint32_t addr, uint32_t n, const uint8_t* be) {
    J3nState& j = J3n();
    if (addr >= 0x600 && addr < 0x680) {
        for (uint32_t i = 0; i < n; ++i) j.eh[0] = J3nMix(j.eh[0], be32(be + i * 4) ^ (addr + i));
    } else if (addr >= 0x1000 && addr < 0x1058) {
        if (addr >= 0x101A && addr <= 0x1026) {
            for (uint32_t i = 0; i < n; ++i) j.eh[2] = J3nMix(j.eh[2], be32(be + i * 4) ^ (addr + i));
        } else if (addr != 0x1018 && addr != 0x1019) {
            for (uint32_t i = 0; i < n; ++i) j.eh[3] = J3nMix(j.eh[3], be32(be + i * 4) ^ (addr + i));
        }
    }
}
void J3nNoteDraw() {
    J3nState& j = J3n();
    ++j.w.dynDraws;
    if (j.runOpen) ++j.w.dynInRun;
    if (j.sampling && j.cur != kTDyn) J3nTick(j, kTDyn);
}
void J3nNoteFx() {
    J3nState& j = J3n();
    ++j.w.fxCmds;
    if (j.sampling) J3nTick(j, kTPart);
}
void J3nNoteList(uint32_t nbytes) { // an unbracketed list call
    J3nState& j = J3n();
    const bool tiny = nbytes <= 64;
    ++(tiny ? j.w.unbrTiny : j.w.unbrList);
    if (j.runOpen) ++(tiny ? j.w.unbrTinyInRun : j.w.unbrListInRun);
    if (j.sampling) J3nTick(j, tiny ? kTPart : kTDyn);
}
void J3nNoteShape() { // a retained shape call (0x59)
    J3nState& j = J3n();
    if (j.runOpen) {
        ++j.runDraws;
        // (j3n2: the shape's time is the run's again. Before, an unbracketed list in the run (the shape's vertex format, J3DShape's
        // VCD / VAT list) moved the timer to "dynamic" and the run's shapes after it were timed there: the console's 11.9 ms "dynamic")
        if (j.sampling && j.cur != kTRunPending) J3nTick(j, kTRunPending);
    } else {
        ++j.w.unbrShape;
        if (j.sampling) J3nTick(j, kTDyn);
    }
}
namespace {
// ---- the skip: StateListFastHit after its lookups, for the entry the record holds
// 0 not done (nothing was changed), 1 done by tier A, 2 done by tier B, 3 the scrub found the game's version wrong (nothing was changed).
// run: 0 a probation run (no scrub: the caller compares the whole register file), 1 a trusted skip (the scrub's periods apply)
int J3nSkipApply(J3nState& j, J3nRec& r, const uint8_t* list, uint32_t nbytes, int run, bool* scrubbed) {
    SlEntry& e = *r.e;
    if (!e.usable || e.nbytes != nbytes || !e.live) return 0;
    if (static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_STATELIST", 1)))) != 1) return 0;
    if (!J3nListPathOk(nbytes)) return 0;
    const uint64_t frame = CurrentFrame();
    if (frame < e.quietUntil) return 0;
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    if (gen == kNotGuest) return 0;
    int tier = 1;
    if (gen != e.gen) {
        if (!SlKeepBytes() || e.bytes.size() != nbytes) return 0;
        tier = 2;
    }
    if (run == 1) {
        // (the value cross-check: the game's version says the bytes are the entry's; the generation is read before the bytes, as StateListFastHit)
        const uint32_t period = static_cast<uint32_t>(tier == 2 ? j.scrubB : j.scrub);
        if (r.skips % period == 0) {
            *scrubbed = true;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.bytes.size() != nbytes || !SameBytes(list, e.bytes.data(), nbytes)) return 3;
        }
    }
    else if (tier == 2) {
        // (a probation run: the claim is checked against the bytes every time, before anything is touched. The generation update below is
        // the normal call's own step and it must only ever be made for bytes that are the entry's, else the normal call that follows
        // the skip would trust it too)
        *scrubbed = true;
        if (e.bytes.size() != nbytes || !SameBytes(list, e.bytes.data(), nbytes)) return 3;
    }
    if (tier == 2) e.gen = gen; // (StateListCall's "a write near it, not on it")
    if (e.changes) e.changes = 0;
    if (frame == 0) e.live = false;
    ++g_slHits;
    const int bpFastMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_BPFAST", 1))));
    SlRun(e, bpFastMode == 2 ? 1 : bpFastMode);
    return tier;
}
void J3nRecord(J3nState& j, J3nRec& r, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl, const uint8_t* list, uint32_t nbytes) {
    SlEntry* e = SlLookup(list);
    const bool same = r.id == id && r.verA == vA && r.verB == vB && r.ext == ext && r.list == list && r.nbytes == nbytes && r.epoch == g_slEpoch &&
                      r.state != kStNew;
    if (!same) {
        // (a change of any version, the call or the store: a new record, on probation again; a distrusted bracket stays so)
        const uint8_t keep = r.id == id && r.state == kStBad ? kStBad : kStProb;
        r.state = keep;
        r.ok = 0;
        r.skips = 0;
    }
    r.id = id; r.verA = vA; r.verB = vB; r.ext = ext; r.flags = static_cast<uint8_t>(fl & 0xFF);
    r.list = list; r.nbytes = nbytes; r.epoch = g_slEpoch; r.pred = j.pred;
    if (e && e->usable && e->live && e->nbytes == nbytes) {
        r.e = e;
        r.gen = e->gen;
    } else {
        r.e = nullptr;
    }
}
void J3nBadKind(J3nState& j, bool isDiff, const char* why, uint32_t id, const uint8_t* list, uint32_t nbytes) {
    j.kindBad[isDiff ? 1 : 0] = true;
    ++j.w.bad;
    if (j.mismatchLogged < 16 && port_log_summary) {
        ++j.mismatchLogged;
        port_log_summary("j3n: MISMATCH (%s): bracket id %u, list %p, %u bytes: %s lists are distrusted for the session (GXV_J3DNATIVE brackets of this class go the normal way from now on)",
                         why, id, static_cast<const void*>(list), nbytes, isDiff ? "differed" : "material");
    }
}
} // namespace

void J3nCall(const uint8_t* list, uint32_t nbytes, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl) {
    const int mode = J3nMode();
    if (mode == 0) { CallList(list, nbytes); return; }
    if (mode >= 4) { J3n2Bracket(list, nbytes, id, vA, vB, ext, fl); return; } // (stage 2: vgx_j3native2.inc)
    J3nState& j = J3n();
    J3nCtr& w = j.w;
    const bool isDiff = (fl & kFlDiff) != 0;
    // (the slot by a hash of the id: the game's ids are list addresses, 32-byte aligned, so id & 4095 used 128 of the 4096 slots and
    // the console's ~165 brackets a picture evicted each other: "new" every picture (j3n2, from the console census))
    J3nRec& r = j.recs[(id * 0x9E3779B1u) >> (32 - 12)];
    static_assert(kJ3nRecs == 4096u, "the slot hash takes 12 bits");
    // ---- stage 0: classify the bracket against what the same holder's last bracket was
    ++(isDiff ? w.diffBr : w.matBr);
    unsigned st = kCStatic;
    if (r.id != id || !r.list) {
        st = kCNew;
        ++w.newBr;
        if (r.list) ++w.newEvict; // (the slot held another holder's record)
    } else if (r.verA != vA || r.list != list || r.nbytes != nbytes || r.epoch != g_slEpoch) {
        st = kCAnim;
        uint32_t k = fl >> 8;
        if (k == 0) ++w.kindUnknown;
        for (; k; k &= k - 1) ++w.kindCnt[__builtin_ctz(k)];
    } else if (r.verB != vB) {
        st = kCMember;
    } else if (r.ext != ext) {
        st = kCExt;
    } else {
        for (int i = 0; i < 4; ++i)
            if (r.eh[i] != j.eh[i]) { st = kCHist; ++w.extHist[i]; }
    }
    for (int i = 0; i < 4; ++i) { r.eh[i] = j.eh[i]; j.eh[i] = 0; }
    if (!isDiff) {
        J3nFinishRun(j);
        j.runOpen = true;
        j.runWorst = static_cast<uint8_t>(st);
        j.runDraws = 0;
        j.runNs = 0;
        if (j.sampling) J3nTick(j, kTRunPending);
    } else if (!j.runOpen) {
        j.runOpen = true;
        j.runWorst = static_cast<uint8_t>(st);
        j.runDraws = 0;
        j.runNs = 0;
        if (j.sampling) J3nTick(j, kTRunPending);
    } else if (st > j.runWorst) {
        j.runWorst = static_cast<uint8_t>(st);
    }
    // ---- stage 1
    bool done = false;
    const bool timed = j.sampling && port_host_time_ns && ((++j.callTick & 15u) == 0);
    int64_t t0 = 0;
    if (timed) t0 = port_host_time_ns();
    bool skipped = false;
    const bool cand = mode >= 2 && !vgx2_mode && r.id == id && r.e && r.state != kStBad && !j.kindBad[isDiff ? 1 : 0] && r.verA == vA &&
                      r.list == list && r.nbytes == nbytes && r.epoch == g_slEpoch &&
                      (!j.strict || (r.verB == vB && r.ext == ext));  // (not the previous bracket, r.pred: the skip is the entry's own replay on the register file as it is, a function of the entry and the state, so it does not depend on what came before; the census reports history separately)
    if (mode >= 2) ++w.seen;
    if (cand) {
        ++w.cand;
        if (mode == 3 && r.state == kStTrusted) {
            ++r.skips;
            bool scrubbed = false;
            workload::FeScope sc(workload::FeJ3n);
            const int res = J3nSkipApply(j, r, list, nbytes, 1, &scrubbed);
            if (scrubbed) ++w.scrubs;
            if (res == 1 || res == 2) {
                ++(res == 1 ? w.skipA : w.skipB);
                done = skipped = true;
            } else if (res == 3) {
                ++w.scrubBad;
                J3nBadKind(j, isDiff, "version unchanged, bytes changed (a missed writer)", id, list, nbytes);
                r.state = kStBad;
            } else {
                ++w.declined;
            }
        } else if (j.vbudget > 0) {
            --j.vbudget;
            // probation: the skip on a copy of the register file, then the normal call, and the two compared. The register file is 39,512
            // bytes, 32,768 of them the palette slots (g.tlut): a list moves those only with a palette BP write (0x64 .. 0x67), so for a
            // list without one the three copies and the compare leave the slots out (6.7 KB instead of 39.5 KB); with one, all of it.
            const SlEntry* ve = r.e;
            bool tlutOps = false;
            for (const SlOp& op : ve->ops)
                if (op.kind == 1 && (op.v >> 24) >= 0x64 && (op.v >> 24) <= 0x67) { tlutOps = true; break; }
            static State s0, s1;
            char* const gb = reinterpret_cast<char*>(&g);
            const size_t tOff = static_cast<size_t>(reinterpret_cast<char*>(&g.tlut) - gb), tEnd = tOff + sizeof(g.tlut);
            auto save = [&](State& d) {
                if (tlutOps) { std::memcpy(&d, &g, sizeof(State)); return; }
                std::memcpy(reinterpret_cast<char*>(&d), gb, tOff);
                std::memcpy(reinterpret_cast<char*>(&d) + tEnd, gb + tEnd, sizeof(State) - tEnd);
            };
            auto restore = [&](const State& sv) {
                if (tlutOps) { std::memcpy(&g, &sv, sizeof(State)); return; }
                std::memcpy(gb, reinterpret_cast<const char*>(&sv), tOff);
                std::memcpy(gb + tEnd, reinterpret_cast<const char*>(&sv) + tEnd, sizeof(State) - tEnd);
            };
            auto same2 = [&](const State& sv) {
                if (tlutOps) return std::memcmp(&g, &sv, sizeof(State)) == 0;
                return std::memcmp(gb, reinterpret_cast<const char*>(&sv), tOff) == 0 &&
                       std::memcmp(gb + tEnd, reinterpret_cast<const char*>(&sv) + tEnd, sizeof(State) - tEnd) == 0;
            };
            save(s0);
            bool scrubbed = false;
            const int res = J3nSkipApply(j, r, list, nbytes, 0, &scrubbed);
            bool have = false;
            if (res == 1 || res == 2) {
                save(s1);
                have = true;
            }
            restore(s0);
            if (res == 3) {
                ++w.scrubBad;
                J3nBadKind(j, isDiff, "version unchanged, bytes changed (a missed writer)", id, list, nbytes);
                r.state = kStBad;
            }
            CallList(list, nbytes);
            done = true;
            if (have) {
                ++w.verified;
                SlEntry* e = SlLookup(list);
                const bool same = e == r.e && same2(s1);
                if (!same) {
                    J3nBadKind(j, isDiff, e == r.e ? "the register file after the skip differs from the normal call's" : "the normal call used another entry", id, list, nbytes);
                    r.state = kStBad;
                } else if (r.state == kStProb && ++r.ok >= j.probation) {
                    r.state = kStTrusted;
                }
            } else {
                ++w.declined;
            }
        } else {
            ++w.vbudgetOut;
            if (mode == 2) {
                // (no register-file verify left in this picture's budget: the cheap value cross-check of the claim all the same. The game's
                // version says the bytes are the entry's; when the generation moved (a tier B claim) they are compared, a few hundred bytes)
                SlEntry& ce = *r.e;
                if (ce.usable && ce.nbytes == nbytes && SlKeepBytes() && ce.bytes.size() == nbytes) {
                    const uint64_t cg = GuestWriteGeneration(list, nbytes);
                    if (cg != kNotGuest && cg != ce.gen) {
                        ++w.scrubs;
                        std::atomic_thread_fence(std::memory_order_acquire);
                        if (!SameBytes(list, ce.bytes.data(), nbytes)) {
                            ++w.scrubBad;
                            J3nBadKind(j, isDiff, "version unchanged, bytes changed (a missed writer)", id, list, nbytes);
                            r.state = kStBad;
                        }
                    }
                }
            }
        }
    }
    if (!done) CallList(list, nbytes);
    if (timed) {
        const uint64_t dt = static_cast<uint64_t>(port_host_time_ns() - t0);
        if (skipped) { ++w.skipN; w.skipNs += dt; } else { ++w.normN; w.normNs += dt; }
    }
    // ---- the record, and what the entry's delta says about the list
    if (mode >= 1) {
        if (mode >= 2 && !skipped) J3nRecord(j, r, id, vA, vB, ext, fl, list, nbytes);
        else if (mode == 1) {
            r.id = id; r.verA = vA; r.verB = vB; r.ext = ext; r.list = list; r.nbytes = nbytes; r.epoch = g_slEpoch; r.pred = j.pred; r.flags = static_cast<uint8_t>(fl & 0xFF);
        }
        if (r.e == nullptr || mode == 1) {
            const SlEntry* e = SlLookup(list);
            if (e && e->usable && (e->delta0 == 2 || e->delta0 == 4) && e->dp && e->dp[kDBpMask] != 0) {
                ++w.maskedLists;
                j.runMasked = true;
            }
        } else if (r.e->dp && (r.e->delta0 == 2 || r.e->delta0 == 4) && r.e->dp[kDBpMask] != 0) {
            ++w.maskedLists;
            j.runMasked = true;
        }
    }
    j.pred = id;
}

void J3nSceneGone() {
    J3nState& j = g_j3n;
    if (!j.init) return;
    std::memset(static_cast<void*>(j.recs), 0, sizeof(J3nRec) * kJ3nRecs);
    j.runOpen = false;
    j.runDraws = 0;
    j.runNs = 0;
    j.pred = 0;
    for (uint32_t& h : j.eh) h = 0;
}

void J3nCounters(uint64_t out[24]) {
    J3nCtr t = g_j3n.tot;
    const J3nCtr& w0 = g_j3n.w;
    t.matBr += w0.matBr; t.diffBr += w0.diffBr; t.newBr += w0.newBr; t.maskedLists += w0.maskedLists; t.unbrShape += w0.unbrShape; t.dynDraws += w0.dynDraws;
    t.seen += w0.seen; t.cand += w0.cand; t.skipA += w0.skipA; t.skipB += w0.skipB; t.verified += w0.verified; t.bad += w0.bad; t.scrubs += w0.scrubs; t.scrubBad += w0.scrubBad;
    t.declined += w0.declined; t.vbudgetOut += w0.vbudgetOut;
    for (unsigned i = 0; i < kCRunClasses; ++i) { t.runs[i] += w0.runs[i]; t.draws[i] += w0.draws[i]; }
    out[0] = t.matBr; out[1] = t.diffBr; out[2] = t.seen; out[3] = t.cand; out[4] = t.skipA; out[5] = t.skipB; out[6] = t.verified; out[7] = t.bad;
    out[8] = t.scrubs; out[9] = t.scrubBad; out[10] = t.declined; out[11] = t.vbudgetOut;
    for (unsigned i = 0; i < kCRunClasses; ++i) { out[12 + i] = t.runs[i]; }
    out[18] = t.newBr; out[19] = t.maskedLists; out[20] = t.unbrShape; out[21] = t.dynDraws;
    uint64_t d = 0;
    for (unsigned i = 0; i < kCRunClasses; ++i) d += t.draws[i];
    out[22] = d; out[23] = t.draws[kCStatic];
}

void J3nPictureEnd() {
    const int mode = J3nMode();
    if (mode == 0) return;
    if (mode >= 4) { J3n2PictureEnd(); return; } // (stage 2: vgx_j3native2.inc)
    J3nState& j = J3n();
    J3nCtr& w = j.w;
    if (j.sampling) J3nTick(j, kTNone);
    J3nFinishRun(j);
    if (j.sampling && port_thread_cpu_us) {
        const int64_t c = port_thread_cpu_us();
        if (j.cpuLast && c >= j.cpuLast) w.cpuUs += static_cast<uint64_t>(c - j.cpuLast);
        ++w.sampled;
    }
    if (port_thread_cpu_us) j.cpuLast = port_thread_cpu_us();
    ++w.pics;
    j.vbudget = j.vbudgetMax;
    j.tAt = 0;
    j.cur = kTNone;
    j.pred = 0;
    for (uint32_t& h : j.eh) h = 0;
    const uint64_t pic = CurrentFrame();
    j.sampling = port_host_time_ns && ((pic % static_cast<uint64_t>(j.sampleEvery)) == 0);
    if (w.pics < 300) return;
    // ---- the log: per picture over the last 300
    const double n = static_cast<double>(w.pics);
    uint64_t draws = 0, runs = 0;
    for (unsigned i = 0; i < kCRunClasses; ++i) { draws += w.draws[i]; runs += w.runs[i]; }
    const double staticPct = draws ? 100.0 * static_cast<double>(w.draws[kCStatic]) / static_cast<double>(draws) : 0.0;
    if (port_log_summary) {
        char runsLine[400]; int at = 0;
        for (unsigned i = 0; i < kCRunClasses; ++i)
            at += std::snprintf(runsLine + at, sizeof runsLine - at, " %s %.1f/%.1f", kCNames[i], w.runs[i] / n, w.draws[i] / n);
        char kinds[300]; int ka = 0;
        static const char* kn[24] = {"matcolor(1)", "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "texmtx(btk,0x200)", "b10", "b11", "b12", "b13", "b14", "b15", "b16", "texno(btp,0x20000)", "b18", "b19", "b20", "b21", "b22", "b23"};
        for (int i = 0; i < 24; ++i)
            if (w.kindCnt[i]) ka += std::snprintf(kinds + ka, sizeof kinds - ka, " %s %.1f", kn[i], w.kindCnt[i] / n);
        port_log_summary("j3n: census (GXV_J3DNATIVE=%d, %llu pictures): per picture %.1f material + %.1f differed brackets (%.1f new, %.1f of them in a slot another holder had), %.1f runs, %.1f J3D shape draws; "
                         "runs / draws by class:%s; STATIC DRAWERS = %.1f%% OF J3D DRAWS (go/no-go gate >= 55%%: %s); lists with masked writes %.1f (runs with one %.1f, static draws with one %.1f: static%% counting those as history = %.1f%%); "
                         "animated by kind (brackets): %s unknown %.1f; class fallback per picture: shape calls outside a run %.1f, dynamic draws %.1f (%.1f inside a drawer run), unbracketed lists %.1f (+%.1f tiny; inside a drawer run %.1f + %.1f tiny: the shapes' own vertex-format lists), fx-merge commands %.1f; "
                         "external-write changes seen before a bracket (light / fog / view / other) %.1f / %.1f / %.1f / %.1f",
                         mode, static_cast<unsigned long long>(w.pics), w.matBr / n, w.diffBr / n, w.newBr / n, w.newEvict / n, runs / n, draws / n, runsLine, staticPct,
                         staticPct >= 55.0 ? "GO" : "NO-GO", w.maskedLists / n, w.maskedRuns / n, w.maskedStaticDraws / n, draws ? 100.0 * (static_cast<double>(w.draws[kCStatic]) - static_cast<double>(w.maskedStaticDraws)) / static_cast<double>(draws) : 0.0, ka ? kinds : " none", w.kindUnknown / n, w.unbrShape / n, w.dynDraws / n, w.dynInRun / n, w.unbrList / n, w.unbrTiny / n, w.unbrListInRun / n, w.unbrTinyInRun / n,
                         w.fxCmds / n, w.extHist[0] / n, w.extHist[1] / n, w.extHist[2] / n, w.extHist[3] / n);
        if (w.sampled) {
            // (one picture in sampleEvery timed; each class's time scaled by pictures / sampled pictures, the clock reads' cost taken off per event)
            const double sc = 1.0 / static_cast<double>(w.sampled); // per sampled picture
            double stat = w.ns[kCStatic] * sc / 1e6, anim = w.ns[kCAnim] * sc / 1e6, other = 0.0;
            for (unsigned i = 0; i < kCRunClasses; ++i) if (i != kCStatic && i != kCAnim) other += w.ns[i] * sc / 1e6;
            const double dyn = w.nsDyn * sc / 1e6, part = w.nsPart * sc / 1e6, cpu = w.cpuUs * sc / 1e3;
            const double inStream = stat + anim + other + dyn + part;
            port_log_summary("j3n: front-end ms per picture (sampled: %llu pictures, %.0f clock events a picture, thread CPU %.2f ms): static J3D %.2f, animated J3D %.2f, "
                             "other J3D (new / history / ext / members) %.2f, dynamic draws %.2f, 2D + particles %.2f, fixed (CPU - the rest) %.2f; time inside the stream %.2f",
                             static_cast<unsigned long long>(w.sampled), w.events / static_cast<double>(w.sampled), cpu, stat, anim, other, dyn, part, cpu > inStream ? cpu - inStream : 0.0, inStream);
        }
        if (mode >= 2) {
            const double perSkip = w.skipN && w.normN ? (static_cast<double>(w.normNs) / w.normN - static_cast<double>(w.skipNs) / w.skipN) / 1000.0 : 0.0;
            const double savedMs = perSkip * static_cast<double>(w.skipA + w.skipB) / n / 1000.0;
            port_log_summary("j3n: stage 1 (GXV_J3DNATIVE=%d) per picture: brackets seen %.1f, candidates %.1f, skipped %.1f (tier A %.1f, tier B %.1f), verified %.1f, "
                             "scrubs %.1f, declined %.1f, no verify budget %.1f; totals: verified %llu, mismatching / distrusted %llu (scrub found %llu); sampled normal call %.2f us (%llu), skipped %.2f us (%llu): "
                             "estimated front-end saving %.3f ms a picture; kinds distrusted: material %d, differed %d",
                             mode, w.seen / n, w.cand / n, (w.skipA + w.skipB) / n, w.skipA / n, w.skipB / n, w.verified / n,
                             w.scrubs / n, w.declined / n, w.vbudgetOut / n, static_cast<unsigned long long>(j.tot.verified + w.verified), static_cast<unsigned long long>(j.tot.bad + w.bad), static_cast<unsigned long long>(j.tot.scrubBad + w.scrubBad),
                             w.normN ? w.normNs / 1000.0 / w.normN : 0.0, static_cast<unsigned long long>(w.normN), w.skipN ? w.skipNs / 1000.0 / w.skipN : 0.0,
                             static_cast<unsigned long long>(w.skipN), savedMs, j.kindBad[0] ? 1 : 0, j.kindBad[1] ? 1 : 0);
        }
    }
    // totals (the window's counters are folded in; the window starts again)
    J3nCtr& t = j.tot;
    t.matBr += w.matBr; t.diffBr += w.diffBr; t.newBr += w.newBr; t.maskedLists += w.maskedLists; t.unbrShape += w.unbrShape; t.dynDraws += w.dynDraws;
    t.seen += w.seen; t.cand += w.cand; t.skipA += w.skipA; t.skipB += w.skipB; t.verified += w.verified; t.bad += w.bad; t.scrubs += w.scrubs; t.scrubBad += w.scrubBad;
    t.declined += w.declined; t.vbudgetOut += w.vbudgetOut;
    for (unsigned i = 0; i < kCRunClasses; ++i) { t.runs[i] += w.runs[i]; t.draws[i] += w.draws[i]; }
    j.w = J3nCtr{};
}
#endif
