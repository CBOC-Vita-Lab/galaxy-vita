// ------------------------------------------------------- tiny state lists (GXV_TINYLIST, GXV_TINYLIST_CENSUS; tinylist31, 17 Nov)
// Included into vgx_state.cpp after StateListCall. vgx_dlcache.cpp CallListBody parses a tiny list (GXV_DL_TINY, 64 bytes) at once:
// decoding it is cheaper than a geometry-cache entry's checks. Only a padded material list (32 / 64 bytes, 32-byte aligned, first command
// BP) went to the state-list cache. J3D's small differed lists (J3DShapePacket::calcDifferedBufferSize: a material colour, 13 bytes, or a
// texture matrix, padded to 32 / 64 bytes) begin with an XF write: register writes only, called before most shapes, mostly with the bytes
// of their last call (GXV_J3D_DIFFKEEP stores a differed list only when its bytes change), and parsed on every call.
//   GXV_TINYLIST=0  (default) as before.
//   GXV_TINYLIST=1  a tiny list whose first command is a state command (XF, CP, BP, an indexed matrix load) goes to the state-list cache:
//                   StateListFastHit, then StateListCall without mixed-list plans. It is replayed while its bytes are unchanged (the
//                   write generation of its bytes or, after a write near them, the bytes compared with the copy kept at the parse:
//                   at most GXV_DL_TINY bytes), from the operations SlParse made of them (SlReplay, or the register deltas: GXV_SLDELTA),
//                   which make the calls ProcessStream makes in its order. A list the cache refuses (a draw, a call, an opcode it does not
//                   replay) is parsed as before, and for 600 pictures goes straight to the parse after one look at its front slot; a list
//                   whose bytes keep changing is parsed for 300 pictures (the state lists' churn back-off).
//   GXV_TINYLIST=2  verify (diagnostics builds; a quiet build takes it as 0): the list is parsed for real; then, from the State before the
//                   call, the path of =1; the State it leaves must be the parse's ("tinylist: MISMATCH ..." and the count). The parse's
//                   State is kept. A list with a BP write that acts outside the registers (draw done, token, EFB copy, palette load,
//                   the port's marker) is parsed only, not compared (it would act twice).
// Not with the two-core front end or the J3D-native hooks (lists parsed as before), not in the second worker.
//
//   GXV_TINYLIST_CENSUS=1 (count only, any build; =<n>, n >= 2: lines every n pictures instead of 300): every tiny-branch call of
//   CallListBody is timed (port_host_time_ns, around the branch: array sync, state-list attempt, parse or replay) and sorted
//     - by content: state only, by its first command (XF / BP / CP / indexed load); state and draws; a particle quad (one draw first,
//       then padding: jpa_dl, FxTinyDraw's shape); other draws only; other (a CALL_DL inside, an unknown opcode, empty);
//     - by way: parsed; padded state list replayed / refused and parsed; GXV_TINYLIST replayed / refused and parsed / verified;
//     - by call site: CALL_DL, J3D shape call (0x59, GXV_RETAINED's handle), particle command item (0x58, GXV_FXMERGE), j3n bracket;
//     - by the stream census's source (GXV_STREAM_CENSUS: the game's markers, only in its counted pictures);
//   and for each, how many calls had the bytes of the last call of the same address ("same"). "Could replay" is the state-only calls
//   with the same bytes that were parsed: what GXV_TINYLIST=1 can take. Classification and the bytes' hash are outside the timed part.
//   Lines ("tinylist-census:", port_log_summary): per picture by content / way / site, the replay counters, the eight costliest
//   addresses (bytes, content, way, calls and ms a picture, us a call, share with the same bytes, first bytes), the sources.
uint8_t g_tlSite = 0;
namespace {
uint64_t g_tlFast = 0, g_tlCall = 0, g_tlRefused = 0, g_tlQuiet = 0, g_tlChecked = 0, g_tlBad = 0, g_tlUncompared = 0, g_tlVerifyOff = 0;
inline int TinyListModeK() {
#if defined(VGX_CLONE_B)
    return 0;
#else
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_TINYLIST", 0);
        if (v == 2) return diag::kEnabled ? 2 : 0; // (verify needs a diagnostics build; a quiet build stays off rather than on unverified)
        return v == 1 ? 1 : 0;
    }()));
    return m;
