#define JPA_LIST_DEFER_INLINE
#define JPA_FIELD_BLOCK_DEFER_INLINE
#include "JSystem/JParticle/JPAResource.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JParticle/JPABaseShape.hpp"
#include "JSystem/JParticle/JPAChildShape.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPAEmitterManager.hpp"
#include "JSystem/JParticle/JPAExTexShape.hpp"
#include "JSystem/JParticle/JPAExtraShape.hpp"
#include "JSystem/JParticle/JPAFieldBlock.hpp"
#include "JSystem/JParticle/JPAKeyBlock.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"

#ifdef TARGET_PC
// GXV_JPA_LOOK (logic12, 5 Nov; default 1; 0 in env.txt: every calc runs the look functions, as before). A particle's look
// functions (mpCalcParticleFuncList / mpCalcParticleChildFuncList: JPACalcTexIdx*, JPACalcAlpha*, JPACalcPrm/Env with
// JPACalcClrIdx*, JPACalcColorCopy, JPACalcScale*, JPACalcChildAlphaOut/ScaleOut) set only what the draw reads (mTexAnmIdx,
// mPrmColorAlphaAnm, mPrmClr/mEnvClr, mParticleScaleX/Y), each as a function of the particle's age and time, of fields fixed at
// its birth (mAnmRandom, mScaleOut, mAlphaWaveRandom), of the resource, and (JPACalcColorCopy) of the emitter's colour, which only
// the emitter's own calc writes. Run at the next draw instead of at every calc, they give the same values for the picture; on the
// turns that draw nothing (about half of them in the lit Observatory) they are not run at all. Left in place where something
// else reads those values between two pictures:
//  - an emitter with a particle callback (it runs before the look functions in calc_p and may read or write them);
//  - a parent whose resource's children inherit its scale, alpha or colour (init_c reads them when the child is made in calc_p).
// A particle not yet calculated since its birth (mAge -1, made outside a calc) keeps the values init_p/init_c gave it, as before.
extern "C" int port_jpa_look_on(void);
// GXV_PTCL_LOOKPAR (gamecpu23; JPAEmitterManager.cpp sets these): during a parallel particle turn, the emitters marked mPortLookNow run their
// look functions in their calc (as the original does) instead of leaving them to the next draw.
extern bool gJpaLookPar;
extern "C" int port_ptcl_lookpar_mode(void);
extern "C" void port_ptcl_lookpar_note(unsigned checked, unsigned bad);
// GXV_JPA_LOOK_PURE (gamecpu23, 8 Nov; perf.cpp): GXV_JPA_LOOK also for an emitter whose particle callback is "pure" (portCalcKind() 1:
// MultiEmitterParticleCallBack itself, whose execute is JPAParticleCallBack's empty one). Every effect the game makes through a MultiEmitter
// has one, so without this GXV_JPA_LOOK leaves out almost nothing (Honeyhive r107: 37 of 399 particles a turn). The callback runs before
// the look functions in calc_p and does nothing, so leaving them to the draw is GXV_JPA_LOOK's case exactly. 2 = verify: such emitters run
// their look functions in the calc as before, and the draw runs them again and compares (the "ptcl-lookpar:" line, PortLookVerify).
extern "C" int port_jpa_look_pure_mode(void);
extern "C" void port_logic_count(int what, unsigned n);
// (GXV_PAR_PTCL: the two flags below are fields of the work area, work->mPortLookSkipP / C, so that threads with work areas of their own
// do not share them.)

static inline void PortLookRunP(JPAResource* pRes, JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    for (int i = pRes->mpCalcParticleFuncListNum - 1; i >= 0; i--) {
        (*pRes->mpCalcParticleFuncList[i])(work, ptcl);
    }
}

static inline void PortLookRunC(JPAResource* pRes, JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    for (int i = pRes->mpCalcParticleChildFuncListNum - 1; i >= 0; i--) {
        (*pRes->mpCalcParticleChildFuncList[i])(work, ptcl);
    }
}

