// Galaxy 3.0 multiplayer: structure-aware fuzz of every message type (ASan + UBSan + LeakSanitizer).
//
// Stage A (codecs): for every MpMsgType that has a payload reader in the tree, a generator writes a VALID payload field by field (with
//   the real writers where they exist), the mutator then changes FIELDS (edge values per width, NaN / Inf / huge floats, field swaps,
//   truncation at and inside field boundaries, extension, duplication, bit flips), and the real reader parses the result. Checks:
//   no crash; the unmutated payload is accepted; whatever a reader accepts satisfies the documented invariants and re-encodes to the
//   same bytes (canonical); fixed-size messages reject every truncation and extension; accepted poses go through MpPoseBuffer and
//   come out finite.
// Stage B (game messages through the core): three live consoles (host + 2 guests, loopback, lossless); every game message type is sent
//   by a random console with a mutated payload through the real send path (headers, origin sequences, relay), received through the real
//   receive path and polled; each delivered message must obey the core's own rules (type range, GameMaxLen, host-only types only
//   from the host, fromSlot valid, no duplicates) and is then run through the game-side reader of its type. Kicked guests are replaced.
// Stage C (core messages on the wire): a corpus of every core message type (HELLO .. SNAPSHOT_DONE, incl. HOST_CHANGE, RESUME, BYE,
//   REJECT) is recorded from real scenarios (join, snapshot, kick, migration, refusals); then word-level mutations (1/2/4-byte edge
//   values at random offsets, the header fields, truncations) are injected into the host and the guests, with reliable sequence
//   numbers stamped so they reach the handlers.
// Coverage guard: every type id the core accepts as a game message must be in this file's table (a new type on mp-int fails the test
//   until a generator and reader are added). Types whose reader lives in a game .cpp that cannot be built on the host (TOAST, STAMP,
//   BUMP, LOOK, EMOTE) are mirrored here from their source (named in the table).
//   usage: payload_fuzz [iterations per stage-A type=40000] [stage B iterations=300000] [stage C iterations=300000] [seed=1]
#include "core_test_util.h"
#include "Game/MP/MpBubbleLogic.hpp"
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpPartyLogic.hpp"
#include "Game/MP/MpPoseMath.hpp"
#include "Game/MP/MpSeriesLogic.hpp"
#include "Game/MP/MpStarBitsLogic.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/MP/MpWorldLogic.hpp"

#include <math.h>
#include <functional>
#include <map>

using namespace MpFlowLogic;

static uint32_t g_rng = 1;
static uint32_t R() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t RN(uint32_t n) { return n ? R() % n : 0; }