#endif
}
// (GXV_TINYLIST: =1's path; true: replayed)
inline bool TinyListReplay(const uint8_t* list, uint32_t nbytes) {
#if defined(GXV_TINYLIST_MUTANT) && GXV_TINYLIST_MUTANT == 1
    // (negative control for the host test: the list's entry replayed without its bytes being checked; a changed list replays its old operations)
    if (SlEntry* m = SlLookup(list)) if (m->usable && m->live && m->nbytes == nbytes) { SlRun(*m, 1); ++g_tlFast; return true; }
#endif
#if defined(GXV_TINYLIST_MUTANT) && GXV_TINYLIST_MUTANT == 2
    nbytes = nbytes > 32 ? 32u : nbytes; // (negative control: the replay made of the first 32 bytes only)
#endif
    // (a list the cache refused or that keeps changing: StateListFastHit and StateListCall would both return false; one look here)
    const SlFront& f = SlFrontOf(list);
    if (f.list == list && f.e && CurrentFrame() < f.e->quietUntil) { ++g_tlQuiet; return false; }
    if (StateListFastHit(list, nbytes)) { ++g_tlFast; return true; }
    if (StateListCallP(list, nbytes, false)) { ++g_tlCall; return true; }
    ++g_tlRefused;
    return false;
}
// a BP write that acts outside the registers (kBpEffect but the mask); the walk stops at anything but a state command
bool TlTrigger(const uint8_t* p, uint32_t n) {
    const uint8_t* const end = p + n;
    while (p < end) {
        const uint8_t c = *p++;
        size_t len;
        if (c == 0x00 || c == 0x44 || c == 0x48) continue;
        if (c == 0x61) {
            if (end - p < 4) return false;
            if (p[0] != 0xFE && kBpEffect.t[p[0]]) return true;
            len = 4;
        } else if (c == 0x08) len = 5;
        else if (c == 0x10) { if (end - p < 4) return false; len = 4u + ((be16(p) & 0xFu) + 1u) * 4u; }
        else if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) len = 4;
        else return false;
        if (static_cast<size_t>(end - p) < len) return false;
        p += len;
    }
    return false;
}
} // namespace
int TinyListMode() { return TinyListModeK(); }
bool TinyListCall(const uint8_t* list, uint32_t nbytes, int mode) {
#if defined(VGX_CLONE_B)
    (void)list; (void)nbytes; (void)mode;
    return false;
#else
    if (vgx2_mode != 0 || J3nMode() != 0) return false;
    if (!(diag::kEnabled && mode == 2)) return TinyListReplay(list, nbytes);
    if (TlTrigger(list, nbytes)) {
        ++g_tlUncompared;
        ProcessStream(list, nbytes);
        return true;
    }
    static State before, byParse; // (static: the state is large for a thread's stack)
    std::memcpy(&before, &g, sizeof(State));
    ProcessStream(list, nbytes);
    std::memcpy(&byParse, &g, sizeof(State));
    std::memcpy(&g, &before, sizeof(State));
    if (TinyListReplay(list, nbytes)) {
        ++g_tlChecked;
        if (std::memcmp(&g, &byParse, sizeof(State)) != 0) {
            ++g_tlBad;
            uint8_t h[8] = {};
            std::memcpy(h, list, nbytes < 8 ? nbytes : 8);
            VGX_LOG_N(16, "tinylist: MISMATCH for list %p (%u bytes, %02X%02X%02X%02X%02X%02X%02X%02X)\n", static_cast<const void*>(list), nbytes, h[0], h[1], h[2], h[3],
                      h[4], h[5], h[6], h[7]);
        }
        if ((g_tlChecked & 0x3FFFu) == 0)
            VGX_LOG_N(100, "tinylist: %llu replays checked, %llu mismatching\n", static_cast<unsigned long long>(g_tlChecked), static_cast<unsigned long long>(g_tlBad));
    } else {
        ++g_tlVerifyOff; // (refused or backed off: the parse alone, nothing to compare)
    }
    std::memcpy(&g, &byParse, sizeof(State));
    return true;
#endif
}
void TinyListCounters(uint64_t out[8]) {
    out[0] = g_tlFast; out[1] = g_tlCall; out[2] = g_tlRefused; out[3] = g_tlQuiet; out[4] = g_tlChecked; out[5] = g_tlBad; out[6] = g_tlUncompared; out[7] = g_tlVerifyOff;
}