// GXV_FXMERGE (default 0; port/gx_fifo.h): an emitter's particle loop gathered into port-private commands for the vgx front end by the
// GX library (decomp/src/RVL_SDK/gx/GXDisplayList.c), which takes only GXSetTevColor, GXLoadPosMtxImm, GXLoadTexMtxImm and
// GXCallDisplayList. So it is started only for a loop whose particle functions make no other GX call: the functions below, which
// make only those (the colour functions, the texture-coordinate matrix, the quad drawers with their projection loaders, the particle
// callback hook with no callback). JPALoadTexAnm (a texture load), the point and line drawers and a particle callback (anything)
// keep the loop as it was.
// ============================================================== GXV_HOTPROF_PTCLCULL (hotprof25, 11 Nov; default 0)
// The console's execution-count profile (12 minutes, 18,618 pictures): 9.19 M dynamic draws reach the renderer's front end, ~7.2 M of them
// particle quads, and GXV_DYNCULL leaves 5.93 M of the 9.16 M it tests out (65 %: quads off screen; c25's Honeyhive census: 126 of 353 a
// picture). Each of those was sent whole by the game thread (GXLoadPosMtxImm, the projection texture's matrix, the list call) and taken
// whole by the front end (an XF load, a list call, a vertex decode, the draw's state resolved) only to be dropped at the end.
//   With the switch on, a quad particle is tested here, before it is sent, with the front end's own test (port/ptcl_cull.h: the same code,
// the same matrix, the same vertices, the clip volume from the GX state the registers are made of): outside one plane of the clip volume
// by more than the error bound, it is not sent. No pixel of such a quad is drawn by anything that clips, so the picture is the same.
//   The GX state every later draw sees is kept exactly: a quad's function writes only position matrix 0 and (projection textures) texture
// matrix 0, and every quad of the loop writes the same two; the matrices of the last quad left out are kept and loaded at the end of the
// loop if no drawn quad wrote them after it. (TEV colours are set by other functions of the list: they are sent as before.)
//   The TEV colours of a quad left out are not sent either: in an armed loop the colour functions (JPARegist*) hand their GXSetTevColor
// calls to this file, which makes them, in their order, right before the quad's matrices when the quad is sent, and for the last
// particle of the loop that set colours without being drawn at the loop's end. So at every quad that is drawn, and after the loop,
// the TEV colour registers hold what they held before; a quad left out costs the stream nothing at all. (About 42 % of all the BP
// commands of the profiled run were particles' TEV colours: 15.5 M GXSetTevColor calls of four commands each.)
//   Armed only for a loop whose particle functions are the ones GXV_FXMERGE knows (PortFxListOk below: the quad functions, the colour
// functions, the texture-coordinate matrix; no particle callback): they make no GX call but those named here.
// Relied on, and true in this tree: no JPAEmitterCallBack overrides draw() (it runs between setPTev's GXSetArray and the loop).
// The GX library's own "a BP register was sent since the last primitive" flag can differ after a loop (it decides whether an all-zero
// flush strip is sent before a primitive while GX_MT_XF_FLUSH is on; the front end drops such a strip; the loop turns the mode off).
//   1: on. 2: dry (tested and counted, everything sent as before). "ptcl-cull:" log line every 4096 particle loops.
#include "port/ptcl_cull.h"
#include "port/ptcl_thin.h"
// GXV_PTCL_THIN (gamecpu25, 12 Nov; port/ptcl_thin.h; default 0): an emitter far from the camera draws a distance-dependent fraction of its particles
// (a hash of each particle's address decides which: stable for the particle's life). The skip is before the particle's draw functions, so a skipped
// particle makes no matrices and no GX calls; its calc, look and life are untouched. weak: host tests that link the JPA units with their own stubs.
extern "C" float port_ptcl_thin_min(void) __attribute__((weak));
extern "C" float port_ptcl_thin_d0(void) __attribute__((weak));
extern "C" float port_ptcl_thin_d1(void) __attribute__((weak));
extern "C" int port_ptcl_thin_dry(void) __attribute__((weak));
extern "C" int64_t port_host_time_ns(void) __attribute__((weak));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
// popin31 (LodCtrl.cpp; weak, as above): GXV_PB_MINPX's squared distance inside which no emitter is thinned (0 = no floor), and the census's counts
extern "C" float port_pb_ptcl_floor_d2(void) __attribute__((weak));
extern "C" void port_popin_ptcl_note(int what, unsigned n) __attribute__((weak));
extern "C" int port_popin_census_on(void) __attribute__((weak));
namespace {
float sPtclThinKeep = 1.0f;  // (this emitter's kept fraction; 1 = all)
bool sPtclThinDry = false;
bool sPtclThinCensus = false;      // (popin31: GXV_POPIN_CENSUS, read once an emitter)
unsigned sPtclThinCensusSkips = 0; // (... particles left out since the last report)
unsigned long long sThinEmitters = 0, sThinFarEmitters = 0, sThinSeen = 0, sThinSkipped = 0, sThinFarSeen = 0;
int64_t sThinT0 = 0;
// set up for one emitter's draw: its kept fraction from its distance to the camera (work->mPosCamMtx is the camera matrix of this draw)
inline void PortThinBegin(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    sPtclThinKeep = 1.0f;
    if (!port_ptcl_thin_min) return;
    const float fmin = port_ptcl_thin_min();
    if (fmin <= 0.0f) return;
    if (work->mPrjMtx[3][3] != 0.0f) return;  // (orthographic: 2D effects, never thinned)
    port::ptclthin::Cfg c = {fmin, port_ptcl_thin_d0(), port_ptcl_thin_d1()};
    float cam[3];
    port::ptclthin::CamPos(reinterpret_cast< const float (*)[4] >(&work->mPosCamMtx[0][0]), cam);
    const float dx = emtr->mGlobalTrs.x - cam[0], dy = emtr->mGlobalTrs.y - cam[1], dz = emtr->mGlobalTrs.z - cam[2];
    sPtclThinDry = port_ptcl_thin_dry && port_ptcl_thin_dry() != 0;
    const float d2 = dx * dx + dy * dy + dz * dz;
    sPtclThinKeep = port::ptclthin::KeepFraction(d2, c);
    if (sPtclThinKeep < 1.0f && port_pb_ptcl_floor_d2 && d2 < port_pb_ptcl_floor_d2()) {
        sPtclThinKeep = 1.0f;  // (popin31: GXV_PB_MINPX: not under the floor here; popin31b: GXV_PB_MINPX_PARTICLE)
        if (port_popin_census_on && port_popin_ptcl_note && port_popin_census_on() != 0) port_popin_ptcl_note(3, 1);  // (popin31b: the census's price line)
    }
    sPtclThinCensus = port_popin_census_on && port_popin_ptcl_note && port_popin_census_on() != 0;
    if (sPtclThinCensus) {
        if (sPtclThinCensusSkips != 0) {
            port_popin_ptcl_note(2, sPtclThinCensusSkips);
            sPtclThinCensusSkips = 0;
        }
        if (sPtclThinKeep < 1.0f) port_popin_ptcl_note(1, 1);
    }
    ++sThinEmitters;
    if (sPtclThinKeep < 1.0f) ++sThinFarEmitters;
    if (port_log_summary && port_host_time_ns) {
        const int64_t now = port_host_time_ns();
        if (sThinT0 == 0) sThinT0 = now;
        if (now - sThinT0 >= 5000000000ll) {
            port_log_summary("ptcl-thin: GXV_PTCL_THIN=%.2f D0 %.0f D1 %.0f%s: in 5 s %llu emitters drawn, %llu thinned; %llu particles reached the draw loops of the thinned ones, %llu %s",
                             fmin, c.d0, c.d1, sPtclThinDry ? " (DRY: counts only)" : "", sThinEmitters, sThinFarEmitters, sThinFarSeen, sThinSkipped,
                             sPtclThinDry ? "would be left out" : "left out");
            sThinEmitters = sThinFarEmitters = sThinFarSeen = sThinSkipped = 0;
            sThinT0 = now;
        }
    }
}
// true: leave this particle out (not in dry mode)
inline bool PortThinSkip(JPABaseParticle* p) {
    ++sThinFarSeen;
    if (port::ptclthin::Keep(reinterpret_cast< uintptr_t >(p), sPtclThinKeep)) return false;
    ++sThinSkipped;
    if (sPtclThinCensus && !sPtclThinDry) ++sPtclThinCensusSkips;  // (popin31, the census)
    return !sPtclThinDry;
}
}  // namespace
// (weak references: host tests that link the JPA units with their own stubs (tools/test_par_ptcl.sh, test_jpa_look.sh) need not know these;
// without the switch function the mode is 0, and without a getter no loop is tested)
extern "C" int port_hotprof_ptclcull_mode(void) __attribute__((weak));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" void GXGetProjectionv(f32* p) __attribute__((weak));
extern "C" void GXGetViewportv(f32* vp) __attribute__((weak));
extern "C" void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) __attribute__((weak));
// GXV_HOTPROF_TEVCOL (decomp/src/RVL_SDK/gx/GXTev.c): a particle loop of the known functions is a scope in which a TEV colour word equal
// to the loop's last for its register is not written again. (weak: host tests that link the JPA units without the GX library)
extern "C" void port_gx_tevcol_scope(int on) __attribute__((weak));
static inline int PortPtclCullMode() {
    return port_hotprof_ptclcull_mode ? port_hotprof_ptclcull_mode() : 0;
}
// perf32, GXV_PTCL_PRE (platform/src/port/perf.cpp; port/ptcl_cull.h QuadPreOutside has the proof): a quad far outside the clip volume is
// answered from its centre, without the kernel's walk over its four or eight vertices; every other quad is asked of the kernel as before, so
// the quads left out are the same ones. 0 off (default until a console A/B), 1 on, 2 verify (the kernel decides every quad; a quad the
// centre test calls outside and the kernel does not is counted: the "ptcl-cull:" line's "pre" part must say 0 wrong).
extern "C" int port_ptcl_pre_mode(void) __attribute__((weak));
void port_jpa_load_prj(JPAEmitterWorkData const* work, const Mtx srt);  // (JPABaseShape.cpp: the loop's projection-texture matrix load)
unsigned char gPortPtclCullArmed = 0;  // (the quad functions' one-byte test: JPABaseShape.cpp, Overwrite.cpp)
namespace {
struct PortPtclCullState {
    int mode = 0;  // of the loop being drawn: 0 none, 1 on, 2 dry
    bool haveClip = false, clipOk = false, pending = false;
    bool haveVerts[2] = {false, false};
    bool havePre[2] = {false, false};  // (perf32, GXV_PTCL_PRE: the centre test's constants of the loop, made at its first quad)
    port::ptclcull::QuadPre pre[2];
    int posOffset = 0;  // (setPTev / setCTev: the position array is jpa_pos + this)
    port::ptclcull::ClipNow clip;
    float verts[2][8][3];
    Mtx pendM;
    // (the TEV colours the particle being drawn has set and that are not sent yet, in the order of its calls)
    const void* tevNode = nullptr;
    int tevN = 0;
    GXTevRegID tevId[4];
    GXColor tevColor[4];
};
PortPtclCullState sPtclCull;
unsigned long long sPcLoops = 0, sPcTested = 0, sPcOut = 0, sPcOwed = 0, sPcNoClip = 0;
unsigned long long sPcPre = 0, sPcPreBad = 0;  // (perf32, GXV_PTCL_PRE: quads answered by the centre test; verify: answered "outside" against the kernel)
// GXV_HOTPROF_PTCLCENSUS=1 (with GXV_HOTPROF_PTCLCULL=1 or 2; hotprof25 round 2): which emitters the tested and the off-screen quads belong
// to, by the resource's user index (the row of the game's particle name table; the name is asked of the game when there is one). A
// "ptcl-census:" line with every "ptcl-cull:" line: the eight resources with the most off-screen quads since the line before.
struct PortPcRow {
    unsigned short idx;
    bool used;
    unsigned tested, out;
};
PortPcRow sPcCensus[128];
unsigned sPcCensusLost = 0;
bool sPcCensusOn = false;
void PortPcCensusAdd(unsigned idx, bool out) {
    const unsigned h = (idx * 2654435761u) >> 25;
    for (unsigned i = 0; i < 128; i++) {
        PortPcRow& r = sPcCensus[(h + i) & 127];
        if (!r.used) {
            r.used = true;
            r.idx = static_cast< unsigned short >(idx);
            r.tested = r.out = 0;
        }
        if (r.idx == idx) {
            r.tested++;
            r.out += out ? 1u : 0u;
            return;
        }
    }
    sPcCensusLost++;  // (more than 128 resources since the last line)
}
}  // namespace
extern "C" int port_hotprof_ptclcensus_on(void) __attribute__((weak));
extern "C" const char* port_particle_name(unsigned usrIdx) __attribute__((weak));  // (Game/Effect/ParticleResourceHolder.cpp)
static void PortPcCensusLog() {
    unsigned n = 0, tested = 0, out = 0;
    for (const PortPcRow& r : sPcCensus) {
        if (r.used) {
            n++;
            tested += r.tested;
            out += r.out;
        }
    }
    // (the eight with the most off-screen quads, as name#index off/tested; no formatter but the log's own: host tests link this file)
    const char* name[8];
    unsigned idx[8], o[8], t[8];
    for (int k = 0; k < 8; k++) {
        PortPcRow* best = nullptr;
        for (PortPcRow& r : sPcCensus) {
            if (r.used && r.out != 0 && (best == nullptr || r.out > best->out)) {
                best = &r;
            }
        }
        name[k] = "";
        idx[k] = o[k] = t[k] = 0;
        if (best != nullptr) {
            const char* nm = port_particle_name ? port_particle_name(best->idx) : nullptr;
            name[k] = nm != nullptr ? nm : "";
            idx[k] = best->idx;
            o[k] = best->out;
            t[k] = best->tested;
            best->out = 0;
        }
    }
    if (port_log_summary) {
        port_log_summary("ptcl-census: since the last line %u resources, %u quads tested, %u off screen (%u not counted); most off screen "
                         "(name#index off/tested): %.40s#%u %u/%u, %.40s#%u %u/%u, %.40s#%u %u/%u, %.40s#%u %u/%u, %.40s#%u %u/%u, %.40s#%u %u/%u, "
                         "%.40s#%u %u/%u, %.40s#%u %u/%u",
                         n, tested, out, sPcCensusLost, name[0], idx[0], o[0], t[0], name[1], idx[1], o[1], t[1], name[2], idx[2], o[2], t[2], name[3],
                         idx[3], o[3], t[3], name[4], idx[4], o[4], t[4], name[5], idx[5], o[5], t[5], name[6], idx[6], o[6], t[6], name[7], idx[7],
                         o[7], t[7]);
    }
    memset(sPcCensus, 0, sizeof(sPcCensus));
    sPcCensusLost = 0;
}
__attribute__((noinline)) static void PortPtclPreLog() {
    if (port_ptcl_pre_mode && port_ptcl_pre_mode() != 0 && port_log_summary) {
        port_log_summary("ptcl-cull: pre: GXV_PTCL_PRE=%d: since the start %llu of the %llu outside quads answered by the centre test (%.1f %%)%s, %llu wrong",
                         port_ptcl_pre_mode(), sPcPre, sPcOut, sPcOut ? 100.0 * static_cast< double >(sPcPre) / static_cast< double >(sPcOut) : 0.0,
                         port_ptcl_pre_mode() == 2 ? " (verify: the kernel decided every quad)" : "", sPcPreBad);
    }
}
static void PortPtclCullBegin(bool listOk) {
    PortPtclCullState& s = sPtclCull;
    s.mode = 0;
    s.pending = false;
    s.haveClip = false;
    s.haveVerts[0] = s.haveVerts[1] = false;
    s.tevNode = nullptr;
    s.tevN = 0;
    const int m = PortPtclCullMode();
    s.havePre[0] = s.havePre[1] = false;
    if (m != 0 && listOk && GXGetProjectionv && GXGetViewportv && GXGetScissor) {
        s.mode = m;
    }
    gPortPtclCullArmed = static_cast< unsigned char >(s.mode);  // (1: tested, colours handed over; 2: tested only)
    sPcCensusOn = s.mode != 0 && port_hotprof_ptclcensus_on && port_hotprof_ptclcensus_on() != 0;
}
static inline void PortPtclCullSendTev(PortPtclCullState& s) {
#ifdef PORT_PTCLCULL_NEG2  // (tools/test_jpa_ptcl_cull.sh's second negative control: the colours kept are never sent: the test must fail)
    s.tevN = 0;
#endif
    for (int i = 0; i < s.tevN; i++) {
        GXSetTevColor(s.tevId[i], s.tevColor[i]);
    }
    s.tevN = 0;
}
// A colour function's GXSetTevColor in a loop armed with mode 1: kept, for the quad of this particle (or the loop's end).
void port_jpa_ptcl_tev(JPAEmitterWorkData* work, GXTevRegID id, GXColor color) {
    PortPtclCullState& s = sPtclCull;
    if (s.tevNode != work->mpCurNode) {
        // (another particle: what the one before set and never sent is overwritten by this one's calls, as the registers would be)
        s.tevNode = work->mpCurNode;
        s.tevN = 0;
    }
    if (s.tevN == 4) {
        PortPtclCullSendTev(s);  // (more calls than any list of the known functions makes: sent as they come)
    }
    s.tevId[s.tevN] = id;
    s.tevColor[s.tevN] = color;
    s.tevN++;
}
static void PortPtclCullEnd(JPAEmitterWorkData* work) {
    PortPtclCullState& s = sPtclCull;
    if (s.mode == 0) {
        return;
    }
    PortPtclCullSendTev(s);  // (the colours of the last particle that set some and was not drawn)
    if (s.pending) {
        // (the last quad left out, and no drawn quad after it: its two loads, so that the state after the loop is what it was)
#ifndef PORT_PTCLCULL_NEG  // (tools/test_jpa_ptcl_cull.sh's negative control builds without them: the test must then fail)
        GXLoadPosMtxImm(s.pendM, GX_PNMTX0);
        port_jpa_load_prj(work, s.pendM);
#endif
        s.pending = false;
        ++sPcOwed;
    }
    s.mode = 0;
    gPortPtclCullArmed = 0;
    if ((++sPcLoops & 0xFFF) == 0 && port_log_summary) {
        port_log_summary("ptcl-cull: GXV_HOTPROF_PTCLCULL=%d: since the start %llu particle loops, %llu quads tested, %llu outside the clip volume "
                         "(%.1f %%%s), %llu loops ended with the last one's matrices loaded, %llu loops without a test (orthographic)",
                         PortPtclCullMode(), sPcLoops, sPcTested, sPcOut, sPcTested ? 100.0 * static_cast< double >(sPcOut) / static_cast< double >(sPcTested) : 0.0,
                         PortPtclCullMode() == 2 ? ", dry: all sent" : ": not sent", sPcOwed, sPcNoClip);
        PortPtclPreLog();  // (perf32: GXV_PTCL_PRE's line)
        if (port_hotprof_ptclcensus_on && port_hotprof_ptclcensus_on() != 0) {
            PortPcCensusLog();
        }
    }
}
// perf32, GXV_PTCL_PRE. The switch lives in these two functions, which are never inlined, and port_jpa_ptcl_cull / PortPtclCullBegin / PortPtclCullEnd gain
// no branch for it: their control flow is what the speed profile (tools/pgo) recorded, so with the switch at 0 they are compiled as before.
__attribute__((noinline)) static bool PortQuadOutsideSw(PortPtclCullState& s, const Mtx m, int t, uint32_t n) {
    const int pm = port_ptcl_pre_mode ? port_ptcl_pre_mode() : 0;
    if (__builtin_expect(pm == 0, 1)) {
        return port::ptclcull::QuadOutside(s.clip, m, s.verts[t], n);
    }
    if (!s.havePre[t]) {
        port::ptclcull::QuadPreOf(s.pre[t], s.clip, s.verts[t], n);
        s.havePre[t] = true;
    }
    const bool pre = s.pre[t].ok && port::ptclcull::QuadPreOutside(s.clip, s.pre[t], m);
    if (pm == 1) {
        if (pre) {
            ++sPcPre;
            return true;  // (the kernel's answer for this quad: port/ptcl_cull.h)
        }
        return port::ptclcull::QuadOutside(s.clip, m, s.verts[t], n);
    }
    const bool kernel = port::ptclcull::QuadOutside(s.clip, m, s.verts[t], n);  // (verify: the kernel decides)
    if (pre) {
        ++sPcPre;
        if (!kernel) {
            ++sPcPreBad;
        }
    }
    return kernel;
}
// (perf32: GXV_PTCL_PRE's counts, for tests/jpa_ptcl_cull.cpp: quads the centre test answered, and those of them the kernel contradicted in verify mode)
extern "C" void port_ptcl_pre_counts(unsigned long long out[2]) {
    out[0] = sPcPre;
    out[1] = sPcPreBad;
}
static const u8* PortPtclCullPosBase(int posOffset);
// A quad function is about to load position matrix `m` and call the 4-vertex (cross: 8-vertex) list. true: left out, nothing to send.
bool port_jpa_ptcl_cull(JPAEmitterWorkData* work, const Mtx m, bool cross) {
    PortPtclCullState& s = sPtclCull;
    if (s.mode == 0) {
        return false;
    }
    if (!s.haveClip) {
        // (once a loop, at its first quad: after setPTev and the emitter functions, so this is the state the quads are drawn with)
        f32 proj[7], vp[6];
        u32 sc[4];
        GXGetProjectionv(proj);
        GXGetViewportv(vp);
        GXGetScissor(&sc[0], &sc[1], &sc[2], &sc[3]);
        const uint32_t scis[4] = {sc[0], sc[1], sc[2], sc[3]};
        s.clipOk = port::ptclcull::ClipFromGx(s.clip, proj, vp, scis);
        s.haveClip = true;
        if (!s.clipOk) {
            ++sPcNoClip;
        }
    }
    if (!s.clipOk) {
        s.pending = false;
        PortPtclCullSendTev(s);  // (no test in this loop: the quad is drawn)
        return false;
    }
    const int t = cross ? 1 : 0;
    if (!s.haveVerts[t]) {
        const u8* base = PortPtclCullPosBase(s.posOffset);
        if (base == nullptr) {
            s.clipOk = false;
            s.pending = false;
            PortPtclCullSendTev(s);
            return false;
        }
        port::ptclcull::QuadVerts(s.verts[t], base, cross);
        s.haveVerts[t] = true;
    }
    ++sPcTested;
    const bool portOutside = PortQuadOutsideSw(s, m, t, cross ? 8u : 4u);  // (perf32: the kernel's answer, through GXV_PTCL_PRE's switch)
    if (sPcCensusOn && work->mpRes != NULL) {
        PortPcCensusAdd(work->mpRes->getUsrIdx(), portOutside);
    }
    if (!portOutside) {
        s.pending = false;  // (it is drawn: it loads both matrices itself)
        PortPtclCullSendTev(s);
        return false;
    }
    ++sPcOut;
    if (s.mode == 2) {
        s.pending = false;
        return false;
    }
    memcpy(s.pendM, m, sizeof(Mtx));
    s.pending = true;
    (void)work;
    return true;
}

