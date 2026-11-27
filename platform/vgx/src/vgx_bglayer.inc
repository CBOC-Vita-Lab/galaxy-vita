// bglayer25 (11 Nov 2026): GXV_BGLAYER (a bit mask: 1 the layer cache, 2 the census; 3 both), the census of the far-background layer idea (agents/bglayer25/DESIGN.md). Included into vgx_draw.cpp, namespace vgx,
// after vgx_geopass.inc. Everything here runs on the player's thread; default 0 = nothing is written into the stream and nothing is counted.
//
// The game writes PORT_GX_MARK_BGCAT (BP 0xFF kind 28) around every draw category it draws (decomp SceneFunction.cpp CategoryList::drawOpa / drawXlu /
// execute): the id is 1 + the MR::DrawBufferType (1..41) or 64 + the MR::DrawType (64..146), 0 closes it. The two draw sites that count indices for
// GXV_GEOPASS (a dynamic draw, a static run) tally the draws and indices under the open category; every 5 s one "bglayer-fe:" line says, per picture, the
// share of the picture's indices that the background-by-nature categories (Sky, Air, Sun, AstroDomeSky) and the far-planet list (PlanetLow) own,
// and the biggest categories. Nothing here changes a pixel; the second worker gives its picture up at the marker (as the other census markers do).

int BgLayerMode() {
    const int m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_BGLAYER", 0), 0, 3)));
    return m;
}