// ---- the census (GXV_TINYLIST_CENSUS)
namespace {
enum : uint8_t { TcXf, TcBp, TcCp, TcIdx, TcStateDraw, TcQuad, TcDraws, TcOther, TcClasses };
inline bool TcStateOnly(uint8_t c) { return c <= TcIdx; }
// What a tiny list holds, by ProcessStream's walk of it (draw lengths by the vertex size of the registers as they are now).
uint8_t TlClassify(const uint8_t* list, uint32_t n) {
    const uint8_t* p = list;
    const uint8_t* const end = list + n;
    uint8_t first = 0xFF; // (the first state command's class)
    uint32_t draws = 0, state = 0;
    bool drawFirst = false, other = false;
    while (p < end) {
        const uint8_t c = *p++;
        if (c == 0x00 || c == 0x44 || c == 0x48) continue;
        size_t len;
        uint8_t k;
        if (c == 0x61) { len = 4; k = TcBp; }
        else if (c == 0x08) { len = 5; k = TcCp; }
        else if (c == 0x10) { if (end - p < 4) break; len = 4u + ((be16(p) & 0xFu) + 1u) * 4u; k = TcXf; }
        else if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) { len = 4; k = TcIdx; }
        else if (c & 0x80) {
            if (end - p < 2) break;
            if (!draws && !state) drawFirst = true;
            ++draws;
            const uint64_t used = static_cast<uint64_t>(be16(p)) * FpStreamSize(c & 7);
            if (used > static_cast<uint64_t>(end - p) - 2u) break;
            p += 2 + used;
            continue;
        } else { other = true; break; } // (a CALL_DL, an unknown opcode)
        if (static_cast<size_t>(end - p) < len) break;
        p += len;
        if (first == 0xFF) first = k;
        ++state;
    }
    if (other) return TcOther;
    if (state && draws) return TcStateDraw;
    if (state) return first;
    if (draws) return (draws == 1 && drawFirst) ? TcQuad : TcDraws;
    return TcOther;
}
enum : uint8_t { TsCall, TsShape, TsFx, TsJ3n, TsSites };
struct TlAcc { uint64_t n = 0, ns = 0, bytes = 0, same = 0; };
// per address, kept from window to window (its last bytes' hash); the counts are the window's
struct TlAddr { const uint8_t* list = nullptr; uint64_t hash = 0, ns = 0; uint32_t nbytes = 0, calls = 0, same = 0; uint8_t cls = 0, way = 0, head[8] = {}; bool seen = false; };
constexpr uint32_t kTlSlots = 8192, kTlFill = 6144;
constexpr int kTlSrc = 24;
struct TlSrc { uint32_t id; TlAcc a; };
struct TlCensus {
    uint64_t first = 0, last = 0, lost = 0;
    TlAcc all, cls[TcClasses], way[kTwWays], site[TsSites], could, stateAll;
    TlSrc src[kTlSrc] = {};
    int nSrc = 0;
    uint64_t srcPics = 0, srcLast = 0; // (pictures the stream census counted in this window)
    uint64_t c0[8] = {};               // (TinyListCounters at the window's start)
} g_tlc;
TlAddr* g_tlAddr = nullptr; // [kTlSlots], made at the first census call
uint32_t g_tlUsed = 0;
inline int TlCensusN() { return static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_TINYLIST_CENSUS", 0)))); }
TlAddr* TlSlot(const uint8_t* list) {
    if (!g_tlAddr) g_tlAddr = new TlAddr[kTlSlots];
    if (g_tlUsed >= kTlFill) { // (full: start again; the window's counts of the dropped addresses are lost)
        for (uint32_t i = 0; i < kTlSlots; ++i)
            if (g_tlAddr[i].calls) ++g_tlc.lost;
        for (uint32_t i = 0; i < kTlSlots; ++i) g_tlAddr[i] = TlAddr{};
        g_tlUsed = 0;
    }
    uint32_t i = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(list) >> 2) * 2654435761u) >> 19; // (13 bits)
    for (;; i = (i + 1) & (kTlSlots - 1)) {
        TlAddr& s = g_tlAddr[i];
        if (s.list == list) return &s;
        if (!s.list) { s.list = list; ++g_tlUsed; return &s; }
    }
}
const char* const kTcName[TcClasses] = {"xf-state", "bp-state", "cp-state", "idx-state", "state+draw", "particle quad", "draws only", "other"};
const char* const kTwName[kTwWays] = {"parsed", "padded state list replayed", "padded state list refused (parsed)", "GXV_TINYLIST replayed",
                                      "GXV_TINYLIST refused/backed off (parsed)", "GXV_TINYLIST verified"};