extern "C" int port_gx_fx_begin(void);
extern "C" void port_gx_fx_end(void);
static bool PortFxListOk(JPAResource::ParticleFunc const* list, int n, const JPABaseEmitter* emtr) {
    typedef JPAResource::ParticleFunc F;
    if (list == NULL || n <= 0 || emtr->mpPtclCallBack != NULL) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        const F f = list[i];
        if (f == static_cast<F>(&JPADrawBillboard) || f == static_cast<F>(&JPADrawRotBillboard) || f == static_cast<F>(&JPADrawYBillboard) ||
            f == static_cast<F>(&JPADrawRotYBillboard) || f == static_cast<F>(&JPADrawDirection) || f == static_cast<F>(&JPADrawRotDirection) ||
            f == static_cast<F>(&JPADrawDBillboard) || f == static_cast<F>(&JPADrawRotation) || f == static_cast<F>(&JPADrawParticleCallBack) ||
            f == static_cast<F>(&JPALoadCalcTexCrdMtxAnm) || f == static_cast<F>(&JPARegistAlpha) || f == static_cast<F>(&JPARegistPrmAlpha) ||
            f == static_cast<F>(&JPARegistPrmAlphaEnv) || f == static_cast<F>(&JPARegistAlphaEnv) || f == static_cast<F>(&JPARegistEnv)) {
            continue;
        }
        return false;
    }
    return true;
}