// ---- a builder that remembers the fields it wrote ----------------------------------------------------------------------------
struct Fld { uint16_t off; uint8_t size; bool isFloat; };
struct Bld {
    std::vector<uint8_t> d;
    std::vector<Fld> f;
    void put(uint64_t v, int size, bool fl = false) {
        f.push_back(Fld{(uint16_t)d.size(), (uint8_t)size, fl});
        for (int i = 0; i < size; i++) d.push_back((uint8_t)(v >> (8 * i)));
    }
    void u8(uint32_t v) { put(v & 0xFF, 1); }
    void u16(uint32_t v) { put(v & 0xFFFF, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void f32(float v) { uint32_t u; memcpy(&u, &v, 4); put(u, 4, true); }
    void raw(const uint8_t* p, size_t n) { f.push_back(Fld{(uint16_t)d.size(), (uint8_t)(n > 255 ? 255 : n), false}); d.insert(d.end(), p, p + n); }
};

static const uint32_t kEdge32[] = {0, 1, 2, 3, 4, 5, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
                                  0x54, 0x4000, 0x7FFF8000u, 0xFFFF0000u};
static uint32_t EdgeFor(int size) {
    uint32_t v = kEdge32[RN(sizeof kEdge32 / sizeof kEdge32[0])];
    if (R() % 3 == 0) v = R();
    if (R() % 4 == 0) v = (uint32_t)((int)(R() % 9) - 4);   // small, incl. negative
    return size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v;
}
static float EdgeFloat() {
    static const float e[] = {0.0f, -0.0f, 1.0f, -1.0f, 1.0e6f, -1.0e6f, 999999.9f, 1000000.1f, 1.0e30f, -1.0e30f, 1.0e-30f, 1.0e-45f, 3.4028235e38f,
                              -3.4028235e38f, INFINITY, -INFINITY, NAN, 0.5f, 65504.0f};
    uint32_t pick = RN(sizeof e / sizeof e[0] + 1);
    if (pick == sizeof e / sizeof e[0]) { uint32_t u = R(); float x; memcpy(&x, &u, 4); return x; }   // any bit pattern
    return e[pick];
}
static void SetField(std::vector<uint8_t>& d, const Fld& f) {
    if ((size_t)f.off + f.size > d.size()) return;
    if (f.isFloat && f.size == 4) {
        const float v = EdgeFloat();
        memcpy(&d[f.off], &v, 4);
        return;
    }
    const uint32_t v = EdgeFor(f.size);
    for (int i = 0; i < f.size && i < 4; i++) d[f.off + i] = (uint8_t)(v >> (8 * i));
}
// Structure-aware mutation of a payload given its field table.
static void MutateFields(std::vector<uint8_t>& d, const std::vector<Fld>& f, int rounds = 1) {
    for (int k = 0; k < rounds; k++) {
        switch (RN(10)) {
        case 0: case 1: case 2:   // one field to an edge value
            if (!f.empty()) SetField(d, f[RN((uint32_t)f.size())]);
            break;
        case 3:   // two fields
            if (!f.empty()) { SetField(d, f[RN((uint32_t)f.size())]); SetField(d, f[RN((uint32_t)f.size())]); }
            break;
        case 4:   // truncate at a field boundary
            if (!f.empty()) { const Fld& x = f[RN((uint32_t)f.size())]; if (x.off < d.size()) d.resize(x.off); }
            break;
        case 5:   // truncate inside a field / by one
            if (!d.empty()) d.resize(R() % 3 == 0 ? RN((uint32_t)d.size()) : d.size() - 1);
            break;
        case 6: {   // extend
            const uint32_t n = 1 + RN(R() % 4 == 0 ? 80 : 4);
            for (uint32_t i = 0; i < n; i++) d.push_back((uint8_t)R());
            break;
        }
        case 7:   // swap two fields' bytes
            if (f.size() >= 2) {
                const Fld& a = f[RN((uint32_t)f.size())];
                const Fld& b = f[RN((uint32_t)f.size())];
                const int n = a.size < b.size ? a.size : b.size;
                if ((size_t)a.off + n <= d.size() && (size_t)b.off + n <= d.size()) for (int i = 0; i < n; i++) std::swap(d[a.off + i], d[b.off + i]);
            }
            break;
        case 8:   // bit flips
            for (int i = 0, n = 1 + (int)RN(4); i < n && !d.empty(); i++) d[RN((uint32_t)d.size())] ^= (uint8_t)(1u << RN(8));
            break;
        default: {   // duplicate the whole payload (two messages glued)
            std::vector<uint8_t> c = d;
            d.insert(d.end(), c.begin(), c.end());
            break;
        }
        }
    }
}

// ---- generators (valid payloads) ---------------------------------------------------------------------------------------------
static int g_zoneCount = 12;   // the "receiving console's" zone count for the world readers
static float RF(float lim) { return ((float)(int32_t)R() / 2147483648.0f) * lim; }

static void GenPose(Bld& b, bool tail) {
    MpPose p;
    memset(&p, 0, sizeof p);
    b.u32(R()); b.u16(R()); b.u8(R()); b.u8(RN(MP_FORM_COUNT));
    for (int i = 0; i < 3; i++) b.f32(RF(90000.0f));
    float q[4], n2 = 0;
    for (int i = 0; i < 4; i++) { q[i] = RF(1.0f); n2 += q[i] * q[i]; }
    if (n2 < 0.01f) { q[3] = 1; n2 = 1; }
    const float inv = 1.0f / sqrtf(n2);
    for (int i = 0; i < 4; i++) b.u16((uint16_t)(int16_t)(q[i] * inv * 32767.0f));
    for (int i = 0; i < 3; i++) b.u8(R());
    b.u8(R()); b.u32(R()); b.u16(R()); b.u8(R());
    for (int i = 0; i < 4; i++) b.u8(R());
    b.u32(R()); b.u16(R()); b.u16(R() & 0x0FFF);
    if (tail) { b.u8(MP_POSE_TAIL_TAG); for (int i = 0; i < 4; i++) b.u32(R()); }
}

struct Spec {
    uint8_t type;
    const char* name;
    const char* reader;                                  // where the reader lives
    std::function<void(Bld&)> gen;
    std::function<bool(const uint8_t*, size_t, bool* accepted)> parse;   // returns false on an invariant violation
    int exactLen;                                        // fixed payload size (0 = variable): truncations / extensions must be rejected
};

#define VIOL(msg) do { fprintf(stderr, "  invariant violated: %s\n", msg); return false; } while (0)

// race (series): a series block, mostly well-formed (so the accepting path is reached), sometimes garbage. Returns the lines mask.
static unsigned GenSeriesBlock(Bld& b, unsigned forceIn) {
    const unsigned in = ((unsigned)R() % 16) | forceIn;
    const unsigned left = in & ((unsigned)R() % 16);
    const unsigned alive = in & ~left;
    for (int i = 0; i < 4; i++) b.u8((in >> i & 1) ? RN(100) : 0);
    b.u8(in | left << 4);
    unsigned len = RN(4), slot = 0;
    if (alive) { do { slot = RN(4); } while (!(alive >> slot & 1)); } else len = 0;
    if (len == 0) slot = 0;
    b.u8(slot | len << 2);
    return in;
}

static std::vector<Spec> BuildSpecs() {
    std::vector<Spec> v;
    static MpPoseBuffer sPbuf;
    MpPoseBuffer* pbuf = &sPbuf;
    pbuf->reset();
    static uint32_t poseNow = 0;
    v.push_back({MP_MSG_STATE, "STATE", "MpPoseMath::readState (+MpPoseBuffer)", [](Bld& b) { GenPose(b, R() % 2); },
        [pbuf](const uint8_t* d, size_t n, bool* acc) {
            MpPoseF pf;
            *acc = MpPoseMath::readState(d, n, &pf);
            if (!*acc) return true;
            if (n != 51 && n != 68) VIOL("STATE accepted with a length other than 51 / 68");
            for (int i = 0; i < 3; i++) if (!(pf.pos[i] == pf.pos[i]) || fabsf(pf.pos[i]) > 1.0e6f) VIOL("pose pos not finite / in range");
            float n2 = 0;
            for (int i = 0; i < 4; i++) { if (!(pf.quat[i] == pf.quat[i])) VIOL("pose quat NaN"); n2 += pf.quat[i] * pf.quat[i]; }
            if (fabsf(n2 - 1.0f) > 0.01f) VIOL("pose quat not unit");
            poseNow += 1 + R() % 4;
            pbuf->push(pf, poseNow);
            MpPoseF out;
            const MpSampleResult r = pbuf->sample(poseNow + R() % 3, &out);
            if (r != MP_SAMPLE_NONE) {
                for (int i = 0; i < 3; i++) if (!(out.pos[i] == out.pos[i]) || fabsf(out.pos[i]) > 1.0e7f) VIOL("sampled pos not finite");
                for (int i = 0; i < 4; i++) if (!(out.quat[i] == out.quat[i])) VIOL("sampled quat NaN");
            }
            return true;
        }, 0});
    v.push_back({MP_MSG_LOOK, "LOOK", "MpPuppets.cpp handle() (data[0] only; core rewrites it), core 1-byte request", [](Bld& b) { b.u8(RN(MP_LOOK_COUNT)); },
        [](const uint8_t*, size_t n, bool* acc) { *acc = n >= 1; return true; }, 0});
    v.push_back({MP_MSG_EMOTE, "EMOTE", "mp_read_emote (MpCoop)", [](Bld& b) { b.u8(RN(MP_EMOTE_COUNT)); b.u8(RN(2)); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpReader r(d, n); MpEmoteMsg e; *acc = mp_read_emote(r, &e); if (!*acc) return true;
            uint8_t o[8]; MpWriter w(o, sizeof o); mp_write_emote(w, e);
            if (n != 2 || w.n != 2 || memcmp(o, d, 2)) VIOL("EMOTE not canonical");
            return true;
        }, 2});
    v.push_back({MP_MSG_POINTER, "POINTER", "mp_read_pointer (MpCoop)", [](Bld& b) {
            b.u16(R()); b.u8(RN(2)); for (int i = 0; i < 3; i++) b.f32((float)((int)(R() % 200001) - 100000)); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpReader r(d, n); MpPointerMsg m; *acc = mp_read_pointer(r, &m); if (!*acc) return true;
            uint8_t o[32]; MpWriter w(o, sizeof o); mp_write_pointer(w, m);
            if (n != 15 || w.n != 15 || memcmp(o, d, 15)) VIOL("POINTER not canonical");
            return true;
        }, 15});
    // (bubble2: kind 4 = a bubble pop request, MpBubble::handlePopRequest through MpBubbleLogic::readPopRequest; 1..3 handleBump)
    v.push_back({MP_MSG_BUMP, "BUMP", "MpPuppets.cpp handleBump / MpBubble handlePopRequest (mirrored)", [](Bld& b) {
            const uint8_t kind = (uint8_t)(1 + RN(4));
            b.u8(RN(4)); b.u8(kind);
            if (kind == MpBubbleLogic::kBumpPop) { b.u8(1 + RN(2)); b.u8(0); b.u8(0); } else for (int i = 0; i < 3; i++) b.u8(R()); },
        [](const uint8_t* d, size_t n, bool* acc) {
            bool ok = false;
            uint8_t pt = 0, pc = 0;
            if (MpBubbleLogic::readPopRequest(d, n, &ok, &pt, &pc)) {
                *acc = ok;
                if (ok && (n != 5 || pt >= MP_MAX_PLAYERS || (pc != MpBubbleLogic::kPopSpin && pc != MpBubbleLogic::kPopShot))) VIOL("BUMP pop fields");
                if (ok) {
                    uint8_t o[8]; MpWriter w(o, sizeof o); MpBubbleLogic::writePopRequest(w, pt, pc);
                    if (w.n != 5 || memcmp(o, d, 5)) VIOL("BUMP pop not canonical");
                }
                return true;
            }
            MpReader r(d, n);
            const uint8_t target = r.u8(), kind = r.u8();
            r.s8(); r.s8(); r.s8();
            *acc = r.done() && kind >= 1 && kind <= 3;
            (void)target;
            return true;
        }, 5});
    auto where = [](bool isMove) {
        return [isMove](Bld& b) {
            b.u8(R()); b.u8(R()); b.u8(R()); b.u16((uint16_t)(R() % 5 == 0 ? 0xFFFF : R() % 2000)); b.u8(R() % 4 == 0 ? 0xFF : R() % 32); b.u16(R());
            if (isMove) { static const uint8_t t[] = {0, 2, 3, 4, 6}; b.u8(t[RN(5)]); }
        };
    };
    auto whereParse = [](bool isMove) {
        return [isMove](const uint8_t* d, size_t n, bool* acc) {
            Where w;
            *acc = readWhere(d, n, &w, isMove);
            if (!*acc) return true;
            if (n != (isMove ? 9u : 8u)) VIOL("WHERE/MOVE accepted with a wrong length");
            if (w.startId < -1) VIOL("startId < -1");
            if (isMove && !moveTypeValid(w.moveType)) VIOL("bad move type accepted");
            uint8_t out[16];
            MpWriter wr(out, sizeof out);
            writeWhere(wr, w, isMove);
            if (wr.n != n || memcmp(out, d, n)) VIOL("WHERE/MOVE does not re-encode to the same bytes");
            return true;
        };
    };
    v.push_back({MP_MSG_WHERE, "WHERE", "MpFlowLogic::readWhere", where(false), whereParse(false), 8});
    v.push_back({MP_MSG_MOVE, "MOVE", "MpFlowLogic::readWhere(isMove)", where(true), whereParse(true), 9});
    auto serialOnly = [](const char* nm, uint8_t t) {
        return Spec{t, nm, "MpFlowLogic::readSerialOnly", [](Bld& b) { b.u16(R()); },
                    [](const uint8_t* d, size_t n, bool* acc) { uint16_t s; *acc = readSerialOnly(d, n, &s); if (*acc && n != 2) VIOL("serial msg length"); return true; }, 2};
    };
    v.push_back(serialOnly("LOADED", MP_MSG_LOADED));
    v.push_back(serialOnly("DEAD", MP_MSG_DEAD));
    v.push_back(serialOnly("RACEREADY", MP_MSG_RACEREADY));   // racefix
    v.push_back({MP_MSG_GO, "GO", "MpFlowLogic::readGo (+ MpSeriesLogic::readBlock)", [](Bld& b) {
            b.u16(R()); b.u32(R());
            if (R() % 4) GenSeriesBlock(b, 0);   // (a GO with the series block, or the plain one)
        },
        [](const uint8_t* d, size_t n, bool* acc) {
            Go g; *acc = readGo(d, n, &g); if (!*acc) return true;
            uint8_t o[16]; MpWriter w(o, sizeof o); writeGo(w, g);
            if ((n != 6 && n != 12) || w.n != n || memcmp(o, d, n)) VIOL("GO not canonical");
            if (g.hasSeries != (n == 12)) VIOL("GO series flag");
            if (g.hasSeries) {
                if (!MpSeriesLogic::blockValid(g.series)) VIOL("GO accepted an invalid series block");
                for (int i = 0; i < 4; i++) if (MpSeriesLogic::headStartFrames(g.series, i) > 540) VIOL("head start beyond 9 s");
                // a head start only for a slot that has a line and is not greyed
                int heads = 0;
                for (int i = 0; i < 4; i++) heads += MpSeriesLogic::headStartFrames(g.series, i) != 0;
                if (heads > 1) VIOL("GO gives a head start to more than one player");
            }
            return true;
        }, 0});
    v.push_back({MP_MSG_STAR, "STAR", "MpFlowLogic::readStar", [](Bld& b) { b.u16(R()); b.u8(1 + RN(MpFlowLogic::kMaxStarId)); b.u8(RN(2)); b.u8(RN(4)); b.u32(R()); b.u16(R() & 0x7FFF); b.u8(1 + RN(4)); },
        [](const uint8_t* d, size_t n, bool* acc) {
            Star s; *acc = readStar(d, n, &s); if (!*acc) return true;
            if (s.starId < 1 || s.starId > 64 || s.grand > 1 || s.bySlot >= MP_MAX_PLAYERS) VIOL("star fields out of range");
            if (s.galaxyHash >= 0x8000 || s.scenario < 1) VIOL("star galaxy hash / scenario out of range");
            uint8_t o[16]; MpWriter w(o, sizeof o); writeStar(w, s);
            if (n != 12 || w.n != 12 || memcmp(o, d, 12)) VIOL("STAR not canonical");
            return true;
        }, 12});
    v.push_back({MP_MSG_STARV, "STARV", "MpFlowLogic::readStarv", [](Bld& b) { b.u16(R()); b.u16(R()); b.u8(1 + RN(MpFlowLogic::kMaxStarId)); b.u8(RN(4)); b.u8(RN(2)); b.u16(R() & 0x7FFF); b.u8(1 + RN(4)); b.u8(1 + RN(MpFlowLogic::SV_LAST)); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpFlowLogic::Starv sv; *acc = MpFlowLogic::readStarv(d, n, &sv); if (!*acc) return true;
            if (sv.starId < 1 || sv.starId > MpFlowLogic::kMaxStarId || sv.by >= MP_MAX_PLAYERS || sv.grand > 1) VIOL("starv fields out of range");
            if (sv.galaxyHash >= 0x8000 || sv.scenario < 1 || sv.verdict < 1 || sv.verdict > MpFlowLogic::SV_LAST) VIOL("starv galaxy hash / scenario / verdict out of range");
            uint8_t o[16]; MpWriter w(o, sizeof o); MpFlowLogic::writeStarv(w, sv);
            if (n != 11 || w.n != 11 || memcmp(o, d, 11)) VIOL("STARV not canonical");
            return true;
        }, 11});
    v.push_back({MP_MSG_FINISH, "FINISH", "MpFlowLogic::readFinish", [](Bld& b) { b.u16(R()); b.u32(1 + R() % 1000000); },
        [](const uint8_t* d, size_t n, bool* acc) {
            Finish f; *acc = readFinish(d, n, &f); if (!*acc) return true;
            if (f.frames == 0 || f.frames >= kMaxRaceFrames) VIOL("finish frames out of range");
            uint8_t o[8]; MpWriter w(o, sizeof o); writeFinish(w, f);
            if (n != 6 || memcmp(o, d, 6)) VIOL("FINISH not canonical");
            return true;
        }, 6});
    v.push_back({MP_MSG_RESULTS, "RESULTS", "MpFlowLogic::readResults + MpUi mp_read_results", [](Bld& b) {
            const int count = (int)RN(5);
            b.u16(R()); b.u8(count);
            unsigned used = 0;
            int place = 1;
            for (int i = 0; i < count; i++) {
                int s; do { s = (int)RN(4); } while (used & (1u << s));
                used |= 1u << s;
                const bool dnf = R() % 4 == 0;
                b.u8(s); b.u32(dnf ? kDnf : 1 + R() % 100000); b.u8(dnf ? 0 : place++);
            }
            if (R() % 3 && count > 0) {   // the series tail: the block (lines usually include the players), the choice, the next scenario
                GenSeriesBlock(b, used);
                const unsigned next = RN(10);
                unsigned choice = RN(4);
                if (choice == 2 && next == 0) choice = 1;   // (Next Star needs a next star)
                b.u8(choice); b.u8(next);
            }
        },
        [](const uint8_t* d, size_t n, bool* acc) {
            Results res; *acc = readResults(d, n, &res);
            MpRaceResults ui; MpReader rd(d, n);
            const bool uiOk = mp_read_results(rd, &ui);
            if (uiOk && !*acc) {
                // The UI reader is looser than the flow reader. Flow validates first and only forwards what it accepted
                // (MpRace.cpp applyResults), so this is harmless; still record it once.
                static bool once = false;
                if (!once) { once = true; fprintf(stderr, "  note: mp_read_results (ui) accepts a RESULTS that readResults (flow) rejects (flow gates it)\n"); }
            }
            if (!*acc) return true;
            if (res.count > MP_MAX_PLAYERS) VIOL("results count");
            if (n != 3u + 6u * res.count + (res.hasTail ? (unsigned)MpSeriesLogic::kTailBytes : 0u)) VIOL("RESULTS accepted with trailing / missing bytes");
            if (res.hasTail) {
                if (!MpSeriesLogic::tailValid(res.tail)) VIOL("RESULTS accepted an invalid tail");
                for (int i = 0; i < res.count; i++) if (!(res.tail.series.in >> res.e[i].slot & 1)) VIOL("RESULTS tail has no line for a player of the race");
                if (!uiOk || !ui.hasTail || !(ui.tail.series == res.tail.series) || ui.tail.choice != res.tail.choice || ui.tail.next != res.tail.next) VIOL("ui and flow readers disagree on the tail");
            }
            uint8_t o[64]; MpWriter w(o, sizeof o); writeResults(w, res);
            if (w.n != n || memcmp(o, d, n)) VIOL("RESULTS not canonical");
            if (res.count >= 1 && !uiOk) VIOL("flow accepts a RESULTS the ui reader rejects");
            return true;
        }, 0});
    v.push_back({MP_MSG_SKIPVOTE, "SKIPVOTE", "MpFlowLogic::readSkipVote", [](Bld& b) { b.u16(R()); b.u32(1 + R() % 0xFFFFFFF0u); },
        [](const uint8_t* d, size_t n, bool* acc) { SkipVoteMsg s; *acc = readSkipVote(d, n, &s); if (*acc && (n != 6 || s.hash == 0)) VIOL("skipvote"); return true; }, 6});
    v.push_back({MP_MSG_WARPREQ, "WARPREQ", "MpFlowLogic::readWarpReq", [](Bld& b) { b.u8(RN(4)); },
        [](const uint8_t* d, size_t n, bool* acc) { uint8_t t; *acc = readWarpReq(d, n, &t); if (*acc && (n != 1 || t >= MP_MAX_PLAYERS)) VIOL("warpreq"); return true; }, 1});
    v.push_back({MP_MSG_HOLD, "HOLD", "MpFlowLogic::readHold", [](Bld& b) { b.u8(RN(2)); b.u32(R() % 100000); },
        [](const uint8_t* d, size_t n, bool* acc) { Hold h; *acc = readHold(d, n, &h); if (*acc && (n != 5 || h.reason > HOLD_RACE_WAIT || h.turnsLeft >= kMaxRaceFrames)) VIOL("hold"); return true; }, 5});
    v.push_back({MP_MSG_SWITCH, "SWITCH", "MpWorldLogic::readSwitch", [](Bld& b) { b.u16(R()); b.u8(R() % 4 == 0 ? 0xFF : R() % g_zoneCount); b.u8(RN(128)); b.u8(RN(2)); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpWorldLogic::SwitchMsg m; *acc = MpWorldLogic::readSwitch(d, n, g_zoneCount, &m); if (!*acc) return true;
            if (n != MpWorldLogic::kSwitchLen || m.bit >= MpWorldLogic::kBits || m.on > 1) VIOL("switch fields");
            if (m.zone != MpWorldLogic::kGlobalWire && (m.zone >= g_zoneCount || m.zone >= MpWorldLogic::kGlobalSlot)) VIOL("switch zone");
            uint8_t o[16]; const size_t k = MpWorldLogic::writeSwitch(o, sizeof o, m);
            if (k != n || memcmp(o, d, n)) VIOL("SWITCH not canonical");
            return true;
        }, 5});
    v.push_back({MP_MSG_SWITCHMAP, "SWITCHMAP", "MpWorldLogic::readMap", [](Bld& b) {
            // a canonical full map: ascending distinct zones, non-empty bit sets, crc of the entries
            MpWorldLogic::MapMsg m; memset(&m, 0, sizeof m);
            int zones[MpWorldLogic::kMapMaxZones], nz = 0;
            for (int z = 0; z < g_zoneCount && z < MpWorldLogic::kGlobalSlot; z++) if (R() % 2) zones[nz++] = z;
            if (R() % 3 == 0) zones[nz++] = MpWorldLogic::kGlobalWire;
            m.serial = (uint16_t)R(); m.count = (uint8_t)nz;
            for (int i = 0; i < nz; i++) { m.zone[i] = (uint8_t)zones[i]; for (int k = 0; k < 4; k++) m.bits[i].w[k] = R(); if (!m.bits[i].any()) m.bits[i].w[0] = 1; }
            m.crc = nz ? MpWorldLogic::mapCrc(m) : 0;
            b.u16(m.serial); b.u32(m.crc); b.u8(m.count);
            for (int i = 0; i < nz; i++) { b.u8(m.zone[i]); for (int k = 0; k < 4; k++) b.u32(m.bits[i].w[k]); }
        },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpWorldLogic::MapMsg m; *acc = MpWorldLogic::readMap(d, n, g_zoneCount, &m); if (!*acc) return true;
            if (n != (size_t)MpWorldLogic::kMapHeader + (size_t)m.count * MpWorldLogic::kMapEntry) VIOL("SWITCHMAP length vs count");
            if (m.count > 0 && MpWorldLogic::mapCrc(m) != m.crc) VIOL("SWITCHMAP accepted with a wrong crc");
            (void)MpWorldLogic::mapIsFull(m);
            return true;
        }, 0});
    auto objGen = [](Bld& b) { b.u16(R()); b.u8(R() % g_zoneCount); b.u8(R()); b.u16(R()); b.u16(R() % 0x8000); };
    auto objParse = [](const uint8_t* d, size_t n, bool* acc) {
        MpWorldLogic::ObjMsg m; *acc = MpWorldLogic::readObj(d, n, g_zoneCount, &m); if (!*acc) return true;
        if (n != MpWorldLogic::kObjLen || m.key.nameHash >= 0x8000 || m.key.zone >= g_zoneCount) VIOL("obj fields");
        uint8_t o[16]; const size_t k = MpWorldLogic::writeObj(o, sizeof o, m);
        if (k != n || memcmp(o, d, n)) VIOL("OBJ not canonical");
        return true;
    };
    v.push_back({MP_MSG_KILL, "KILL", "MpWorldLogic::readObj", objGen, objParse, 8});
    v.push_back({MP_MSG_PURPLE, "PURPLE", "MpWorldLogic::readObj", objGen, objParse, 8});
    v.push_back({MP_MSG_CHIP, "CHIP", "MpWorldLogic::readObj", objGen, objParse, 8});
    v.push_back({MP_MSG_REGCRC, "REGCRC", "MpWorldLogic::readRegCrc", [](Bld& b) { b.u16(R()); b.u32(R()); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpWorldLogic::RegCrcMsg m; *acc = MpWorldLogic::readRegCrc(d, n, &m); if (!*acc) return true;
            uint8_t o[16]; const size_t k = MpWorldLogic::writeRegCrc(o, sizeof o, m);
            if ((n != 6 && n != 7) || k != n || memcmp(o, d, n)) VIOL("REGCRC not canonical");
            return true;
        }, 0});
    v.push_back({MP_MSG_OBJMAP, "OBJMAP", "MpWorldLogic::readObjMap", [](Bld& b) {
            b.u16(R()); b.u32(R());
            if (R() % 3 == 0) { b.u8(0); b.u16(R() % 2561); b.u16(R() % 2561); b.u16(R() % 2561); b.u32(R()); }
            else { const int n = 1 + (int)(R() % 128); b.u8(1 + R() % 3); b.u16(R() % (320 - n + 1)); b.u8((uint8_t)n); for (int i = 0; i < n; i++) b.u8(R()); }
        },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpWorldLogic::ObjMapMsg m; *acc = MpWorldLogic::readObjMap(d, n, &m); if (!*acc) return true;
            if (m.kind == 0 ? n != (size_t)MpWorldLogic::kObjSummaryLen : n != (size_t)MpWorldLogic::kObjMapHeader + m.n) VIOL("OBJMAP length vs kind");
            if (m.kind != 0 && (int)m.start + m.n > MpWorldLogic::kObjMapMaxBytes) VIOL("OBJMAP chunk out of range");
            return true;
        }, 0});
    v.push_back({MP_MSG_TOAST, "TOAST", "MpUiSystem.cpp handleMessage (mirrored)", [](Bld& b) { b.u16(1 + RN(MP_T_TOAST_LAST)); b.u8(R() % 4 == 0 ? 0xFF : RN(4)); b.u32(R()); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpReader r(d, n <= MP_MAX_PAYLOAD ? n : 0);
            const uint16_t id = r.u16(); const uint8_t slot = r.u8(); r.u32();
            *acc = r.done() && !(id == MP_T_NONE || id > MP_T_TOAST_LAST || (slot != 0xFF && slot >= MP_MAX_PLAYERS));
            return true;
        }, 7});
    // TALLY carries three forms by length: the team star's counters (8 bytes, MpPartyLogic) and, since costar, the team Star Bit
    // DELTA (7) and TOTAL (16) (MpStarBitsLogic). A payload is exactly one of them or none; none of the readers accepts another's length.
    v.push_back({MP_MSG_TALLY, "TALLY", "MpPartyLogic::readTally / MpStarBitsLogic::readDelta, readTotal", [](Bld& b) {
            const uint32_t form = RN(3);
            if (form == 0) {
                b.u16(R()); b.u16(RN(MpPartyLogic::kMaxCount + 1)); b.u16(RN(MpPartyLogic::kMaxCount + 1)); b.u8(RN(MpPartyLogic::kMaxHelped + 1)); b.u8(RN(2));
            } else if (form == 1) {
                b.u16(R()); b.u8(MpStarBitsLogic::kKindDelta); b.u16(1 + RN(65535)); int d = (int)RN(201) - 100; if (d == 0) d = 1; b.u16((uint16_t)(int16_t)d);
            } else {
                b.u16(R()); b.u8(MpStarBitsLogic::kKindTotal); b.u8(R()); b.u16(R()); b.u16(RN(MpStarBitsLogic::kCapStock + 1));
                for (int i = 0; i < MP_MAX_PLAYERS; i++) b.u16(R());
            } },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpPartyLogic::Tally t; MpStarBitsLogic::Delta dl; MpStarBitsLogic::Total tt;
            const bool a0 = MpPartyLogic::readTally(d, n, &t), a1 = MpStarBitsLogic::readDelta(d, n, &dl), a2 = MpStarBitsLogic::readTotal(d, n, &tt);
            *acc = a0 || a1 || a2;
            if ((int)a0 + (int)a1 + (int)a2 > 1) VIOL("TALLY accepted as two forms");
            const uint8_t cls = MpStarBitsLogic::classify(d, n);
            if (a0 && cls != 0) VIOL("TALLY classify (old tally)");
            if (a1 && cls != MpStarBitsLogic::kKindDelta) VIOL("TALLY classify (delta)");
            if (a2 && cls != MpStarBitsLogic::kKindTotal) VIOL("TALLY classify (total)");
            if (!*acc) return true;
            uint8_t o[24]; MpWriter w(o, sizeof o);
            if (a0) {
                MpPartyLogic::writeTally(w, t);
                if (n != MpPartyLogic::kTallyBytes) VIOL("TALLY length");
            } else if (a1) {
                MpStarBitsLogic::writeDelta(w, dl);
                if (n != MpStarBitsLogic::kDeltaBytes || dl.seq == 0 || dl.delta == 0 || dl.delta < -MpStarBitsLogic::kMaxMsgDelta || dl.delta > MpStarBitsLogic::kMaxMsgDelta) VIOL("DELTA fields");
            } else {
                MpStarBitsLogic::writeTotal(w, tt);
                if (n != MpStarBitsLogic::kTotalBytes || tt.total > MpStarBitsLogic::kCapStock) VIOL("TOTAL fields");
            }
            if (!w.ok() || w.n != n || memcmp(o, d, n)) VIOL("TALLY not canonical");
            return true;
        }, 0});
    v.push_back({MP_MSG_BUBBLE, "BUBBLE", "mp_read_bubble (mp_proto.h)", [](Bld& b) {
            const uint8_t on = (uint8_t)RN(2);
            b.u16(R()); b.u8(on); b.u8(RN(100)); b.u8(R() % 3 == 0 ? MP_BUBBLE_BY_NONE : RN(MP_MAX_PLAYERS)); b.u8(on ? RN(3) : 0); },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpBubbleMsg m; MpReader r(d, n); *acc = mp_read_bubble(r, &m); if (!*acc) return true;
            if (m.on > 1 || m.lives > 99 || (m.by >= MP_MAX_PLAYERS && m.by != MP_BUBBLE_BY_NONE) || (!m.on && m.flags) ||
                m.flags > MP_BUBBLE_ARRIVAL) VIOL("BUBBLE fields");
            uint8_t o[8]; MpWriter w(o, sizeof o); mp_write_bubble(w, m);
            if (n != 6 || w.n != 6 || memcmp(o, d, 6)) VIOL("BUBBLE not canonical");
            return true;
        }, 6});
    v.push_back({MP_MSG_RESTART, "RESTART", "MpBubble.cpp handle (mirrored)", [](Bld& b) { b.u16(R()); },
        [](const uint8_t* d, size_t n, bool* acc) { MpReader r(d, n); r.u16(); *acc = r.done(); return true; }, 2});
    v.push_back({MP_MSG_STAMP, "STAMP", "MpUiSystem.cpp handleMessage (mirrored)", [](Bld& b) { b.u8(RN(4)); },
        [](const uint8_t* d, size_t n, bool* acc) { MpReader r(d, n); const uint8_t s = r.u8(); *acc = r.done() && s <= 3; return true; }, 1});
    // chat: a phrase id, or a typed text (length + UTF-8, all kinds of characters before cleaning: the generator cleans, as a sender
    // does); the reader takes only clean text, so whatever it accepts re-encodes to the same bytes
    v.push_back({MP_MSG_CHAT, "CHAT", "port/mp_chat.h mp_read_chat (MpChat.cpp, mp_core.cpp's host check)", [](Bld& b) {
            if (R() % 2) { b.u8(MP_CHAT_PHRASE); b.u8(RN(MP_CHAT_PHRASES)); return; }
            static const char* const kBits[] = {"a", "Z", " ", "!", "\xc3\xa9", "\xc2\xbf", "\xe2\x82\xac", "\xe3\x81\x82", "\x01", "\x7f", "\xc2\x85",
                                                "\xe2\x80\xae", "\xef\xbb\xbf", "\xee\x80\x81", "\xf0\x9f\x98\x80", "\xff", "\xc0\xaf", "\n"};
            std::string raw;
            for (int i = 0, n = 1 + (int)RN(60); i < n; i++) raw += kBits[RN(sizeof kBits / sizeof kBits[0])];
            MpChatMsg m;
            if (!mp_chat_make_text(&m, raw.data(), raw.size())) { b.u8(MP_CHAT_PHRASE); b.u8(0); return; }
            b.u8(MP_CHAT_TEXT); b.u8(m.len);
            for (int i = 0; i < m.len; i++) b.u8((uint8_t)m.text[i]);   // (field by field: the mutator works per byte)
        },
        [](const uint8_t* d, size_t n, bool* acc) {
            MpReader r(d, n);
            MpChatMsg m;
            *acc = mp_read_chat(r, &m);
            if (!*acc) return true;
            if (m.kind == MP_CHAT_PHRASE && (m.phrase >= MP_CHAT_PHRASES || n != 2)) VIOL("CHAT phrase out of range");
            if (m.kind == MP_CHAT_TEXT && (m.len == 0 || m.len > MP_CHAT_TEXT_BYTES || n != (size_t)m.len + 2 || !mp_chat_text_ok(m.text, m.len))) VIOL("CHAT text not clean");
            uint8_t o[MP_CHAT_MAX_PAYLOAD + 4];
            MpWriter w(o, sizeof o);
            mp_write_chat(w, m);
            if (!w.ok() || w.n != n || memcmp(o, d, n)) VIOL("CHAT not canonical");
            return true;
        }, 0});
    return v;
}

