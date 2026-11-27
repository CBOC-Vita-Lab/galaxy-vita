// GXV_STREAM_CENSUS=<n> (lighter25, 11 Nov; default 0; census only: it changes no state, no draw, no command buffer). Included into
// vgx_state.cpp, before ProcessStream. What the stream sent to the front end is made of, and where the player's time in it goes, for one
// picture in n (the game decides which: platform/src/port/perf.cpp port_stream_census_*; the picture after a display copy at which the
// game writes the BEGIN marker, decomp GXFrameBuf.c GXCopyDisp, is counted from its first byte to its own display copy).
//
// The game's markers (BP 0xFF writes, like every PORT_GX_MARK_*, written only in a counted picture):
//   kind 21 + payload 0xFFFF   BEGIN: this picture is counted (ignored with GXV_STREAM_CENSUS=0 here)
//   kind 21 + payload id       the source from here on: a draw list of SceneFunction.cpp's CategoryList (row: 0x0XX execute(DrawType),
//                              0x1XX drawOpa(DrawBufferType), 0x2XX drawXlu, 0x30X entry), a part of GameScene::draw (0x400 + PORT_DP_*),
//                              0x7FFE nothing (between them). Nested lists re-send the outer id when they end.
//   kind 22 + payload          a J3D material packet's draw begins (J3DMatPacket::draw): bits 0-11 the model data's census id
//                              (perf.cpp: names from ResourceHolder's .bdl / .bmd file names), bits 12-15 its size code (below)
//   kind 23                    that packet's draw ends
//   size code: 0 not known (no actor clipping sphere this turn), 1..8: the projected diameter of the actor's clipping sphere (the one
//   ClippingActorHolder judges) in game picture lines (456 lines: the picture the game projects for; the console's rung multiplies it,
//   e.g. 348 / 456 at 608x348): 1 < 1, 2 < 2, 3 < 4, 4 < 8, 5 < 16, 6 < 32, 7 < 64, 8 >= 64.
//
// In a counted picture the top-level stream is parsed by ProcessStreamT<true>: the same calls in the same order as ProcessStreamT<false>
// (the census only reads the register file before a write and counter values around a call), with the BP no-op burst loop left out (each
// command is then counted; the burst only skips writes WriteBP would not have made either). Per picture:
//   (a) top-level bytes and commands by kind (pad, bp, cp, xf, indexed matrix loads, list calls, J3D shape calls (0x59), particle commands
//       (0x58), inline draws, markers, j3n brackets, metric no-ops, other); the sum of the kinds is checked against the bytes parsed;
//       register writes by group, "changed nothing" = the register (or every word of the XF block / indexed load) already held the value;
//       the writes inside called lists are counted by a walk of the list before the call (the same test, against the register file and
//       what the list itself wrote before); list calls by what the front end did (tiny parsed / tiny state list / state-list replay /
//       state list parsed / geometry cache hit / build / bypassed / other) with their content bytes;
//   (b) by source (marker id) and by model: bytes, inline-draw bytes, shape calls and the player's wall time in the parse between markers
//       (port_host_time_ns: 1 us on the console; the sums are exact, single intervals are not);
//   (c) per J3D shape call: the register writes since the shape call before it (top level and in the lists called), and how many of them
//       changed nothing; a histogram;
//   (d) by size code: material packets drawn, shape calls, bytes and time.
// Lines ("stream-census:", every GXV_STREAM_CENSUS_PRINT (default 5) counted pictures, averages per counted picture).
// Cost: off, ProcessStream's dispatch tests one flag per call, and a changed top-level BP write one more; the game writes nothing. On, a
// counted picture costs the walk of its called lists (~ list bytes), two counter reads per call and a clock read per marker (a few hundred
// a picture); the pictures in between cost nothing more. Needs GXV_FE_TWOCORE=0 and GXV_J3DNATIVE<4 (the census stays off otherwise).
extern bool g_j3n2On;
namespace sc {
#if !defined(VGX_CLONE_B)
enum Kind : uint8_t { kPad, kBp, kCp, kXf, kIdx, kCall, kShape, kFx, kDraw, kMark, kJ3n, kNop, kOther, kKinds };
const char* const kKindName[kKinds] = {"pad", "bp", "cp", "xf", "idx-load", "list-call", "shape-call", "fx-cmd", "inline-draw", "marker", "j3n", "nop", "other"};
enum Grp : uint8_t {
    gTevStage, gTevColor, gTex, gTexCoord, gPe, gInd, gGen, gFog, gMask, gTrig, gBpOther,
    gCpVcd, gCpArr, gCpMtxIdx, gCpOther,
    gXfPos, gXfTexMtx, gXfNrm, gXfPost, gXfLight, gXfMtxIdx, gXfTexgen, gXfChan, gXfView, gXfOther,
    gIdxPos, gIdxNrm, gIdxTex, gIdxLight, kGroups
};
const char* const kGrpName[kGroups] = {"tev-stage", "tev-colour", "tex", "texcoord-scale", "pixel", "indirect", "genmode", "fog", "bp-mask", "trigger", "bp-other",
                                       "vtx-desc/fmt", "array-base/stride", "cp-mtxidx", "cp-other",
                                       "pos-mtx", "tex-mtx", "nrm-mtx", "post-mtx", "lights", "xf-mtxidx", "texgen", "chan-colour/ctrl", "viewport/proj", "xf-other",
                                       "idx-pos", "idx-nrm", "idx-tex", "idx-light"};
enum Call : uint8_t { cTinyParse, cTinyState, cStateHit, cStateParse, cGeoHit, cGeoBuild, cBypass, cOther, kCalls };
const char* const kCallName[kCalls] = {"tiny-parsed", "tiny-statelist", "statelist-replay", "statelist-parsed", "geo-hit", "geo-build", "bypassed", "other"};
constexpr int kSizeCodes = 9; // 0 unknown, 1..8 (see above)
const char* const kSizeName[kSizeCodes] = {"?", "<1", "<2", "<4", "<8", "<16", "<32", "<64", ">=64"};
constexpr int kHist = 7;
const char* const kHistName[kHist] = {"0", "1-2", "3-4", "5-8", "9-16", "17-32", "33+"};
constexpr uint32_t kSrcNone = 0x7FFE, kSrcFirst = 0x7FFF;
constexpr int kModels = 4096, kSources = 96;

struct Src { uint32_t id; uint64_t bytes, drawBytes, fxBytes, shapes, calls, writes, same, ns; };
struct Mdl { uint64_t bytes, drawBytes, shapes, packets, ns; uint8_t code; bool touched; };
struct Win {
    uint64_t pics = 0, bytes = 0, parsed = 0, cmds = 0;
    uint64_t kb[kKinds] = {}, kc[kKinds] = {};
    uint64_t rw[kGroups][2][2] = {}; // [group][0 top / 1 in lists][0 writes / 1 changed nothing]
    uint64_t cn[kCalls] = {}, cb[kCalls] = {};
    uint64_t shapes = 0, shapeW = 0, shapeSame = 0, hist[kHist] = {}, histSame[kHist] = {};
    uint64_t ns = 0, drawVerts = 0;
    uint64_t szPackets[kSizeCodes] = {}, szShapes[kSizeCodes] = {}, szBytes[kSizeCodes] = {}, szNs[kSizeCodes] = {};
    uint64_t markers = 0;
    Src src[kSources] = {};
    int nSrc = 0;
    uint64_t srcOverflow = 0;
};
// (the player's thread only)
bool g_on = false;             // this picture is counted (ProcessStream dispatches on it)
int g_every = -1;              // GXV_STREAM_CENSUS (read at the first BEGIN)
int g_print = 5;
Win* g_w = nullptr;
Mdl* g_m = nullptr;            // [kModels]
uint32_t g_curSrc = kSrcFirst;
int g_curModel = -1;
uint8_t g_curCode = 0;
int64_t g_last = 0;
bool g_clockRunning = false;
uint64_t g_wSince = 0, g_sameSince = 0; // register writes since the last shape call
uint64_t g_picBytes = 0;
uint64_t g_seen = 0;           // counted pictures in all (for the line)

inline int64_t Now() { return port_host_time_ns ? port_host_time_ns() : 0; }
int Every() {
    if (g_every < 0) {
        const long v = EnvInt("GXV_STREAM_CENSUS", 0);
        g_every = v < 0 ? 0 : static_cast<int>(v);
        const long pr = EnvInt("GXV_STREAM_CENSUS_PRINT", 5);
        g_print = pr < 1 ? 1 : static_cast<int>(pr);
    }
    return g_every;
}
Src* SrcOf(uint32_t id) {
    Win& w = *g_w;
    for (int i = 0; i < w.nSrc; ++i) if (w.src[i].id == id) return &w.src[i];
    if (w.nSrc < kSources) { Src& s = w.src[w.nSrc++]; s = Src{}; s.id = id; return &s; }
    ++w.srcOverflow;
    return nullptr;
}
// the clock: the interval since the last tick goes to the current source, model and size code
void Tick() {
    if (!g_clockRunning) return;
    const int64_t t = Now();
    const uint64_t ns = t > g_last ? static_cast<uint64_t>(t - g_last) : 0;
    g_last = t;
    g_w->ns += ns;
    if (Src* s = SrcOf(g_curSrc)) s->ns += ns;
    if (g_curModel >= 0) { g_m[g_curModel].ns += ns; g_w->szNs[g_curCode] += ns; }
}
void ClockStart() { g_last = Now(); g_clockRunning = true; }
void ClockStop() { Tick(); g_clockRunning = false; }

// ---- register groups and the "changed nothing" tests
inline Grp BpGroup(uint32_t reg) {
    if (reg >= 0xE0 && reg <= 0xE7) return gTevColor;
    if ((reg >= 0xC0 && reg <= 0xDF) || (reg >= 0x28 && reg <= 0x2F) || (reg >= 0xF6 && reg <= 0xFD)) return gTevStage;
    if (reg >= 0x80 && reg <= 0xBB) return gTex;
    if (reg >= 0x30 && reg <= 0x3F) return gTexCoord;
    if ((reg >= 0x06 && reg <= 0x0D) || (reg >= 0x10 && reg <= 0x1F) || (reg >= 0x25 && reg <= 0x27)) return gInd;
    if (reg == 0x00) return gGen;
    if (reg >= 0xEE && reg <= 0xF2) return gFog;
    if (reg == 0xE8 || (reg >= 0xE9 && reg <= 0xED)) return gFog;
    if (reg == 0xFE) return gMask;
    if (reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52 || reg == 0x65 || reg == 0xFF) return gTrig;
    if ((reg >= 0x20 && reg <= 0x22) || (reg >= 0x40 && reg <= 0x44) || reg == 0x59 || reg == 0xF3) return gPe;
    return gBpOther;
}
inline Grp CpGroup(uint32_t reg) {
    if (reg == 0x50 || reg == 0x60 || (reg >= 0x70 && reg <= 0x97)) return gCpVcd;
    if (reg >= 0xA0 && reg <= 0xBF) return gCpArr;
    if (reg == 0x30 || reg == 0x40) return gCpMtxIdx;
    return gCpOther;
}
inline Grp XfGroup(uint32_t a) {
    if (a < 0x78) return gXfPos;
    if (a < 0x100) return gXfTexMtx;
    if (a >= 0x400 && a < 0x460) return gXfNrm;
    if (a >= 0x500 && a < 0x600) return gXfPost;
    if (a >= 0x600 && a < 0x680) return gXfLight;
    if (a == 0x1018 || a == 0x1019) return gXfMtxIdx;
    if (a == 0x103F || (a >= 0x1040 && a <= 0x1047) || (a >= 0x1050 && a <= 0x1057)) return gXfTexgen;
    if (a >= 0x1009 && a <= 0x1011) return gXfChan;
    if (a >= 0x101A && a <= 0x1026) return gXfView;
    return gXfOther;
}
// the XF word the register file holds at `a` (false: an address the front end does not keep)
inline bool XfWord(uint32_t a, uint32_t& w) {
    if (a >= 0x1000) { if (a < 0x1058) { w = g.xfReg[a - 0x1000]; return true; } return false; }
    if (a < 0x100) { w = f2u(g.xfPos[a >> 2][a & 3]); return true; }
    if (a >= 0x400 && a < 0x460) { const uint32_t o = a - 0x400; w = f2u(g.xfNrm[o / 3][o % 3]); return true; }
    if (a >= 0x500 && a < 0x600) { const uint32_t o = a - 0x500; w = f2u(g.xfPost[o >> 2][o & 3]); return true; }
    if (a >= 0x600 && a < 0x680) { const uint32_t o = a - 0x600; w = g.xfLight[o >> 4][o & 15]; return true; }
    return false;
}
// a BP write that would change nothing (the register or TEV colour half holds the value; mask, triggers and markers never count)
inline bool BpSame(uint32_t rv, uint32_t cur, uint32_t mask) {
    const uint32_t nv = (cur & ~mask) | (rv & mask & 0x00FFFFFFu);
    return nv == cur;
}
inline uint32_t BpCur(uint32_t reg, uint32_t rv) { // (the value a write of rv is compared with: the TEV colour half it addresses)
    if (reg >= 0xE0 && reg <= 0xE7) return g.tevRaw[((reg - 0xE0) << 1) | ((rv >> 23) & 1u)];
    return g.bp[reg];
}
inline bool CpCur(uint32_t reg, uint32_t& cur) {
    if (reg >= 0xA0 && reg <= 0xBF) { cur = (reg & 0x10) ? g.cpStride[reg & 0xF] : g.cpBase[reg & 0xF]; return true; }
    if (reg == 0x30) { cur = g.matIdxA; return true; }
    if (reg == 0x40) { cur = g.matIdxB; return true; }
    const int slot = CpRawSlot(static_cast<uint8_t>(reg));
    if (slot >= 0 && ((g.cpRawValid >> slot) & 1u)) { cur = g.cpRaw[slot]; return true; }
    return false;
}
inline uint32_t CpVal(uint32_t reg, uint32_t v) { return (reg >= 0xB0 && reg <= 0xBF) ? (v & 0xFF) : v; }

inline void Write(Grp gr, int level, bool same) {
    ++g_w->rw[gr][level][0];
    if (same) ++g_w->rw[gr][level][1];
    if (gr != gMask && gr != gTrig) { ++g_wSince; if (same) ++g_sameSince; }
    if (Src* s = SrcOf(g_curSrc)) { ++s->writes; if (same) ++s->same; }
}
// ---- top level (ProcessStreamT<true>): the tests before the real write
inline void TopBp(uint32_t rv) {
    const uint32_t reg = rv >> 24;
    if (reg == 0xFF) return; // (a marker: counted as a kind, not a register)
    const Grp gr = BpGroup(reg);
    const bool same = gr != gMask && gr != gTrig && BpSame(rv, BpCur(reg, rv), g.bpMask);
    Write(gr, 0, same);
}
inline void TopCp(uint8_t reg, uint32_t v) {
    uint32_t cur = 0;
    const bool same = CpCur(reg, cur) && cur == CpVal(reg, v);
    Write(CpGroup(reg), 0, same);
}
inline void TopXf(uint32_t addr, uint32_t n, const uint8_t* be) {
    bool same = true;
    for (uint32_t i = 0; i < n && same; ++i) { uint32_t cur; same = XfWord(addr + i, cur) && cur == be32(be + i * 4); }
    Write(XfGroup(addr), 0, same);
}
inline void TopIdx(int which, uint32_t addr, uint32_t n, const uint8_t* src, bool le) {
    bool same = src != nullptr;
    for (uint32_t i = 0; i < n && same; ++i) {
        uint32_t w;
        if (le) std::memcpy(&w, src + i * 4, 4); else w = be32(src + i * 4);
        uint32_t cur;
        same = XfWord(addr + i, cur) && cur == w;
    }
    Write(static_cast<Grp>(gIdxPos + which), 0, same);
}

// ---- a called list's register writes, walked before the call: the same tests against the register file and what the list wrote before
struct Overlay {
    uint32_t bp[256], tev[16], cp[256];
    uint32_t bpSet[8], cpSet[8];
    uint16_t tevSet;
    uint32_t mask;
    uint32_t xfA[96], xfW[96];
    int nXf;
    bool xfFull;
    void Reset() { std::memset(bpSet, 0, sizeof bpSet); std::memset(cpSet, 0, sizeof cpSet); tevSet = 0; mask = g.bpMask; nXf = 0; xfFull = false; }
};
Overlay* g_ov = nullptr;
void WalkList(const uint8_t* p, uint32_t n) {
    Overlay& o = *g_ov;
    o.Reset();
    const uint8_t* const end = p + n;
    while (p < end) {
        const uint8_t c = *p++;
        if (c == 0x00) continue;
        if (c == 0x61) {
            if (end - p < 4) break;
            const uint32_t rv = be32(p);
            p += 4;
            const uint32_t reg = rv >> 24;
            if (reg == 0xFE) { o.mask = rv & 0x00FFFFFFu; Write(gMask, 1, false); continue; }
            const Grp gr = BpGroup(reg);
            uint32_t cur;
            if (reg >= 0xE0 && reg <= 0xE7) {
                const uint32_t slot = ((reg - 0xE0) << 1) | ((rv >> 23) & 1u);
                cur = (o.tevSet >> slot & 1u) ? o.tev[slot] : g.tevRaw[slot];
                const uint32_t nv = (cur & ~o.mask) | (rv & o.mask & 0x00FFFFFFu);
                Write(gr, 1, nv == cur);
                o.tev[slot] = nv; o.tevSet |= static_cast<uint16_t>(1u << slot);
            } else {
                cur = (o.bpSet[reg >> 5] >> (reg & 31) & 1u) ? o.bp[reg] : g.bp[reg];
                const uint32_t nv = (cur & ~o.mask) | (rv & o.mask & 0x00FFFFFFu);
                Write(gr, 1, gr != gTrig && nv == cur);
                o.bp[reg] = nv; o.bpSet[reg >> 5] |= 1u << (reg & 31);
            }
            o.mask = 0x00FFFFFFu;
        } else if (c == 0x08) {
            if (end - p < 5) break;
            const uint8_t reg = p[0];
            const uint32_t v = CpVal(reg, be32(p + 1));
            p += 5;
            uint32_t cur = 0;
            bool known;
            if (o.cpSet[reg >> 5] >> (reg & 31) & 1u) { cur = o.cp[reg]; known = true; }
            else known = CpCur(reg, cur);
            Write(CpGroup(reg), 1, known && cur == v);
            o.cp[reg] = v; o.cpSet[reg >> 5] |= 1u << (reg & 31);
        } else if (c == 0x10) {
            if (end - p < 4) break;
            const uint32_t cnt = (be16(p) & 0xFu) + 1u, addr = be16(p + 2);
            if (static_cast<uint32_t>(end - p) < 4u + cnt * 4u) break;
            bool same = true;
            for (uint32_t i = 0; i < cnt; ++i) {
                const uint32_t a = addr + i, w = be32(p + 4 + i * 4);
                uint32_t cur = 0;
                bool known = false;
                for (int k = o.nXf - 1; k >= 0; --k) if (o.xfA[k] == a) { cur = o.xfW[k]; known = true; break; }
                if (!known && !o.xfFull) known = XfWord(a, cur);
                if (!(known && cur == w)) same = false;
                if (o.nXf < 96) { o.xfA[o.nXf] = a; o.xfW[o.nXf] = w; ++o.nXf; } else o.xfFull = true;
            }
            Write(XfGroup(addr), 1, same);
            p += 4u + cnt * 4u;
        } else if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) {
            if (end - p < 4) break;
            p += 4;
            Write(static_cast<Grp>(gIdxPos + ((c - 0x20) >> 3)), 1, false); // (not followed into the arrays here)
            o.xfFull = true; // (what it loads is not known to the walk: later XF words are compared as not known)
            o.nXf = 0;
        } else if (c == 0x44 || c == 0x48) {
            continue;
        } else {
            break; // (a draw: a geometry list's state ends here; anything else: stop)
        }
    }
}

