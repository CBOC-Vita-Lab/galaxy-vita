// geopass23 (8 Nov 2026): GXV_GEOPASS (where the picture's geometry goes: a census) and GXV_OCCLCULL (draws hidden behind a closed
// occluder are not drawn). Included into vgx_draw.cpp, namespace vgx, after the clip-volume culls (it uses g_cha's pending bounds, the
// matrices of State g and the command buffer g_buf); everything here runs on the player's thread.
//
// ================================================================ hidden behind a closed occluder (GXV_OCCLCULL)
// Galaxy's planets are big closed shapes, and much of what a picture draws lies on their far side. A draw is left out when its every
// point lies in the "shadow" (as seen from the eye) of a ball that is inside an occluder drawn earlier in the picture with depth writes:
//   * The occluder. One static run of a cached list (one GL draw: one program, one matrix, so shared vertices are transformed to the
//     same bits and its triangles meet without cracks) whose triangles form a closed, consistently wound surface: every directed edge,
//     by the 16 bytes of its end points' positions, appears exactly once and its reverse exactly once (OcclAnalyse, once per list at its
//     promotion). Its inscribed ball: a candidate centre (box centre, vertex centroid, area centroid) whose generalised winding number is
//     +-1 (inside; the sign says which way the surface is wound), radius = the least distance to any triangle, in double precision,
//     shrunk for the float rounding of the centre. Registered when drawn (after its own culls) with: depth test LESS / LEQUAL with
//     writes, no Z texture, an alpha test that always passes (no `discard`), a cull mode that keeps the surface's eye-facing outer side
//     (GX front faces are clockwise on screen; worked out from the winding, the matrix's determinant, the projection's and the
//     viewport's signs), a perspective projection, a near-uniform, unsheared matrix, the whole surface (its bounding radius about the
//     centre) farther from the eye than the near plane along any ray through the viewport, the ball in front of the eye and nearer
//     than the far plane.
//   * Why its depth covers the shadow: for a sample whose ray passes through the ball, the ray starts outside the surface (the eye is
//     farther from the centre than the bounding radius) and reaches the ball inside it, so it enters the surface before the ball
//     through a face whose outer side faces the eye: that face is not culled, covers the sample (the surface is watertight in the
//     rasteriser's snapped coordinates, which move points sideways by a fraction of a pixel: the ball is shrunk by 2 EFB pixels at its
//     depth for that) and writes a depth no larger than the ball's far side. Depth only falls after that (below), so a later fragment
//     deeper than the ball's far side by a margin fails a LESS / LEQUAL test: the draw writes nothing.
//   * The occludee: a later draw of the same picture with the same projection, viewport (with its depth range) and scissor (XF
//     0x101A-0x1026, BP 0x20 / 0x21 / 0x59, the EFB size: one signature), depth test LESS or LEQUAL, not blended (no blend, subtract or
//     logic op: as asked; a blended draw would fail the same test), no Z texture, triangles; every corner of its list's bounds (each row's
//     box through that row's matrix for skinned runs) or every decoded vertex (dynamic draws), in eye space, inside ONE occluder's cone
//     (apex the eye, tangent to the shrunk ball) and deeper than the ball's far side by a margin of 2^-16 of the depth range in window
//     depth (the depth buffer and the GPU's interpolation resolve ~2^-23). The cone intersected with that half space is convex, so the corners
//     decide for the whole box. The first use of a list is drawn (its programs are linked when it appears, as GXV_STATICCULL does).
//   * Depth may only fall in between ("epoch"): the occluders are forgotten at every draw that writes depth with a test other than
//     NEVER / LESS / EQUAL / LEQUAL (or without the test, or with a Z texture), at every EFB copy that clears depth, a change of the EFB
//     size and the picture's end. (A copy that does not clear leaves depth alone: what it reads is the same with the hidden draw left
//     out, since that draw wrote nothing.)
//   * Left out the way GXV_STATICCULL leaves a run out (before its resolution: the state commands are applied as before) or GXV_DYNCULL
//     a dynamic draw (after its resolution).
//   GXV_OCCLCULL=1 on, 2 count only (everything is worked out, nothing left out), 0 off (default: the lists are not analysed). Read once.
//   GXV_OCCLCULL_MINTRIS (64): runs with fewer triangles are not analysed. GXV_OCCLCULL_BUDGET (200000): triangles analysed a picture at
//   most (the analysis runs when a list is promoted, mostly on entering an area; a run over the budget is never an occluder).
//   Counters: the "occlcull:" basic-log line every 10 s (and the GXV_GEOPASS line's occluder census).

int OcclCullMode() {
    const int m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_OCCLCULL", 0), 0, 2)));
    return m;
}
int GeoPassMode() {
    const int m = VGX_KNOB(EnvInt("GXV_GEOPASS", 0) != 0 ? 1 : 0);
    return m;
}

namespace {
uint32_t OcclMinTris() { return static_cast<uint32_t>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_OCCLCULL_MINTRIS", 64), 4, 1 << 20)))); }
uint64_t OcclBudget() { return static_cast<uint64_t>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_OCCLCULL_BUDGET", 200000), 0, 1 << 30)))); }
uint64_t g_occlAnalysedThisPicture = 0; // (triangles: OcclBudget)

struct V3 { double x, y, z; };
inline V3 Sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
// Squared distance from p to the triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
double PointTriDist2(const V3& p, const V3& a, const V3& b, const V3& c) {
    const V3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
    const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    auto d2of = [&](const V3& q) { const V3 d = Sub(p, q); return Dot(d, d); };
    if (d1 <= 0.0 && d2 <= 0.0) return d2of(a);
    const V3 bp = Sub(p, b);
    const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return d2of(b);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double v = d1 / (d1 - d3);
        return d2of(V3{a.x + v * ab.x, a.y + v * ab.y, a.z + v * ab.z});
    }
    const V3 cp = Sub(p, c);
    const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return d2of(c);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double w = d2 / (d2 - d6);
        return d2of(V3{a.x + w * ac.x, a.y + w * ac.y, a.z + w * ac.z});
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return d2of(V3{b.x + w * (c.x - b.x), b.y + w * (c.y - b.y), b.z + w * (c.z - b.z)});
    }
    const double den = 1.0 / (va + vb + vc);
    const double v = vb * den, w = vc * den;
    return d2of(V3{a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w});
}
struct Key16 {
    uint32_t w[4];
    bool operator==(const Key16& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3]; }
};
struct Key16Hash {
    size_t operator()(const Key16& k) const { return static_cast<size_t>(XXH3_64bits(k.w, sizeof k.w)); }
};
} // namespace