static const Spec* FindSpec(const std::vector<Spec>& specs, uint8_t t) {
    for (auto& s : specs) if (s.type == t) return &s;
    return nullptr;
}

// ---- stage A ----------------------------------------------------------------------------------------------------------------
static void StageA(const std::vector<Spec>& specs, long iters, std::map<std::string, std::pair<long, long>>& stats) {
    for (const Spec& sp : specs) {
        long acc = 0, rej = 0;
        // the valid payload is accepted
        for (int i = 0; i < 2000; i++) {
            Bld b; sp.gen(b);
            bool a = false;
            const bool ok = sp.parse(b.d.data(), b.d.size(), &a);
            if (!ok) { fprintf(stderr, "  (type %s, unmutated)\n", sp.name); CHECK(ok); }
            if (!a) { if (g_fail < 20) fprintf(stderr, "  %s: a valid generated payload was rejected (len %zu)\n", sp.name, b.d.size()); CHECK(a); break; }
            if (sp.exactLen && b.d.size() != (size_t)sp.exactLen) { fprintf(stderr, "  %s: generator length %zu != %d\n", sp.name, b.d.size(), sp.exactLen); CHECK(!"generator length"); break; }
        }
        for (long it = 0; it < iters; it++) {
            Bld b; sp.gen(b);
            const std::vector<uint8_t> orig = b.d;
            std::vector<uint8_t> d = b.d;
            MutateFields(d, b.f, 1 + (R() % 4 == 0));
            bool a = false;
            // (a null buffer for an empty message is a legal call: the reader must not touch it)
            const bool ok = sp.parse(d.empty() ? (const uint8_t*)"" : d.data(), d.size(), &a);
            if (!ok) {
                fprintf(stderr, "  (type %s, %zu bytes:", sp.name, d.size());
                for (size_t i = 0; i < d.size() && i < 80; i++) fprintf(stderr, " %02x", d[i]);
                fprintf(stderr, ")\n");
                CHECK(ok);
                if (g_fail > 30) return;
            }
            a ? acc++ : rej++;
            if (sp.exactLen && d.size() != (size_t)sp.exactLen && a) {
                // (STATE aside) a fixed-size message accepted at another length
                fprintf(stderr, "  %s accepted at length %zu (fixed %d)\n", sp.name, d.size(), sp.exactLen);
                CHECK(!"fixed-size message accepted at another length");
            }
        }
        // every truncation / extension of a valid fixed-size payload is rejected
        if (sp.exactLen) {
            for (int i = 0; i < 500; i++) {
                Bld b; sp.gen(b);
                for (size_t cut = 0; cut < b.d.size(); cut++) { bool a = true; sp.parse(b.d.data(), cut, &a); if (a) { CHECK(!"truncated message accepted"); fprintf(stderr, "  %s accepted truncated to %zu\n", sp.name, cut); } }
                for (int ext = 1; ext <= 3; ext++) { std::vector<uint8_t> d = b.d; for (int k = 0; k < ext; k++) d.push_back((uint8_t)R()); bool a = true; sp.parse(d.data(), d.size(), &a); if (a) { CHECK(!"extended message accepted"); fprintf(stderr, "  %s accepted extended to %zu\n", sp.name, d.size()); } }
            }
        }
        stats[sp.name] = {acc, rej};
    }
    // STATE: exactly the two lengths
    {
        const Spec* st = FindSpec(specs, MP_MSG_STATE);
        for (int tail = 0; tail < 2; tail++)
            for (int i = 0; i < 300; i++) {
                Bld b; GenPose(b, tail);
                for (size_t cut = 0; cut < b.d.size(); cut++) { bool a = true; st->parse(b.d.data(), cut, &a); if (a && cut != 51) { CHECK(!"STATE accepted at a bad length"); } }
                std::vector<uint8_t> d = b.d; d.push_back(0); bool a = true; st->parse(d.data(), d.size(), &a); if (a) CHECK(!"STATE accepted with one extra byte");
                if (tail) { d = b.d; d[51] = (uint8_t)(MP_POSE_TAIL_TAG ^ 1); a = true; st->parse(d.data(), d.size(), &a); if (a) CHECK(!"STATE accepted a wrong tail tag"); }
            }
    }
}