// ---- the counted picture
uint32_t g_tiny = 0;
inline uint32_t TinyBytes() { // (as vgx_dlcache.cpp TinyListBytes; read once)
    if (!g_tiny) { const long n = EnvInt("GXV_DL_TINY", 64); g_tiny = n < 15 ? 15u : static_cast<uint32_t>(n); }
    return g_tiny;
}
struct CallSnap { uint64_t sl[5], dl[7]; };
inline void Snap(CallSnap& s) {
    StateListCounters(s.sl);
    if (&DlCacheBranchCounts) DlCacheBranchCounts(s.dl); else std::memset(s.dl, 0, sizeof s.dl);
}
// before a top-level list call (0x40) or shape call (0x59): its writes walked
inline void CallBefore(const uint8_t* list, uint32_t n, CallSnap& s) {
    WalkList(list, n);
    Snap(s);
}
inline void CallAfter(const uint8_t* list, uint32_t n, const CallSnap& a) {
    CallSnap b;
    Snap(b);
    Call k;
    if (n <= TinyBytes()) k = b.sl[0] != a.sl[0] ? cTinyState : cTinyParse;
    else if (b.dl[3] != a.dl[3]) k = cBypass;
    else if (b.sl[0] != a.sl[0]) k = cStateHit;
    else if (b.sl[1] != a.sl[1]) k = cStateParse;
    else if (b.dl[0] != a.dl[0]) k = cGeoHit;
    else if (b.dl[4] != a.dl[4]) k = cGeoBuild;
    else k = cOther;
    (void)list;
    ++g_w->cn[k];
    g_w->cb[k] += n;
    if (Src* s = SrcOf(g_curSrc)) ++s->calls;
}
inline void Shape() {
    Win& w = *g_w;
    ++w.shapes;
    w.shapeW += g_wSince;
    w.shapeSame += g_sameSince;
    const uint64_t x = g_wSince;
    const int h = x == 0 ? 0 : x <= 2 ? 1 : x <= 4 ? 2 : x <= 8 ? 3 : x <= 16 ? 4 : x <= 32 ? 5 : 6;
    ++w.hist[h];
    w.histSame[h] += g_sameSince;
    g_wSince = g_sameSince = 0;
    if (Src* s = SrcOf(g_curSrc)) ++s->shapes;
    if (g_curModel >= 0) { ++g_m[g_curModel].shapes; ++w.szShapes[g_curCode]; }
}
// one top-level command's bytes, at its end
inline void Cmd(Kind k, uint32_t bytes, uint32_t verts) {
    Win& w = *g_w;
    w.kb[k] += bytes;
    ++w.kc[k];
    ++w.cmds;
    g_picBytes += bytes;
    if (k == kDraw) w.drawVerts += verts;
    if (Src* s = SrcOf(g_curSrc)) {
        s->bytes += bytes;
        if (k == kDraw) s->drawBytes += bytes;
        if (k == kFx) s->fxBytes += bytes;
    }
    if (g_curModel >= 0) {
        Mdl& m = g_m[g_curModel];
        m.bytes += bytes;
        if (k == kDraw) m.drawBytes += bytes;
        w.szBytes[g_curCode] += bytes;
    }
}
inline void Parsed(uint32_t n) { g_w->parsed += n; }