const char* const kTwShort[kTwWays] = {"parsed", "pad-sl", "pad-parsed", "tl", "tl-parsed", "tl-verify"};
const char* const kTsName[TsSites] = {"CALL_DL", "shape call", "particle item", "j3n bracket"};
void TlCensusReport() {
    TlCensus& c = g_tlc;
    const uint64_t pics = c.last - c.first;
    uint64_t now[8];
    TinyListCounters(now);
    if (port_log_summary && pics) {
        const double p = static_cast<double>(pics);
        auto acc = [&](char* b, size_t cap, int& at, const char* name, const TlAcc& a) {
            if (a.n && at < static_cast<int>(cap) - 90)
                at += std::snprintf(b + at, cap - at, " %s %.1f (%.3f ms, same %.1f);", name, a.n / p, a.ns / 1e6 / p, a.same / p);
        };
        char bc[640], bw[640], bs[320];
        int ac = 0, aw = 0, as = 0;
        bc[0] = bw[0] = bs[0] = 0;
        for (int i = 0; i < TcClasses; ++i) acc(bc, sizeof bc, ac, kTcName[i], c.cls[i]);
        for (int i = 0; i < kTwWays; ++i) acc(bw, sizeof bw, aw, kTwName[i], c.way[i]);
        for (int i = 0; i < TsSites; ++i) acc(bs, sizeof bs, as, kTsName[i], c.site[i]);
        auto d = [&](int i) { return (now[i] - c.c0[i]) / p; };
        port_log_summary("tinylist-census: %llu pictures (GXV_TINYLIST=%d); tiny calls per picture %.1f (%.2f KB, %.3f ms), same bytes as the address's last call %.1f; "
                         "state only %.1f (%.3f ms, same %.1f); could replay (state only, same bytes, parsed) %.1f (%.3f ms); by content:%s by way:%s by site:%s "
                         "GXV_TINYLIST per picture: replays by the fast hit %.1f, by StateListCall %.2f, refused %.2f, backed off %.1f; verify: checked %.1f, mismatching %.2f, "
                         "not compared (trigger %.2f, not replayed %.2f)",
                         static_cast<unsigned long long>(pics), TinyListModeK(), c.all.n / p, c.all.bytes / 1024.0 / p, c.all.ns / 1e6 / p, c.all.same / p, c.stateAll.n / p,
                         c.stateAll.ns / 1e6 / p, c.stateAll.same / p, c.could.n / p, c.could.ns / 1e6 / p, ac ? bc : " -", aw ? bw : " -", as ? bs : " -", d(0), d(1), d(2), d(3),
                         d(4), d(5), d(6), d(7));
        TlAddr* top[8] = {};
        if (g_tlAddr)
            for (uint32_t k = 0; k < kTlSlots; ++k) {
                TlAddr& s = g_tlAddr[k];
                if (!s.list || !s.calls) continue;
                for (int i = 0; i < 8; ++i)
                    if (!top[i] || s.ns > top[i]->ns) {
                        for (int j = 7; j > i; --j) top[j] = top[j - 1];
                        top[i] = &s;
                        break;
                    }
            }
        char line[1100];
        int at = 0;
        line[0] = 0;
        for (int i = 0; i < 8 && top[i]; ++i) {
            const TlAddr& s = *top[i];
            at += std::snprintf(line + at, sizeof line - at, " [%p %uB %s %s %.2f/pic %.3fms %.1fus same %u%% %02X%02X%02X%02X%02X%02X%02X%02X]", static_cast<const void*>(s.list),
                                s.nbytes, kTcName[s.cls < TcClasses ? s.cls : static_cast<uint8_t>(TcOther)], kTwShort[s.way < kTwWays ? s.way : 0], s.calls / p, s.ns / 1e6 / p,
                                s.ns / 1e3 / s.calls, static_cast<unsigned>(100u * s.same / s.calls), s.head[0], s.head[1], s.head[2], s.head[3], s.head[4], s.head[5],
                                s.head[6], s.head[7]);
            if (at >= static_cast<int>(sizeof line) - 150) break;
        }
        port_log_summary("tinylist-census: costliest tiny lists (address, bytes, content, way, calls a picture, ms a picture, us a call, calls with the last call's bytes, "
                         "first bytes):%s%s",
                         at ? line : " none", c.lost ? " (address table refilled: some not listed)" : "");
#if !defined(VGX_CLONE_B)
        if (c.nSrc && c.srcPics) {
            // (sources: the stream census's counted pictures only; per counted picture)
            int idx[kTlSrc];
            for (int i = 0; i < c.nSrc; ++i) idx[i] = i;
            std::sort(idx, idx + c.nSrc, [&](int a, int b) { return c.src[a].a.ns > c.src[b].a.ns; });
            const double q = static_cast<double>(c.srcPics);
            char bsrc[900];
            int a2 = 0;
            for (int i = 0; i < c.nSrc && a2 < static_cast<int>(sizeof bsrc) - 90; ++i) {
                const TlSrc& s = c.src[idx[i]];
                char nm[24];
                sc::SrcName(s.id, nm, sizeof nm);
                a2 += std::snprintf(bsrc + a2, sizeof bsrc - a2, " %s %.1f (%.3f ms, same %.1f);", nm, s.a.n / q, s.a.ns / 1e6 / q, s.a.same / q);
            }
            port_log_summary("tinylist-census: by the stream census's source (%llu counted pictures, per counted picture):%s", static_cast<unsigned long long>(c.srcPics), bsrc);
        }
#endif
    }
    const uint64_t keepLast = c.last;
    c = TlCensus{};
    c.first = c.last = keepLast;
    for (int i = 0; i < 8; ++i) c.c0[i] = now[i];
    if (g_tlAddr)
        for (uint32_t k = 0; k < kTlSlots; ++k) { g_tlAddr[k].calls = g_tlAddr[k].same = 0; g_tlAddr[k].ns = 0; }
}
} // namespace
bool TinyListCensusOn() { return TlCensusN() != 0; }
void TinyListCensusCall(const uint8_t* list, uint32_t nbytes, uint8_t (*branch)(const uint8_t*, uint32_t)) {
    const uint64_t frame = CurrentFrame();
    TlCensus& c = g_tlc;
    if (frame != c.last) {
        if (!c.first) c.first = frame;
        c.last = frame;
        const int n = TlCensusN();
        if (frame - c.first >= (n >= 2 ? static_cast<uint64_t>(n) : 300u)) TlCensusReport();
    }
    // (outside the timed part: what the list holds, and whether its bytes are those of its address's last call)
    const uint8_t cls = TlClassify(list, nbytes);
    const uint64_t h = XXH3_64bits(list, nbytes);
    TlAddr* s = TlSlot(list);
    const bool same = s->seen && s->hash == h && s->nbytes == nbytes;
    const uint8_t site = g_tlSite < TsSites ? g_tlSite : static_cast<uint8_t>(TsCall);
    const int64_t t0 = port_host_time_ns ? port_host_time_ns() : 0;
    const uint8_t way = branch(list, nbytes);
    const uint64_t ns = port_host_time_ns ? static_cast<uint64_t>(port_host_time_ns() - t0) : 0;
    auto add = [&](TlAcc& a) { ++a.n; a.ns += ns; a.bytes += nbytes; if (same) ++a.same; };
    add(c.all);
    add(c.cls[cls]);
    add(c.way[way < kTwWays ? way : static_cast<uint8_t>(kTwParsed)]);
    add(c.site[site]);
    if (TcStateOnly(cls)) {
        add(c.stateAll);
        if (same && (way == kTwParsed || way == kTwPadParsed || way == kTwTlParsed)) add(c.could);
    }
#if !defined(VGX_CLONE_B)
    if (sc::g_on) {
        if (c.srcLast != frame) { c.srcLast = frame; ++c.srcPics; }
        int i = 0;
        for (; i < c.nSrc; ++i)
            if (c.src[i].id == sc::g_curSrc) break;
        if (i == c.nSrc && c.nSrc < kTlSrc) { c.src[c.nSrc].id = sc::g_curSrc; c.src[c.nSrc].a = TlAcc{}; ++c.nSrc; }
        if (i < c.nSrc) add(c.src[i].a);
    }
#endif
    if (!s->calls) { std::memcpy(s->head, list, nbytes < 8 ? nbytes : 8); }
    s->seen = true;
    s->hash = h;
    s->nbytes = nbytes;
    s->cls = cls;
    s->way = way;
    ++s->calls;
    s->ns += ns;
    if (same) ++s->same;
}