namespace {
void AnalyseRun(OccShape& s, const uint8_t* vtx, size_t vtxBytes, uint32_t stride, uint32_t addrBase, const uint16_t* idx, uint32_t idxCount) {
    s = OccShape{};
    const uint32_t nt = idxCount / 3;
    s.tris = nt;
    if (nt < OcclMinTris() || !vtx || !idx || stride < 16) return;
    if (g_occlAnalysedThisPicture + nt > OcclBudget()) { s.analysed = 2; return; }
    g_occlAnalysedThisPicture += nt;
    s.analysed = 1;
    static std::unordered_map<Key16, uint32_t, Key16Hash> ids;
    static std::vector<V3> pos;
    static std::vector<uint64_t> hpos; // (the position's hash: boundary edges of different runs compare by it)
    static std::vector<uint32_t> tri;
    static std::unordered_map<uint64_t, uint32_t> edges;
    ids.clear(); pos.clear(); hpos.clear(); tri.clear(); edges.clear();
    ids.reserve(static_cast<size_t>(nt) + 16);
    tri.reserve(static_cast<size_t>(nt) * 3);
    for (uint32_t t = 0; t < nt; ++t) {
        uint32_t id[3];
        for (int k = 0; k < 3; ++k) {
            const size_t at = static_cast<size_t>(addrBase) + static_cast<size_t>(idx[3 * t + k]) * stride;
            if (at + 16 > vtxBytes) return;
            Key16 key;
            std::memcpy(key.w, vtx + at, 16);
            float f[4];
            std::memcpy(f, key.w, 16);
            if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3])) return;
            auto it = ids.emplace(key, static_cast<uint32_t>(pos.size()));
            if (it.second) {
                pos.push_back(V3{f[0], f[1], f[2]});
                hpos.push_back(XXH3_64bits(key.w, sizeof key.w) | 1u);
            }
            id[k] = it.first->second;
        }
        if (id[0] == id[1] || id[1] == id[2] || id[0] == id[2]) continue; // (two equal positions: no area, no pixel, no edges)
        tri.push_back(id[0]); tri.push_back(id[1]); tri.push_back(id[2]);
    }
    const size_t n3 = tri.size();
    if (n3 < 12) return;
    edges.reserve(n3 + 16);
    for (size_t i = 0; i < n3; i += 3)
        for (int k = 0; k < 3; ++k) {
            const uint64_t u = tri[i + k], v = tri[i + (k + 1) % 3];
            ++edges[(u << 32) | v];
        }
    bool closed = true;
    for (const auto& e : edges) {
        const uint64_t u = e.first >> 32, v = e.first & 0xFFFFFFFFu;
        const auto r = edges.find((v << 32) | u);
        const uint32_t back = r == edges.end() ? 0u : r->second;
        if (e.second != 1 || back != 1) closed = false;
        if (e.second > back) {
            const uint32_t extra = e.second - back;
            s.bEdges += extra;
            // (an ordered pair's hash: the two positions' hashes mixed asymmetrically)
            const uint64_t h = hpos[u] * 0x9E3779B97F4A7C15ull + ((hpos[v] << 17) | (hpos[v] >> 47)) * 0xC2B2AE3D27D4EB4Full;
            const uint64_t hr = hpos[v] * 0x9E3779B97F4A7C15ull + ((hpos[u] << 17) | (hpos[u] >> 47)) * 0xC2B2AE3D27D4EB4Full;
            s.bSum += h * extra;
            s.bRev += hr * extra;
        }
    }
    if (!closed) return;
    // candidate centres: the bounding box's centre, the vertex centroid, the area-weighted centroid of the triangles
    V3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300}, sum{0, 0, 0};
    for (const V3& p : pos) {
        mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
        mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
        sum.x += p.x; sum.y += p.y; sum.z += p.z;
    }
    V3 area{0, 0, 0};
    double areaSum = 0.0;
    for (size_t i = 0; i < n3; i += 3) {
        const V3 &a = pos[tri[i]], &b = pos[tri[i + 1]], &c = pos[tri[i + 2]];
        const double ar = Len(Cross(Sub(b, a), Sub(c, a)));
        area.x += ar * (a.x + b.x + c.x); area.y += ar * (a.y + b.y + c.y); area.z += ar * (a.z + b.z + c.z);
        areaSum += ar;
    }
    const double np = static_cast<double>(pos.size());
    V3 cand[3] = {V3{0.5 * (mn.x + mx.x), 0.5 * (mn.y + mx.y), 0.5 * (mn.z + mx.z)}, V3{sum.x / np, sum.y / np, sum.z / np},
                  areaSum > 0.0 ? V3{area.x / (3.0 * areaSum), area.y / (3.0 * areaSum), area.z / (3.0 * areaSum)} : V3{0.5 * (mn.x + mx.x), 0.5 * (mn.y + mx.y), 0.5 * (mn.z + mx.z)}};
    double bestR = 0.0, bestBig = 0.0;
    V3 bestC{0, 0, 0};
    int bestO = 0;
    constexpr double kFourPi = 12.566370614359172;
    for (const V3& c : cand) {
        // generalised winding number (sum of the triangles' signed solid angles, Van Oosterom & Strackee)
        double wsum = 0.0, d2min = 1e300;
        for (size_t i = 0; i < n3; i += 3) {
            const V3 a = Sub(pos[tri[i]], c), b = Sub(pos[tri[i + 1]], c), d = Sub(pos[tri[i + 2]], c);
            const double la = Len(a), lb = Len(b), ld = Len(d);
            const double num = Dot(a, Cross(b, d));
            const double den = la * lb * ld + Dot(a, b) * ld + Dot(a, d) * lb + Dot(b, d) * la;
            wsum += 2.0 * std::atan2(num, den);
            d2min = std::min(d2min, PointTriDist2(c, pos[tri[i]], pos[tri[i + 1]], pos[tri[i + 2]]));
        }
        const double w = wsum / kFourPi;
        int o = 0;
        if (std::fabs(w - 1.0) < 1e-4) o = 1; // (closed: a whole number but for rounding)
        else if (std::fabs(w + 1.0) < 1e-4) o = -1;
        if (!o) continue; // (outside, or on the surface)
        const double r = std::sqrt(d2min);
        if (r <= bestR) continue;
        double big = 0.0;
        for (const V3& p : pos) big = std::max(big, Len(Sub(p, c)));
        bestR = r; bestBig = big; bestC = c; bestO = o;
    }
    if (bestO == 0 || !(bestR > 0.0)) return;
    // (the centre is kept as floats: the ball shrinks and the bounding radius grows by what that rounding moved it, and a little more)
    const float cf[3] = {static_cast<float>(bestC.x), static_cast<float>(bestC.y), static_cast<float>(bestC.z)};
    const double moved = Len(Sub(bestC, V3{cf[0], cf[1], cf[2]}));
    const double slack = 1e-6 * (Len(bestC) + bestBig) + moved;
    const double r = bestR * (1.0 - 1e-5) - slack, big = bestBig * (1.0 + 1e-5) + slack;
    if (!(r > 0.0) || !(big < 1e30)) return;
    s.c[0] = cf[0]; s.c[1] = cf[1]; s.c[2] = cf[2];
    s.r = static_cast<float>(r * (1.0 - 1e-6));
    s.R = static_cast<float>(big * (1.0 + 1e-6));
    s.orient = static_cast<int8_t>(bestO);
}
} // namespace