// race: everything the series adds fits the core's per-type maxima (GO 16, RESULTS 64) and the event history entry (124 bytes)
static_assert(MpFlowLogic::kGoBytes <= 16 && 3 + 6 * MP_MAX_PLAYERS + MpSeriesLogic::kTailBytes <= 64, "series wire size");
static_assert(3 + 6 * MP_MAX_PLAYERS + MpSeriesLogic::kTailBytes <= 124, "a RESULTS fits a history entry");

// ---- stage B: game messages through two live cores ----------------------------------------------------------------------------
static uint16_t GameMax(uint8_t t) {   // the core's documented maxima (mp_core.cpp GameMaxLen) as an independent table
    switch (t) {
    case MP_MSG_STATE: return 68; case MP_MSG_LOOK: case MP_MSG_EMOTE: case MP_MSG_LOADED: case MP_MSG_DEAD: case MP_MSG_RACEREADY: case MP_MSG_WARPREQ: case MP_MSG_STAMP: return 8;
    case MP_MSG_BUMP: case MP_MSG_WHERE: case MP_MSG_MOVE: case MP_MSG_GO: case MP_MSG_FINISH: case MP_MSG_SKIPVOTE: case MP_MSG_HOLD: case MP_MSG_SWITCH:
    case MP_MSG_KILL: case MP_MSG_PURPLE: case MP_MSG_CHIP: case MP_MSG_REGCRC: case MP_MSG_TOAST: case MP_MSG_TALLY: case MP_MSG_POINTER: return 16;
    case MP_MSG_BUBBLE: case MP_MSG_RESTART: return 8;   // (bubble)
    case MP_MSG_STAR: return 24; case MP_MSG_STARV: return 16; case MP_MSG_RESULTS: return 64;
    case MP_MSG_OBJMAP: return 138;   // worldsync: header 10 + 128-byte chunk
    case MP_MSG_SWITCHMAP: return 7 + 17 * 33;   // header + MpWorldLogic::kMapMaxZones entries (the largest legal full map)
    case MP_MSG_CHAT: return 2 + MP_CHAT_TEXT_BYTES;   // (chat: kind, length, 120 bytes of UTF-8)
    default: return 0;
    }
}