void Begin() {
    if (Every() <= 0) return;
    if (vgx2_mode != 0 || g_j3n2On) return; // (the census needs the single front end and no j3n stage-2 chains)
    if (!g_w) {
        g_w = new Win();
        g_m = new Mdl[kModels]();
        g_ov = new Overlay();
    }
    g_on = true;
    g_curSrc = kSrcFirst;
    g_curModel = -1;
    g_curCode = 0;
    g_wSince = g_sameSince = 0;
    g_picBytes = 0;
    ++g_w->pics;
    ++g_seen;
    ClockStart();
}
void Print();
bool g_inParse = false, g_endPending = false; // (a display copy inside ProcessStreamT<true>: the window is closed at the parse's end)
void Finish() {
    g_endPending = false;
    g_w->bytes += g_picBytes;
    if (g_w->pics >= static_cast<uint64_t>(g_print)) Print();
}
// the picture's display copy (vgx_draw.cpp QueueCopyDisp)
void PictureEnd() {
    if (!g_on) return;
    g_on = false;
    if (g_inParse) { g_endPending = true; return; }
    ClockStop();
    Finish();
}
inline void ParseBegin() { g_inParse = true; ClockStart(); }
inline void ParseEnd() {
    ClockStop();
    g_inParse = false;
    if (g_endPending) Finish();
}
// kinds 21-23 (WriteBP)
void Marker(uint32_t kind, uint32_t payload) {
    if (kind == 21 && payload == 0xFFFF) { if (!g_on) Begin(); return; }
    if (!g_on) return;
    Tick();
    ++g_w->markers;
    if (kind == 21) {
        g_curSrc = payload;
    } else if (kind == 22) {
        const int id = static_cast<int>(payload & 0xFFFu);
        uint8_t code = static_cast<uint8_t>((payload >> 12) & 0xFu);
        if (code >= kSizeCodes) code = 0;
        g_curModel = id;
        g_curCode = code;
        Mdl& m = g_m[id];
        m.touched = true;
        m.code = code;
        ++m.packets;
        ++g_w->szPackets[code];
    } else if (kind == 23) {
        g_curModel = -1;
    }
}