namespace {
constexpr unsigned kBgCats = 160;
struct BgTally {
    uint64_t calls[kBgCats];
    uint64_t idx[kBgCats];
};
uint8_t g_bgCat = 0;
BgTally g_bgWin;
uint32_t g_bgPics = 0;
int64_t g_bgLastNs = 0;
const char* const kBgBufName[41] = {
    "ClippedMapParts", "Sky", "Air", "Sun", "Planet", "PlanetLow", "Environment", "EnvironmentStrongLight", "MapObj", "MapObjWeakLight",
    "MapObjStrongLight", "NoShadowedMapObj", "NoShadowedMapObjStrongLight", "NoSilhouettedMapObj", "NoSilhouettedMapObjWeakLight",
    "NoSilhouettedMapObjStrongLight", "NPC", "Ride", "Enemy", "EnemyDecoration", "Player", "PlayerDecoration", "CrystalBox", "UNK17", "UNK18",
    "IndirectMapObj", "IndirectMapObjStrongLight", "IndirectNpc", "IndirectEnemy", "IndirectPlanet", "BloomModel", "TripodBoss", "Crystal",
    "CrystalItem", "GlaringLight", "AstroDomeSky", "Model3DFor2D", "UNK25", "UNK26", "MirrorMapObj", "UNK28"};
void BgCatName(unsigned c, char* out, size_t n) {
    if (c == 0) std::snprintf(out, n, "other");
    else if (c <= 41) std::snprintf(out, n, "%s", kBgBufName[c - 1]);
    else if (c >= 64) std::snprintf(out, n, "DrawType_0x%X", c - 64);
    else std::snprintf(out, n, "cat%u", c);
}
inline int64_t BgNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

static void BgCatMarker(uint32_t cat) { g_bgCat = static_cast<uint8_t>(cat < kBgCats ? cat : 0); }

// (the two draw sites that count for GXV_GEOPASS)
static inline void BgCatCount(uint32_t idxCount) {
    ++g_bgWin.calls[g_bgCat];
    g_bgWin.idx[g_bgCat] += idxCount;
}

namespace {
void BgFrameEnd() {
    g_bgCat = 0;
    ++g_bgPics;
    const int64_t now = BgNowNs();
    if (g_bgLastNs == 0) { g_bgLastNs = now; return; }
    static const int64_t windowNs = static_cast<int64_t>(std::clamp<long>(EnvInt("GXV_BGLAYER_LOG_MS", 5000), 1, 600000)) * 1000000ll; // (a test shortens it)
    if (now - g_bgLastNs < windowNs || g_bgPics == 0) return;
    const double n = static_cast<double>(g_bgPics);
    double allCalls = 0, allIdx = 0;
    for (unsigned i = 0; i < kBgCats; ++i) { allCalls += static_cast<double>(g_bgWin.calls[i]); allIdx += static_cast<double>(g_bgWin.idx[i]); }
    auto cat = [&](unsigned c, double& calls, double& idx) { calls += static_cast<double>(g_bgWin.calls[c]) / n; idx += static_cast<double>(g_bgWin.idx[c]) / n; };
    double natC = 0, natI = 0, lowC = 0, lowI = 0, planC = 0, planI = 0, envC = 0, envI = 0, mapC = 0, mapI = 0, ind = 0, indC = 0, dtC = 0, dtI = 0;
    cat(1 + 1, natC, natI); cat(1 + 0x23, natC, natI);  // Sky AstroDomeSky
    double airC = 0, airI = 0;
    cat(1 + 2, airC, airI); cat(1 + 3, airC, airI);  // Air Sun (by distance in the game's classification; shown apart)
    cat(1 + 5, lowC, lowI);                                                                                 // PlanetLow
    cat(1 + 4, planC, planI);                                                                               // Planet
    cat(1 + 6, envC, envI); cat(1 + 7, envC, envI);                                                         // Environment (+ strong light)
    for (unsigned b = 8; b <= 0xF; ++b) cat(1 + b, mapC, mapI);                                             // MapObj family
    cat(1 + 0, mapC, mapI);                                                                                 // ClippedMapParts
    for (unsigned b = 0x19; b <= 0x1D; ++b) cat(1 + b, indC, ind);                                          // Indirect*
    for (unsigned c = 64; c < kBgCats; ++c) cat(c, dtC, dtI);                                               // every DrawType list
    // the five biggest categories by indices
    unsigned top[5] = {0, 0, 0, 0, 0};
    for (unsigned k = 0; k < 5; ++k) {
        uint64_t best = 0;
        for (unsigned c = 0; c < kBgCats; ++c) {
            bool used = false;
            for (unsigned j = 0; j < k; ++j) used = used || top[j] == c;
            if (!used && g_bgWin.idx[c] > best) { best = g_bgWin.idx[c]; top[k] = c; }
        }
        if (best == 0) top[k] = 0xFFFFu;
    }
    char tops[400];
    size_t at = 0;
    tops[0] = 0;
    for (unsigned k = 0; k < 5 && top[k] != 0xFFFFu; ++k) {
        char nm[48];
        BgCatName(top[k], nm, sizeof nm);
        const int w = std::snprintf(tops + at, sizeof tops - at, "%s%s %.1f/%.1fk", k ? ", " : "", nm, static_cast<double>(g_bgWin.calls[top[k]]) / n,
                                    static_cast<double>(g_bgWin.idx[top[k]]) / n / 1000.0);
        if (w < 0 || static_cast<size_t>(w) >= sizeof tops - at) break;
        at += static_cast<size_t>(w);
    }
    const double tot = allIdx / n;
    auto pc = [&](double v) { return tot > 0 ? 100.0 * v / tot : 0.0; };
    char line[1300];
    std::snprintf(line, sizeof line,
                  "bglayer-fe: %u pictures: per picture %.0f draws %.1fk indices; by nature (Sky+AstroDomeSky) %.1f draws %.1fk idx (%.1f%%); Air+Sun %.1f / %.1fk (%.1f%%); PlanetLow %.1f / %.1fk (%.1f%%); "
                  "Planet %.1f / %.1fk (%.1f%%); Environment %.1f / %.1fk (%.1f%%); MapObj family %.1f / %.1fk (%.1f%%); Indirect* %.1f / %.1fk (%.1f%%); "
                  "DrawType lists %.1f / %.1fk (%.1f%%); biggest (draws/kidx): %s",
                  g_bgPics, allCalls / n, tot / 1000.0, natC, natI / 1000.0, pc(natI), airC, airI / 1000.0, pc(airI), lowC, lowI / 1000.0, pc(lowI), planC, planI / 1000.0, pc(planI), envC,
                  envI / 1000.0, pc(envI), mapC, mapI / 1000.0, pc(mapI), indC, ind / 1000.0, pc(ind), dtC, dtI / 1000.0, pc(dtI), tops);
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_bgWin = BgTally{};
    g_bgPics = 0;
    g_bgLastNs = now;
}
} // namespace

// BgMarker is defined in vgx_bglayer2.inc (after WarmSnap, which it uses): kind 28 is the census (above), 29..31 the layer cache.