static void StageB(const std::vector<Spec>& specs, long iters) {
    // coverage guard: every game type the core takes must have a spec
    MpLoopHub* hub = new MpLoopHub();
    hub->setClock(VNow, 0);
    std::vector<MpCore*> all;
    MpCore* host = MakeCore(*hub, 1, "Host", 7);
    all.push_back(host);
    MpHostParams hp = HostParams();
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    uint32_t nextEp = 2;
    auto addGuest = [&]() {
        char name[17]; snprintf(name, sizeof name, "Pf%u", nextEp);
        MpCore* g = MakeCore(*hub, nextEp, name, 500 + nextEp); nextEp++;
        all.push_back(g);
        for (int attempt = 0; attempt < 3; attempt++) {
            if (ScanJoin(all, g) && (WaitPhase(all, g, MP_UI_PLAYING, 3000) || g->active())) return;
            g->uiLeave(); Step(all, MP_PEER_LOST_MS + 1000);
        }
        CHECK(!"guest could not join");
    };
    addGuest(); addGuest();
    host->uiStart();
    Step(all, 200);
    // guard: probe every type id with a trivially valid payload; the core takes only types in the table
    for (int t = MP_MSG_GAME_FIRST; t <= MP_MSG_GAME_LAST; t++) {
        uint8_t p[8] = {0};
        const bool taken = host->send((uint8_t)t, p, 1, true);
        const bool known = FindSpec(specs, (uint8_t)t) != nullptr;
        if (taken && !known) { fprintf(stderr, "  message type 0x%02X is accepted by the core but has no entry in payload_fuzz\n", t); CHECK(!"uncovered message type"); }
        if (known && GameMax((uint8_t)t) == 0) CHECK(!"table without a max length");
    }
    Step(all, 200);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    // the largest legal full SWITCHMAP (every zone slot with bits: 7 + 33 x 17 = 568 bytes) must reach the guests whole
    {
        MpWorldLogic::MapMsg mm;
        memset(&mm, 0, sizeof mm);
        mm.serial = 9; mm.count = MpWorldLogic::kMapMaxZones;
        for (int i = 0; i < mm.count; i++) { mm.zone[i] = (uint8_t)(i < MpWorldLogic::kGlobalSlot ? i : MpWorldLogic::kGlobalWire); mm.bits[i].w[0] = 1u << (i % 32); }
        mm.crc = MpWorldLogic::mapCrc(mm);
        Bld b;
        b.u16(mm.serial); b.u32(mm.crc); b.u8(mm.count);
        for (int i = 0; i < mm.count; i++) { b.u8(mm.zone[i]); for (int k = 0; k < 4; k++) b.u32(mm.bits[i].w[k]); }
        CHECK(b.d.size() == 568);
        CHECK(host->send(MP_MSG_SWITCHMAP, b.d.data(), (uint16_t)b.d.size(), true));
        Step(all, 400);
        int got = 0;
        for (size_t i = 1; i < all.size(); i++)
            while (all[i]->poll(&m))
                if (m.type == MP_MSG_SWITCHMAP && m.len == 568) got++;
        if (got != 2) fprintf(stderr, "  the largest full SWITCHMAP reached %d of 2 guests\n", got);
        CHECK(got == 2);
        while (host->poll(&m)) {}
    }
    std::map<uint8_t, long> delivered;
    long sent = 0, refused = 0, parserAcc = 0, parserRej = 0;
    int replaced = 0;
    long dupGuard = 0;
    for (long it = 0; it < iters; it++) {
        const Spec& sp = specs[R() % specs.size()];
        Bld b; sp.gen(b);
        std::vector<uint8_t> d = b.d;
        if (R() % 5) MutateFields(d, b.f, 1 + (R() % 4 == 0));
        // reliable or not (a hostile peer picks either); STATE is only meaningful unreliable but may be sent either way
        const bool reliable = R() % 3 != 0;
        std::vector<MpCore*> live;
        for (MpCore* c : all) if (c->active()) live.push_back(c);
        if (live.empty()) break;
        MpCore* from = live[R() % live.size()];
        if (d.size() > 1000) d.resize(1000);
        if (from->send(sp.type, d.empty() ? (const uint8_t*)"" : d.data(), (uint16_t)d.size(), reliable)) sent++; else refused++;
        if (it % 8 == 0) Step(all, 5);
        if (it % 8 == 0) {
            for (MpCore* c : all) {
                while (c->poll(&m)) {
                    const Spec* ms = FindSpec(specs, m.type);
                    if (!ms) { CHECK(!"delivered a type without a spec"); continue; }
                    delivered[m.type]++;
                    if (m.type < MP_MSG_GAME_FIRST || m.type > MP_MSG_GAME_LAST) CHECK(!"delivered a non-game type");
                    if (m.len > GameMax(m.type)) { fprintf(stderr, "  delivered %s with %u bytes (max %u)\n", ms->name, m.len, GameMax(m.type)); CHECK(!"delivered over the maximum"); }
                    if (m.fromSlot >= MP_MAX_PLAYERS) CHECK(!"fromSlot out of range");
                    if (mp_msg_host_only(m.type) && m.fromSlot != (uint8_t)c->hostSlot() && c->active()) { fprintf(stderr, "  host-only %s delivered from slot %u (host %d)\n", ms->name, m.fromSlot, c->hostSlot()); CHECK(!"host-only message from a guest delivered"); }
                    if (m.fromSlot == (uint8_t)c->localSlot()) CHECK(!"own message delivered back");
                    bool a = false;
                    const bool ok = ms->parse(m.data, m.len, &a);
                    if (!ok) { fprintf(stderr, "  (delivered %s len %u)\n", ms->name, m.len); CHECK(ok); }
                    a ? parserAcc++ : parserRej++;
                }
                MpCoreEvent ev;
                while (c->pollEvent(&ev)) {}
            }
        }
        if (it % 5000 == 0) {
            for (size_t i = 1; i < all.size();) {
                if (!all[i]->active()) { delete all[i]; all.erase(all.begin() + (long)i); replaced++; } else i++;
            }
            if (!host->active()) { fprintf(stderr, "  host ended (iteration %ld)\n", it); CHECK(!"host ended during stage B"); break; }
            Step(all, 50);
            while (all.size() < 3) addGuest();
        }
        (void)dupGuard;
    }
    Step(all, 3000);
    for (MpCore* c : all) while (c->poll(&m)) {}
    printf("stage B: %ld sent, %ld refused by send(), delivered %zu types, parsers accepted %ld rejected %ld, %d guests replaced\n", sent, refused, delivered.size(), parserAcc, parserRej, replaced);
    for (auto& s : specs) if (!delivered.count(s.type)) { fprintf(stderr, "  type %s was never delivered through the core\n", s.name); CHECK(!"a type never made it through the core"); }
    for (MpCore* c : all) delete c;
    delete hub;
}