// ---- the lines
extern "C" const char* port_stream_census_model_name(unsigned id) __attribute__((weak));
void SrcName(uint32_t id, char* out, size_t n) {
    if (id == kSrcFirst) { std::snprintf(out, n, "before-any"); return; }
    if (id == kSrcNone) { std::snprintf(out, n, "between-lists"); return; }
    if (id >= 0x400 && id < 0x410) {
        static const char* const parts[] = {"part-shadow", "part-effect", "part-2d", "part-3d", "part-mirror"};
        if (id - 0x400 < 5) { std::snprintf(out, n, "%s", parts[id - 0x400]); return; }
    }
    const uint32_t k = id >> 8, i = id & 0xFF;
    static const char* const kinds[] = {"exec", "opa", "xlu", "entry"};
    if (k < 4) std::snprintf(out, n, "%s-%02X", kinds[k], i);
    else std::snprintf(out, n, "src-%04X", id);
}
void Print() {
    Win& w = *g_w;
    const double P = static_cast<double>(w.pics ? w.pics : 1);
    char line[4096];
    int o = 0;
#define add(...) do { if (o < static_cast<int>(sizeof line) - 1) o += std::snprintf(line + o, sizeof line - o, __VA_ARGS__); } while (0)
    auto out = [&]() { if (port_log_summary) port_log_summary("%s", line); o = 0; line[0] = 0; };
    uint64_t kSum = 0;
    for (int k = 0; k < kKinds; ++k) kSum += w.kb[k];
    add("stream-census: %llu counted pictures (1 in %d, %llu in all), per picture: top-level stream %.0f bytes in %.0f commands (kinds add up to %.0f: %s), "
        "player's time in the parse %.3f ms; by kind bytes (commands):",
        static_cast<unsigned long long>(w.pics), g_every, static_cast<unsigned long long>(g_seen), w.bytes / P, w.cmds / P, kSum / P,
        kSum == w.parsed ? "equal to the bytes parsed" : "NOT EQUAL to the bytes parsed", w.ns / P / 1e6);
    for (int k = 0; k < kKinds; ++k) if (w.kc[k]) add(" %s %.0f (%.1f)", kKindName[k], w.kb[k] / P, w.kc[k] / P);
    add("; inline draws %.0f vertices; markers %.1f; window total %llu bytes", w.drawVerts / P, w.markers / P, static_cast<unsigned long long>(w.bytes));
    out();
    add("stream-census: list calls per picture by what the front end did, calls (content bytes):");
    for (int c = 0; c < kCalls; ++c) if (w.cn[c]) add(" %s %.1f (%.0f)", kCallName[c], w.cn[c] / P, w.cb[c] / P);
    out();
    uint64_t t[2][2] = {};
    for (int gr = 0; gr < kGroups; ++gr) for (int l = 0; l < 2; ++l) for (int s = 0; s < 2; ++s) t[l][s] += w.rw[gr][l][s];
    add("stream-census: register writes per picture, top level %.0f (changed nothing %.0f), in called lists %.0f (%.0f); by group top/lists, writes:same:",
        t[0][0] / P, t[0][1] / P, t[1][0] / P, t[1][1] / P);
    for (int gr = 0; gr < kGroups; ++gr)
        if (w.rw[gr][0][0] || w.rw[gr][1][0])
            add(" %s %.0f:%.0f/%.0f:%.0f", kGrpName[gr], w.rw[gr][0][0] / P, w.rw[gr][0][1] / P, w.rw[gr][1][0] / P, w.rw[gr][1][1] / P);
    out();
    add("stream-census: J3D shape calls %.1f per picture; register writes since the shape before: %.1f a shape (changed nothing %.1f); histogram writes: shapes (changed nothing a shape):",
        w.shapes / P, w.shapes ? double(w.shapeW) / w.shapes : 0.0, w.shapes ? double(w.shapeSame) / w.shapes : 0.0);
    for (int h = 0; h < kHist; ++h) add(" %s: %.1f (%.1f)", kHistName[h], w.hist[h] / P, w.hist[h] ? double(w.histSame[h]) / w.hist[h] : 0.0);
    out();
    // sources
    std::vector<int> idx(w.nSrc);
    for (int i = 0; i < w.nSrc; ++i) idx[i] = i;
    for (int pass = 0; pass < 2; ++pass) {
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return pass == 0 ? w.src[a].bytes > w.src[b].bytes : w.src[a].ns > w.src[b].ns; });
        add("stream-census: sources by %s (bytes %% of the stream, ms, inline-draw / fx bytes, shapes, list calls, writes:same; per picture):", pass == 0 ? "bytes" : "time");
        for (int j = 0; j < w.nSrc && j < 20; ++j) {
            const Src& s = w.src[idx[j]];
            char nm[32];
            SrcName(s.id, nm, sizeof nm);
            add(" %s %.0f %.1f%% %.3f ms %.0f/%.0f %.1f %.1f %.0f:%.0f;", nm, s.bytes / P, w.bytes ? 100.0 * s.bytes / w.bytes : 0.0, s.ns / P / 1e6, s.drawBytes / P, s.fxBytes / P,
                s.shapes / P, s.calls / P, s.writes / P, s.same / P);
        }
        if (w.srcOverflow) add(" (%llu commands of sources past the table)", static_cast<unsigned long long>(w.srcOverflow));
        out();
    }
    // models
    std::vector<int> mi;
    for (int i = 0; i < kModels; ++i) if (g_m[i].touched) mi.push_back(i);
    for (int pass = 0; pass < 2; ++pass) {
        std::sort(mi.begin(), mi.end(), [&](int a, int b) { return pass == 0 ? g_m[a].bytes > g_m[b].bytes : g_m[a].ns > g_m[b].ns; });
        add("stream-census: %" VGX_PRIZU " models drawn; models by %s (bytes, ms, packets, shapes, size; per picture):", mi.size(), pass == 0 ? "bytes" : "time");
        for (size_t j = 0; j < mi.size() && j < 20; ++j) {
            const Mdl& m = g_m[mi[j]];
            const char* nm = port_stream_census_model_name ? port_stream_census_model_name(static_cast<unsigned>(mi[j])) : nullptr;
            if (nm) add(" %s", nm); else add(" model#%d", mi[j]);
            add(" %.0f %.3f %.1f %.1f %s;", m.bytes / P, m.ns / P / 1e6, m.packets / P, m.shapes / P, kSizeName[m.code]);
        }
        out();
    }
    add("stream-census: J3D material packets by the projected diameter of their actor's clipping sphere in game lines (456; packets, shapes, bytes %% of the stream, ms; per picture):");
    for (int c = 0; c < kSizeCodes; ++c)
        if (w.szPackets[c]) add(" %s: %.1f %.1f %.1f%% %.3f;", kSizeName[c], w.szPackets[c] / P, w.szShapes[c] / P, w.bytes ? 100.0 * w.szBytes[c] / w.bytes : 0.0, w.szNs[c] / P / 1e6);
    out();
    // (a new window)
    for (int i : mi) g_m[i] = Mdl{};
    *g_w = Win{};