// The start of a draw of this emitter: the look functions its calcs left out, for every particle calculated at least once.
static void PortLookFlush(JPAResource* pRes, JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    emtr->mPortLookStale = 0;
    unsigned n = 0;
    if (pRes->mpCalcParticleFuncList != NULL) {
        for (JPANode< JPABaseParticle >* node = emtr->mAlivePtclBase.getFirst(); node != emtr->mAlivePtclBase.getEnd(); node = node->getNext()) {
            JPABaseParticle* ptcl = node->getObject();
            if (ptcl->mAge >= 0) {
                PortLookRunP(pRes, work, ptcl);
                n++;
            }
        }
    }
    if (pRes->mpCalcParticleChildFuncList != NULL) {
        for (JPANode< JPABaseParticle >* node = emtr->mAlivePtclChld.getFirst(); node != emtr->mAlivePtclChld.getEnd(); node = node->getNext()) {
            JPABaseParticle* ptcl = node->getObject();
            if (ptcl->mAge >= 0) {
                PortLookRunC(pRes, work, ptcl);
                n++;
            }
        }
    }
    port_logic_count(1, n);
}
#endif

#ifdef TARGET_PC
// GXV_PTCL_LOOKPAR=2 (gamecpu23): an emitter whose last calc ran its look functions on the particle worker (instead of leaving them to this
// draw, GXV_JPA_LOOK) has them run again here, as GXV_JPA_LOOK's flush would, and every particle is compared byte for byte with what the calc
// left. Equal: GXV_JPA_LOOK's draw-time values and the calc-time values (the original's) are the same, and nothing changed the particles
// between. A difference is counted ("ptcl-lookpar:" line); the particle then keeps the draw-time value (GXV_JPA_LOOK's picture).
static void PortLookVerify(JPAResource* pRes, JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    unsigned checked = 0, bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0 ? pRes->mpCalcParticleFuncList : pRes->mpCalcParticleChildFuncList) == NULL) {
            continue;
        }
        JPAList< JPABaseParticle >& L = pass == 0 ? emtr->mAlivePtclBase : emtr->mAlivePtclChld;
        for (JPANode< JPABaseParticle >* node = L.getFirst(); node != L.getEnd(); node = node->getNext()) {
            JPABaseParticle* ptcl = node->getObject();
            if (ptcl->mAge < 0) {
                continue;
            }
            unsigned char before[sizeof(JPABaseParticle)];
            memcpy(before, static_cast< const void* >(ptcl), sizeof(before));
            if (pass == 0) {
                PortLookRunP(pRes, work, ptcl);
            } else {
                PortLookRunC(pRes, work, ptcl);
            }
            ++checked;
            if (memcmp(before, static_cast< const void* >(ptcl), sizeof(before)) != 0) {
                ++bad;
            }
        }
    }
    port_ptcl_lookpar_note(checked, bad);
}
// The draw's look flush of one emitter, as JPAResource::draw begins (host test tests/par_ptcl.cpp, PTCL_LOOKPAR: a picture between turns).
extern "C" void port_jpa_look_flush_emitter(JPABaseEmitter* emtr, JPAEmitterWorkData* work) {
    work->mpEmtr = emtr;
    work->mpRes = emtr->mpRes;
    if (emtr->mPortLookStale) {
        PortLookFlush(emtr->mpRes, work, emtr);
    } else if (emtr->mPortLookByPar) {
        emtr->mPortLookByPar = 0;
        if (port_ptcl_lookpar_mode() == 2 || port_jpa_look_pure_mode() == 2) {
            PortLookVerify(emtr->mpRes, work, emtr);
        }
    }
}
#endif

JPAResource::JPAResource() {
    mpDrawEmitterChildFuncList = NULL;
    mpDrawEmitterFuncList = NULL;
    mpCalcEmitterFuncList = NULL;
    mpDrawParticleChildFuncList = NULL;
    mpCalcParticleChildFuncList = NULL;
    mpDrawParticleFuncList = NULL;
    mpCalcParticleFuncList = NULL;
    mpBaseShape = NULL;
    mpExtraShape = NULL;
    mpChildShape = NULL;
    mpExTexShape = NULL;
    mpDynamicsBlock = NULL;
    mpFieldBlocks = NULL;
    mpKeyBlocks = NULL;
    mpTDB1 = NULL;
    mpDrawParticleChildFuncListNum = 0;
    mpCalcParticleChildFuncListNum = 0;
    mpDrawParticleFuncListNum = 0;
    mpCalcParticleFuncListNum = 0;
    mpDrawEmitterChildFuncListNum = 0;
    mpDrawEmitterFuncListNum = 0;
    mpCalcEmitterFuncListNum = 0;
    mTDB1Num = 0;
    mKeyBlockNum = 0;
    mFieldBlockNum = 0;
    mUsrIdx = 0;
}

static u8 jpa_pos[324] ATTRIBUTE_ALIGN(32) = {
    0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0xCE, 0x00, 0xE7, 0xCE, 0x00,
    0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0xCE, 0xCE, 0x00, 0x00, 0x19, 0x00, 0x32, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x00, 0xE7, 0x00,
    0xE7, 0x19, 0x00, 0x19, 0x19, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0xE7, 0x00, 0xCE, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0xCE, 0xE7, 0x00,
    0x00, 0x32, 0x00, 0x32, 0x32, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x32, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00,
    0xCE, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0x00, 0x32, 0x00, 0x00, 0x32,
    0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x32, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x32,
    0x00, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x00, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0x19, 0x00, 0xE7, 0x19, 0x00, 0x19, 0xE7, 0x00, 0x19,
    0xCE, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE7, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xCE, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0xE7,
    0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0xCE, 0x00, 0x19, 0x00, 0x00, 0x19, 0x32, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x00,
    0x00, 0x19, 0xE7, 0x00, 0x19, 0x19, 0x00, 0xE7, 0x19, 0x00, 0xE7, 0xE7, 0x00, 0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0xCE,
    0x00, 0x32, 0x00, 0x00, 0x32, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x32, 0xE7, 0x00, 0x32, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7,
    0x00, 0x32, 0xCE, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCE,
};

#ifdef TARGET_PC
// (GXV_HOTPROF_PTCLCULL: the position array a loop's quads index; nullptr when the crossed quad's last vertex would lie past the table)
static const u8* PortPtclCullPosBase(int posOffset) {
    if (posOffset < 0 || posOffset + (0x4B + 1) * 3 > static_cast< int >(sizeof(jpa_pos))) {
        return nullptr;
    }
    return jpa_pos + posOffset;
}
#endif

static u8 jpa_crd[32] ATTRIBUTE_ALIGN(32) = {
    0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x02, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x02, 0x00, 0x02,
};