// ---- stage C: core messages on the wire ---------------------------------------------------------------------------------------
struct Pkt { MpAddr from, to; std::vector<uint8_t> d; };
static std::vector<Pkt> g_corpus;
static void Tap(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    static uint8_t s_open[2048];   // core2: sealed datagrams go into the corpus opened (the fuzzer delivers them as the member's)
    if (len >= 2 && ((const uint8_t*)data)[0] == 'G' && ((const uint8_t*)data)[1] == 'S') {
        size_t pl = 0;
        if (!MpCore::debugOpenAny(data, len, s_open, sizeof s_open, &pl)) return;
        data = s_open;
        len = pl;
    }
    if (g_corpus.size() >= 30000 || len < 12) return;
    const uint8_t* d = (const uint8_t*)data;
    if (d[0] == 'G' && d[1] == 'B') {   // netpack: a bundle goes into the corpus as its packets (stage C picks them by type)
        size_t off = 4;
        for (unsigned i = 0; i < d[3] && off + 2 <= len; i++) {
            const size_t n = d[off] | (d[off + 1] << 8);
            off += 2;
            if (n > len - off) break;
            g_corpus.push_back(Pkt{from, to, std::vector<uint8_t>(d + off, d + off + n)});
            off += n;
        }
        return;
    }
    g_corpus.push_back(Pkt{from, to, std::vector<uint8_t>(d, d + len)});
}
static const uint8_t kCoreTypes[] = {MP_MSG_HELLO, MP_MSG_WELCOME, MP_MSG_REJECT, MP_MSG_ROSTER, MP_MSG_BYE, MP_MSG_HEARTBEAT, MP_MSG_SETTINGS,
                                     MP_MSG_HOST_CHANGE, MP_MSG_RESUME, MP_MSG_SNAPSHOT_BEGIN, MP_MSG_SNAPSHOT_CHUNK, MP_MSG_SNAPSHOT_DONE};