#undef add
}
// GXV_ARRAYSKIP=2 (the game's port/array_skip.h): before an array-base write that =1 would leave out, the game sends the register and the
// value (kinds 24..26: register 0xA0 + k and the high half; 27: the low half). Here, before the write: does the register hold it already?
// "arrayskip verify:" every 2^18 checks and at each of the first 8 that differ (any differing one means the game's shadow missed a writer).
uint32_t g_avReg = 0, g_avHi = 0;
uint64_t g_avChecked = 0, g_avBad = 0;
void ArrVerify(uint32_t kind, uint32_t payload) {
    if (kind <= 26) { g_avReg = 0xA0u + (kind - 24u); g_avHi = payload; return; }
    if (!g_avReg) return;
    const uint32_t v = (g_avHi << 16) | payload;
    const uint32_t cur = g.cpBase[g_avReg & 0xFu];
    ++g_avChecked;
    const bool bad = cur != v;
    if (bad) ++g_avBad;
    if ((bad && g_avBad <= 8) || (g_avChecked & 0x3FFFFu) == 0) {
        if (port_log_summary)
            port_log_summary("arrayskip verify: %llu checked, %llu differing%s (register %02X: front end %08X, the game's shadow %08X)",
                             static_cast<unsigned long long>(g_avChecked), static_cast<unsigned long long>(g_avBad), bad ? ": MISMATCH" : "", g_avReg, cur, v);
    }
    g_avReg = 0;
}
uint64_t ArrVerifyCounts(uint64_t* bad) { *bad = g_avBad; return g_avChecked; }
#else
void ArrVerify(uint32_t, uint32_t) {}
// (the second worker's instance: never counts; the names ProcessStreamT<true> uses, which it never instantiates with a census)
enum Kind : uint8_t { kPad, kBp, kCp, kXf, kIdx, kCall, kShape, kFx, kDraw, kMark, kJ3n, kNop, kOther, kKinds };
struct CallSnap {};
void PictureEnd() {}
void Marker(uint32_t, uint32_t) {}
constexpr bool g_on = false;
inline void ParseBegin() {}
inline void ParseEnd() {}
inline void Cmd(Kind, uint32_t, uint32_t) {}
inline void Parsed(uint32_t) {}
inline void TopBp(uint32_t) {}
inline void TopCp(uint8_t, uint32_t) {}
inline void TopXf(uint32_t, uint32_t, const uint8_t*) {}
inline void TopIdx(int, uint32_t, uint32_t, const uint8_t*, bool) {}
inline void CallBefore(const uint8_t*, uint32_t, CallSnap&) {}
inline void CallAfter(const uint8_t*, uint32_t, const CallSnap&) {}
inline void Shape() {}
#endif
} // namespace sc