void JPAResource::init(JKRHeap* heap) {
    u32 is_glbl_clr_anm = mpBaseShape->isGlblClrAnm();
    u32 is_glbl_tex_anm = mpBaseShape->isGlblTexAnm();
    BOOL is_prm_anm = mpBaseShape->isPrmAnm();
    BOOL is_env_anm = mpBaseShape->isEnvAnm();
    BOOL is_tex_anm = mpBaseShape->isTexAnm();
    BOOL is_tex_crd_anm = mpBaseShape->isTexCrdAnm();
    BOOL is_prj_tex = mpBaseShape->isPrjTex();
    BOOL is_enable_scale_anm = mpExtraShape != NULL && mpExtraShape->isEnableScaleAnm();
    BOOL is_enable_alpha_anm = mpExtraShape != NULL && mpExtraShape->isEnableAlphaAnm();
    BOOL is_enable_alpha_flick = mpExtraShape != NULL && mpExtraShape->isEnableAlphaAnm() && mpExtraShape->isEnableAlphaFlick();
    BOOL is_enable_rotate_anm = mpExtraShape != NULL && mpExtraShape->isEnableRotateAnm();
    BOOL is_rotate_on = is_enable_rotate_anm || (mpChildShape != NULL && mpChildShape->isRotateOn());
    BOOL base_type_5_6 = mpBaseShape->getType() == 5 || mpBaseShape->getType() == 6;
    BOOL base_type_0 = mpBaseShape->getType() == 0;
    BOOL base_type_0_1 = mpBaseShape->getType() == 0 || mpBaseShape->getType() == 1;
    BOOL child_type_5_6 = mpChildShape != NULL && (mpChildShape->getType() == 5 || mpChildShape->getType() == 6);
    BOOL child_type_0 = mpChildShape != NULL && mpChildShape->getType() == 0;
    BOOL child_type_0_1 = mpChildShape != NULL && (mpChildShape->getType() == 0 || mpChildShape->getType() == 1);
    BOOL is_draw_parent = !mpBaseShape->isNoDrawParent();
    BOOL is_draw_child = !mpBaseShape->isNoDrawChild();

    if (is_glbl_tex_anm && is_tex_anm) {
        mpCalcEmitterFuncListNum++;
    }

    if (is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcEmitterFuncListNum++;
        }
        if (is_env_anm) {
            mpCalcEmitterFuncListNum++;
        }
        if (is_prm_anm || is_env_anm) {
            mpCalcEmitterFuncListNum++;
        }
    }

    if (mpCalcEmitterFuncListNum != 0) {
        mpCalcEmitterFuncList = (EmitterFunc*)JKRAllocFromHeap(heap, mpCalcEmitterFuncListNum * sizeof(void*), sizeof(void*));
    }

    int func_no = 0;

    if (is_glbl_tex_anm && is_tex_anm) {
        switch (mpBaseShape->getTexAnmType()) {
        case 0:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxNormal;
            break;
        case 1:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxRepeat;
            break;
        case 2:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxReverse;
            break;
        case 3:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxMerge;
            break;
        case 4:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxRandom;
            break;
        }
        func_no++;
    }

    if (is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcEmitterFuncList[func_no] = &JPACalcPrm;
            func_no++;
        }
        if (is_env_anm) {
            mpCalcEmitterFuncList[func_no] = &JPACalcEnv;
            func_no++;
        }
        if (is_prm_anm || is_env_anm) {
            switch (mpBaseShape->getClrAnmType()) {
            case 0:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxNormal;
                break;
            case 1:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxRepeat;
                break;
            case 2:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxReverse;
                break;
            case 3:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxMerge;
                break;
            case 4:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxRandom;
                break;
            }
        }
    }

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpCalcParticleFuncListNum++;
    }

    if (!base_type_5_6 && (is_enable_alpha_anm || is_enable_alpha_flick)) {
        mpCalcParticleFuncListNum++;
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcParticleFuncListNum++;
        }
        if (is_env_anm) {
            mpCalcParticleFuncListNum++;
        }
        if (is_prm_anm || is_env_anm) {
            mpCalcParticleFuncListNum++;
        }
    } else {
        mpCalcParticleFuncListNum++;
    }

    if (is_enable_scale_anm) {
        if (mpBaseShape->getType() != 0) {
            if (mpExtraShape->isScaleXYDiff()) {
                if (mpExtraShape->getScaleAnmTypeX() == 0 && mpExtraShape->getScaleAnmTypeY() == 0) {
                    mpCalcParticleFuncListNum++;
                } else {
                    mpCalcParticleFuncListNum++;
                    mpCalcParticleFuncListNum++;
                }
            } else {
                mpCalcParticleFuncListNum++;
            }
        }
        mpCalcParticleFuncListNum++;
        mpCalcParticleFuncListNum++;
    }

    if (mpCalcParticleFuncListNum != 0) {
        mpCalcParticleFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpCalcParticleFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (!is_glbl_tex_anm && is_tex_anm) {
        switch (mpBaseShape->getTexAnmType()) {
        case 0:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxNormal;
            break;
        case 1:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxRepeat;
            break;
        case 2:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxReverse;
            break;
        case 3:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxMerge;
            break;
        case 4:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxRandom;
            break;
        }
        func_no++;
    }

    if (!base_type_5_6 && (is_enable_alpha_anm || is_enable_alpha_flick)) {
        if (is_enable_alpha_flick) {
            mpCalcParticleFuncList[func_no] = &JPACalcAlphaFlickAnm;
            func_no++;
        } else {
            mpCalcParticleFuncList[func_no] = &JPACalcAlphaAnm;
            func_no++;
        }
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcParticleFuncList[func_no] = &JPACalcPrm;
            func_no++;
        }
        if (is_env_anm) {
            mpCalcParticleFuncList[func_no] = &JPACalcEnv;
            func_no++;
        }
        if (is_prm_anm || is_env_anm) {
            switch (mpBaseShape->getClrAnmType()) {
            case 0:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxNormal;
                break;
            case 1:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxRepeat;
                break;
            case 2:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxReverse;
                break;
            case 3:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxMerge;
                break;
            case 4:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxRandom;
                break;
            }
            func_no++;
        }
    } else {
        mpCalcParticleFuncList[func_no] = &JPACalcColorCopy;
        func_no++;
    }

    if (is_enable_scale_anm) {
        if (mpBaseShape->getType() != 0) {
            if (mpExtraShape->isScaleXYDiff()) {
                mpCalcParticleFuncList[func_no] = &JPACalcScaleY;
                func_no++;
                if (mpExtraShape->getScaleAnmTypeY() != 0 || mpExtraShape->getScaleAnmTypeX() != 0) {
                    switch (mpExtraShape->getScaleAnmTypeY()) {
                    case 0:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmNormal;
                        break;
                    case 1:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmRepeatY;
                        break;
                    case 2:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmReverseY;
                        break;
                    }
                    func_no++;
                }
            } else {
                mpCalcParticleFuncList[func_no] = &JPACalcScaleCopy;
                func_no++;
            }
        }
        mpCalcParticleFuncList[func_no] = &JPACalcScaleX;
        func_no++;
        switch (mpExtraShape->getScaleAnmTypeX()) {
        case 0:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmNormal;
            break;
        case 1:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmRepeatX;
            break;
        case 2:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmReverseX;
            break;
        }
    }

    if (mpChildShape != NULL && mpChildShape->isScaleOutOn()) {
        mpCalcParticleChildFuncListNum++;
    }

    if (mpChildShape != NULL && mpChildShape->isAlphaOutOn()) {
        mpCalcParticleChildFuncListNum++;
    }

    if (mpCalcParticleChildFuncListNum != 0) {
        mpCalcParticleChildFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpCalcParticleChildFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (mpChildShape != NULL && mpChildShape->isScaleOutOn()) {
        mpCalcParticleChildFuncList[func_no] = &JPACalcChildScaleOut;
        func_no++;
    }

    if (mpChildShape != NULL && mpChildShape->isAlphaOutOn()) {
        mpCalcParticleChildFuncList[func_no] = &JPACalcChildAlphaOut;
    }

    if (is_draw_parent && base_type_5_6) {
        mpDrawEmitterFuncListNum++;
    }

    mpDrawEmitterFuncListNum++;

    if (mpExTexShape != NULL) {
        mpDrawEmitterFuncListNum++;
    }

    if (is_glbl_tex_anm || !is_tex_anm) {
        mpDrawEmitterFuncListNum++;
    }

    mpDrawEmitterFuncListNum++;

    if (base_type_0_1) {
        mpDrawEmitterFuncListNum++;
    }

    if (base_type_0_1 && !is_enable_scale_anm) {
        mpDrawEmitterFuncListNum++;
    }

    if (is_glbl_clr_anm || (!is_prm_anm && !is_enable_alpha_anm) || !is_env_anm) {
        mpDrawEmitterFuncListNum++;
    }

    if (mpDrawEmitterFuncListNum != 0) {
        mpDrawEmitterFuncList = (EmitterFunc*)JKRAllocFromHeap(heap, mpDrawEmitterFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_parent && base_type_5_6) {
        if (mpBaseShape->getType() == 5) {
            mpDrawEmitterFuncList[func_no] = &JPADrawStripe;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPADrawStripeX;
            func_no++;
        }
    }

    mpDrawEmitterFuncList[func_no] = &JPADrawEmitterCallBackB;
    func_no++;

    if (mpExTexShape != NULL) {
        mpDrawEmitterFuncList[func_no] = &JPALoadExTex;
        func_no++;
    }

    if (!is_tex_anm) {
        mpDrawEmitterFuncList[func_no] = &JPALoadTex;
        func_no++;
    } else if (is_glbl_tex_anm) {
        mpDrawEmitterFuncList[func_no] = &JPALoadTexAnm;
        func_no++;
    }

    if (base_type_0_1) {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxIdt;
        func_no++;
    } else if (is_prj_tex) {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxPrj;
        func_no++;
    } else if (is_tex_crd_anm) {
        if (base_type_5_6) {
            mpDrawEmitterFuncList[func_no] = &JPAGenCalcTexCrdMtxAnm;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxAnm;
            func_no++;
        }
    } else {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxIdt;
        func_no++;
    }

    if (base_type_0_1) {
        mpDrawEmitterFuncList[func_no] = &JPALoadPosMtxCam;
        func_no++;
    }

    if (base_type_0_1 && !is_enable_scale_anm) {
        if (base_type_0) {
            mpDrawEmitterFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    }

    if (is_glbl_clr_anm) {
        if (base_type_5_6 || !is_enable_alpha_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrmEnv;
        } else if (is_enable_alpha_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistEnv;
        }
    } else if (!is_prm_anm && !is_enable_alpha_anm) {
        if (!is_env_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrmEnv;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrm;
        }
    } else if (!is_env_anm) {
        mpDrawEmitterFuncList[func_no] = &JPARegistEnv;
    }

    if (is_draw_child && child_type_5_6) {
        mpDrawEmitterChildFuncListNum++;
    }

    mpDrawEmitterChildFuncListNum++;

    if (child_type_0_1) {
        mpDrawEmitterChildFuncListNum++;
    }

    if (mpChildShape != NULL && !mpChildShape->isAlphaOutOn() && !mpChildShape->isAlphaInherited() && !mpChildShape->isColorInherited()) {
        mpDrawEmitterChildFuncListNum++;
    }

    if (mpDrawEmitterChildFuncListNum != 0) {
        mpDrawEmitterChildFuncList = (EmitterFunc*)JKRAllocFromHeap(heap, mpDrawEmitterChildFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_child && child_type_5_6) {
        if (mpChildShape->getType() == 5) {
            mpDrawEmitterChildFuncList[func_no] = &JPADrawStripe;
            func_no++;
        } else {
            mpDrawEmitterChildFuncList[func_no] = &JPADrawStripeX;
            func_no++;
        }
    }

    mpDrawEmitterChildFuncList[func_no] = &JPADrawEmitterCallBackB;
    func_no++;

    if (child_type_0_1) {
        mpDrawEmitterChildFuncList[func_no] = &JPALoadPosMtxCam;
        func_no++;
    }

    if (mpChildShape != NULL && !mpChildShape->isAlphaOutOn() && !mpChildShape->isAlphaInherited() && !mpChildShape->isColorInherited()) {
        mpDrawEmitterChildFuncList[func_no] = &JPARegistChildPrmEnv;
    }

    if (is_draw_parent && !base_type_5_6) {
        mpDrawParticleFuncListNum++;
    }

    mpDrawParticleFuncListNum++;

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpDrawParticleFuncListNum++;
    }

    if ((base_type_0_1 && is_enable_scale_anm) || (is_tex_crd_anm && !is_prj_tex)) {
        mpDrawParticleFuncListNum++;
    }

    if ((!is_glbl_clr_anm && (is_prm_anm || is_env_anm || is_enable_alpha_anm)) || (is_glbl_clr_anm && is_enable_alpha_anm && !base_type_5_6)) {
        mpDrawParticleFuncListNum++;
    }

    if (mpDrawParticleFuncListNum != 0) {
        mpDrawParticleFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpDrawParticleFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_parent && !base_type_5_6) {
        switch (mpBaseShape->getType()) {
        case 2:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotBillboard;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawBillboard;
            }
            break;
        case 10:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotYBillboard;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawYBillboard;
            }
            break;
        case 3:
        case 4:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotDirection;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawDirection;
            }
            break;
        case 9:
            mpDrawParticleFuncList[func_no] = &JPADrawDBillboard;
            break;
        case 7:
        case 8:
            mpDrawParticleFuncList[func_no] = &JPADrawRotation;
            break;
        case 0:
            mpDrawParticleFuncList[func_no] = &JPADrawPoint;
            break;
        case 1:
            mpDrawParticleFuncList[func_no] = &JPADrawLine;
            break;
        }
        func_no++;
    }

    mpDrawParticleFuncList[func_no] = &JPADrawParticleCallBack;
    func_no++;

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpDrawParticleFuncList[func_no] = &JPALoadTexAnm;
        func_no++;
    }

    if (base_type_0_1 && is_enable_scale_anm) {
        if (base_type_0) {
            mpDrawParticleFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawParticleFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    } else if (is_tex_crd_anm && !is_prj_tex) {
        mpDrawParticleFuncList[func_no] = &JPALoadCalcTexCrdMtxAnm;
        func_no++;
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            if (is_env_anm) {
                mpDrawParticleFuncList[func_no] = &JPARegistPrmAlphaEnv;
            } else {
                mpDrawParticleFuncList[func_no] = &JPARegistPrmAlpha;
            }
        } else if (is_enable_alpha_anm) {
            if (is_env_anm) {
                mpDrawParticleFuncList[func_no] = &JPARegistAlphaEnv;
            } else {
                mpDrawParticleFuncList[func_no] = &JPARegistAlpha;
            }
        } else if (is_env_anm) {
            mpDrawParticleFuncList[func_no] = &JPARegistEnv;
        }
    } else if (is_enable_alpha_anm && !base_type_5_6) {
        mpDrawParticleFuncList[func_no] = &JPARegistAlpha;
    }

    if (is_draw_child && mpChildShape != NULL && !child_type_5_6) {
        mpDrawParticleChildFuncListNum++;
    }

    mpDrawParticleChildFuncListNum++;

    if (child_type_0_1) {
        mpDrawParticleChildFuncListNum++;
    }

    if (mpChildShape != NULL && (mpChildShape->isAlphaOutOn() || mpChildShape->isAlphaInherited() || mpChildShape->isColorInherited())) {
        mpDrawParticleChildFuncListNum++;
    }

    if (mpDrawParticleChildFuncListNum != 0) {
        mpDrawParticleChildFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpDrawParticleChildFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_child && mpChildShape != NULL && !child_type_5_6) {
        switch (mpChildShape->getType()) {
        case 2:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotBillboard;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawBillboard;
            }
            break;
        case 10:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotYBillboard;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawYBillboard;
            }
            break;
        case 3:
        case 4:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotDirection;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawDirection;
            }
            break;
        case 9:
            mpDrawParticleChildFuncList[func_no] = &JPADrawDBillboard;
            break;
        case 7:
        case 8:
            mpDrawParticleChildFuncList[func_no] = &JPADrawRotation;
            break;
        case 0:
            mpDrawParticleChildFuncList[func_no] = &JPADrawPoint;
            break;
        case 1:
            mpDrawParticleChildFuncList[func_no] = &JPADrawLine;
            break;
        }
        func_no++;
    }

    mpDrawParticleChildFuncList[func_no] = &JPADrawParticleCallBack;
    func_no++;

    if (child_type_0_1) {
        if (child_type_0) {
            mpDrawParticleChildFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawParticleChildFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    }

    if (mpChildShape != NULL && (mpChildShape->isAlphaOutOn() || mpChildShape->isAlphaInherited() || mpChildShape->isColorInherited())) {
        mpDrawParticleChildFuncList[func_no] = &JPARegistPrmAlphaEnv;
    }
}