static void RecordCorpus() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setTap(Tap, 0);
    MpLoopImpairment imp = {0.03f, 0.01f, 5, 20, 0};
    hub.setImpairment(imp, 99);
    std::vector<MpCore*> all;
    for (uint32_t i = 1; i <= 4; i++) { char nm[8]; snprintf(nm, sizeof nm, "C%u", i); all.push_back(MakeCore(hub, i, nm, 40 + i)); }
    MpHostParams hp = HostParams();
    all[0]->uiHost(&hp);
    WaitPhase(all, all[0], MP_UI_LOBBY, 2000);
    static uint8_t img[4000];
    for (int i = 0; i < 4000; i++) img[i] = (uint8_t)(i * 7);
    all[0]->snapshotOffer(img, 4000);
    for (int i = 1; i <= 2; i++) { ScanJoin(all, all[i]); WaitPhase(all, all[i], MP_UI_PLAYING, 3000); }
    all[0]->uiStart();
    Step(all, 500);
    all[0]->snapshotOffer(img, 3000);
    MpSettings s = hp.settings; s.interaction = 2; all[0]->updateSettings(&s);
    uint8_t pose[68] = {0};
    for (int t = 0; t < 40; t++) { for (MpCore* c : all) c->send(MP_MSG_STATE, pose, 51, false); Step(all, 20); }
    // a refusal (REJECT) and a kick
    MpCore* g4 = all[3];
    ScanJoin(all, g4);
    Step(all, 1500);   // full room? (3 of 4 present: this one joins)
    all[0]->uiKick(all[1]->localSlot() >= 0 ? all[1]->localSlot() : 1);
    Step(all, 2000);
    // host migration: HOST_CHANGE, RESUME, BYE(host-leaving)
    all[0]->uiLeave();
    Step(all, 12000);
    // a stranger HELLO with a bad fingerprint (REJECT version)
    for (MpCore* c : all) c->uiLeave();
    Step(all, 1500);
    for (MpCore* c : all) delete c;
    hub.setTap(0, 0);
}

static void MutateWire(std::vector<uint8_t>& d) {
    switch (RN(9)) {
    case 0: case 1: case 2: {   // a 1 / 2 / 4-byte word at a random offset (aligned to the payload start half the time) to an edge value
        if (d.size() > 12) {
            const int sz = R() % 3 == 0 ? 4 : R() % 2 ? 2 : 1;
            size_t off = 12 + RN((uint32_t)(d.size() - 12));
            if (R() % 2) off = 12 + (off - 12) / (size_t)sz * (size_t)sz;
            const uint32_t v = EdgeFor(sz);
            for (int i = 0; i < sz && off + (size_t)i < d.size(); i++) d[off + (size_t)i] = (uint8_t)(v >> (8 * i));
        }
        break;
    }
    case 3: {   // header fields
        if (d.size() >= 12) { const int k = 2 + (int)RN(10); d[(size_t)k] = (uint8_t)EdgeFor(1); }
        break;
    }
    case 4: if (d.size() > 12) d.resize(12 + RN((uint32_t)(d.size() - 12))); break;      // truncate payload
    case 5: if (!d.empty()) d.resize(d.size() - 1); break;
    case 6: { const uint32_t n = 1 + RN(40); for (uint32_t i = 0; i < n; i++) d.push_back((uint8_t)R()); break; }
    case 7: for (int i = 0, n = 1 + (int)RN(3); i < n && d.size() > 12; i++) d[12 + RN((uint32_t)(d.size() - 12))] ^= (uint8_t)(1u << RN(8)); break;
    default: if (d.size() > 12) { const size_t a = 12 + RN((uint32_t)(d.size() - 12)), b = 12 + RN((uint32_t)(d.size() - 12)); std::swap(d[a], d[b]); } break;
    }
}