namespace {
// ---- the picture's occluders
struct OccInst {
    uint64_t sig;  // the view signature (ViewSig) of the occluder's draw
    float c[3];    // the ball's centre in eye space
    float c2;      // |c|^2
    float cos2;    // cos^2 of the cone's half angle (shrunk ball), raised by a relative 1e-5 (the test's rounding)
    float sin2;    // (the replacement order: the cone's size)
    float D1;      // eye-space depth (-z) beyond which a point is deeper than the ball's far side by the margin
};
constexpr int kOccMax = 8;
struct OccFrame {
    OccInst o[kOccMax];
    int n = 0;
} g_occ;
const OccShape* g_occPending = nullptr; // SetPendingOccluder
// counters (window: the "occlcull:" line; totals: OcclCounters)
struct OccCtr {
    uint64_t pictures = 0, offered = 0, registered = 0, refState = 0, refCull = 0, refMtx = 0, refNear = 0, refDepth = 0, breaks = 0;
    uint64_t sTested = 0, sCulled = 0, sIdx = 0, pTested = 0, pCulled = 0, pIdx = 0, dTested = 0, dCulled = 0, dIdx = 0, dLater = 0;
    uint64_t runsAnalysed = 0, runsClosed = 0, runsOver = 0, trisAnalysed = 0;
} g_oc, g_ocTotal;
uint64_t g_ocTotalAnalysed = 0, g_ocTotalClosed = 0;

uint64_t ViewSig() {
    uint32_t w[17];
    std::memcpy(w, &g.xfReg[0x1A], 13 * 4); // viewport (scale, offset: with the depth range) and projection
    w[13] = g.bp[0x20] & 0xFFFFFF;
    w[14] = g.bp[0x21] & 0xFFFFFF;
    w[15] = g.bp[0x59] & 0xFFFFFF;
    w[16] = static_cast<uint32_t>(g.efbW) | (static_cast<uint32_t>(g.efbH) << 16);
    return XXH3_64bits(w, sizeof w);
}
inline void OcclBreak() {
    if (!g_occ.n) return;
    g_occ.n = 0;
    ++g_oc.breaks;
}
// At every draw (EmitDraw, EmitStaticRun, EmitStaticDraw), before anything else: a draw that may raise a sample's stored depth ends the
// occluders' epoch.
inline void OcclDepthNote() {
    if (__builtin_expect(g_occ.n == 0, 1)) return;
    const uint32_t zm = g.bp[0x40];
    if (!(zm & 0x10u)) return;                                  // (no depth writes)
    const uint32_t func = (zm >> 1) & 7u;
    const bool ztex = bits(g.bp[0xF5], 2, 2) != 0;
    if ((zm & 1u) && func <= CMP_LEQUAL && !ztex) return;       // NEVER, LESS, EQUAL, LEQUAL: what is stored can only fall
    OcclBreak();
}
// The raw GX state of a draw that may be left out (see above).
inline bool OccludeeState() {
    const uint32_t zm = g.bp[0x40];
    if (!(zm & 1u)) return false;
    const uint32_t func = (zm >> 1) & 7u;
    if (func != CMP_LESS && func != CMP_LEQUAL) return false;
    const uint32_t cm0 = g.bp[0x41];
    if ((cm0 & 1u) || bits(cm0, 11, 1) || (bits(cm0, 1, 1) && bits(cm0, 12, 4) != 3)) return false; // (blend, subtract, logic op)
    if (bits(g.bp[0xF5], 2, 2) != 0) return false;                                                // (Z texture)
    if (g.xfReg[0x26] & 1u) return false;                                                          // (orthographic: no occluder has one)
    return true;
}
inline bool InShadow(const OccInst& o, float x, float y, float z) {
    if (!(-z >= o.D1)) return false; // (also NaN)
    const float dot = x * o.c[0] + y * o.c[1] + z * o.c[2];
    if (!(dot > 0.0f)) return false;
    const float p2 = x * x + y * y + z * z;
    return dot * dot >= p2 * o.c2 * o.cos2;
}
inline void EyeOf(const float* const m[3], float x, float y, float z, float e[3]) {
    for (int r = 0; r < 3; ++r) e[r] = m[r][0] * x + m[r][1] * y + m[r][2] * z + m[r][3];
}
// Bit k of the result: every corner of box b through rows m lies in occluder k's shadow.
uint32_t BoxShadowMask(const RowBox& b, const float* const m[3], uint32_t candidates) {
    for (int k = 0; k < 8 && candidates; ++k) {
        float e[3];
        EyeOf(m, (k & 1) ? b.mx[0] : b.mn[0], (k & 2) ? b.mx[1] : b.mn[1], (k & 4) ? b.mx[2] : b.mn[2], e);
        for (uint32_t c = candidates; c; c &= c - 1) {
            const int i = __builtin_ctz(c);
            if (!InShadow(g_occ.o[i], e[0], e[1], e[2])) candidates &= ~(1u << i);
        }
    }
    return candidates;
}
// The occluders of the current view (bits), 0 when there are none.
inline uint32_t ViewOccluders() {
    if (!g_occ.n) return 0;
    const uint64_t sig = ViewSig();
    uint32_t m = 0;
    for (int i = 0; i < g_occ.n; ++i)
        if (g_occ.o[i].sig == sig) m |= 1u << i;
    return m;
}
uint8_t g_geoPhase = 16; // (GXV_GEOPASS: the game's draw phase of the draws being emitted; 16 = before the first marker of the picture)
struct GeoPhaseCtr { uint64_t calls = 0, idx = 0, occCalls = 0, occIdx = 0; };
GeoPhaseCtr g_geoPh[17];
// EmitStaticRun, after GXV_STATICCULL: whether the run is hidden (and, unless GXV_OCCLCULL=2, left out: what the run's resolution would
// have consumed is consumed here).
bool StaticRunOccluded(uint8_t prim, uint32_t idxCount, bool firstUse) {
    if (prim != PRIM_TRIANGLES || !g_cha.pend || !g_cha.pendN) return false;
    RowBox& all = const_cast<RowBox&>(g_cha.pend[0]);
    if (firstUse || !all.drawn) { all.drawn = 1; return false; } // (the list's first use: drawn, as GXV_STATICCULL draws it)
    if (!OccludeeState()) return false;
    const uint32_t cand = ViewOccluders();
    if (!cand) return false;
    const bool pal = g.vcd[0] != 0;
    uint32_t got = 0;
    if (!pal) {
        ++g_oc.sTested;
        const uint32_t pi = g.matIdxA & 0x3F;
        if (pi > 61) return false;
        const float* m[3] = {g.xfPos[pi], g.xfPos[pi + 1], g.xfPos[pi + 2]};
        got = BoxShadowMask(all, m, cand);
        if (!got) return false;
        ++g_oc.sCulled;
        g_oc.sIdx += idxCount;
    } else {
        if (g_cha.pendN < 2) return false;
        ++g_oc.pTested;
        got = cand;
        for (uint32_t i = 1; i < g_cha.pendN && got; ++i) {
            const uint32_t row = g_cha.pend[i].row;
            if (row > 61) return false; // (the shader clamps there: another matrix)
            const float* m[3] = {g.xfPos[row], g.xfPos[row + 1], g.xfPos[row + 2]};
            got = BoxShadowMask(g_cha.pend[i], m, got);
        }
        if (!got) return false;
        ++g_oc.pCulled;
        g_oc.pIdx += idxCount;
    }
    ++g_geoPh[g_geoPhase].occCalls;
    g_geoPh[g_geoPhase].occIdx += idxCount;
    if (OcclCullMode() != 1) return false; // (=2: counted only)
    g_pendingPosMtxMax = 0xFF;
    g_cha.pend = nullptr;
    g_cha.pendN = 0;
    return true;
}
// EmitDraw, after its resolution and GXV_DYNCULL: whether every decoded vertex v[0..count) lies in one occluder's shadow.
bool DynDrawOccluded(const uint8_t* v, uint32_t count, uint32_t stride) {
    if (!OccludeeState()) return false;
    uint32_t cand = ViewOccluders();
    if (!cand) return false;
    const bool perVtx = cur.vs.perVtxPos != 0;
    uint32_t maxRow = 61;
    if (perVtx) maxRow = cur.vs.perVtxPos == 3 ? static_cast<uint32_t>(kPalSmallMaxRow) : (cur.vs.perVtxPos == 2 ? static_cast<uint32_t>(kPalShortMaxRow) : 61u);
    const float* m[3];
    if (!perVtx) {
        const uint32_t pi = g.matIdxA & 0x3F;
        if (pi > 61) return false;
        m[0] = g.xfPos[pi]; m[1] = g.xfPos[pi + 1]; m[2] = g.xfPos[pi + 2];
    }
    for (uint32_t i = 0; i < count && cand; ++i, v += stride) {
        float q[4];
        std::memcpy(q, v, sizeof q);
        if (perVtx) {
            if (!(q[3] >= 0.0f && q[3] < 64.0f)) return false;
            const uint32_t row = static_cast<uint32_t>(q[3] + 0.5f);
            if (row > maxRow) return false;
            m[0] = g.xfPos[row]; m[1] = g.xfPos[row + 1]; m[2] = g.xfPos[row + 2];
        }
        float e[3];
        EyeOf(m, q[0], q[1], q[2], e);
        for (uint32_t c = cand; c; c &= c - 1) {
            const int k = __builtin_ctz(c);
            if (!InShadow(g_occ.o[k], e[0], e[1], e[2])) cand &= ~(1u << k);
        }
    }
    return cand != 0;
}
// EmitStaticRun, after the run is handed on (drawn): the run as an occluder of the draws after it.
void OcclRegister(const OccShape& s) {
    ++g_oc.offered;
    // ---- the state: its depth is written wherever its faces are rasterised
    const uint32_t zm = g.bp[0x40];
    const uint32_t func = (zm >> 1) & 7u;
    const uint32_t ac = g.bp[0xF3];
    const uint32_t c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    const bool noDiscard = (op == 0 && c0 == 7 && c1 == 7) || (op == 1 && (c0 == 7 || c1 == 7)) || (op == 3 && c0 == 7 && c1 == 7) ||
                           (op == 2 && ((c0 == 7 && c1 == 0) || (c0 == 0 && c1 == 7)));
    if (!(zm & 1u) || !(zm & 0x10u) || (func != CMP_LESS && func != CMP_LEQUAL) || bits(g.bp[0xF5], 2, 2) != 0 || !noDiscard || g.vcd[0] != 0 ||
        (g.xfReg[0x26] & 1u)) {
        ++g_oc.refState;
        return;
    }
    const uint32_t pi = g.matIdxA & 0x3F;
    if (pi > 61) { ++g_oc.refMtx; return; }
    const float* m[3] = {g.xfPos[pi], g.xfPos[pi + 1], g.xfPos[pi + 2]};
    // ---- the matrix: unsheared, its scales within 0.1 % of each other (so the ball stays a ball, within the margins below)
    double col[3][3], sc[3];
    for (int j = 0; j < 3; ++j) {
        for (int r = 0; r < 3; ++r) col[j][r] = m[r][j];
        sc[j] = std::sqrt(col[j][0] * col[j][0] + col[j][1] * col[j][1] + col[j][2] * col[j][2]);
    }
    const double smin = std::min(sc[0], std::min(sc[1], sc[2])), smax = std::max(sc[0], std::max(sc[1], sc[2]));
    bool okM = smin > 0.0 && smax < 1e30 && smax <= smin * 1.001;
    for (int a = 0; a < 3 && okM; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const double d = col[a][0] * col[b][0] + col[a][1] * col[b][1] + col[a][2] * col[b][2];
            if (std::fabs(d) > 1e-4 * sc[a] * sc[b]) okM = false;
        }
    if (!okM) { ++g_oc.refMtx; return; }
    const double det = col[0][0] * (col[1][1] * col[2][2] - col[1][2] * col[2][1]) - col[1][0] * (col[0][1] * col[2][2] - col[0][2] * col[2][1]) +
                       col[2][0] * (col[0][1] * col[1][2] - col[0][2] * col[1][1]);
    // ---- the projection (GX: x = p0 ex + p1 ez, y = p2 ey + p3 ez, z = p4 ez + p5, w = -ez) and the viewport (window y downwards)
    double p[6];
    for (int i = 0; i < 6; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
    const double sx = u2f(g.xfReg[0x1A]), sy = u2f(g.xfReg[0x1B]), sz = u2f(g.xfReg[0x1C]);
    const double ox = u2f(g.xfReg[0x1D]), oy = u2f(g.xfReg[0x1E]);
    if (!(p[0] != 0.0 && p[2] != 0.0 && sx != 0.0 && sy != 0.0 && sz > 0.0 && p[5] < 0.0 && p[4] < 0.0)) { ++g_oc.refState; return; }
    // ---- the cull mode keeps the faces whose outer side faces the eye (their winding on screen: sigma)
    const int sigma = s.orient * (det > 0.0 ? 1 : -1) * ((p[0] * p[2] * sx * sy) > 0.0 ? 1 : -1);
    const uint32_t cull = bits(g.bp[0x00], 14, 2); // (BuildRenderState: 1 CULL_BACK, 2 CULL_FRONT, 3 CULL_ALL)
    const bool kept = cull == 0 || (cull == 1 && sigma > 0) || (cull == 2 && sigma < 0);
    if (!kept) { ++g_oc.refCull; return; }
    // ---- the ball in eye space
    double ce[3];
    for (int r = 0; r < 3; ++r) ce[r] = m[r][0] * s.c[0] + m[r][1] * s.c[1] + m[r][2] * s.c[2] + m[r][3];
    const double clen = std::sqrt(ce[0] * ce[0] + ce[1] * ce[1] + ce[2] * ce[2]);
    const double rEye = static_cast<double>(s.r) * smin * (1.0 - 2e-6), REye = static_cast<double>(s.R) * smax * (1.0 + 2e-6);
    // near and far planes (GX perspective: p4 = -n / (f - n), p5 = -f n / (f - n))
    const double n = p[5] / (p[4] - 1.0), f = p[5] / p[4];
    if (!(n > 0.0 && f > n && clen < 1e30)) { ++g_oc.refState; return; }
    // the steepest ray through the viewport or the scissor (whichever is wider): its length to the near plane is at most n * k
    const int xoff = static_cast<int>(bits(g.bp[0x59], 0, 10)) * 2, yoff = static_cast<int>(bits(g.bp[0x59], 10, 10)) * 2;
    const double scx0 = static_cast<double>(static_cast<int>(bits(g.bp[0x20], 12, 11)) - xoff), scy0 = static_cast<double>(static_cast<int>(bits(g.bp[0x20], 0, 11)) - yoff);
    const double scx1 = static_cast<double>(static_cast<int>(bits(g.bp[0x21], 12, 11)) - xoff + 1), scy1 = static_cast<double>(static_cast<int>(bits(g.bp[0x21], 0, 11)) - yoff + 1);
    const double vpx0 = ox - std::fabs(sx) - xoff, vpx1 = ox + std::fabs(sx) - xoff, vpy0 = oy - std::fabs(sy) - yoff, vpy1 = oy + std::fabs(sy) - yoff;
    const double wx0 = std::min(scx0, vpx0) - 8.0, wx1 = std::max(scx1, vpx1) + 8.0, wy0 = std::min(scy0, vpy0) - 8.0, wy1 = std::max(scy1, vpy1) + 8.0;
    double tx = 0.0, ty = 0.0;
    for (double wx : {wx0, wx1}) tx = std::max(tx, std::fabs(((wx + xoff - ox) / sx + p[1]) / p[0]));
    for (double wy : {wy0, wy1}) ty = std::max(ty, std::fabs(((wy + yoff - oy) / sy + p[3]) / p[2]));
    const double k = std::sqrt(1.0 + tx * tx + ty * ty) * 1.05;
    // the whole surface farther than the near plane along any such ray (so the eye is outside it, and nothing of it is clipped there),
    // and the ball in front of the eye
    if (!(clen > REye + n * k + 1e-3 * REye) || !(-ce[2] > rEye)) { ++g_oc.refNear; return; }
    // ---- the depth margin: Df (the ball's far side, eye depth) to D1, a window-depth step of 2^-16 of the range (depth 0..2^24 - 1; the
    // console's depth buffer is float (vitaGL gxm.c: DF32M), the host's 24-bit: either resolves far finer, and so does the interpolation)
    const double Df = -ce[2] + rEye;
    const double tolZ = 256.0;
    const double inv = 1.0 / Df - tolZ / (sz * -p[5]);
    if (!(inv > 0.0) || !(Df * 1.001 < f)) { ++g_oc.refDepth; return; }
    const double D1 = 1.0 / inv;
    // ---- the cone: the ball shrunk by 2 EFB pixels at its far side's depth (twice the sideways error eps: the rasteriser's snapping of the
    // occluder's and of the occludee's vertices, 1/16 of a render pixel or less, a render pixel being at most ~1.2 EFB pixels) and 1e-4
    const double eps = Df * std::max(1.0 / std::fabs(sx * p[0]), 1.0 / std::fabs(sy * p[2]));
    const double rOcc = rEye * (1.0 - 1e-4) - 2.0 * eps;
    if (!(rOcc > 1e-3 * clen)) { ++g_oc.refDepth; return; }
    const double sinA2 = (rOcc * rOcc) / (clen * clen);
    OccInst o;
    o.sig = ViewSig();
    o.c[0] = static_cast<float>(ce[0]); o.c[1] = static_cast<float>(ce[1]); o.c[2] = static_cast<float>(ce[2]);
    o.c2 = static_cast<float>(clen * clen);
    o.cos2 = static_cast<float>((1.0 - sinA2) * (1.0 + 1e-5));
    o.sin2 = static_cast<float>(sinA2);
    o.D1 = static_cast<float>(D1 * (1.0 + 1e-6));
    if (!(o.cos2 < 1.0f)) { ++g_oc.refDepth; return; }
    int slot = g_occ.n;
    if (slot == kOccMax) {
        // (full: replace the smallest cone, if this one is larger)
        slot = 0;
        for (int i = 1; i < kOccMax; ++i)
            if (g_occ.o[i].sin2 < g_occ.o[slot].sin2) slot = i;
        if (g_occ.o[slot].sin2 >= o.sin2) { ++g_oc.registered; return; }
    } else {
        ++g_occ.n;
    }
    g_occ.o[slot] = o;
    ++g_oc.registered;
}
} // namespace

void SetPendingOccluder(const OccShape* s) { g_occPending = s; }
void OcclCounters(uint64_t out[8]) {
    const OccCtr& a = g_ocTotal;
    const OccCtr& b = g_oc;
    out[0] = a.registered + b.registered;
    out[1] = a.sCulled + b.sCulled + a.pCulled + b.pCulled;
    out[2] = a.sIdx + b.sIdx + a.pIdx + b.pIdx;
    out[3] = a.dCulled + b.dCulled;
    out[4] = a.dIdx + b.dIdx;
    out[5] = g_ocTotalAnalysed;
    out[6] = g_ocTotalClosed;
    out[7] = a.breaks + b.breaks;
}

// ================================================================ the census (GXV_GEOPASS)
// The phases are the game's (decomp GameScene::draw3D / draw2D, SceneFunction): the markers are written into the GX stream only with the
// switch on (platform/src/gx/vgx_galaxy.cpp), so with it off the stream is the game's own.
namespace {
const char* const kGeoPhaseName[17] = {"start", "sky/crystal", "planet", "map", "shadowvol", "noshadow+silh", "fill+alphashadow", "actors",
                                       "sky", "lists(plants,player)", "xlu", "shadowsurf", "effects+capture", "after-indirect", "image-effect",
                                       "2D", "unmarked"};
// the copies of the picture: the phase each was queued in (index = the copy's index in g_buf.copyTex)
std::vector<uint8_t> g_geoCopyPhase;
// repeated static geometry: one picture's table of (buffer, index range) -> first phase, matrix hash
struct GeoRep { uint32_t gen = 0, bufId = 0, idxOffset = 0, idxCount = 0; uint64_t mtx = 0; uint8_t phase = 0; };
constexpr uint32_t kGeoRepSlots = 2048;
GeoRep g_geoRep[kGeoRepSlots];
uint32_t g_geoGen = 1;
struct GeoCtr {
    uint64_t pictures = 0, staticRuns = 0, repRuns = 0, repIdx = 0, repSame = 0, repSameIdx = 0;
    uint64_t pair[17][17] = {};
    // EFB scenes by their number in the picture (at most 8 kept apart), and what ended them
    uint64_t sceneDraws[8] = {}, sceneIdx[8] = {}, sceneSeen[8] = {}, scenes = 0, sceneMore = 0;
    // occluder census: groups of consecutive runs with one matrix whose boundary edges cancel
    uint64_t groups = 0, groupsClosed = 0, groupTris = 0, closedRunsDrawn = 0;
} g_geo;
char g_geoEnds[8][96]; // (the last picture's: what ended each EFB scene)
// (group census) the open group
struct GeoGroup { uint64_t mtx = 0, bSum = 0, bRev = 0, bEdges = 0, tris = 0; uint32_t runs = 0; } g_geoGrp;
inline uint64_t MtxHash(uint32_t pi) { return pi > 61 ? 0 : XXH3_64bits(g.xfPos[pi], 3 * sizeof g.xfPos[0]); }
void GeoGroupClose() {
    if (g_geoGrp.runs >= 1 && g_geoGrp.tris) {
        ++g_geo.groups;
        if (g_geoGrp.bEdges && g_geoGrp.runs >= 2 && g_geoGrp.bSum == g_geoGrp.bRev) { ++g_geo.groupsClosed; g_geo.groupTris += g_geoGrp.tris; }
    }
    g_geoGrp = GeoGroup{};
}
void GeoStaticRun(uint32_t bufId, uint32_t idxOffset, uint32_t idxCount, const OccShape* occ) {
    ++g_geo.staticRuns;
    const bool pal = g.vcd[0] != 0;
    const uint64_t mh = pal ? 0 : MtxHash(g.matIdxA & 0x3F);
    uint32_t h = (bufId * 0x9E3779B1u) ^ (idxOffset * 0x85EBCA6Bu) ^ (idxCount * 0xC2B2AE35u);
    for (uint32_t probe = 0; probe < 8; ++probe, ++h) {
        GeoRep& e = g_geoRep[h & (kGeoRepSlots - 1)];
        if (e.gen != g_geoGen) {
            e.gen = g_geoGen; e.bufId = bufId; e.idxOffset = idxOffset; e.idxCount = idxCount; e.mtx = mh; e.phase = g_geoPhase;
            break;
        }
        if (e.bufId == bufId && e.idxOffset == idxOffset && e.idxCount == idxCount) {
            ++g_geo.repRuns;
            g_geo.repIdx += idxCount;
            if (mh && e.mtx == mh) { ++g_geo.repSame; g_geo.repSameIdx += idxCount; }
            ++g_geo.pair[e.phase][g_geoPhase];
            break;
        }
    }
    // (groups: consecutive analysed runs with one matrix)
    if (!occ || !occ->analysed || pal) { GeoGroupClose(); return; }
    if (g_geoGrp.runs && g_geoGrp.mtx != mh) GeoGroupClose();
    g_geoGrp.mtx = mh;
    ++g_geoGrp.runs;
    g_geoGrp.bSum += occ->bSum;
    g_geoGrp.bRev += occ->bRev;
    g_geoGrp.bEdges += occ->bEdges;
    g_geoGrp.tris += occ->tris;
    if (occ->orient) ++g_geo.closedRunsDrawn;
}
// FrontPresent (after the copies' and passes' planning): the picture's EFB scenes, as the back end will begin them (CountScenes' rule)
void GeoFrameScenes() {
    bool inPass = false;
    uint32_t passDirectTex = 0, target = 0;
    int scene = -1;
    for (int i = 0; i < 8; ++i) g_geoEnds[i][0] = 0;
    auto end = [&](const char* what) {
        if (scene >= 0 && scene < 8 && !g_geoEnds[scene][0]) std::snprintf(g_geoEnds[scene], sizeof g_geoEnds[scene], "%s", what);
    };
    auto efb = [&] {
        if (target == 1) return;
        target = 1;
        ++scene;
        ++g_geo.scenes;
        if (scene < 8) ++g_geo.sceneSeen[scene];
        else ++g_geo.sceneMore;
    };
    auto other = [&](uint32_t id, const char* what) {
        if (target == 1) end(what);
        target = id;
    };
    for (const Cmd& c : g_buf.cmds) {
        switch (c.type) {
        case CMD_PASS_BEGIN:
            other(g_buf.passes[c.arg].atlasTex ? g_buf.passes[c.arg].atlasTex + 16 : 2, "an offscreen pass");
            inPass = true;
            passDirectTex = g_buf.passes[c.arg].directTex;
            break;
        case CMD_DRAW:
            if (!inPass) {
                efb();
                if (scene >= 0 && scene < 8) { ++g_geo.sceneDraws[scene]; g_geo.sceneIdx[scene] += g_buf.draws[c.arg].idxCount; }
            }
            break;
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!t.skip) {
                const bool direct = t.fromPass && inPass && passDirectTex == t.texId;
                if (!direct) {
                    char what[96];
                    const bool whole = t.src.x == 0 && t.src.y == 0 && t.src.w >= static_cast<int>(g.efbW) && t.src.h >= static_cast<int>(g.efbH) - 8;
                    const bool alpha = !t.depth && t.mat[0][3] == 1.0f && t.mat[1][3] == 1.0f && t.mat[2][3] == 1.0f && t.mat[0][0] == 0.0f;
                    const bool rgb565 = !t.depth && t.mat[3][3] == 0.0f && t.add[3] == 1.0f && t.mat[0][0] == 1.0f;
                    const unsigned ph = c.arg < g_geoCopyPhase.size() ? g_geoCopyPhase[c.arg] : 16u;
                    std::snprintf(what, sizeof what, "copy %s%s (%d,%d) %dx%d -> %ux%u%s in %s", whole ? "whole EFB " : "", t.depth ? "depth" : alpha ? "A8" : rgb565 ? "RGB565" : "colour",
                                  t.src.x, t.src.y, t.src.w, t.src.h, t.dstW, t.dstH, t.fromPass ? " from a pass" : "", kGeoPhaseName[ph]);
                    other(t.dstTexW ? t.texId + 16 : 2, what);
                }
            }
            if (!t.srcTex) inPass = false;
            if (t.clear.enable) efb();
            break;
        }
        case CMD_COPY_DISP:
            if (target == 1) end("the display copy");
            if (g_buf.copyDisp[c.arg].clear.enable) efb();
            break;
        default: break;
        }
    }
    end("the picture's end");
}
// (GXV_GEOPASS_EVERY=<n>, tests: the two lines every n pictures instead of every 10 s)
uint32_t LogEveryPictures() { return static_cast<uint32_t>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_GEOPASS_EVERY", 0), 0, 1 << 20)))); }
inline bool LogDue(std::chrono::steady_clock::time_point& last, uint64_t pictures) {
    if (const uint32_t every = LogEveryPictures()) return pictures >= every;
    return std::chrono::steady_clock::now() - last >= std::chrono::seconds(10);
}
void GeoLog() {
    static auto last = std::chrono::steady_clock::now();
    if (!LogDue(last, g_geo.pictures)) return;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const double n = g_geo.pictures ? static_cast<double>(g_geo.pictures) : 1.0;
    char line[3072];
    int len = std::snprintf(line, sizeof line, "geopass: %.0f s, %llu pictures; a picture: EFB scenes %.2f", secs, static_cast<unsigned long long>(g_geo.pictures),
                            g_geo.scenes / n);
    for (int i = 0; i < 8 && len > 0 && len < static_cast<int>(sizeof line); ++i) {
        if (!g_geo.sceneSeen[i]) continue;
        len += std::snprintf(line + len, sizeof line - len, "; scene %d (in %.0f%%): %.0f draws %.1fk indices, ended by %s", i, 100.0 * g_geo.sceneSeen[i] / n,
                             g_geo.sceneDraws[i] / n, g_geo.sceneIdx[i] / n / 1000.0, g_geoEnds[i][0] ? g_geoEnds[i] : "?");
    }
    if (len > 0 && len < static_cast<int>(sizeof line)) len += std::snprintf(line + len, sizeof line - len, "; GX draws / k indices by phase:");
    for (int p = 0; p < 17 && len > 0 && len < static_cast<int>(sizeof line); ++p) {
        if (!g_geoPh[p].calls) continue;
        len += std::snprintf(line + len, sizeof line - len, " %s %.0f/%.1f", kGeoPhaseName[p], g_geoPh[p].calls / n, g_geoPh[p].idx / n / 1000.0);
        if (g_geoPh[p].occCalls) len += std::snprintf(line + len, sizeof line - len, " (hidden %.1f/%.1f)", g_geoPh[p].occCalls / n, g_geoPh[p].occIdx / n / 1000.0);
    }
    if (len > 0 && len < static_cast<int>(sizeof line))
        len += std::snprintf(line + len, sizeof line - len, "; static runs %.0f, of them the same buffer range again %.1f (%.1fk indices), with the same matrix too %.1f (%.1fk)",
                             g_geo.staticRuns / n, g_geo.repRuns / n, g_geo.repIdx / n / 1000.0, g_geo.repSame / n, g_geo.repSameIdx / n / 1000.0);
    // (the four commonest phase pairs of a repeat: first use -> again)
    for (int top = 0; top < 4 && len > 0 && len < static_cast<int>(sizeof line); ++top) {
        int ba = -1, bb = -1;
        uint64_t best = 0;
        for (int a = 0; a < 17; ++a)
            for (int b = 0; b < 17; ++b)
                if (g_geo.pair[a][b] > best) { best = g_geo.pair[a][b]; ba = a; bb = b; }
        if (ba < 0) break;
        len += std::snprintf(line + len, sizeof line - len, "%s%s->%s %.1f", top ? ", " : " [", kGeoPhaseName[ba], kGeoPhaseName[bb], best / n);
        g_geo.pair[ba][bb] = 0;
        if (top == 3 || len <= 0) break;
    }
    if (len > 0 && len < static_cast<int>(sizeof line) && g_geo.repRuns) len += std::snprintf(line + len, sizeof line - len, "]");
    if (len > 0 && len < static_cast<int>(sizeof line))
        len += std::snprintf(line + len, sizeof line - len, "; occluder census: runs analysed %llu, closed %llu (since the start), closed runs drawn %.1f, groups of runs with one matrix "
                             "%.1f of which closed together (not alone) %.2f (%.1fk triangles)",
                             static_cast<unsigned long long>(g_ocTotalAnalysed), static_cast<unsigned long long>(g_ocTotalClosed), g_geo.closedRunsDrawn / n, g_geo.groups / n,
                             g_geo.groupsClosed / n, g_geo.groupTris / n / 1000.0);
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    for (GeoPhaseCtr& c : g_geoPh) c = GeoPhaseCtr{};
    g_geo = GeoCtr{};
}
void OcclLog() {
    static auto last = std::chrono::steady_clock::now();
    if (!LogDue(last, g_oc.pictures)) return;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const OccCtr& k = g_oc;
    const double n = k.pictures ? static_cast<double>(k.pictures) : 1.0;
    char line[1024];
    std::snprintf(line, sizeof line, "occlcull: GXV_OCCLCULL=%d, %.0f s, %llu pictures; a picture: left out%s: static runs %.1f of %.1f tested (%.1fk indices), skinned runs %.1f of %.1f "
                         "(%.1fk), dynamic draws %.1f of %.1f (%.1fk), not decoded yet %.1f; in all %.1f draws %.1fk indices; occluders drawn %.1f, registered %.1f, "
                         "refused: state %.1f, cull %.1f, matrix %.1f, too near %.1f, depth %.1f; epochs ended %.1f; runs analysed %llu (%llu triangles, %llu over the "
                         "budget), closed with a ball %llu",
                         OcclCullMode(), secs, static_cast<unsigned long long>(k.pictures), OcclCullMode() == 2 ? " (would be: GXV_OCCLCULL=2 counts only)" : "",
                         k.sCulled / n, k.sTested / n, k.sIdx / n / 1000.0, k.pCulled / n, k.pTested / n, k.pIdx / n / 1000.0, k.dCulled / n, k.dTested / n,
                         k.dIdx / n / 1000.0, k.dLater / n, (k.sCulled + k.pCulled + k.dCulled) / n, (k.sIdx + k.pIdx + k.dIdx) / n / 1000.0, k.offered / n,
                         k.registered / n, k.refState / n, k.refCull / n, k.refMtx / n, k.refNear / n, k.refDepth / n, k.breaks / n,
                         static_cast<unsigned long long>(k.runsAnalysed), static_cast<unsigned long long>(k.trisAnalysed), static_cast<unsigned long long>(k.runsOver),
                         static_cast<unsigned long long>(k.runsClosed));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    OccCtr& t = g_ocTotal;
    t.registered += k.registered; t.sCulled += k.sCulled; t.pCulled += k.pCulled; t.sIdx += k.sIdx; t.pIdx += k.pIdx;
    t.dCulled += k.dCulled; t.dIdx += k.dIdx; t.breaks += k.breaks;
    g_oc = OccCtr{};
}
} // namespace