#ifdef TARGET_PC
// (GXV_PAR_PTCL: the emitter makes its particles with the work area this calc runs on, and with no other afterwards)
bool JPAResource::calc(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    emtr->mPortWork = work;
    const bool r = calcBody(work, emtr);
    emtr->mPortWork = nullptr;
    return r;
}
bool JPAResource::calcBody(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
#else
bool JPAResource::calc(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
#endif
    work->mpEmtr = emtr;
    work->mpRes = this;
    work->mEmitCount = 0;

    if (!emtr->processTillStartFrame()) {
        return false;
    }

    if (emtr->processTermination()) {
        return true;
    }

    if (emtr->checkStatus(2)) {
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->execute(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
            emtr->mpEmtrCallBack->executeAfter(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

    } else {
        calcKey(work);

        for (int i = mFieldBlockNum - 1; i >= 0; i--) {
            mpFieldBlocks[i]->initOpParam();
        }

        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->execute(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

        calcWorkData_c(work);

        for (int i = mpCalcEmitterFuncListNum - 1; i >= 0; i--) {
            (*mpCalcEmitterFuncList[i])(work);
        }

        for (int i = mFieldBlockNum - 1; i >= 0; i--) {
            mpFieldBlocks[i]->prepare(work);
        }

        if (!emtr->checkStatus(8)) {
            mpDynamicsBlock->create(work);
        }

        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->executeAfter(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

#ifdef TARGET_PC
        // (GXV_JPA_LOOK, above: which look functions this calc leaves to the next draw)
        work->mPortLookSkipP = false;
        work->mPortLookSkipC = false;
        const int pureMode = emtr->mpPtclCallBack != NULL ? port_jpa_look_pure_mode() : 0;
        const bool lookable = (emtr->mpPtclCallBack == NULL || (pureMode != 0 && emtr->mpPtclCallBack->portCalcKind() == 1)) && port_jpa_look_on();
        const bool lookNow = gJpaLookPar && emtr->mPortLookNow;  // (GXV_PTCL_LOOKPAR: the original's way, on the particle worker)
        const bool verifyPure = pureMode == 2 && emtr->mpPtclCallBack != NULL;  // (GXV_JPA_LOOK_PURE=2: as before, checked at the draw)
        emtr->mPortLookByPar = (lookable && (lookNow || verifyPure)) ? 1 : 0;
        if (lookable && !lookNow && !verifyPure) {
            work->mPortLookSkipC = mpCalcParticleChildFuncList != NULL;
            work->mPortLookSkipP = mpCalcParticleFuncList != NULL &&
                             (mpChildShape == NULL ||
                              !(mpChildShape->isScaleInherited() || mpChildShape->isAlphaInherited() || mpChildShape->isColorInherited()));
        }
        // (a calc that runs them all leaves every particle as the draw needs it)
        emtr->mPortLookStale = (work->mPortLookSkipP || work->mPortLookSkipC) ? 1 : 0;
        port_logic_count(0, emtr->mAlivePtclBase.getNum() + emtr->mAlivePtclChld.getNum());
        port_logic_count(2, 1);
#endif
        JPANode< JPABaseParticle >* node = emtr->mAlivePtclBase.getFirst();
        JPANode< JPABaseParticle >* next;
        while (node != emtr->mAlivePtclBase.getEnd()) {
            next = node->getNext();
            if (node->getObject()->calc_p(work)) {
#ifdef TARGET_PC
                jpaPoolPush(emtr->mpPtclPool, emtr->mAlivePtclBase.erase(node));
#else
                emtr->mpPtclPool->push_front(emtr->mAlivePtclBase.erase(node));
#endif
            }
            node = next;
        }

        JPANode< JPABaseParticle >* child = emtr->mAlivePtclChld.getFirst();
        while (child != emtr->mAlivePtclChld.getEnd()) {
            JPANode< JPABaseParticle >* childNext = child->getNext();
            if (child->getObject()->calc_c(work)) {
#ifdef TARGET_PC
                jpaPoolPush(emtr->mpPtclPool, emtr->mAlivePtclChld.erase(child));
#else
                emtr->mpPtclPool->push_front(emtr->mAlivePtclChld.erase(child));
#endif
            }
            child = childNext;
        }
#ifdef TARGET_PC
        work->mPortLookSkipP = false;
        work->mPortLookSkipC = false;
#endif

        emtr->mTick++;
    }

    return false;
}

void JPAResource::draw(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    work->mpEmtr = emtr;
    work->mpRes = this;
    work->mDrawCount = 0;
#ifdef TARGET_PC
    if (emtr->mPortLookStale) {
        PortLookFlush(this, work, emtr);  // (GXV_JPA_LOOK)
    } else if (emtr->mPortLookByPar) {
        emtr->mPortLookByPar = 0;
        if (port_ptcl_lookpar_mode() == 2 || port_jpa_look_pure_mode() == 2) {
            PortLookVerify(this, work, emtr);  // (GXV_PTCL_LOOKPAR=2, GXV_JPA_LOOK_PURE=2)
        }
    }
#endif
    calcWorkData_d(work);
    mpBaseShape->setGX(work);
#ifdef TARGET_PC
    PortThinBegin(work, emtr);  // (GXV_PTCL_THIN)
#endif
    for (s32 i = 1; i <= emtr->getDrawTimes(); i++) {
        work->mDrawCount++;
        if (mpBaseShape->isDrawPrntAhead() && mpChildShape != NULL)
            drawC(work);
        drawP(work);
        if (!mpBaseShape->isDrawPrntAhead() && mpChildShape != NULL)
            drawC(work);
    }
#ifdef TARGET_PC
    sPtclThinKeep = 1.0f;
#endif
}

void JPAResource::drawP(JPAEmitterWorkData* work) {
    work->mpEmtr->clearStatus(0x80);

    work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * mpBaseShape->getBaseSizeX();
    work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * mpBaseShape->getBaseSizeY();

    if (mpBaseShape->getType() == 0) {
        work->mGlobalPtclScl.x *= 1.02f;
    } else if (mpBaseShape->getType() == 1) {
        work->mGlobalPtclScl.x *= 1.02f;
        work->mGlobalPtclScl.y *= 0.4f;
    }

    if (mpExtraShape != NULL && mpExtraShape->isEnableScaleAnm()) {
        work->mPivot.x = mpExtraShape->getScaleCenterX() - 1.0f;
        work->mPivot.y = mpExtraShape->getScaleCenterY() - 1.0f;
    } else {
        work->mPivot.x = work->mPivot.y = 0.0f;
    }

    work->mDirType = mpBaseShape->getDirType();
    work->mRotType = mpBaseShape->getRotType();
    work->mDLType = mpBaseShape->getType() == 4 || mpBaseShape->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : mpBaseShape->getBasePlaneType();
    work->mPrjType = mpBaseShape->isPrjTex() ? (mpBaseShape->isTexCrdAnm() ? 2 : 1) : 0;

    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclBase;
    setPTev();

    for (int i = mpDrawEmitterFuncListNum - 1; i >= 0; i--) {
        (*mpDrawEmitterFuncList[i])(work);
    }

#ifdef TARGET_PC
    // (GXV_FXMERGE: the particles' GX calls gathered, above)
    const bool portFxOk = PortFxListOk(mpDrawParticleFuncList, mpDrawParticleFuncListNum, work->mpEmtr);
    const bool portFx = portFxOk && port_gx_fx_begin();
    PortPtclCullBegin(portFxOk);  // (GXV_HOTPROF_PTCLCULL)
    const bool portTevScope = portFxOk && !portFx && port_gx_tevcol_scope;  // (GXV_HOTPROF_TEVCOL: not while the loop is gathered)
    if (portTevScope) {
        port_gx_tevcol_scope(1);
    }
#endif
    if (mpBaseShape->isDrawFwdAhead()) {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclBase.getLast();
        for (; node != work->mpEmtr->mAlivePtclBase.getEnd(); node = node->getPrev()) {
            work->mpCurNode = node;
            if (sPtclThinKeep < 1.0f && PortThinSkip(node->getObject())) {
                continue;  // (GXV_PTCL_THIN)
            }
            if (mpDrawParticleFuncList != NULL) {
                for (int i = mpDrawParticleFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleFuncList[i])(work, node->getObject());
                }
            }
        }
    } else {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclBase.getFirst();
        for (; node != work->mpEmtr->mAlivePtclBase.getEnd(); node = node->getNext()) {
            work->mpCurNode = node;
            if (sPtclThinKeep < 1.0f && PortThinSkip(node->getObject())) {
                continue;  // (GXV_PTCL_THIN)
            }
            if (mpDrawParticleFuncList != NULL) {
                for (int i = mpDrawParticleFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleFuncList[i])(work, node->getObject());
                }
            }
        }
    }
#ifdef TARGET_PC
    PortPtclCullEnd(work);  // (GXV_HOTPROF_PTCLCULL: the last left-out quad's matrices, inside the gathering)
    if (portTevScope) {
        port_gx_tevcol_scope(0);
    }
    if (portFx) {
        port_gx_fx_end();
    }
#endif

    GXSetMisc(GX_MT_XF_FLUSH, 0);

    if (work->mpEmtr->mpEmtrCallBack != NULL) {
        work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
    }
}

void JPAResource::drawC(JPAEmitterWorkData* work) {
    work->mpEmtr->setStatus(0x80);

    if (mpChildShape->isScaleInherited()) {
        work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * mpBaseShape->getBaseSizeX();
        work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * mpBaseShape->getBaseSizeY();
    } else {
        work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * mpChildShape->getScaleX();
        work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * mpChildShape->getScaleY();
    }

    if (mpChildShape->getType() == 0) {
        work->mGlobalPtclScl.x *= 1.02f;
    } else if (mpChildShape->getType() == 1) {
        work->mGlobalPtclScl.x *= 1.02f;
        work->mGlobalPtclScl.y *= 0.4f;
    }

    work->mPivot.x = work->mPivot.y = 0.0f;

    work->mDirType = mpChildShape->getDirType();
    work->mRotType = mpChildShape->getRotType();
    work->mDLType = mpChildShape->getType() == 4 || mpChildShape->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : mpChildShape->getBasePlaneType();
    work->mPrjType = 0;

    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclChld;
    setCTev(work);

    for (int i = mpDrawEmitterChildFuncListNum - 1; i >= 0; i--) {
        (*mpDrawEmitterChildFuncList[i])(work);
    }

#ifdef TARGET_PC
    // (GXV_FXMERGE, as in drawP)
    const bool portFxOk = PortFxListOk(mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum, work->mpEmtr);
    const bool portFx = portFxOk && port_gx_fx_begin();
    PortPtclCullBegin(portFxOk);  // (GXV_HOTPROF_PTCLCULL)
    const bool portTevScope = portFxOk && !portFx && port_gx_tevcol_scope;  // (GXV_HOTPROF_TEVCOL: not while the loop is gathered)
    if (portTevScope) {
        port_gx_tevcol_scope(1);
    }
#endif
    if (mpBaseShape->isDrawFwdAhead()) {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclChld.getLast();
        for (; node != work->mpEmtr->mAlivePtclChld.getEnd(); node = node->getPrev()) {
            work->mpCurNode = node;
            if (sPtclThinKeep < 1.0f && PortThinSkip(node->getObject())) {
                continue;  // (GXV_PTCL_THIN)
            }
            if (mpDrawParticleChildFuncList != NULL) {
                for (int i = mpDrawParticleChildFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleChildFuncList[i])(work, node->getObject());
                }
            }
        }
    } else {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclChld.getFirst();
        for (; node != work->mpEmtr->mAlivePtclChld.getEnd(); node = node->getNext()) {
            work->mpCurNode = node;
            if (sPtclThinKeep < 1.0f && PortThinSkip(node->getObject())) {
                continue;  // (GXV_PTCL_THIN)
            }
            if (mpDrawParticleChildFuncList != NULL) {
                for (int i = mpDrawParticleChildFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleChildFuncList[i])(work, node->getObject());
                }
            }
        }
    }
#ifdef TARGET_PC
    PortPtclCullEnd(work);  // (GXV_HOTPROF_PTCLCULL: the last left-out quad's matrices, inside the gathering)
    if (portTevScope) {
        port_gx_tevcol_scope(0);
    }
    if (portFx) {
        port_gx_fx_end();
    }
#endif

    GXSetMisc(GX_MT_XF_FLUSH, 0);

    if (work->mpEmtr->mpEmtrCallBack != NULL) {
        work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
    }
}

void JPAResource::setPTev() {
    GXTexCoordID tex_coord = GX_TEXCOORD1;
    u8 tev_stages = 1;
    u8 tex_gens = 1;
    u8 ind_stages = 0;

    int base_plane_type = (mpBaseShape->getType() == 3 || mpBaseShape->getType() == 7) ? mpBaseShape->getBasePlaneType() : 0;
    int center_offset;
    if (mpExtraShape != NULL) {
        center_offset = (mpExtraShape->getScaleCenterX() + 3 * mpExtraShape->getScaleCenterY()) * 0xC;
    } else {
        center_offset = 0x30;
    }
    int pos_offset = center_offset + base_plane_type * 0x6C;
    int crd_offset = (mpBaseShape->getTilingS() + 2 * mpBaseShape->getTilingT()) * 8;
#ifdef TARGET_PC
    sPtclCull.posOffset = pos_offset;  // (GXV_HOTPROF_PTCLCULL)
#endif
    GXSetArray(GX_VA_POS, jpa_pos + pos_offset, 3);
    GXSetArray(GX_VA_TEX0, jpa_crd + crd_offset, 2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);

    if (mpExTexShape != NULL) {
        if (mpExTexShape->isUseIndirect()) {
            GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP2);
            GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
            GXSetIndTexMtx(GX_ITM_0, (f32(*)[3])mpExTexShape->getIndTexMtx(), mpExTexShape->getExpScale());
            GXSetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, 0, 0, GX_ITBA_OFF);
            ind_stages++;
            tex_gens++;
            tex_coord = GX_TEXCOORD2;
        }
        if (mpExTexShape->isUseSecTex()) {
            GXSetTevOrder(GX_TEVSTAGE1, tex_coord, GX_TEXMAP3, GX_COLOR_NULL);
            GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
            GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            tev_stages++;
            tex_gens++;
        }
    }

    GXSetNumTevStages(tev_stages);
    GXSetNumIndStages(ind_stages);
    if (mpBaseShape->isClipOn()) {
        GXSetMisc(GX_MT_XF_FLUSH, 8);
        GXSetClipMode(GX_CLIP_ENABLE);
    } else {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXSetNumTexGens(tex_gens);
}

