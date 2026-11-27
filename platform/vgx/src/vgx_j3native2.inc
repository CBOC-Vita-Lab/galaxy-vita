// GXV_J3DNATIVE=4 / 5 (j3n2): stage 2 of the J3D-native path. Included at the end of vgx_state.cpp (namespace vgx), after stage 0 / 1
// (vgx_j3native.inc). The draw side (templated resolutions) is vgx_j3native2_draw.inc in vgx_draw.cpp; vgx_j3n2.h is what they share.
//
// THE IDEA. In a picture like the one before it, a J3D draw buffer sends the same material and differed lists, with the same bytes, in the
// same order; what changes is matrices (and lights, culling, what is visible). The normal path replays every list into the register file
// (SlRun: a compare, a store and the dirty marks per register written, the key hash) and resolves every static run from the registers.
// Stage 2 makes the same register file with less work where it can prove the outcome is the one it saw, and resolves from templates:
//   - CHAIN: a stretch of the top-level stream that starts at a material bracket, recorded once and then matched picture by picture. Its
//     TOKENS are the state-list calls in it (bracketed material / differed lists and plain ones: vertex formats): list, size, the state-list
//     entry, the write generation, the bracket's id / versions / ext / flags. Shape calls, draws, CP writes, XF matrix / light memory writes,
//     indexed loads, and BP / XF register writes may come between tokens (see DEVIATION); a list call that is not a compiled state list,
//     a particle command or an unknown opcode ends the chain.
//   - LAZY REGION: the register words a compiled delta's register part writes: bp[256], tevReg / tevKonst, tevRaw, xfReg (but the matrix
//     indices 0x1018 / 0x1019, which CP writes share), and derived from them the key hash and the TEV range flag.
//   - RECORDING (the normal path, with notes): for each token, the lazy words its application changed and their new values (its PATCH), the
//     key hash and range flag after it, and the dirty flags its application set (measured with the flags cleared around the call: they are
//     a function of the old and new register values only); a version number of the lazy state (moved by a token that changed anything);
//     at every static run a template (draw side) keyed by that version. And the chain's HISTORY: the lazy words (bits, for masked writes)
//     its tokens do not write before its first draw, with their values at the start.
//   - REPLAY (=5): at a material bracket whose chain is ready, the history is compared with the register file, bit for bit; the bits that
//     differ go into D (GXV_J3DNATIVE_HSEED=1, default; 0: a differing history records the chain again). Then every list call is matched
//     with the next token: the same list, size, entry, epoch, bracket fields, and the same bytes (the write generation; when it moved, the
//     bytes compared: GXV_J3DNATIVE_TRUST=1 lets a bracket's unchanged version vouch for them after a probation, scrubbed 1 in
//     GXV_J3DNATIVE_SCRUB). A token whose write map (every word its deltas write, all TEV colour words of a TEV write, both mode words of a
//     texture map's LOD write) meets no word of D sees the recorded state on every word it touches, so its patch IS its application: its CP
//     and XF-memory part applied (SlDeltaApplyT<.., 2>), the patch's words stored, the key hash moved by the recorded delta, the range flag
//     set, its dirty flags ORed in. Prefix tokens (before the first draw) are applied the normal way. The register file is real at every point.
//     Static runs resolve the normal way (GXV_J3DNATIVE_TPL=1: from templates of the lazy-state version; measured slower than dj::Resolve).
//   - DEVIATION: a set D of lazy-word bits that may differ from the recording at this point. A token that meets D is applied the normal way
//     and removes the bits it writes whole; a list with the token's bracket id but other contents (an animated material's differed list, a
//     VARIABLE token) is applied the normal way and adds what it and the recorded token write; a BP or XF register write between tokens adds
//     its bits. RESYNCHRONISATION: a call that is a recorded token a little further on (GXV_J3DNATIVE_LOOK) skips the tokens before it (their
//     write maps go into D); a call that is not in the recording (an object came) is applied the normal way and adds what it writes.
//   - MEASURED (host model, callgrind, see the REPORT): exact, and it does NOT pay: a patch costs about half a replay, and on the fixture only
//     a third of the lists are patched while the matching / deviation bookkeeping costs more than that saves. Keep =0 (the default).
//   - VERIFY (=4): everything runs the normal way; chains are matched and recorded alike; every patch is compared with what the normal call
//     did to the register file (all lazy words, the key hash, the range flag, and that the dirty flags it set are among the patch's), and every
//     template with the normal resolution of its run (draw side). The pictures are the normal path's.
// Off (the normal path) by reason, logged once: the second worker (GXV_FE_TWOCORE), switches that read the registers per static run
// (DrawUsable), GXV_STATELIST / GXV_SLDELTA / GXV_BPFAST / GXV_REGSHADOW / GXV_SLBYTES / GXV_SLFAST not at their defaults, GXV_RETAINED off,
// the first picture.
#if defined(VGX_CLONE_B)
bool g_j3n2On = false;
void J3n2End(int) {}
void J3n2Flush() {}
void J3n2Bracket(const uint8_t* list, uint32_t nbytes, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { CallList(list, nbytes); }
void J3n2Plain(const uint8_t* list, uint32_t nbytes) { CallList(list, nbytes); }
void J3n2Shape(const uint8_t* list, uint32_t nbytes, uint32_t handle) { CallListRetained(list, nbytes, handle); }
void J3n2NoteBp(uint32_t) {}
void J3n2NoteXf(uint32_t, uint32_t) {}
void J3n2SceneGone() {}
void J3n2PictureEnd() {}
void J3n2Counters(uint64_t out[24]) { for (int i = 0; i < 24; ++i) out[i] = 0; }
namespace j3n2 { void Bail(int) {} }
#else
bool g_j3n2On = false;
namespace {
// ---- the lazy region
struct Lz {
    uint32_t bp[256];
    int tev[2][16]; // tevReg, tevKonst
    uint32_t raw[16];
    uint32_t xf[0x58];
    uint64_t keyHash;
    uint8_t tevOor;
};
constexpr uint32_t kLzWords = 256 + 32 + 16 + 0x58; // the word index: bp, tevReg, tevKonst, tevRaw, xfReg
constexpr uint32_t kLzTev = 256, kLzRaw = 288, kLzXf = 304;
inline void LzSave(Lz& z) {
    std::memcpy(z.bp, g.bp, sizeof z.bp);
    std::memcpy(z.tev[0], g.tevReg, 64);
    std::memcpy(z.tev[1], g.tevKonst, 64);
    std::memcpy(z.raw, g.tevRaw, sizeof z.raw);
    std::memcpy(z.xf, g.xfReg, sizeof z.xf);
    z.keyHash = g.keyHash;
    z.tevOor = g.tevOor;
}
inline void LzLoad(const Lz& z) {
    std::memcpy(g.bp, z.bp, sizeof z.bp);
    std::memcpy(g.tevReg, z.tev[0], 64);
    std::memcpy(g.tevKonst, z.tev[1], 64);
    std::memcpy(g.tevRaw, z.raw, sizeof z.raw);
    std::memcpy(g.xfReg, z.xf, sizeof z.xf);
    g.keyHash = z.keyHash;
    g.tevOor = z.tevOor;
}
inline uint32_t* LzPtrG(uint32_t i) {
    if (i < kLzTev) return &g.bp[i];
    if (i < kLzTev + 16) return reinterpret_cast<uint32_t*>(&g.tevReg[0][0] + (i - kLzTev));
    if (i < kLzRaw) return reinterpret_cast<uint32_t*>(&g.tevKonst[0][0] + (i - kLzTev - 16));
    if (i < kLzXf) return &g.tevRaw[i - kLzRaw];
    return &g.xfReg[i - kLzXf];
}
inline uint32_t LzWordZ(const Lz& z, uint32_t i) {
    if (i < kLzTev) return z.bp[i];
    if (i < kLzTev + 16) return static_cast<uint32_t>(z.tev[0][i - kLzTev]);
    if (i < kLzRaw) return static_cast<uint32_t>(z.tev[1][i - kLzTev - 16]);
    if (i < kLzXf) return z.raw[i - kLzRaw];
    return z.xf[i - kLzXf];
}
inline bool LzLive(uint32_t i) { return i == kLzXf + 0x18 || i == kLzXf + 0x19; } // (the matrix indices: CP writes share them)
// The dirty flags a list's application may set: the run of twelve bytes, the TEV colour, texture slot and texture staleness bits.
struct Flags {
    uint8_t run[12];
    uint8_t tevCol, texSlot, texStale;
};
inline void FlagsSave(Flags& f) {
    std::memcpy(f.run, &g.bpDirty, 12);
    f.tevCol = g.tevColDirty;
    f.texSlot = g.texSlotDirty;
    f.texStale = g.bpTexStale;
}
inline void FlagsClear() {
    std::memset(&g.bpDirty, 0, 12);
    g.tevColDirty = g.texSlotDirty = g.bpTexStale = 0;
}
inline void FlagsOr(const Flags& f) {
    uint8_t* r = reinterpret_cast<uint8_t*>(&g.bpDirty);
    for (int i = 0; i < 12; ++i) r[i] = static_cast<uint8_t>(r[i] | f.run[i]);
    g.tevColDirty |= f.tevCol;
    g.texSlotDirty |= f.texSlot;
    g.bpTexStale |= f.texStale;
}
inline void FlagsLoad(const Flags& f) {
    std::memcpy(&g.bpDirty, f.run, 12);
    g.tevColDirty = f.tevCol;
    g.texSlotDirty = f.texSlot;
    g.bpTexStale = f.texStale;
}
// The run bits an eager part (XF memory, the matrix indices, CP: applied live at the replay, SlDeltaApplyT<.., 2>) can set: xfMtxDirty,
// xfLightDirty. Its flags depend on what the memory held before, so they are not recorded: the live application makes them.
constexpr uint16_t kEagerRun = (1u << 8) | (1u << 9);
inline uint16_t RunBits(const uint8_t run[12]) {
    uint16_t b = 0;
    for (int i = 0; i < 12; ++i) if (run[i]) b = static_cast<uint16_t>(b | (1u << i));
    return b;
}

// ---- a compiled delta, read as what it writes (the register part) and whether it may be a token
inline bool DeltaCompact(const SlEntry& e) { return e.delta0 == 4; }
// Calls f(word, bits) for each lazy word the register part of e's delta writes (bits ~0u: the whole word).
template <class F> void DeltaWalk(const SlEntry& e, F&& f) {
    const bool compact = DeltaCompact(e);
    const uint32_t kBpKeyRec = compact ? 1 : 3, kXfKeyRec = compact ? 2 : 4;
    const uint32_t* const n = e.dp;
    const uint32_t* p = e.dp + kDSections;
    for (uint32_t i = 0; i < n[kDBpKey]; ++i, p += kBpKeyRec) f(p[0] >> 24, ~0u);
    for (uint32_t i = 0; i < n[kDBp]; ++i, ++p) f(p[0] >> 24, ~0u);
    for (uint32_t i = 0; i < n[kDBpMask]; ++i, p += 2) f(p[0] >> 24, ~p[1]);
    for (uint32_t i = 0; i < n[kDTex]; ++i, ++p) f(p[0] >> 24, ~0u);
    for (uint32_t i = 0; i < n[kDTev]; ++i, ++p) {
        const uint32_t slot = p[0] >> 24, idx = slot >> 2, konst = slot & 1u, bg = (slot >> 1) & 1u;
        const uint32_t base = kLzTev + konst * 16u + idx * 4u;
        if (bg) { f(base + 2, ~0u); f(base + 1, ~0u); } else { f(base + 0, ~0u); f(base + 3, ~0u); }
        f(kLzRaw + slot, ~0u);
    }
    for (uint32_t i = 0; i < n[kDTevBp]; ++i, ++p) f(p[0] >> 24, ~0u);
    for (uint32_t i = 0; i < n[kDXfWKey]; ++i, p += kXfKeyRec) f(kLzXf + p[0], ~0u);
    for (uint32_t i = 0; i < n[kDXfW]; ++i, p += 2) f(kLzXf + p[0], ~0u);
}
// May a list with this entry be a token: compiled, its XF-memory blocks memory or the matrix indices only (the rest of its effect is the
// register part, which the patch is), and not a writer of what the templated runs read live (BP 0x20 0x21 0x22 0x59: the scissor, which
// the cull reads; XF 0x101A..0x1026: the viewport and projection).
bool DeltaTokenOk(const SlEntry& e) {
    if (!(e.delta0 == 2 || e.delta0 == 4) || !e.dp) return false;
    bool ok = true;
    DeltaWalk(e, [&](uint32_t w, uint32_t) {
        if (w == 0x20 || w == 0x21 || w == 0x22 || w == 0x59 || (w >= kLzXf + 0x1A && w <= kLzXf + 0x26)) ok = false;
    });
    if (!ok) return false;
    const bool compact = DeltaCompact(e);
    const uint32_t* const n = e.dp;
    const uint32_t* p = e.dp + kDSections + n[kDBpKey] * (compact ? 1 : 3) + n[kDBp] + n[kDBpMask] * 2 + n[kDTex] + n[kDTev] + n[kDTevBp] +
                        n[kDXfWKey] * (compact ? 2 : 4) + n[kDXfW] * 2;
    for (uint32_t i = 0; i < n[kDXfB]; ++i) {
        const uint32_t addr = p[0] & 0xFFFFu, cnt = p[0] >> 16;
        if (addr >= 0x1000 ? !(addr >= 0x1018 && addr + cnt <= 0x101A) : addr + cnt > 0x1000) return false;
        p += 1 + cnt;
    }
    return true;
}
inline void DeltaLive(const SlEntry& e) { if (DeltaCompact(e)) SlDeltaApplyT<true, 2>(e); else SlDeltaApplyT<false, 2>(e); }

// ---- tokens and chains
enum : uint8_t { kTkMat = 0, kTkDiff = 1, kTkPlain = 2 };
struct Tok {
    const uint8_t* list = nullptr;
    SlEntry* e = nullptr;
    uint64_t gen = 0;
    uint64_t keyHash = 0;  // after it
    uint32_t nbytes = 0, id = 0, verA = 0, verB = 0, ext = 0, fl = 0;
    uint32_t v = 0;        // the lazy-state version after it
    uint32_t pOff = 0, pN = 0;
    uint64_t keyDelta = 0; // the key hash after it XOR before it (a function of the old and new values of what it writes, see Patch)
    uint32_t wmap[(kLzWords + 31) / 32] = {}; // the lazy words its application depends on: those it writes, and with them all TEV
                                               // registers (the range flag) and both texture-unit modes of a map (the LOD-bias key term)
    uint32_t gmap[(kLzWords + 31) / 32] = {}; // the lazy words that commands between the previous token and this one changed at the
                                               // recording (top-level BP / XF register writes, a shape's list that was parsed): at the replay
                                               // they go into D before this token, whatever the replay's own gap did
    uint8_t tevTouch = 0;  // it writes a TEV register: the range flag after it is tevOor
    uint16_t run = 0;      // the dirty flags (bits of the run of twelve bytes) its application set
    uint8_t tevCol = 0, texSlot = 0, texStale = 0, tevOor = 0;
    uint8_t eager = 0;     // its delta has an eager part (XF memory / matrix indices / CP)
    uint8_t kind = 0;
    uint8_t var = 0;       // a VARIABLE token: a bracketed list whose contents change (an animated material's): matched by kind and id only
    uint8_t probe = 0;     // (GXV_J3DNATIVE_TRUST=1) tier-B uses checked against the bytes so far
    uint16_t skips = 0;    // (GXV_J3DNATIVE_TRUST=1) tier-B uses since the last scrub
};
enum : uint8_t { kChEmpty = 0, kChRec = 1, kChReady = 2 };
struct Chain {
    uint32_t id = 0, epoch = 0;
    uint8_t state = kChEmpty;
    uint8_t fails = 0;               // starts in a row that failed
    uint64_t backoffUntil = 0;       // (a picture: no start or recording before it)
    std::vector<Tok> toks;
    std::vector<uint16_t> pIdx;      // the patches' words and values
    std::vector<uint32_t> pVal;
    Lz start;                        // the lazy state before the first token (the history's values)
    uint32_t firstDraw = ~0u;        // tokens before the first shape call
    uint32_t lastV = 0;
    std::vector<uint16_t> hIdx;      // the history: lazy words (and bits) the tokens do not write before the first draw
    std::vector<uint32_t> hMask;
    std::vector<uint32_t> prefW;     // (recording, until the first draw) the bits the tokens so far write
    j3n2::TplSet tpl;
};
constexpr uint32_t kChains = 4096;
enum : int { kEndCmd = 0, kEndMismatch = 1, kEndFlush = 2, kEndScene = 3, kEndReasons = 4 };
struct Ctr2 {
    uint64_t pics = 0, starts = 0, startNoH = 0, startNoTok = 0, recs = 0, recDone = 0, recNoList = 0, backoff = 0;
    uint64_t endExact = 0, endMid[kEndReasons] = {}, cut = 0;
    uint64_t patched = 0, prefLists = 0, varRec = 0, resumed = 0, resyncs = 0, skipped = 0, insertedLists = 0, tierB = 0, tierBBytes = 0, normalLists = 0, devLists = 0, varLists = 0, devWrites = 0, shapes = 0, devShapes = 0, notHit = 0;
    uint64_t verPatch = 0, verPatchBad = 0, missed = 0;
    uint64_t sampN = 0, sampNs = 0, sampLazyN = 0, sampLazyNs = 0;
};
struct J2 {
    bool init = false;
    int trust = 0, probation = 3, scrub = 32, maxTok = 4096, backoffPics = 60, tpl = 0;
    std::unique_ptr<Chain> chains[kChains];
    // the active chain
    int mode = j3n2::kOff;
    Chain* c = nullptr;
    uint32_t pos = 0, v = 0;
    uint32_t inserted = 0;          // lists taken as inserted since the last token matched
    int hseed = 1;                  // (GXV_J3DNATIVE_HSEED) 1: a chain whose start differs from the recording's runs with the difference in D
    uint32_t maskIn = 0x00FFFFFFu;  // (MeasuredCall) the BP mask pending when the measured call began
    uint32_t gap[(kLzWords + 31) / 32] = {}; // (recording) the lazy words changed since the last token by what is not a token
    Lz shapeBefore;                 // (ShapeCall) the lazy region before a shape call that was not a record hit
    int resume = 1;                 // (GXV_J3DNATIVE_RESUME) 0: an animated list that is stable again stays a variable token (no recording)
    int look = 24;                  // (GXV_J3DNATIVE_LOOK) how far ahead a resynchronisation looks for the call among the tokens
    uint32_t dev[kLzWords] = {};    // D: per lazy word, the bits that may differ from the recording here (bit granularity: masked writes)
    uint32_t devN = 0;              // words of D with a bit set
    uint32_t devMap[(kLzWords + 31) / 32] = {}; // the same as a bitmap of words
    Lz before;                      // (recording, verify) the lazy state before the list being called
    Flags fl0;                      // (recording, verify) the flags before it
    j3n2::DrawCtx ctx;
    bool kindBad[2] = {false, false};
    uint32_t mismatchLogged = 0;
    // usability, decided per picture
    uint64_t usablePic = ~0ull;
    bool usable = false;
    const char* whyLogged = nullptr;
    uint32_t tick = 0;
    Ctr2 w, tot;
} g_j2;

void J2Init() {
    J2& j = g_j2;
    j.init = true;
    auto knob = [](const char* n, long d, long lo, long hi) { const long v = EnvInt(n, d); return static_cast<int>(v < lo ? lo : (v > hi ? hi : v)); };
    j.trust = knob("GXV_J3DNATIVE_TRUST", 0, 0, 1);
    j.probation = knob("GXV_J3DNATIVE_PROBATION", 3, 1, 250);
    j.scrub = knob("GXV_J3DNATIVE_SCRUB", 32, 1, 1000000);
    j.maxTok = knob("GXV_J3DNATIVE_MAXTOK", 4096, 8, 65535);
    j.backoffPics = knob("GXV_J3DNATIVE_BACKOFF", 60, 0, 100000);
    j.tpl = knob("GXV_J3DNATIVE_TPL", 0, 0, 1);
    j.look = knob("GXV_J3DNATIVE_LOOK", 24, 0, 1024);
    j.resume = knob("GXV_J3DNATIVE_RESUME", 1, 0, 1);
    j.hseed = knob("GXV_J3DNATIVE_HSEED", 1, 0, 1);
    if (port_log_summary)
        port_log_summary("j3n: stage 2: GXV_J3DNATIVE=%d (4 verify, 5 on): chains of patched lists%s; tier-B bytes %s (probation %d, scrub 1 in %d), "
                         "at most %d tokens a chain, back-off %d pictures",
                         J3nMode(), j.tpl ? " and templated static runs" : " (GXV_J3DNATIVE_TPL=0: runs resolved the normal way)",
                         j.trust ? "trusted to the game's version" : "always compared", j.probation, j.scrub, j.maxTok, j.backoffPics);
}
inline J2& J2s() {
    J2& j = g_j2;
    if (__builtin_expect(!j.init, 0)) J2Init();
    return j;
}
// Whether this picture may use chains at all (decided at its first bracket).
bool J2Usable() {
    J2& j = J2s();
    const uint64_t pic = CurrentFrame();
    if (j.usablePic == pic) return j.usable;
    j.usablePic = pic;
    const char* why = nullptr;
    if (vgx2_mode != 0) why = "the second worker (GXV_FE_TWOCORE)";
    else if (pic == 0) why = "the first picture";
    else if (static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_STATELIST", 1)))) != 1) why = "GXV_STATELIST is not 1";
    else if (SlDeltaMode() != 1) why = "GXV_SLDELTA is not 1";
    else if (static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_BPFAST", 1)))) != 1) why = "GXV_BPFAST is not 1";
    else if (RegShadow() != 1) why = "GXV_REGSHADOW is not 1";
    else if (!SlKeepBytes()) why = "GXV_SLBYTES=0";
    else if (SlFastMode() == 0) why = "GXV_SLFAST=0";
    else if (!RetainedOn()) why = "GXV_RETAINED is off";
    else if (!j3n2::DrawUsable(&why)) {}
    j.usable = why == nullptr;
    if (why && why != j.whyLogged && port_log_summary) {
        j.whyLogged = why;
        port_log_summary("j3n: stage 2 off in this picture: %s", why);
    }
    return j.usable;
}
Chain* ChainOf(uint32_t id, bool make) {
    J2& j = J2s();
    std::unique_ptr<Chain>& c = j.chains[(id * 0x9E3779B1u) >> (32 - 12)]; // (a hash of the id: the game's ids are 32-byte aligned addresses)
    static_assert(kChains == 4096u, "the slot hash takes 12 bits");
    if (!c) {
        if (!make) return nullptr;
        c = std::make_unique<Chain>();
    }
    return c.get();
}
// ---- D
inline bool DevEmpty() { return g_j2.devN == 0; }
inline uint32_t WordBits(uint32_t w) { return w < kLzTev ? 0x00FFFFFFu : ~0u; } // (BP registers hold 24 bits)
inline void DevAdd(uint32_t w, uint32_t bits = ~0u) {
    J2& j = g_j2;
    bits &= WordBits(w);
    if (!bits) return;
    if (!j.dev[w]) { ++j.devN; j.devMap[w >> 5] |= 1u << (w & 31); }
    j.dev[w] |= bits;
}
inline void DevClear(uint32_t w, uint32_t bits) {
    J2& j = g_j2;
    if (!j.dev[w]) return;
    j.dev[w] &= ~bits;
    if (!j.dev[w]) { --j.devN; j.devMap[w >> 5] &= ~(1u << (w & 31)); }
}
// Whether nothing the token's patch depends on may differ from the recording (then the patch is its application).
inline bool DevClearFor(const uint32_t* wmap) {
    const J2& j = g_j2;
    if (!j.devN) return true;
    for (uint32_t i = 0; i < (kLzWords + 31) / 32; ++i)
        if (j.devMap[i] & wmap[i]) return false;
    return true;
}
// The words of a token's recorded write map, whole, into D (what the token wrote when it was recorded: its entry may hold other contents now).
void DevAddMap(const uint32_t* wmap) {
    for (uint32_t i = 0; i < (kLzWords + 31) / 32; ++i)
        for (uint32_t m = wmap[i]; m; m &= m - 1) DevAdd(i * 32u + static_cast<uint32_t>(__builtin_ctz(m)));
}
inline void GapAdd(uint32_t w) { g_j2.gap[w >> 5] |= 1u << (w & 31); }
// A shape call in a chain: a record hit draws and writes no register; otherwise the call may parse a list that does (a shape list with
// state commands): the lazy words it changed go into the gap (recording) or into D (replay).
void ShapeCall(const uint8_t* list, uint32_t nbytes, uint32_t handle, bool tryHit = true) {
    J2& j = g_j2;
    if (tryHit && CallListRetainedHit(list, nbytes, handle)) return;
    LzSave(j.shapeBefore);
    CallListRetained(list, nbytes, handle);
    for (uint32_t i = 0; i < kLzWords; ++i) {
        if (LzLive(i) || *LzPtrG(i) == LzWordZ(j.shapeBefore, i)) continue;
        if (j.mode == j3n2::kRec) GapAdd(i);
        else DevAdd(i);
    }
}
// The state-list entry of a list just called, when it describes the bytes the call read (parsed, the same size, not written since).
const SlEntry* CurEntry(const uint8_t* list, uint32_t nbytes) {
    const SlEntry* e = SlLookup(list);
    if (!e || !e->usable || !e->live || e->nbytes != nbytes) return nullptr;
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    return gen != kNotGuest && gen == e->gen ? e : nullptr;
}
// The words a parsed list's operations write, as a word map (a variable token's recorded write map).
void OpsMap(const SlEntry& e, uint32_t* wmap) {
    auto set = [wmap](uint32_t w) { wmap[w >> 5] |= 1u << (w & 31); };
    auto bp = [&set](uint32_t reg) {
        if (reg == 0xFE) return;
        set(reg);
        if (reg >= 0xE0 && reg <= 0xE7) {
            const uint32_t idx = (reg - 0xE0) >> 1;
            for (uint32_t k = 0; k < 4; ++k) { set(kLzTev + idx * 4 + k); set(kLzTev + 16 + idx * 4 + k); }
            set(kLzRaw + ((reg - 0xE0) << 1));
            set(kLzRaw + ((reg - 0xE0) << 1) + 1);
        }
    };
    auto xf = [&set](uint32_t a) { if (a >= 0x1000 && a < 0x1058 && !LzLive(kLzXf + (a - 0x1000))) set(kLzXf + (a - 0x1000)); };
    for (const SlOp& op : e.ops) {
        switch (op.kind) {
        case 1: bp(op.v >> 24); break;
        case 5: case 6: for (uint32_t i = 0; i < op.b; ++i) bp(e.words[op.v + i] >> 24); break;
        case 3: for (uint32_t i = 0; i < op.a; ++i) xf(op.b + i); break;
        case 7: xf(op.b); break;
        default: break;
        }
    }
}
// The bits a parsed list's operations write (any list the state-list store has parsed, compiled or not), into D: a BP write behind a mask
// adds the mask's bits; a TEV colour write adds its register's components and both halves' raw words.
void DevAddOps(const SlEntry& e) {
    uint32_t pend = 0x00FFFFFFu;
    auto bp = [&pend](uint32_t reg, uint32_t val) {
        if (reg == 0xFE) { pend = val & 0x00FFFFFFu; return; }
        DevAdd(reg, pend);
        pend = 0x00FFFFFFu;
        if (reg >= 0xE0 && reg <= 0xE7) {
            const uint32_t idx = (reg - 0xE0) >> 1;
            for (uint32_t k = 0; k < 4; ++k) { DevAdd(kLzTev + idx * 4 + k); DevAdd(kLzTev + 16 + idx * 4 + k); }
            DevAdd(kLzRaw + ((reg - 0xE0) << 1));
            DevAdd(kLzRaw + ((reg - 0xE0) << 1) + 1);
        }
    };
    auto xf = [](uint32_t a) { if (a >= 0x1000 && a < 0x1058 && !LzLive(kLzXf + (a - 0x1000))) DevAdd(kLzXf + (a - 0x1000)); };
    for (const SlOp& op : e.ops) {
        switch (op.kind) {
        case 1: bp(op.v >> 24, op.v); break;
        case 5: case 6: for (uint32_t i = 0; i < op.b; ++i) bp(e.words[op.v + i] >> 24, e.words[op.v + i]); break;
        case 3: for (uint32_t i = 0; i < op.a; ++i) xf(op.b + i); break;
        case 7: xf(op.b); break;
        default: break; // (2 CP, 4 an indexed load into XF memory: not lazy words)
        }
    }
}
// The bits it writes out of D (applied the normal way to a register file that agrees with the recording outside D, a list writes the
// recorded values).
void DevClearEntry(const SlEntry& e) { DeltaWalk(e, [](uint32_t w, uint32_t bits) { DevClear(w, bits); }); }
void DevReset() {
    J2& j = g_j2;
    if (j.devN) {
        std::memset(j.dev, 0, sizeof j.dev);
        std::memset(j.devMap, 0, sizeof j.devMap);
        j.devN = 0;
    }
}

// ---- the end of a chain (the register file is real: nothing to undo)
void Off() {
    J2& j = g_j2;
    j.mode = j3n2::kOff;
    j.c = nullptr;
    j.inserted = 0;
    DevReset();
    g_j3n2On = false;
}
void HistoryFrom(Chain& c) {
    c.hIdx.clear();
    c.hMask.clear();
    for (uint32_t i = 0; i < kLzWords; ++i) {
        if (LzLive(i)) continue;
        const uint32_t keep = ~c.prefW[i];
        if (keep) { c.hIdx.push_back(static_cast<uint16_t>(i)); c.hMask.push_back(keep); }
    }
}
void RecFinish() {
    J2& j = g_j2;
    Chain& c = *j.c;
    if (c.firstDraw == ~0u) {
        c.firstDraw = static_cast<uint32_t>(c.toks.size());
        HistoryFrom(c);
    }
    c.prefW.clear();
    c.prefW.shrink_to_fit();
    c.state = c.toks.empty() ? kChEmpty : kChReady;
    ++j.w.recDone;
    Off();
}
// The chain is cut at the current position: what follows was not this picture's.
void Cut() {
    J2& j = g_j2;
    Chain& c = *j.c;
    if (j.pos == 0) { c.state = kChEmpty; return; }
    if (j.pos >= c.toks.size()) return;
    c.toks.resize(j.pos);
    const Tok& last = c.toks.back();
    c.pIdx.resize(last.pOff + last.pN);
    c.pVal.resize(last.pOff + last.pN);
    if (c.firstDraw > j.pos) c.firstDraw = j.pos;
    ++j.w.cut;
}
} // namespace

// Ends the active chain or recording (ProcessStream at a command a chain cannot hold; the entry points from outside the stream; a token
// that does not match).
void J3n2End(int reason) {
    J2& j = g_j2;
    if (j.mode == j3n2::kOff) return;
    if (j.mode == j3n2::kRec) { RecFinish(); return; }
    Chain& c = *j.c;
    if (j.pos == c.toks.size()) {
        ++j.w.endExact;
        c.fails = 0;
    } else {
        ++j.w.endMid[reason];
        if (c.state == kChReady) Cut();
    }
    Off();
}
void J3n2Flush() { if (g_j3n2On) J3n2End(kEndFlush); }
// A BP / XF register write at the top level while a chain is active: its words may now differ from the recording (D).
void J3n2NoteBp(uint32_t rv) {
    J2& j = g_j2;
    if (j.mode == j3n2::kOff) return;
    const uint32_t reg = rv >> 24;
    if (reg == 0xFE) return; // (a mask: the write it applies to is noted when it comes; a list after it is not patched, see ListCall)
    ++j.w.devWrites;
    if (j.mode == j3n2::kRec) { // (recording: what comes between tokens is the next token's gap)
        GapAdd(reg);
        if (reg >= 0xE0 && reg <= 0xE7) {
            const uint32_t idx = (reg - 0xE0) >> 1;
            for (uint32_t k = 0; k < 4; ++k) { GapAdd(kLzTev + idx * 4 + k); GapAdd(kLzTev + 16 + idx * 4 + k); }
            GapAdd(kLzRaw + ((reg - 0xE0) << 1));
            GapAdd(kLzRaw + ((reg - 0xE0) << 1) + 1);
        }
        return;
    }
    DevAdd(reg, g.bpMask); // (the bits the write can change: the pending mask's)
    if (reg >= 0xE0 && reg <= 0xE7) {
        const uint32_t idx = (reg - 0xE0) >> 1;
        for (uint32_t k = 0; k < 4; ++k) { DevAdd(kLzTev + idx * 4 + k); DevAdd(kLzTev + 16 + idx * 4 + k); }
        DevAdd(kLzRaw + ((reg - 0xE0) << 1));
        DevAdd(kLzRaw + ((reg - 0xE0) << 1) + 1);
    }
}
void J3n2NoteXf(uint32_t addr, uint32_t n) {
    J2& j = g_j2;
    if (j.mode == j3n2::kOff || addr + n <= 0x1000) return;
    ++j.w.devWrites;
    for (uint32_t a = addr; a < addr + n; ++a)
        if (a >= 0x1000 && a < 0x1058 && !LzLive(kLzXf + (a - 0x1000))) {
            if (j.mode == j3n2::kRec) GapAdd(kLzXf + (a - 0x1000));
            else DevAdd(kLzXf + (a - 0x1000));
        }
}

namespace {
// ---- starting
void RecStart(uint32_t id) {
    J2& j = g_j2;
    Chain& c = *ChainOf(id, true);
    c.id = id;
    c.epoch = g_slEpoch;
    c.state = kChRec;
    c.toks.clear();
    c.pIdx.clear();
    c.pVal.clear();
    c.firstDraw = ~0u;
    c.lastV = 0;
    c.hIdx.clear();
    c.hMask.clear();
    c.prefW.assign(kLzWords, 0u);
    c.tpl.Clear();
    j.mode = j3n2::kRec;
    j.c = &c;
    j.pos = 0;
    j.v = 0;
    DevReset();
    std::memset(j.gap, 0, sizeof j.gap);
    g_j3n2On = true;
    ++j.w.recs;
}
// The history check: the lazy words the chain does not write before its first draw are what they were at the recording's start.
// (GXV_J3DNATIVE_HSEED=1) The history's differing bits into D instead: the chain runs from a start that differs, the tokens that write
// what differs taking the normal way until D clears (the same rule as a resynchronisation). Returns the number of differing words.
uint32_t HistorySeed(const Chain& c) {
    uint32_t n = 0;
    for (size_t i = 0; i < c.hIdx.size(); ++i)
        if (const uint32_t d = (*LzPtrG(c.hIdx[i]) ^ LzWordZ(c.start, c.hIdx[i])) & c.hMask[i]) { DevAdd(c.hIdx[i], d); ++n; }
    return n;
}
bool HistoryHolds(const Chain& c) {
    for (size_t i = 0; i < c.hIdx.size(); ++i)
        if ((*LzPtrG(c.hIdx[i]) ^ LzWordZ(c.start, c.hIdx[i])) & c.hMask[i]) return false;
    return true;
}
// Whether the call is the token (list, bytes, entry, bracket fields); false: nothing done. On a match the entry's bookkeeping is the normal
// fast hit's (a tier-B rewrite with the same bytes moves the entry's generation).
bool TokSame(Tok& t, const uint8_t* list, uint32_t nbytes, uint8_t kind, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl) {
    J2& j = g_j2;
    Chain& c = *j.c;
    if (t.kind != kind || t.list != list || t.nbytes != nbytes || c.epoch != g_slEpoch) return false;
    if (kind != kTkPlain && (t.id != id || t.verA != vA || t.verB != vB || t.ext != ext || t.fl != fl)) return false;
    SlEntry& e = *t.e;
    if (!e.usable || !e.live || e.nbytes != nbytes || !(e.delta0 == 2 || e.delta0 == 4)) return false;
    if (CurrentFrame() < e.quietUntil || !J3nListPathOk(nbytes)) return false;
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    if (gen == kNotGuest) return false;
    if (gen != t.gen || gen != e.gen) {
        // (tier B: the page was written since; the bytes are compared, or, GXV_J3DNATIVE_TRUST=1, a bracket's unchanged version vouches
        // for them after its probation and between scrubs)
        ++j.w.tierB;
        const bool vouch = j.trust && kind != kTkPlain && !j.kindBad[kind == kTkDiff ? 1 : 0] && t.probe >= j.probation && ++t.skips < static_cast<uint32_t>(j.scrub);
        if (!vouch) {
            ++j.w.tierBBytes;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.bytes.size() != nbytes || !SameBytes(list, e.bytes.data(), nbytes)) {
                if (kind != kTkPlain) {
                    // (the version is the recorded one and the bytes are not: a writer the game's version bumps missed)
                    ++j.w.missed;
                    if (j.trust) j.kindBad[kind == kTkDiff ? 1 : 0] = true;
                    if (j.mismatchLogged < 16 && port_log_summary) {
                        ++j.mismatchLogged;
                        port_log_summary("j3n: MISMATCH (stage 2: version unchanged, bytes changed: a missed writer) bracket id %u, list %p, %u bytes (%s list)", id,
                                         static_cast<const void*>(list), nbytes, kind == kTkDiff ? "differed" : "material");
                    }
                }
                return false;
            }
            t.skips = 0;
            if (t.probe < 255) ++t.probe;
        }
        e.gen = gen; // (StateListCall's "a write near it, not on it": what the normal call records)
        t.gen = gen;
    }
    if (e.changes) e.changes = 0;
    return true;
}
// The token's patch applied: the register file after it is the normal call's (see the head of the file).
inline void Patch(const Chain& c, const Tok& t) {
    DeltaLive(*t.e);
    const uint16_t* pi = c.pIdx.data() + t.pOff;
    const uint32_t* pv = c.pVal.data() + t.pOff;
    for (uint32_t k = 0; k < t.pN; ++k) *LzPtrG(pi[k]) = pv[k];
    // (the key hash is a XOR over the key registers' terms (KeyHashOf): what the list changes moves it by the same terms whatever the other
    // registers hold, and the derived terms (the TEV range flag, the LOD-bias bits) depend only on words in the token's map)
    g.keyHash ^= t.keyDelta;
    if (t.tevTouch) g.tevOor = t.tevOor;
    if (t.run) {
        uint8_t* r = reinterpret_cast<uint8_t*>(&g.bpDirty);
        for (uint32_t m = t.run; m; m &= m - 1) r[__builtin_ctz(m)] = 1;
    }
    g.tevColDirty |= t.tevCol;
    g.texSlotDirty |= t.texSlot;
    g.bpTexStale |= t.texStale;
    ++g_slHits;
}
// The normal call with what it changed measured (recording and verify): `before` and `fl0` are the state before; the flags it set are left
// in `set` and the register file's flags are the old ones ORed with them, as the plain call leaves them.
void MeasuredCall(const uint8_t* list, uint32_t nbytes, Flags& set) {
    J2& j = g_j2;
    j.maskIn = g.bpMask; // (a mask pending from before the call applies to its first BP write: not the list's own effect)
    LzSave(j.before);
    FlagsSave(j.fl0);
    FlagsClear();
    CallList(list, nbytes);
    FlagsSave(set);
    FlagsOr(j.fl0);
}
// Records the list just called (MeasuredCall). False: it cannot be a token.
bool RecTake(const uint8_t* list, uint32_t nbytes, uint8_t kind, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl, const Flags& set) {
    J2& j = g_j2;
    Chain& c = *j.c;
    if (static_cast<int>(c.toks.size()) >= j.maxTok) return false;
    SlEntry* e = SlLookup(list);
    if (!e || !e->usable || e->nbytes != nbytes || !J3nListPathOk(nbytes) || g.bpMask != 0x00FFFFFFu || j.maskIn != 0x00FFFFFFu) return false;
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    if (gen == kNotGuest) return false;
    const bool compiled = (e->delta0 == 2 || e->delta0 == 4) && e->dp;
    if (compiled && !DeltaTokenOk(*e)) return false; // (writes what the templated runs read live)
    if (!compiled || !e->live || gen != e->gen) {
        // (a list parsed again in this picture (its bytes changed: an animated material's differed list) is not compiled yet. A bracketed one
        // is a variable token: at the replay any list of the holder is applied the normal way and what it writes is followed in D)
        if (kind == kTkPlain || c.toks.empty()) return false;
        for (const SlOp& op : e->ops) // (the same live-read test on its operations)
            if ((op.kind == 1 || op.kind == 5 || op.kind == 6) ) {
                const uint32_t n = op.kind == 1 ? 1u : op.b;
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t reg = (op.kind == 1 ? op.v : e->words[op.v + i]) >> 24;
                    if (reg == 0x20 || reg == 0x21 || reg == 0x22 || reg == 0x59) return false;
                }
            } else if ((op.kind == 3 && op.b + op.a > 0x101A && op.b <= 0x1026) || (op.kind == 7 && op.b >= 0x101A && op.b <= 0x1026)) {
                return false;
            }
        if (c.firstDraw == ~0u) {
            // (in the prefix: what it writes is not known for the next picture's contents; the history must not count on it)
            return false;
        }
        Tok t;
        t.var = 1;
        OpsMap(*e, t.wmap);
        std::memcpy(t.gmap, j.gap, sizeof t.gmap);
        std::memset(j.gap, 0, sizeof j.gap);
        t.kind = kind;
        t.id = id;
        t.list = list;
        t.nbytes = nbytes;
        t.pOff = static_cast<uint32_t>(c.pIdx.size());
        t.pN = 0;
        t.v = ++c.lastV;
        c.toks.push_back(t);
        j.v = t.v;
        ++j.w.varRec;
        return true;
    }
    if (c.toks.empty()) c.start = j.before;
    if (c.firstDraw == ~0u) DeltaWalk(*e, [&c](uint32_t w, uint32_t bits) { c.prefW[w] |= bits; });
    Tok t;
    t.list = list;
    t.e = e;
    t.gen = gen;
    t.nbytes = nbytes;
    t.kind = kind;
    t.id = id; t.verA = vA; t.verB = vB; t.ext = ext; t.fl = fl;
    std::memcpy(t.gmap, j.gap, sizeof t.gmap);
    std::memset(j.gap, 0, sizeof j.gap);
    t.keyHash = g.keyHash;
    t.keyDelta = g.keyHash ^ j.before.keyHash;
    t.tevOor = g.tevOor;
    {
        bool tev = false, lod = false;
        DeltaWalk(*e, [&](uint32_t w, uint32_t) {
            t.wmap[w >> 5] |= 1u << (w & 31);
            if (w >= kLzTev && w < kLzTev + 16) tev = true;
            if (w >= 0x80 && w <= 0xBB) lod = true;
        });
        if (tev) for (uint32_t w = kLzTev; w < kLzTev + 16; ++w) t.wmap[w >> 5] |= 1u << (w & 31);
        if (lod) for (uint32_t m = 0; m < 8; ++m) {
            const uint32_t a = fixes::TexMode0Reg(m), b = fixes::TexMode1Reg(m);
            if ((t.wmap[a >> 5] >> (a & 31) & 1u) | (t.wmap[b >> 5] >> (b & 31) & 1u)) { t.wmap[a >> 5] |= 1u << (a & 31); t.wmap[b >> 5] |= 1u << (b & 31); }
        }
        t.tevTouch = (tev || g.tevOor != j.before.tevOor) ? 1 : 0;
    }
    // (the patch: the words of the write map that the call changed. A word outside the map is not written by the list (the map holds every
    // register its deltas write, all four TEV colour words of a TEV write, both mode words of a texture map's LOD write); verify (=4)
    // compares every lazy word against the patch, so a write outside the map shows as a mismatch there)
    t.pOff = static_cast<uint32_t>(c.pIdx.size());
    for (uint32_t i = 0; i < (kLzWords + 31) / 32; ++i)
        for (uint32_t m = t.wmap[i]; m; m &= m - 1) {
            const uint32_t w = i * 32u + static_cast<uint32_t>(__builtin_ctz(m));
            if (LzLive(w)) continue;
            const uint32_t now = *LzPtrG(w);
            if (now != LzWordZ(j.before, w)) { c.pIdx.push_back(static_cast<uint16_t>(w)); c.pVal.push_back(now); }
        }
    t.pN = static_cast<uint32_t>(c.pIdx.size()) - t.pOff;
    {
        // (the dirty flags of the register part alone: applied once more to the state before the call, the flags cleared, then the state
        // after the call put back. The normal call's flags (`set`) include the eager part's, which depend on the memory it wrote: those are
        // made live at the replay)
        (void)set;
        Lz after;
        LzSave(after);
        Flags fa, reg;
        FlagsSave(fa);
        auto& st = stats::Cur();
        const uint32_t s0 = st.streams, s1 = st.streamBytes;
        LzLoad(j.before);
        FlagsClear();
        if (DeltaCompact(*e)) SlDeltaApplyT<true, 1>(*e); else SlDeltaApplyT<false, 1>(*e);
        FlagsSave(reg);
        LzLoad(after);
        FlagsLoad(fa);
        st.streams = s0;
        st.streamBytes = s1;
        t.run = RunBits(reg.run);
        t.tevCol = reg.tevCol;
        t.texSlot = reg.texSlot;
        t.texStale = reg.texStale;
        const uint32_t* const n = e->dp;
        t.eager = (n[kDXfB] || n[kDCp]) ? 1 : 0;
    }
    if (t.pN || g.keyHash != j.before.keyHash || g.tevOor != j.before.tevOor) ++c.lastV;
    t.v = c.lastV;
    c.toks.push_back(t);
    j.v = t.v;
    return true;
}
// Verify (=4): what the normal call just did (MeasuredCall) against the token's patch: every lazy word, the key hash, the range flag, and
// the dirty flags the call set must be among the token's.
void VerifyPatch(const Chain& c, const Tok& t, const Flags& set) {
    J2& j = g_j2;
    ++j.w.verPatch;
    bool ok = g.keyHash == (j.before.keyHash ^ t.keyDelta) && g.tevOor == (t.tevTouch ? t.tevOor : j.before.tevOor) && (RunBits(set.run) & ~(t.run | (t.eager ? kEagerRun : 0))) == 0 &&
              (set.tevCol & ~t.tevCol) == 0 &&
              (set.texSlot & ~t.texSlot) == 0 && (set.texStale & ~t.texStale) == 0;
    uint32_t bad = ~0u;
    {
        const uint16_t* pi = c.pIdx.data() + t.pOff;
        const uint32_t* pv = c.pVal.data() + t.pOff;
        uint32_t k = 0;
        for (uint32_t i = 0; i < kLzWords && ok; ++i) {
            if (LzLive(i)) continue;
            const uint32_t want = (k < t.pN && pi[k] == i) ? pv[k++] : LzWordZ(j.before, i);
            if (*LzPtrG(i) != want) { ok = false; bad = i; }
        }
    }
    if (ok) return;
    ++j.w.verPatchBad;
    j.c->state = kChEmpty; // (recorded again)
    if (j.mismatchLogged < 16 && port_log_summary) {
        ++j.mismatchLogged;
        port_log_summary("j3n: MISMATCH (stage 2 patch) chain %u, token %u of %u (list %p, %u bytes): lazy word %u, key hash %s, flags %s", c.id, j.pos,
                         static_cast<unsigned>(c.toks.size()), static_cast<const void*>(t.list), t.nbytes, bad, g.keyHash == t.keyHash ? "same" : "different",
                         (RunBits(set.run) & ~(t.run | (t.eager ? kEagerRun : 0))) == 0 ? "within" : "beyond the token's");
    }
}
// Whether the list just called the normal way is now one a token can be (compiled, unchanged since it was parsed).
inline bool StableNow(const uint8_t* list, uint32_t nbytes) {
    const SlEntry* e = SlLookup(list);
    if (!e || !e->usable || !e->live || e->nbytes != nbytes || !(e->delta0 == 2 || e->delta0 == 4) || !e->dp) return false;
    return GuestWriteGeneration(list, nbytes) == e->gen;
}
// The chain is recorded again from the current position (the register file is the recorded one there: D empty): what follows replaces the
// rest; the templates of the lazy-state versions after the position go.
void ResumeRec() {
    J2& j = g_j2;
    Chain& c = *j.c;
    c.toks.resize(j.pos);
    const uint32_t keep = j.pos ? c.toks.back().pOff + c.toks.back().pN : 0u;
    c.pIdx.resize(keep);
    c.pVal.resize(keep);
    c.lastV = j.pos ? c.toks.back().v : 0u;
    j3n2::TplSet old;
    std::swap(old, c.tpl);
    for (const j3n2::Tpl& t : old.t)
        if (t.k.v <= c.lastV) { j3n2::Tpl* n = c.tpl.Add(t.k); const uint32_t img = static_cast<uint32_t>(c.tpl.img.size()); *n = t; n->img = img; c.tpl.img.push_back(old.img[t.img]); }
    c.state = kChRec;
    j.mode = j3n2::kRec;
    DevReset();
    std::memset(j.gap, 0, sizeof j.gap);
    ++j.w.resumed;
}
// A list call while a chain is active (or none, for a bracket): bracketed (kind 0 / 1, with its fields) or plain (kind 2).
void ListCall(const uint8_t* list, uint32_t nbytes, uint8_t kind, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl) {
    J2& j = J2s();
    const bool timed = port_host_time_ns && ((++j.tick & 15u) == 0);
    const int64_t t0 = timed ? port_host_time_ns() : 0;
    for (int pass = 0; pass < 2; ++pass) {
        if (j.mode == j3n2::kTpl || j.mode == j3n2::kShadow) {
            Chain& c = *j.c;
        again:
            if (j.pos < c.toks.size()) {
                Tok& t = c.toks[j.pos];
                DevAddMap(t.gmap); // (what the recording's gap before it changed: may differ now)
                if (t.var) {
                    // (a variable token: the holder's list, whatever its contents, applied the normal way; its words may now differ)
                    if (kind != kTkPlain && t.kind == kind && t.id == id) {
                        if (j.resume && DevEmpty() && j.pos >= c.firstDraw && c.state == kChReady && StableNow(list, nbytes)) {
                            // (its list is a stable compiled one now (a picture's first parses, a material changed for good): recorded again from here)
                            ResumeRec();
                            continue;
                        }
                        CallList(list, nbytes);
                        if (const SlEntry* e = CurEntry(list, nbytes)) {
                            DevAddOps(*e);
                            DevAddMap(t.wmap);
                            ++j.w.varLists;
                            ++j.pos;
                            j.v = t.v;
                            return;
                        }
                        J3n2End(kEndMismatch); // (not a state list: what it wrote is not known)
                        ++j.w.normalLists;
                        return;
                    }
                } else if (TokSame(t, list, nbytes, kind, id, vA, vB, ext, fl)) {
                    if (j.pos < c.firstDraw) {
                        // (the chain's prefix: applied the normal way. The history check did not cover the words it writes, whose old values a
                        // patch takes as the recorded ones; after the prefix they are the recorded ones, and every patch holds. Unless a mask
                        // was pending from the top level: then its first write took it and what it writes may differ)
                        const bool masked = g.bpMask != 0x00FFFFFFu;
                        CallList(list, nbytes);
                        if (masked) DevAddMap(t.wmap);
                        ++j.w.prefLists;
                    } else if (j.mode == j3n2::kTpl && DevClearFor(t.wmap) && g.bpMask == 0x00FFFFFFu) {
                        Patch(c, t);
                        ++j.w.patched;
                        if (timed) { ++j.w.sampLazyN; j.w.sampLazyNs += static_cast<uint64_t>(port_host_time_ns() - t0); }
                    } else if (j.mode == j3n2::kShadow && DevClearFor(t.wmap) && g.bpMask == 0x00FFFFFFu) {
                        Flags set;
                        MeasuredCall(list, nbytes, set);
                        VerifyPatch(c, t, set);
                        ++j.w.patched;
                    } else {
                        // (D not empty, or a mask pending from the top level: the normal call; it rewrites what it writes whole, unless its first
                        // write took the pending mask: then what it writes may differ)
                        const bool masked = g.bpMask != 0x00FFFFFFu;
                        CallList(list, nbytes);
                        if (masked) DevAddMap(t.wmap);
                        else DevClearEntry(*t.e);
                        ++j.w.devLists;
                    }
                    ++j.pos;
                    j.v = t.v;
                    j.inserted = 0;
                    return;
                }
                if (!t.var && kind != kTkPlain && t.kind == kind && t.id == id) {
                    if (j.resume && DevEmpty() && j.pos >= c.firstDraw && c.state == kChReady && StableNow(list, nbytes)) {
                        ResumeRec(); // (other contents that are stable now: recorded again from here)
                        continue;
                    }
                    // (the same holder with other contents: an animated list. The normal call; what it and the recorded one write may differ now)
                    CallList(list, nbytes);
                    if (const SlEntry* e = CurEntry(list, nbytes)) {
                        DevAddOps(*e);
                        DevAddMap(t.wmap);
                        ++j.w.varLists;
                        ++j.pos;
                        j.v = t.v;
                        return;
                    }
                    // (not a compiled list: what it wrote is not known; the chain ends after the call)
                    J3n2End(kEndMismatch);
                    ++j.w.normalLists;
                    return;
                }
            }
            if (j.pos < c.toks.size() && c.state == kChReady && j.pos >= c.firstDraw) {
                // ---- resynchronisation (the register file is real; what may differ from the recording goes into D)
                // (a) a recorded token a little further on is this call (objects went away, or changed places): the tokens before it are
                // skipped, what they write may differ now;
                const uint32_t look = static_cast<uint32_t>(std::min<size_t>(c.toks.size(), j.pos + static_cast<size_t>(j.look) + 1));
                for (uint32_t k = j.pos + 1; k < look; ++k) {
                    const Tok& u = c.toks[k];
                    if (u.var || u.kind != kind || u.list != list || u.nbytes != nbytes || (kind != kTkPlain && u.id != id)) continue;
                    for (uint32_t q = j.pos; q < k; ++q) { DevAddMap(c.toks[q].wmap); DevAddMap(c.toks[q].gmap); }
                    j.w.skipped += k - j.pos;
                    ++j.w.resyncs;
                    j.pos = k;
                    j.inserted = 0;
                    goto again;
                }
                // (b) else a list that was not there (an object came): the normal call, what it writes may differ now
                if (j.inserted < static_cast<uint32_t>(j.look) * 4u) {
                    CallList(list, nbytes);
                    if (const SlEntry* e = CurEntry(list, nbytes)) {
                        DevAddOps(*e);
                        ++j.inserted;
                        ++j.w.insertedLists;
                        return;
                    }
                    J3n2End(kEndMismatch); // (not a state list: what it wrote is not known)
                    ++j.w.normalLists;
                    return;
                }
            }
            // (the recorded end, or another list: the chain ends here; the call is then taken as if none were active)
            J3n2End(j.pos == c.toks.size() ? kEndCmd : kEndMismatch);
            continue;
        }
        if (j.mode == j3n2::kRec) {
            // (a recording goes on through the start of another chain: that one stays as it is, for a picture that starts there)
            Flags set;
            MeasuredCall(list, nbytes, set);
            ++j.w.normalLists;
            if (!RecTake(list, nbytes, kind, id, vA, vB, ext, fl, set)) {
                ++j.w.recNoList;
                RecFinish(); // (the list is applied, and not a token: the chain ends before it, which its last token's patch describes)
            }
            if (timed) { ++j.w.sampN; j.w.sampNs += static_cast<uint64_t>(port_host_time_ns() - t0); }
            return;
        }
        break;
    }
    // ---- no chain active
    if (kind == kTkMat && J3nMode() >= 4 && J2Usable() && g.bpMask == 0x00FFFFFFu) {
        Chain* c = ChainOf(id, false);
        const uint64_t frame = CurrentFrame();
        const bool mine = c && c->id == id;
        if (mine && frame < c->backoffUntil) {
            ++j.w.backoff;
        } else {
            if (mine && c->state == kChReady && c->epoch == g_slEpoch) {
                bool held = true;
                if (j.hseed) {
                    DevReset();
                    if (HistorySeed(*c)) ++j.w.startNoH;
                } else {
                    held = HistoryHolds(*c);
                }
                if (held) {
                    j.mode = J3nMode() == 5 ? j3n2::kTpl : j3n2::kShadow;
                    j.c = c;
                    j.pos = 0;
                    j.v = 0;
                    if (!j.hseed) DevReset();
                    g_j3n2On = true;
                    Tok& t = c->toks[0];
                    if (TokSame(t, list, nbytes, kind, id, vA, vB, ext, fl)) {
                        ++j.w.starts;
                        if (c->firstDraw > 0) { // (the prefix: the normal way, see ListCall)
                            CallList(list, nbytes);
                            ++j.w.prefLists;
                        } else if (!DevClearFor(t.wmap)) { // (a start that differs where the token writes: the normal call)
                            CallList(list, nbytes);
                            DevClearEntry(*t.e);
                            ++j.w.devLists;
                        } else if (j.mode == j3n2::kTpl) {
                            Patch(*c, t);
                            ++j.w.patched;
                        } else {
                            Flags set;
                            MeasuredCall(list, nbytes, set);
                            VerifyPatch(*c, t, set);
                            ++j.w.patched;
                        }
                        j.pos = 1;
                        j.v = t.v;
                        return;
                    }
                    ++j.w.startNoTok;
                    Off();
                } else {
                    ++j.w.startNoH;
                }
                if (++c->fails >= 4) { c->backoffUntil = frame + static_cast<uint64_t>(j.backoffPics); c->fails = 0; }
            }
            if (!(mine && frame < c->backoffUntil)) {
                RecStart(id);
                Flags set;
                MeasuredCall(list, nbytes, set);
                ++j.w.normalLists;
                if (!RecTake(list, nbytes, kind, id, vA, vB, ext, fl, set)) {
                    ++j.w.recNoList;
                    j.c->state = kChEmpty;
                    Off();
                }
                return;
            }
        }
    }
    CallList(list, nbytes);
    ++j.w.normalLists;
    if (timed) { ++j.w.sampN; j.w.sampNs += static_cast<uint64_t>(port_host_time_ns() - t0); }
}
} // namespace

namespace j3n2 {
// (vgx_j3n2.h) A templated run that cannot be: the register file is real here, so the caller only resolves it the normal way.
void Bail(int) {}
} // namespace j3n2

void J3n2Bracket(const uint8_t* list, uint32_t nbytes, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl) {
    ListCall(list, nbytes, (fl & 1u) ? kTkDiff : kTkMat, id, vA, vB, ext, fl);
}
void J3n2Plain(const uint8_t* list, uint32_t nbytes) {
    // (a plain call that is not a compiled state list ends the chain first: its effect is not known)
    J2& j = g_j2;
    if (j.mode != j3n2::kOff) {
        const SlEntry* e = SlLookup(list);
        if (!e || !e->usable || !(e->delta0 == 2 || e->delta0 == 4) || e->nbytes != nbytes || !J3nListPathOk(nbytes)) {
            J3n2End(j.mode == j3n2::kRec ? kEndCmd : (j.c && j.pos == j.c->toks.size() ? kEndCmd : kEndMismatch));
            CallList(list, nbytes);
            return;
        }
    }
    ListCall(list, nbytes, kTkPlain, 0, 0, 0, 0, 0);
}
void J3n2Shape(const uint8_t* list, uint32_t nbytes, uint32_t handle) {
    J2& j = g_j2;
    Chain& c = *j.c;
    ++j.w.shapes;
    if (j.mode == j3n2::kRec) {
        if (c.firstDraw == ~0u) {
            c.firstDraw = static_cast<uint32_t>(c.toks.size());
            HistoryFrom(c);
        }
        if (j.tpl) {
            j.ctx = j3n2::DrawCtx{j3n2::kRec, &c.tpl, j.v};
            j3n2::g_draw = &j.ctx;
        }
        ShapeCall(list, nbytes, handle);
        j3n2::g_draw = nullptr;
        return;
    }
    if (j.pos < c.firstDraw) { // (a draw before the recorded first one: the history check did not cover it)
        J3n2End(kEndMismatch);
        CallListRetained(list, nbytes, handle);
        return;
    }
    if (!DevEmpty() || !j.tpl) {
        if (!DevEmpty()) ++j.w.devShapes;
        ShapeCall(list, nbytes, handle);
        return;
    }
    j.ctx = j3n2::DrawCtx{j.mode, &c.tpl, j.v};
    j3n2::g_draw = &j.ctx;
    if (j.mode == j3n2::kShadow) {
        ShapeCall(list, nbytes, handle);
    } else if (!CallListRetainedHit(list, nbytes, handle)) {
        // (not a record hit: the normal call, its runs' templates captured)
        ++j.w.notHit;
        j.ctx.mode = j3n2::kRec;
        ShapeCall(list, nbytes, handle, false);
    }
    j3n2::g_draw = nullptr;
}
void J3n2SceneGone() {
    // (the state-list store is about to free its entries: the tokens point at them)
    J2& j = g_j2;
    if (!j.init) return;
    J3n2End(kEndScene);
    for (std::unique_ptr<Chain>& c : j.chains)
        if (c) { c->state = kChEmpty; c->toks.clear(); c->pIdx.clear(); c->pVal.clear(); c->tpl.Clear(); }
}
void J3n2PictureEnd() {
    J2& j = J2s();
    J3n2End(kEndFlush);
    Ctr2& w = j.w;
    ++w.pics;
    if (w.pics < 300) return;
    const double n = static_cast<double>(w.pics);
    const j3n2::DrawCtr& d = j3n2::g_dctr;
    static j3n2::DrawCtr d0;
    const double tpl = static_cast<double>(d.templated - d0.templated);
    const double bails = static_cast<double>(d.bails[0] + d.bails[1] + d.bails[2] - d0.bails[0] - d0.bails[1] - d0.bails[2]);
    // (estimated saving: the patched lists at the sampled normal list cost less a patch's, the templated runs at 0.6 of a list call each (the
    // host model's ratio of a static run's resolution to a material list's replay); 0 without samples)
    const double perList = w.sampN ? static_cast<double>(w.sampNs) / w.sampN / 1000.0 : 0.0, perLazy = w.sampLazyN ? static_cast<double>(w.sampLazyNs) / w.sampLazyN / 1000.0 : 0.0;
    const double savedMs = (static_cast<double>(w.patched) * (perList - perLazy) + tpl * perList * 0.6) / n / 1000.0;
    if (port_log_summary) {
        port_log_summary("j3n: stage 2 (GXV_J3DNATIVE=%d, %llu pictures) per picture: chains started %.1f (history differed %.1f, first list %.1f, backed off %.1f), "
                         "recorded %.1f (finished %.1f, ended by a list that cannot be a token %.1f); ends: as recorded %.1f, elsewhere %.1f (command %.1f, list %.1f, "
                         "outside %.1f, scene %.1f), cut %.1f; lists: patched %.1f (tier B %.1f, bytes compared %.1f), prefix %.1f, animated %.1f, normal while deviated %.1f, normal %.1f; "
                         "register writes between tokens %.1f; shapes in chains %.1f (deviated %.1f, not a record hit %.1f); runs templated %.1f, resolved the normal way %.1f "
                         "(no template %.1f, texture %.1f, not usable %.1f), captured %.1f; blocks sent by templates: projection %.1f, light %.1f, TEV %.1f; "
                         "verify: patches %llu (mismatching %llu), templates %llu (mismatching %llu); missed writers %llu; sampled list call %.2f us, patch %.2f us; "
                         "estimated front-end saving %.3f ms a picture",
                         J3nMode(), static_cast<unsigned long long>(w.pics), w.starts / n, w.startNoH / n, w.startNoTok / n, w.backoff / n, w.recs / n, w.recDone / n,
                         w.recNoList / n, w.endExact / n, (w.endMid[0] + w.endMid[1] + w.endMid[2] + w.endMid[3]) / n, w.endMid[0] / n, w.endMid[1] / n, w.endMid[2] / n,
                         w.endMid[3] / n, w.cut / n, w.patched / n, w.tierB / n, w.tierBBytes / n, w.prefLists / n, w.varLists / n, w.devLists / n, w.normalLists / n, w.devWrites / n,
                         w.shapes / n, w.devShapes / n, w.notHit / n, tpl / n, bails / n, (d.bails[0] - d0.bails[0]) / n, (d.bails[1] - d0.bails[1]) / n,
                         (d.bails[2] - d0.bails[2]) / n, (d.captured - d0.captured) / n, (d.projSent - d0.projSent) / n, (d.lightSent - d0.lightSent) / n,
                         (d.tevSent - d0.tevSent) / n, static_cast<unsigned long long>(j.tot.verPatch + w.verPatch), static_cast<unsigned long long>(j.tot.verPatchBad + w.verPatchBad),
                         static_cast<unsigned long long>(d.checked), static_cast<unsigned long long>(d.bad), static_cast<unsigned long long>(j.tot.missed + w.missed),
                         perList, perLazy, savedMs);
    }
    d0 = d;
    Ctr2& t = j.tot;
    t.pics += w.pics; t.starts += w.starts; t.startNoH += w.startNoH; t.startNoTok += w.startNoTok; t.recs += w.recs; t.recDone += w.recDone; t.recNoList += w.recNoList;
    t.backoff += w.backoff; t.endExact += w.endExact; for (int i = 0; i < kEndReasons; ++i) t.endMid[i] += w.endMid[i];
    t.cut += w.cut; t.patched += w.patched; t.prefLists += w.prefLists; t.tierB += w.tierB; t.tierBBytes += w.tierBBytes; t.normalLists += w.normalLists; t.devLists += w.devLists;
    t.varLists += w.varLists; t.devWrites += w.devWrites; t.shapes += w.shapes; t.devShapes += w.devShapes; t.notHit += w.notHit;
    t.verPatch += w.verPatch; t.verPatchBad += w.verPatchBad; t.missed += w.missed;
    j.w = Ctr2{};
}
// (the fixture's line) totals since the start
void J3n2Counters(uint64_t out[24]) {
    const J2& j = g_j2;
    Ctr2 t = j.tot;
    const Ctr2& w = j.w;
    t.starts += w.starts; t.startNoH += w.startNoH; t.recs += w.recs; t.endExact += w.endExact; t.patched += w.patched; t.normalLists += w.normalLists;
    t.devLists += w.devLists; t.varLists += w.varLists; t.verPatch += w.verPatch; t.verPatchBad += w.verPatchBad; t.missed += w.missed; t.cut += w.cut;
    t.notHit += w.notHit; t.devShapes += w.devShapes;
    uint64_t mid = 0;
    for (int i = 0; i < kEndReasons; ++i) mid += t.endMid[i] + w.endMid[i];
    const j3n2::DrawCtr& d = j3n2::g_dctr;
    out[0] = t.starts; out[1] = t.startNoH; out[2] = t.recs; out[3] = t.endExact; out[4] = mid; out[5] = t.patched; out[6] = t.normalLists; out[7] = d.templated;
    out[8] = d.bails[0] + d.bails[1] + d.bails[2]; out[9] = d.captured; out[10] = t.devLists; out[11] = t.varLists; out[12] = d.checked; out[13] = d.bad;
    out[14] = t.verPatch; out[15] = t.verPatchBad; out[16] = t.missed; out[17] = t.cut; out[18] = t.devShapes; out[19] = t.notHit; out[20] = d.projSent; out[21] = d.lightSent;
    out[22] = d.tevSent; out[23] = d.refused;
}
#endif