static void StageC(long iters) {
    RecordCorpus();
    std::map<uint8_t, std::vector<size_t>> byType;
    for (size_t i = 0; i < g_corpus.size(); i++) if (g_corpus[i].d.size() >= 12 && g_corpus[i].d[0] == 'G') byType[g_corpus[i].d[3]].push_back(i);
    bool complete = true;
    for (uint8_t t : kCoreTypes) {
        if (!byType.count(t)) { fprintf(stderr, "  core type 0x%02X was not recorded in the corpus\n", t); complete = false; }
    }
    CHECK(complete);
    printf("stage C: corpus %zu datagrams; types:", g_corpus.size());
    for (uint8_t t : kCoreTypes) printf(" %02X=%zu", t, byType.count(t) ? byType[t].size() : 0u);
    printf("\n");

    MpLoopHub* hub = new MpLoopHub();
    hub->setClock(VNow, 0);
    std::vector<MpCore*> all;
    MpCore* host = MakeCore(*hub, 1, "C1", 40 + 1);
    all.push_back(host);
    MpHostParams hp = HostParams();
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    static uint8_t img[3000];
    for (int i = 0; i < 3000; i++) img[i] = (uint8_t)i;
    host->snapshotOffer(img, sizeof img);
    uint32_t nextEp = 2;
    auto addGuest = [&]() {
        char name[17]; snprintf(name, sizeof name, "Wf%u", nextEp);
        MpCore* g = MakeCore(*hub, nextEp, name, 900 + nextEp); nextEp++;
        all.push_back(g);
        // payfix: guests the fuzz killed without a goodbye are removed by the host for silence ("timeout"), and the host holds each of
        // their slots for MP_LINKLOST_HOLD_MS for its owner only (corefix D1/2): a newcomer is told "full" while every free place is
        // held. That is the product's rule, and these replacement guests are other consoles (own identity and address), so they may
        // have to wait the hold out. What stays proven: once the holds are over, an honest newcomer ALWAYS gets a free place (a ban or
        // a leaked reservation would fail the last attempts). The first 3 attempts are the old ones; then one wait for the holds.
        for (int attempt = 0; attempt < 6; attempt++) {
            if (ScanJoin(all, g) && (WaitPhase(all, g, MP_UI_PLAYING, 3000) || g->active())) return;
            g->uiLeave();
            Step(all, attempt == 2 ? MP_LINKLOST_HOLD_MS + 2000 : MP_PEER_LOST_MS + 1000);
        }
        CHECK(!"guest could not join");
    };
    addGuest(); addGuest();
    host->uiStart();
    long perType[256] = {0};
    int replaced = 0;
    for (long it = 0; it < iters; it++) {
        const uint8_t t = kCoreTypes[it % sizeof kCoreTypes];
        const std::vector<size_t>& pool = byType[t];
        if (pool.empty()) continue;
        std::vector<uint8_t> d = g_corpus[pool[R() % pool.size()]].d;
        perType[t]++;
        if (R() % 6) MutateWire(d);
        if (R() % 4 == 0) MutateWire(d);
        const uint32_t pick = R() % 100;
        MpCore* target = host;
        MpAddr from = {1000 + RN(20), (uint16_t)MP_SESSION_PORT};
        if (pick < 45) {
            std::vector<MpCore*> gs;
            for (size_t i = 1; i < all.size(); i++) if (all[i]->active()) gs.push_back(all[i]);
            if (!gs.empty()) {
                target = gs[R() % gs.size()];
                from = host->debugLocalAddr();
                if (d.size() >= 12 && (d[5] & MP_HDR_RELIABLE) && R() % 3) {
                    const uint16_t s = target->debugExpectedSeq(target->hostSlot());
                    d[6] = (uint8_t)s; d[7] = (uint8_t)(s >> 8);
                    d[4] = (uint8_t)target->hostSlot();
                }
            }
        } else if (pick < 80) {
            std::vector<MpCore*> gs;
            for (size_t i = 1; i < all.size(); i++) if (all[i]->active()) gs.push_back(all[i]);
            if (!gs.empty()) {
                MpCore* g = gs[R() % gs.size()];
                from = g->debugLocalAddr();
                if (d.size() >= 12 && (d[5] & MP_HDR_RELIABLE) && R() % 3) {
                    const int slot = g->localSlot();
                    const uint16_t s = host->debugExpectedSeq(slot);
                    d[6] = (uint8_t)s; d[7] = (uint8_t)(s >> 8);
                    d[4] = (uint8_t)slot;
                }
            }
        }   // else: a stranger to the host
        if (R() % 5 == 0 && d.size() >= 12) {   // netpack: the same message inside a bundle (with other recorded packets; sometimes broken)
            std::vector<std::vector<uint8_t>> pk;
            pk.push_back(d);
            const int n = 1 + (int)(R() % 3);
            for (int i = 0; i < n; i++) pk.push_back(g_corpus[R() % g_corpus.size()].d);
            d = BundleOf(pk);
            if (R() % 6 == 0) ForgeBundle(d, R);
        }
        {   // core2: from a member (an address the target has a peer at): as sealed by it; else plain (a stranger: only a join may pass)
            const int as = target->debugSlotOf(from);
            if (as >= 0) target->debugReceiveAuthed(as, d.data(), d.size());
            else target->debugReceive(from, d.data(), d.size());
        }
        if (it % 400 == 0) {
            Step(all, 5);
            MpMsg m; MpCoreEvent e;
            for (MpCore* c : all) { while (c->poll(&m)) {} while (c->pollEvent(&e)) {} }
        }
        if (it % 2500 == 0) {
            for (size_t i = 1; i < all.size();) {
                if (!all[i]->active()) { delete all[i]; all.erase(all.begin() + (long)i); replaced++; } else i++;
            }
            if (!host->active()) { host->uiLeave(); CHECK(host->uiHost(&hp)); WaitPhase(all, host, MP_UI_LOBBY, 2000); }
            while (all.size() < 3) addGuest();
        }
    }
    Step(all, 3000);
    for (size_t i = 1; i < all.size();) if (!all[i]->active()) { delete all[i]; all.erase(all.begin() + (long)i); } else i++;
    while (all.size() < 3) addGuest();
    Step(all, 1000);
    // payfix: the injector speaks as the authenticated host, so a forged "host is leaving" can still be running down its wait in a guest
    // (a guest that joined in the last second ends 1 s later with "nobody followed the new host"; netopt's one failure). No more injections
    // happen here: the room must settle, i.e. after replacing what ended, the same three consoles stay for a quiet 3 s.
    for (int round = 0; round < 3; round++) {
        for (size_t i = 1; i < all.size();) if (!all[i]->active()) { delete all[i]; all.erase(all.begin() + (long)i); } else i++;
        while (all.size() < 3) addGuest();
        Step(all, 3000);
    }
    CHECK(host->active() && all.size() == 3 && all[1]->active() && all[2]->active());
    printf("stage C: %ld datagrams injected, %d guests replaced, bad=%u; per type:", iters, replaced, (unsigned)host->debugBadCount());
    for (uint8_t t : kCoreTypes) printf(" %02X=%ld", t, perType[t]);
    printf("\n");
    for (MpCore* c : all) delete c;
    delete hub;
}

int main(int argc, char** argv) {
    const long itA = argc > 1 ? atol(argv[1]) : 40000, itB = argc > 2 ? atol(argv[2]) : 300000, itC = argc > 3 ? atol(argv[3]) : 300000;
    g_rng = argc > 4 ? (uint32_t)atoi(argv[4]) * 2654435761u + 7u : 1;
    if (!g_rng) g_rng = 1;
    mp_log_set_sink(LogSink, true);
    std::vector<Spec> specs = BuildSpecs();
    std::map<std::string, std::pair<long, long>> stats;
    StageA(specs, itA, stats);
    printf("stage A: %zu types x %ld mutations (accepted/rejected):", specs.size(), itA);
    for (auto& s : specs) printf(" %s=%ld/%ld", s.name, stats[s.name].first, stats[s.name].second);
    printf("\n");
    for (auto& s : specs) {   // the mutator must reach both outcomes for every type that has a reader (a fuzz that only rejects tests nothing)
        const auto& st = stats[s.name];
        if (!st.first && s.type != MP_MSG_EMOTE) { fprintf(stderr, "  %s: no mutation was ever accepted\n", s.name); CHECK(!"no accepted mutation"); }
        if (!st.second && s.type != MP_MSG_EMOTE && s.type != MP_MSG_LOOK) { fprintf(stderr, "  %s: no mutation was ever rejected\n", s.name); CHECK(!"no rejected mutation"); }
    }
    StageB(specs, itB);
    StageC(itC);
    if (getenv("FUZZ_LOG") || g_fail) for (size_t i = g_log.size() > 200 ? g_log.size() - 200 : 0; i < g_log.size(); i++) printf("  %s\n", g_log[i].c_str());
    if (g_fail) { printf("PAYLOAD FUZZ FAILED: %d checks\n", g_fail); return 1; }
    printf("payload fuzz passed\n");
    return 0;
}