void JPAResource::setCTev(JPAEmitterWorkData* work) {
    int base_plane_type = (mpChildShape->getType() == 3 || mpChildShape->getType() == 7) ? mpChildShape->getBasePlaneType() : 0;
    int pos_offset = 0x30 + base_plane_type * 0x6C;
#ifdef TARGET_PC
    sPtclCull.posOffset = pos_offset;  // (GXV_HOTPROF_PTCLCULL)
#endif
    GXSetArray(GX_VA_POS, jpa_pos + pos_offset, 3);
    GXSetArray(GX_VA_TEX0, jpa_crd, 2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR_NULL);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3C, GX_FALSE, 0x7D);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetNumTevStages(1);
    GXSetNumIndStages(0);
    if (mpChildShape->isClipOn()) {
        GXSetMisc(GX_MT_XF_FLUSH, 8);
        GXSetClipMode(GX_CLIP_ENABLE);
    } else {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXSetNumTexGens(1);
    work->mpResMgr->CALL_INLINE_FUNC(load, work->mpRes->getTexIdx(mpChildShape->getTexIdx()), GX_TEXMAP1);
}

void JPAResource::calc_p(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
#ifdef TARGET_PC
    if (work->mPortLookSkipP) {
        return;  // (GXV_JPA_LOOK: at the next draw)
    }
#endif
    if (mpCalcParticleFuncList != NULL) {
        for (int i = mpCalcParticleFuncListNum - 1; i >= 0; i--) {
            (*mpCalcParticleFuncList[i])(work, ptcl);
        }
    }
}