void GeoPhaseMarker(uint32_t phase) { g_geoPhase = static_cast<uint8_t>(phase < 16 ? phase : 16); }

namespace {
// (vgx_dlcache.cpp Promote, through OcclAnalyse's callers: the census of what was analysed)
void OcclNoteAnalysed(const OccShape& s) {
    if (s.analysed == 2) { ++g_oc.runsOver; return; }
    if (s.analysed != 1) return;
    ++g_oc.runsAnalysed;
    ++g_ocTotalAnalysed;
    g_oc.trisAnalysed += s.tris;
    if (s.orient) { ++g_oc.runsClosed; ++g_ocTotalClosed; }
}
// QueueCopyTex: the copy's phase (census) and the end of the occluders' epoch when it clears depth.
inline void GeoOcclCopy(bool clearDepth) {
    if (clearDepth) OcclBreak();
    if (GeoPassMode()) {
        const size_t i = g_buf.copyTex.size(); // (the copy about to be appended)
        if (g_geoCopyPhase.size() <= i) g_geoCopyPhase.resize(i + 1, 16);
        g_geoCopyPhase[i] = g_geoPhase;
    }
}
// FrontPresent, before the buffer is handed on: the census of the picture, the occluders forgotten.
void GeoOcclFrameEnd() {
    g_occ.n = 0;
    g_occlAnalysedThisPicture = 0;
    if (OcclCullMode()) {
        ++g_oc.pictures;
        OcclLog();
    }
    if (GeoPassMode()) {
        GeoGroupClose();
        GeoFrameScenes();
        ++g_geo.pictures;
        if (++g_geoGen == 0) { for (GeoRep& r : g_geoRep) r = GeoRep{}; g_geoGen = 1; }
        g_geoCopyPhase.clear();
        GeoLog();
        g_geoPhase = 16;
    }
}
} // namespace

void OcclAnalyse(OccShape& s, const uint8_t* vtx, size_t vtxBytes, uint32_t stride, uint32_t addrBase, const uint16_t* idx, uint32_t idxCount) {
    AnalyseRun(s, vtx, vtxBytes, stride, addrBase, idx, idxCount);
    OcclNoteAnalysed(s);
}