void JPAResource::calc_c(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
#ifdef TARGET_PC
    if (work->mPortLookSkipC) {
        return;  // (GXV_JPA_LOOK: at the next draw)
    }
#endif
    if (mpCalcParticleChildFuncList != NULL) {
        for (int i = mpCalcParticleChildFuncListNum - 1; i >= 0; i--) {
            (*mpCalcParticleChildFuncList[i])(work, ptcl);
        }
    }
}

void JPAResource::calcField(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    for (int i = mFieldBlockNum - 1; i >= 0; i--) {
        mpFieldBlocks[i]->calc(work, ptcl);
    }
}

void JPAResource::calcKey(JPAEmitterWorkData* work) {
    for (int i = mKeyBlockNum - 1; i >= 0; i--) {
        f32 val = mpKeyBlocks[i]->calc(work->mpEmtr->mTick);
        switch (mpKeyBlocks[i]->getID()) {
        case 0:
            work->mpEmtr->mRate = val;
            break;
        case 1:
            work->mpEmtr->mVolumeSize = val;
            break;
        case 3:
            work->mpEmtr->mVolumeMinRad = val;
            break;
        case 4:
            work->mpEmtr->mLifeTime = val;
            break;
        case 6:
            work->mpEmtr->mAwayFromCenterSpeed = val;
            break;
        case 7:
            work->mpEmtr->mAwayFromAxisSpeed = val;
            break;
        case 8:
            work->mpEmtr->mDirSpeed = val;
            break;
        case 9:
            work->mpEmtr->mSpread = val;
            break;
        case 10:
            work->mpEmtr->mScaleOut = val;
            break;
        }
    }
}

void JPAResource::calcWorkData_c(JPAEmitterWorkData* work) {
    work->mVolumeSize = work->mpEmtr->mVolumeSize;
    work->mVolumeMinRad = work->mpEmtr->mVolumeMinRad;
    work->mVolumeSweep = work->mpEmtr->mVolumeSweep;
    work->mVolumeX = 0;
    work->mVolumeAngleNum = 0;
    work->mVolumeAngleMax = 1;
    work->mDivNumber = mpDynamicsBlock->getDivNumber() * 2 + 1;
    Mtx local_scl_mtx, local_rot_mtx, global_mtx;
    PSMTXScale(local_scl_mtx, work->mpEmtr->mLocalScl.x, work->mpEmtr->mLocalScl.y, work->mpEmtr->mLocalScl.z);
    JPAGetXYZRotateMtx(work->mpEmtr->mLocalRot.x * 182, work->mpEmtr->mLocalRot.y * 182, work->mpEmtr->mLocalRot.z * 182, local_rot_mtx);
    PSMTXScale(global_mtx, work->mpEmtr->mGlobalScl.x, work->mpEmtr->mGlobalScl.y, work->mpEmtr->mGlobalScl.z);
    PSMTXConcat(work->mpEmtr->mGlobalRot, global_mtx, global_mtx);
    global_mtx[0][3] = work->mpEmtr->mGlobalTrs.x;
    global_mtx[1][3] = work->mpEmtr->mGlobalTrs.y;
    global_mtx[2][3] = work->mpEmtr->mGlobalTrs.z;
    PSMTXCopy(work->mpEmtr->mGlobalRot, work->mRotationMtx);
    PSMTXConcat(work->mRotationMtx, local_rot_mtx, work->mGlobalRot);
    PSMTXConcat(work->mGlobalRot, local_scl_mtx, work->mGlobalSR);
    work->mEmitterPos.set(work->mpEmtr->mLocalTrs);
    work->mGlobalScl.mul(work->mpEmtr->mGlobalScl, work->mpEmtr->mLocalScl);
    JPAGetDirMtx(work->mpEmtr->mLocalDir, work->mDirectionMtx);
    work->mPublicScale.set(work->mpEmtr->mGlobalScl);
    PSMTXMultVec(global_mtx, &work->mpEmtr->mLocalTrs, &work->mGlobalPos);
}

void JPAResource::calcWorkData_d(JPAEmitterWorkData* work) {
    Mtx mtx;
    JPAGetXYZRotateMtx(work->mpEmtr->mLocalRot.x * 0xB6, work->mpEmtr->mLocalRot.y * 0xB6, work->mpEmtr->mLocalRot.z * 0xB6, mtx);
    PSMTXConcat(work->mpEmtr->mGlobalRot, mtx, work->mGlobalRot);
    PSMTXMultVecSR(work->mGlobalRot, &work->mpEmtr->mLocalDir, &work->mGlobalEmtrDir);
}

#include "JSystem/JParticle/JPAFieldBlockInline.hpp"
#include "JSystem/JParticle/JPAListInline.hpp"

template JPANode< JPABaseParticle >* JPAList< JPABaseParticle >::erase(JPANode< JPABaseParticle >*);
template void JPAList< JPABaseParticle >::push_front(JPANode< JPABaseParticle >*);
